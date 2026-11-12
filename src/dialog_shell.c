/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - a Unix shell on the radio's own screen, opened from the Sentry's E SYSTEM.
 *
 *  /bin/sh as a login shell on a pseudo terminal, its output in the app's body in a monospaced font, as a plain
 *  terminal that only scrolls: a carriage return starts the line again (the shell's line editing redraws it whole),
 *  a backspace or a step left takes the last character back, other escape sequences are dropped (TERM=dumb asks for
 *  few). What a keyboard types goes to it as typed (a USB or Bluetooth one; Enter, Backspace, Tab, the arrows, Esc);
 *  Type opens the on-screen keyboard for a line, Ctrl-C interrupts what runs. Closing the app ends the shell and
 *  everything it started (SIGHUP to its session, then SIGKILL). It runs as the GUI does: root.
 */

#define _GNU_SOURCE        /* posix_openpt, ptsname */

#include "dialog_shell.h"

#include "app_frame.h"
#include "button_bar.h"
#include "gui_styles.h"
#include "key_input.h"
#include "radio.h"
#include "text_popup.h"
#include "theme.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#define TERM_SIZE   32768   /* the text kept; whole lines go from the top when it is full */
#define TICK_MS     40
#define COLS        74      /* codec_18 across the body */
#define ROWS        13

extern const lv_font_t codec_18;

static void construct_cb(lv_obj_t *parent);
static void destruct_cb(void);
static void page_cb(button_data_t *b);
static void type_cb(button_data_t *b);
static void ctrl_c_cb(button_data_t *b);
static void clear_cb(button_data_t *b);
static void close_cb(button_data_t *b);

static button_data_t btn_page  = {.type = BTN_TEXT, .label = "(SHELL)", .press = page_cb};
static button_data_t btn_type  = {.type = BTN_TEXT, .label = "Type", .press = type_cb};
static button_data_t btn_ctrlc = {.type = BTN_TEXT, .label = "Ctrl-C", .press = ctrl_c_cb};
static button_data_t btn_clear = {.type = BTN_TEXT, .label = "Clear", .press = clear_cb};
static button_data_t btn_close = {.type = BTN_TEXT, .label = "Close", .press = close_cb};

static buttons_page_t page = {.items = {&btn_page, &btn_type, &btn_ctrlc, &btn_clear, &btn_close}};

static dialog_t dialog = {.construct_cb = construct_cb, .destruct_cb = destruct_cb, .btn_page = &page};

dialog_t *dialog_shell = &dialog;

static app_frame_t frame;
static lv_obj_t   *body, *label;
static lv_timer_t *timer;
static int         master = -1;
static pid_t       child = -1;
static bool        ended, osk_open;

static char   text[TERM_SIZE];
static size_t len, line_start;      /* line_start: where the line being written starts */
static int    esc;                  /* in an escape sequence: 1 after ESC, 2 in CSI, 3 in OSC */

/* ---- the terminal: the shell's output into text ---- */

static void put(char c) {
    if (len >= TERM_SIZE - 1) {     /* full: the oldest lines go */
        char  *nl  = memchr(text + TERM_SIZE / 4, '\n', len - TERM_SIZE / 4);
        size_t cut = nl ? (size_t)(nl - text) + 1 : TERM_SIZE / 4;

        memmove(text, text + cut, len - cut);
        len -= cut;
        line_start = line_start > cut ? line_start - cut : 0;
    }
    text[len++] = c;
    if (c == '\n') line_start = len;
}

static void back(void) {
    if (len > line_start) {
        len--;
        while (len > line_start && ((unsigned char)text[len] & 0xc0) == 0x80) len--;    /* all of a UTF-8 letter */
    }
}

static void feed(const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];

        if (esc == 1) {             /* after ESC */
            esc = c == '[' ? 2 : c == ']' ? 3 : 0;
            continue;
        }
        if (esc == 2) {             /* CSI: parameters, then a final byte */
            if (c >= 0x40 && c <= 0x7e) {
                if (c == 'D') back();   /* a step left: the line editor taking a character back */
                esc = 0;
            }
            continue;
        }
        if (esc == 3) {             /* OSC: to BEL or ESC */
            if (c == 7) esc = 0;
            else if (c == 0x1b) esc = 1;
            continue;
        }
        switch (c) {
            case 0x1b: esc = 1; break;
            case '\r':
                if (i + 1 < n && s[i + 1] == '\n') break;
                len = line_start;   /* the line again: the shell redraws it */
                break;
            case '\b': back(); break;
            case '\t':
                do put(' '); while ((len - line_start) % 8);
                break;
            case '\n': put('\n'); break;
            default:
                if (c >= ' ' || c >= 0x80) put((char)c);
                break;
        }
    }
    text[len] = '\0';
}

static void feed_str(const char *s) {
    feed(s, strlen(s));
}

/* ---- the shell ---- */

static void send(const char *s, size_t n) {
    if (master >= 0 && !ended) {
        ssize_t w = write(master, s, n);
        (void)w;
    }
}

static bool start_shell(void) {
    struct winsize ws = {.ws_row = ROWS, .ws_col = COLS};
    char          *argv[] = {"sh", "-l", NULL};
    char          *envp[] = {"TERM=dumb", "HOME=/root", "SHELL=/bin/sh", "USER=root", "LOGNAME=root",
                             "PATH=/usr/sbin:/usr/bin:/sbin:/bin", "PS1=\\h:\\w\\$ ", NULL};
    const char    *slave;
    int            fd;

    master = posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0 || grantpt(master) || unlockpt(master) || !(slave = ptsname(master))) goto fail;
    ioctl(master, TIOCSWINSZ, &ws);

    child = fork();
    if (child < 0) goto fail;
    if (child == 0) {               /* only what is safe between fork and exec in a threaded program */
        setsid();
        fd = open(slave, O_RDWR);
        if (fd < 0) _exit(127);
        ioctl(fd, TIOCSCTTY, 0);
        dup2(fd, 0);
        dup2(fd, 1);
        dup2(fd, 2);
        if (fd > 2) close(fd);
        close(master);
        if (chdir("/root")) {
            if (chdir("/")) _exit(127);
        }
        execve("/bin/sh", argv, envp);
        _exit(127);
    }
    fcntl(master, F_SETFL, fcntl(master, F_GETFL) | O_NONBLOCK);
    fcntl(master, F_SETFD, FD_CLOEXEC);
    return true;
fail:
    if (master >= 0) close(master);
    master = -1;
    return false;
}

static void stop_shell(void) {
    if (child > 0) {
        kill(-child, SIGHUP);       /* its session: the shell and what it started */
        kill(child, SIGHUP);
        usleep(50000);
        kill(-child, SIGKILL);
        kill(child, SIGKILL);
        waitpid(child, NULL, 0);
    }
    if (master >= 0) close(master);
    master = -1;
    child  = -1;
}

static void show(void) {
    lv_label_set_text(label, text);
    lv_obj_scroll_to_y(body, LV_COORD_MAX, LV_ANIM_OFF);
}

static void timer_cb(lv_timer_t *t) {
    char    buf[2048];
    ssize_t n;
    bool    got = false;

    (void)t;
    while (master >= 0 && (n = read(master, buf, sizeof(buf))) > 0) {
        feed(buf, (size_t)n);
        got = true;
    }
    if (master >= 0 && !ended && child > 0 && waitpid(child, NULL, WNOHANG) == child) {
        ended = true;
        child = -1;
        feed_str("\n[the shell ended: Close, and open it again for a new one]\n");
        lv_label_set_text(frame.head_right, "ENDED");
        got = true;
    }
    if (got) show();
}

/* ---- keys and buttons ---- */

static void key_cb(lv_event_t *e) {
    uint32_t key = *((uint32_t *)lv_event_get_param(e));

    switch (key) {
        case LV_KEY_ENTER:      /* '\n' */
        case '\r': send("\r", 1); break;
        case LV_KEY_BACKSPACE: send("\x7f", 1); break;
        case LV_KEY_ESC: send("\x1b", 1); break;
        case LV_KEY_UP: send("\x1b[A", 3); break;
        case LV_KEY_DOWN: send("\x1b[B", 3); break;
        case LV_KEY_RIGHT: send("\x1b[C", 3); break;
        case LV_KEY_LEFT: send("\x1b[D", 3); break;
        case '\t': send("\t", 1); break;
        case KEY_VOL_LEFT_EDIT:
        case KEY_VOL_LEFT_SELECT: radio_change_vol(-1); break;
        case KEY_VOL_RIGHT_EDIT:
        case KEY_VOL_RIGHT_SELECT: radio_change_vol(1); break;
        default:
            if (key >= 1 && key < 127) {        /* a letter, or a control character a keyboard sent as one */
                char c = (char)key;
                send(&c, 1);
            }
            break;
    }
}

static void osk_end(void) {
    osk_open = false;
    lv_group_add_obj(keyboard_group, dialog.obj);
    lv_group_focus_obj(dialog.obj);
    lv_group_set_editing(keyboard_group, true);
}

static bool osk_ok(void) {
    const char *s = textarea_window_get();

    send(s, strlen(s));
    send("\r", 1);
    osk_end();
    return true;
}

static bool osk_cancel(void) {
    osk_end();
    return true;
}

static void type_cb(button_data_t *b) {
    (void)b;
    if (osk_open || ended) return;
    osk_open = true;
    lv_group_remove_obj(dialog.obj);    /* the text box gets the keys */
    textarea_window_open_w_label(osk_ok, osk_cancel, "$ ");
    textarea_window_lower();            /* commands are lower case */
}

static void ctrl_c_cb(button_data_t *b) {
    (void)b;
    send("\x03", 1);
}

static void clear_cb(button_data_t *b) {
    (void)b;
    len = line_start = 0;
    text[0] = '\0';
    send("\r", 1);                      /* a fresh prompt */
    show();
}

static void close_cb(button_data_t *b) {
    (void)b;
    dialog_destruct();
}

static void page_cb(button_data_t *b) {
    (void)b;
}

/* ---- the window ---- */

static void construct_cb(lv_obj_t *parent) {
    dialog.obj = dialog_init(parent);
    app_frame_create(&frame, dialog.obj);
    lv_label_set_text(frame.head_left, "UNIX SHELL   /bin/sh");
    lv_label_set_text(frame.head_right, "ROOT");
    lv_label_set_text(frame.status, "Keyboard: types - Type: a line on screen - Ctrl-C: interrupt - Close: ends it");

    body = lv_obj_create(dialog.obj);
    lv_obj_remove_style_all(body);
    app_frame_body(body);
    lv_obj_set_style_pad_all(body, 6, 0);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_AUTO);

    label = lv_label_create(body);
    lv_obj_set_width(label, lv_pct(100));
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(label, &codec_18, 0);
    lv_obj_set_style_text_color(label, TC(text), 0);

    len = line_start = 0;
    esc   = 0;
    ended = false;
    text[0] = '\0';
    if (!start_shell()) {
        feed_str("The shell could not start (no pseudo terminal).\n");
        ended = true;
    }
    show();

    lv_group_add_obj(keyboard_group, dialog.obj);
    lv_obj_add_event_cb(dialog.obj, key_cb, LV_EVENT_KEY, NULL);
    lv_group_focus_obj(dialog.obj);
    lv_group_set_editing(keyboard_group, true);
    timer = lv_timer_create(timer_cb, TICK_MS, NULL);
}

static void destruct_cb(void) {
    if (osk_open) {
        textarea_window_close();
        osk_open = false;
    }
    if (timer) lv_timer_del(timer);
    timer = NULL;
    stop_shell();
    lv_group_remove_obj(dialog.obj);
    body = label = NULL;
}
