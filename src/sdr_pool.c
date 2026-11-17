/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - The SDR pool (sdr_pool.h). The user, 2026-11-16: "ensure the SDR pool magement is robust, and self
 *  contained, and that the rest of the GUI tabs have the ability to select, start, stop, and mux their audio as
 *  needed". Its own look at USB each second (a thread, never the GUI's: a USB host stuck in a disconnect must not take
 *  the GUI with it), and who has which decided here alone, again at each look and at each change asked for.
 */

#include "sdr_pool.h"

#include "board.h"

#include <ctype.h>
#include <dirent.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define USB_DEVICES "/sys/bus/usb/devices"
#define STOP_FILE   BOARD_DATA "/sdr_pool"

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static sdr_unit_t      units[SDR_POOL_UNITS];
static int             n_units, n_rtls;
static bool            wants[SDR_OWNERS];
static bool            muted[SDR_OWNERS];
static int             held_by[SDR_OWNERS];            /* the unit each owner has, -1 none */
static rtl_dev_t       b_held = RTL_DEV_NONE, b_want = RTL_DEV_NONE;
static int             b_held_rtl = -1, b_want_rtl = -1;
static char            stopped[SDR_POOL_UNITS][24];    /* serials (or "HACKRF", "BLADERF") the user stopped */
static int             n_stopped;
static atomic_uint     gen;

static bool read_line(const char *path, char *out, size_t n) {
    FILE *f = fopen(path, "r");

    if (!f) return false;
    bool ok = fgets(out, (int)n, f) != NULL;
    fclose(f);
    if (ok) out[strcspn(out, "\r\n")] = 0;
    return ok;
}

/* Numbers as numbers: 1004 after 1000, "3-1.10" after "3-1.9" (x6x00-rtlspec orders its RTLs so too: sdr_rtl.c) */
static int num_cmp(const char *a, const char *b) {
    while (*a && *b) {
        if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
            unsigned long long x = strtoull(a, (char **)&a, 10), y = strtoull(b, (char **)&b, 10);
            if (x != y) return x < y ? -1 : 1;
        } else {
            if (*a != *b) return (unsigned char)*a - (unsigned char)*b;
            a++, b++;
        }
    }
    return *a ? 1 : *b ? -1 : 0;
}

static int unit_cmp(const void *pa, const void *pb) {
    const sdr_unit_t *a = pa, *b = pb;
    int               c = num_cmp(a->sn, b->sn);

    return c ? c : num_cmp(a->loc, b->loc);
}

static const char *stop_key(const sdr_unit_t *u) {
    return u->kind == RTL_DEV_RTL ? u->sn : u->name;
}

static bool is_stopped(const sdr_unit_t *u) {
    for (int i = 0; i < n_stopped; i++)
        if (!strcmp(stopped[i], stop_key(u))) return true;
    return false;
}

/* What is on USB: each RTL (0bda:2838, 0bda:2832), a HackRF (1d50:6089), a bladeRF (2cf0:5250, 2cf0:5246, 1d50:6066);
 * an RTL's power its hub's (bmAttributes bit 6; one straight on the HOST port has the radio's) */
static int scan(sdr_unit_t *out, int *rtls) {
    DIR           *d = opendir(USB_DEVICES);
    struct dirent *e;
    sdr_unit_t     r[8], hk = {0}, bl = {0};
    int            nr = 0, n = 0;
    bool           has_hk = false, has_bl = false;

    while (d && (e = readdir(d))) {
        char path[300], v[8], p[8];

        if (e->d_name[0] == '.' || strchr(e->d_name, ':')) continue;
        snprintf(path, sizeof(path), USB_DEVICES "/%s/idVendor", e->d_name);
        if (!read_line(path, v, sizeof(v))) continue;
        snprintf(path, sizeof(path), USB_DEVICES "/%s/idProduct", e->d_name);
        if (!read_line(path, p, sizeof(p))) continue;
        bool rtl = !strcmp(v, "0bda") && (!strcmp(p, "2838") || !strcmp(p, "2832"));
        bool hackrf = !strcmp(v, "1d50") && !strcmp(p, "6089");
        bool blade = (!strcmp(v, "2cf0") && (!strcmp(p, "5250") || !strcmp(p, "5246"))) ||
                     (!strcmp(v, "1d50") && !strcmp(p, "6066"));
        sdr_unit_t u = {.rtl = -1, .owner = SDR_OWN_NONE};

        if (!rtl && !hackrf && !blade) continue;
        snprintf(u.loc, sizeof(u.loc), "%s", e->d_name);
        snprintf(path, sizeof(path), USB_DEVICES "/%s/serial", e->d_name);
        if (!read_line(path, u.sn, sizeof(u.sn))) u.sn[0] = 0;
        {
            char  hub[32], attr[8], *dot;

            snprintf(hub, sizeof(hub), "%s", e->d_name);
            if ((dot = strrchr(hub, '.'))) {
                *dot = 0;
                snprintf(path, sizeof(path), USB_DEVICES "/%s/bmAttributes", hub);
                u.own_power = read_line(path, attr, sizeof(attr)) && (strtoul(attr, NULL, 16) & 0x40);
            }
        }
        if (rtl && nr < 8) {
            u.kind  = RTL_DEV_RTL;
            r[nr++] = u;
        } else if (hackrf && !has_hk) {
            u.kind = RTL_DEV_HACKRF;
            snprintf(u.name, sizeof(u.name), "HACKRF");
            hk     = u;
            has_hk = true;
        } else if (blade && !has_bl) {
            u.kind = RTL_DEV_BLADERF;
            snprintf(u.name, sizeof(u.name), "BLADERF");
            bl     = u;
            has_bl = true;
        }
    }
    if (d) closedir(d);
    qsort(r, (size_t)nr, sizeof(r[0]), unit_cmp);
    for (int i = 0; i < nr; i++) {
        r[i].rtl = i;
        snprintf(r[i].name, sizeof(r[i].name), "%s", rtl_spectrum_rtl_tag(i));
        out[n++] = r[i];
    }
    if (has_hk) out[n++] = hk;
    if (has_bl) out[n++] = bl;
    *rtls = nr;
    return n;
}

int sdr_pool_array_ch(const sdr_unit_t *u) {
    if (u->kind != RTL_DEV_RTL || strlen(u->sn) != 4 || strncmp(u->sn, "100", 3) || u->sn[3] < '0' || u->sn[3] > '4')
        return -1;
    return u->sn[3] - '0';
}

int sdr_pool_array_count(void) {
    int n = 0;

    sdr_pool_init();
    pthread_mutex_lock(&lock);
    for (int i = 0; i < n_units; i++) n += sdr_pool_array_ch(&units[i]) >= 0;
    pthread_mutex_unlock(&lock);
    return n;
}

static int find(rtl_dev_t kind, int rtl) {
    for (int i = 0; i < n_units; i++)
        if (units[i].kind == kind && (kind != RTL_DEV_RTL || units[i].rtl == (rtl < 0 ? 0 : rtl))) return i;
    return -1;
}

/* Who has which, from scratch: B; page I's array; the watch channels from the last RTL down; AIRSPACE's spares from the
 * first up. Under lock; true when anything changed */
static bool resolve(void) {
    int  before[SDR_OWNERS];
    bool bus = false;                   /* one on the radio's own power streams already */

    memcpy(before, held_by, sizeof(before));
    for (int o = 0; o < SDR_OWNERS; o++) held_by[o] = -1;
    for (int i = 0; i < n_units; i++) {
        units[i].owner   = SDR_OWN_NONE;
        units[i].stopped = is_stopped(&units[i]);
    }
    /* B: the one asked for while it is there, else the one the daemon has (it may be opening the other) */
    int b = b_want != RTL_DEV_NONE ? find(b_want, b_want_rtl) : -1;

    if (b < 0 || units[b].stopped) b = b_held != RTL_DEV_NONE ? find(b_held, b_held_rtl) : -1;
    if (b >= 0) {
        units[b].owner     = SDR_OWN_B;
        held_by[SDR_OWN_B] = b;
        bus                = !units[b].own_power;
    }
    if (wants[SDR_OWN_DF])                      /* page I: every stick of the Kraken B has not (B is lent to it) */
        for (int i = 0; i < n_units; i++)
            if (sdr_pool_array_ch(&units[i]) >= 0 && units[i].owner == SDR_OWN_NONE && !units[i].stopped) {
                units[i].owner = SDR_OWN_DF;
                if (held_by[SDR_OWN_DF] < 0) held_by[SDR_OWN_DF] = i;
            }
    for (int k = 0; k < SDR_POOL_WATCH; k++) {    /* watch channels: an RTL of its own power each, from the last */
        int o = SDR_OWN_WATCH + k;

        if (!wants[o]) continue;
        for (int i = n_units - 1; i >= 0 && held_by[o] < 0; i--)
            if (units[i].kind == RTL_DEV_RTL && units[i].owner == SDR_OWN_NONE && units[i].own_power &&
                !units[i].stopped) {
                units[i].owner = o;
                held_by[o]     = i;
            }
    }
    for (int k = 0; k < SDR_POOL_ADSB; k++) {     /* AIRSPACE's spares: from the first */
        int o = SDR_OWN_ADSB + k;

        if (!wants[o]) continue;
        for (int i = 0; i < n_units && held_by[o] < 0; i++)
            if (units[i].kind == RTL_DEV_RTL && units[i].owner == SDR_OWN_NONE && !units[i].stopped &&
                (units[i].own_power || (k == 0 && !bus && b >= 0 && units[b].kind != RTL_DEV_RTL))) {
                /* one on the radio's power only as the first, beside B's HackRF or bladeRF (adsb_rx.c streams it
                 * only when two were chosen), and only with nothing else on that power */
                units[i].owner = o;
                held_by[o]     = i;
            }
    }
    return memcmp(before, held_by, sizeof(before)) != 0;
}

static void changed(void) {
    if (resolve()) atomic_fetch_add(&gen, 1);
}

static void *look_thread(void *arg) {
    (void)arg;
    pthread_setname_np(pthread_self(), "sdr_pool");
    for (;;) {
        sdr_unit_t u[SDR_POOL_UNITS];
        int        rtls, n = scan(u, &rtls);

        pthread_mutex_lock(&lock);
        bool moved = n != n_units;
        for (int i = 0; i < n && !moved; i++)
            moved = u[i].kind != units[i].kind || strcmp(u[i].sn, units[i].sn) || strcmp(u[i].loc, units[i].loc);
        memcpy(units, u, sizeof(u[0]) * (size_t)n);
        n_units = n;
        n_rtls  = rtls;
        if (resolve() || moved) atomic_fetch_add(&gen, 1);
        pthread_mutex_unlock(&lock);
        sleep(1);
    }
    return NULL;
}

void sdr_pool_init(void) {
    static atomic_bool started;
    bool               no = false;
    FILE              *f;
    pthread_t          t;

    if (!atomic_compare_exchange_strong(&started, &no, true)) return;
    for (int o = 0; o < SDR_OWNERS; o++) held_by[o] = -1;
    if ((f = fopen(STOP_FILE, "r"))) {
        char line[32];

        while (n_stopped < SDR_POOL_UNITS && fgets(line, sizeof(line), f)) {
            line[strcspn(line, "\r\n")] = 0;
            if (line[0]) snprintf(stopped[n_stopped++], sizeof(stopped[0]), "%s", line);
        }
        fclose(f);
    }
    pthread_mutex_lock(&lock);
    n_units = scan(units, &n_rtls);     /* at once: the first owners need not wait a second */
    resolve();
    pthread_mutex_unlock(&lock);
    if (pthread_create(&t, NULL, look_thread, NULL) == 0) pthread_detach(t);
}

int sdr_pool_units(sdr_unit_t *out, int max) {
    sdr_pool_init();
    pthread_mutex_lock(&lock);
    int n = n_units < max ? n_units : max;
    memcpy(out, units, sizeof(units[0]) * (size_t)n);
    pthread_mutex_unlock(&lock);
    return n;
}

int sdr_pool_rtl_count(void) {
    sdr_pool_init();
    pthread_mutex_lock(&lock);
    int n = n_rtls;
    pthread_mutex_unlock(&lock);
    return n;
}

int sdr_pool_held(int owner, sdr_unit_t *u) {
    int i = -1;

    if (owner < 0 || owner >= SDR_OWNERS) return -1;
    sdr_pool_init();
    pthread_mutex_lock(&lock);
    i = held_by[owner];
    if (i >= 0 && u) *u = units[i];
    pthread_mutex_unlock(&lock);
    return i;
}

void sdr_pool_want(int owner, bool want) {
    if (owner < 0 || owner >= SDR_OWNERS) return;
    sdr_pool_init();
    pthread_mutex_lock(&lock);
    if (wants[owner] != want) {
        wants[owner] = want;
        changed();
    }
    pthread_mutex_unlock(&lock);
}

void sdr_pool_b(rtl_dev_t held, int held_rtl, rtl_dev_t want, int want_rtl) {
    sdr_pool_init();
    pthread_mutex_lock(&lock);
    if (held != b_held || held_rtl != b_held_rtl || want != b_want || want_rtl != b_want_rtl) {
        b_held     = held;
        b_held_rtl = held_rtl;
        b_want     = want;
        b_want_rtl = want_rtl;
        changed();
    }
    pthread_mutex_unlock(&lock);
}

void sdr_pool_stop(int unit, bool stop) {
    FILE *f;

    sdr_pool_init();
    pthread_mutex_lock(&lock);
    if (unit >= 0 && unit < n_units) {
        const char *k = stop_key(&units[unit]);
        int         at = -1;

        for (int i = 0; i < n_stopped; i++)
            if (!strcmp(stopped[i], k)) at = i;
        if (stop && at < 0 && n_stopped < SDR_POOL_UNITS) snprintf(stopped[n_stopped++], sizeof(stopped[0]), "%s", k);
        if (!stop && at >= 0) memmove(&stopped[at], &stopped[at + 1], sizeof(stopped[0]) * (size_t)(--n_stopped - at));
        if ((f = fopen(STOP_FILE ".new", "w"))) {
            for (int i = 0; i < n_stopped; i++) fprintf(f, "%s\n", stopped[i]);
            fclose(f);
            rename(STOP_FILE ".new", STOP_FILE);
        }
        changed();
        atomic_fetch_add(&gen, 1);
    }
    pthread_mutex_unlock(&lock);
}

unsigned sdr_pool_gen(void) { return atomic_load(&gen); }

void sdr_pool_hear(int owner, bool on) {
    if (owner >= 0 && owner < SDR_OWNERS) muted[owner] = !on;
}

bool sdr_pool_heard(int owner) {
    return owner >= 0 && owner < SDR_OWNERS && !muted[owner];
}

void sdr_pool_owner_name(int owner, char *s, size_t n) {
    if (owner == SDR_OWN_B) snprintf(s, n, "B");
    else if (owner == SDR_OWN_DF) snprintf(s, n, "PAGE I");
    else if (owner >= SDR_OWN_ADSB && owner < SDR_OWN_WATCH) snprintf(s, n, "AIRSPACE");
    else if (owner >= SDR_OWN_WATCH && owner < SDR_OWNERS) snprintf(s, n, "WATCH %d", owner - SDR_OWN_WATCH + 1);
    else snprintf(s, n, "FREE");
}
