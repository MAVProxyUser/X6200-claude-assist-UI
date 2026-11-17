/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - x6x00-rtlspec: the daemon that owns the radio's SDR on its HOST port, an RTL-SDR stick, a HackRF or a
 *  bladeRF (one backend each behind sdr.h: sdr_rtl.c, sdr_hackrf.c, sdr_bladerf.c). It opens the device once and
 *  keeps it streaming for as long as the radio runs; the GUI (src/rtl_spectrum.c) connects to it and asks for what it
 *  wants. So the device is never closed and opened again for a GUI started again (a hot install, a crash), a screen
 *  changed or AIRSPACE opened: the radio's USB host (MUSB) has been left sending nothing for the stick ("THE RTL-SDR
 *  SENDS NOTHING") by its stream stopped and started. A program of its own, as dump1090 is, so a device that hangs or
 *  goes away never takes the GUI with it. Every backend hands its samples on as the RTL-SDR sends them (unsigned
 *  8-bit I/Q), so all below works the same on each: rows, audio, SAME, RDS, dump1090's samples.
 *
 *  The device: opened when it is there (looked for each second; with more than one plugged in, a HackRF, then a
 *  bladeRF, then an RTL-SDR: those that transmit first), set as the GUI last asked, its I/Q read at the sample rate all
 *  the time. Closed only when it is pulled out (another one is opened when it is plugged in) or the daemon is told to
 *  end. One that opens and sends nothing, or whose stream ends with it still plugged in, is said to be STALLED: an RTL
 *  is then bounced on its hub port after 5 s, as pulling it out and back would (stall_bounce: a kernel with the musb
 *  fixes only; "touch /tmp/rtlspec_stall" tries it), anything else waits to be pulled out. Pulled out is its USB device
 *  file gone ("/dev/bus/usb/003/002 (deleted)" among this process's files), which a device plugged back at once does
 *  not hide (it comes back under another number). The stream's transfers are all back before the stick is closed
 *  (librtlsdr patched for it: buildroot/br2_external/package/rtlsdr-blog); should the close hang anyway, the device
 *  gone 12 s and still held, the daemon ends, and the GUI starts it again.
 *
 *  Out of the samples, only what the client asks for, and nothing with no client: ROWS rows a second while it wants
 *  rows ("r 1"), each the average of up to AVG windowed FFTs of BINS points from that row's samples, in dB, lowest
 *  frequency first (the samples between are read and not used: the processor is not spent on all of them); a
 *  channel's audio while it listens; SAME alerts while it watches; the raw samples to AIRSPACE's dump1090 while that
 *  has them.
 *
 *  Transmitting (a HackRF or a bladeRF, the Sentry's G WALKIE armed by its owner): narrow FM voice from the GUI's
 *  microphone audio, made here: 300 Hz-3 kHz, pre-emphasised, levelled, clipped to 4 kHz deviation and filtered after
 *  the clipper, ramped up and down over 5 ms. Only in the amateur bands from 2 m up (tx_band below; the GUI narrows it
 *  to its TX ranges first), never while AIRSPACE has the samples; the receiver's settings are left alone meanwhile
 *  and put back after. It ends when asked, when the client goes, when no audio came for TX_IDLE_MS (the GUI hung)
 *  and after TX_TOT_MS in one go (a time-out timer); the HackRF stops by itself 0.1 s after its samples stop.
 *
 *  The client: a UNIX socket in the abstract namespace, "@x6x00-rtlspec", SOCK_SEQPACKET, one client at a time (a
 *  new one replaces the old: the GUI started again). Each message is one thing. From the daemon, little endian:
 *      "RTLSPEC 2 <state> <tuner> <rate> <bins> <kind> <lo Hz> <hi Hz> <tx> <rtl>"   on connecting and each change:
 *                          state NOSTICK, BUSY (another program has it), OPENING, ON (samples coming), STALLED (open,
 *                          nothing coming), ADSB (AIRSPACE's dump1090 has the samples), TX (transmitting), NEEDS
 *                          (there and not usable: tuner says what, "LIBBLADERF" or the FPGA image's file); tuner the
 *                          model ("R820T", "HACKRF-ONE", "BLADERF2"); kind RTL, HACKRF or BLADERF ("-" none); the
 *                          tuning range; tx 0 receive only, 1 it can transmit, 2 transmitting; rtl the open RTL's
 *                          place in serial order (RTL0 the first; -1 no RTL open). The first five as before: a GUI
 *                          that reads only them still works
 *      "RSPC", u32 seq, u64 centre Hz, u32 rate, u16 bins, u16 flags (1: the first since a retune), f32 dB[bins]
 *      "RAUD", u32 count, s16 samples[count]       the channel listened to, mono, 44.1 kHz, 50 ms a message
 *      "RSAM", u32 length, the header's text and "|" and the event's name when known
 *                          ("ZCZC-WXR-TOR-039049+0030-3170815-KILN/NWS-|TORNADO WARNING"; "NNNN": a message's end)
 *      "RRDS", u32 length, "<Hz>|<PI hex>|<PTY>|<TP>|<TA>|<PS>|<RT>"   what the station decoded for RDS sends (rds.c),
 *                          each time it changes; "<Hz>|||||" at a new channel. PI "" and PTY -1 until they came
 *      "RACR", u32 length, "<Hz>|<mode>|<registration>|<ack>|<label>|<block>|<msn>|<flight>|<more>|<level dB>|<text>"
 *                          an ACARS message (acars.c) whose check passed, while "c 1"; the text last, "|" in it as "/"
 *      "RDSD", u32 length, "<Hz>|<protocol>|<slot>|<call>"     P25, D-STAR, YSF or X2-TDMA on the channel listened
 *                          to in NFM (dsd-fme, dsd.c): as it changes, and each second while it is there
 *      "RDMR", u32 length, "<Hz>|<slot>|<cc>|<voice>|<lc>|<flco>|<src>|<dst>|<bs>|<vocoder>"   DMR on the channel
 *                          listened to in NFM (dmr.c, found by its sync): a timeslot's call as it changes (dmr_call)
 *      "RTX <word> <Hz> <why>" the transmitter: ON (on the air), END (off again: ASKED, IDLE, TIMEOUT, GONE, FAILED),
 *                          NO (refused: NOT-AMATEUR, RECEIVE-ONLY, AIRSPACE, NO-DEVICE)
 *  A client that does not read misses messages: the stream never waits for it. From the client, a line each:
 *      "r 1", "r 0"        rows wanted, or not
 *      "f <Hz>"            the centre (within what the device tunes); "s <rate>" the sample rate; "g <tenths of dB>"
 *                          the gain, on the RTL's scale ("g -1": the AGC)
 *      "l <Hz> <mode>"     listen (mode 1 NFM, 2 AM, 3 WFM; "l 0 0" stops; NFM with DMR in it: its voice): mixed to 0 Hz, decimated to 264.6 kHz
 *                          (CIC, order 2) and by 6 (a 63-tap FIR) to 44.1 kHz and demodulated. Only at a sample rate
 *                          that is a multiple of 264.6 kHz (within 50 Hz: a device's own clock): 1058400 or 2116800
 *      "w <Hz>"            watch that channel (narrow FM, NOAA Weather Radio) for SAME alerts (same.c); "w 0" stops
 *      "d <Hz>"            decode that FM broadcast channel's RDS (rds.c); "d 0" stops. Off the listened channel's
 *                          own multiplex when it listens there in WFM, else demodulated for it alone
 *      "c 1", "c 0"        ACARS: the device at ACARS_HZ (130.8 MHz), the tuner's top gain and the rate as asked, each
 *                          ACARS channel within it (131.550, 131.725, 131.125, 130.450, 130.425, 130.025 MHz) heard in AM
 *                          and decoded, its messages out as "RACR"; rows as asked meanwhile, no listening. "c 0" (or the
 *                          client gone): the device back to what the client asked. Never with "a": dump1090 comes first
 *      "a" and a file descriptor (SCM_RIGHTS)  the raw samples (unsigned 8-bit I/Q) written to it at 1090 MHz,
 *                          2.4 MS/s and the tuner's top gain, for AIRSPACE's dump1090 ("--ifile -"); no rows or audio
 *                          meanwhile. "a 0", or dump1090 gone: the stick back to what the client asked
 *      "t <Hz> <gain dB> <amp 0|1>"   transmit FM there (a device that transmits, an amateur band); "t 0" ends it
 *      "TAUD", u32 count, s16 samples[count]       the voice to transmit, mono, 44.1 kHz, as it comes
 *      "k <KIND>"          the device to hold, RTL, HACKRF or BLADERF, when it is plugged in; "k -" the first by the
 *                          order below. RTL<n>: the RTL at place n in serial order (sdr_rtl.c: a KrakenSDR's CH0-CH4
 *                          are RTL0-RTL4), RTL0 while it is not on USB; another RTL asked for is switched to too. The one open is closed for it (not while transmitting) and it is opened; one
 *                          that will not open (another program holds it) leaves the one before, till "k" again
 *      "R"                 the receiver's stream stopped and started again, as a transmission does, without one (a
 *                          test of that on the radio's USB host)
 *      "Q"                 the daemon ends and closes the stick (a GUI that wants another version of it)
 *  A client gone: its rows, listening, watch, ACARS and transmission end; the device goes on, tuned as it was.
 *
 *      x6x00-rtlspec [-F] [-d index] [-f Hz] [-s rate] [-n bins] [-r rows/s] [-g tenths] [-i FILE] [-S name]
 *                    [-T FILE] [-W]
 *
 *  -W: a watch channel's (the GUI's rtl_watch.c, beside the radio's own under another -S): RTL -d alone, never
 *  another kind nor RTL0 in its place; "k" is not for it.
 *  It goes into the background (-F: it stays in front); with one already running it says "RTLSPEC RUNNING" and ends.
 *  -d: which RTL-SDR stick. -i replays FILE (unsigned 8-bit I/Q at the rate, as rtl_sdr writes it) in place of the
 *  device, at its own pace and over again: the same rows, audio and alerts, to try the GUI without a signal on the air
 *  (a SAME alert, say); a retune only moves what the rows say. -S: another socket name (a test beside the radio's own
 *  daemon). -T: a test device in place of any (sdr_file.c): it "transmits" into FILE (signed 8-bit I/Q) and hears
 *  nothing, to check the transmitter with nothing on the air (tools/rtlspec/tx_check.py).
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

#include "acars.h"
#include "ambe.h"
#include "dmr.h"
#include "dsd.h"
#include "rds.h"
#include "same.h"
#include "sdr.h"

#define AVG       8         /* FFTs averaged into a row, at most */
#define MAX_BINS  8192
#define READ_LEN  (16384 * 2)     /* a replay's read */

static volatile sig_atomic_t stop;

static const sdr_ops_t *volatile dev_ops;   /* the device open, NULL none */
static volatile int             rx_halt;    /* its stream to end (sdr.h): stop, a transmission, a pull-out, "R" */
static volatile int             tx_halt;    /* the transmission to end now (ramped down) */

static void halt_rx(void) {
    const sdr_ops_t *o = dev_ops;

    rx_halt = 1;
    if (o && o->rx_stop) o->rx_stop();
}

/* As rtl_sdr's handler: the stream ended here too, so one that has stalled (no callback to see stop) still ends
 * cleanly; the radio's USB host (MUSB) is left broken by a stick killed with transfers in flight */
static void on_term(int s) {
    (void)s;
    stop    = 1;
    tx_halt = 1;
    halt_rx();
}

/* In place, radix 2, n a power of two */
static void fft(float *re, float *im, int n) {
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            float t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        double a = -2 * M_PI / len;
        float  wr = (float)cos(a), wi = (float)sin(a);

        for (int i = 0; i < n; i += len) {
            float cr = 1, ci = 0;

            for (int k = 0; k < len / 2; k++) {
                int   p = i + k, q = p + len / 2;
                float xr = re[q] * cr - im[q] * ci, xi = re[q] * ci + im[q] * cr;

                re[q] = re[p] - xr;
                im[q] = im[p] - xi;
                re[p] += xr;
                im[p] += xi;
                float t = cr * wr - ci * wi;
                ci      = cr * wi + ci * wr;
                cr      = t;
            }
        }
    }
}

/* ---- Channels: one demodulated to the speaker, one watched for SAME alerts ---- */

static uint32_t rate_now;       /* the stick's sample rate */
static int      want_mode;      /* the mode asked for: kept across a retune that leaves the channel outside */

#define AUD_RATE  44100
#define MID_RATE  264600        /* the CIC's output: the rate / MID_RATE, 4 or 8 */
#define R2        6             /* 264600 -> 44100 (FIR) */
#define TAPS      63
#define AUD_CHUNK 2205          /* 50 ms a frame */

static int16_t sin_lut[1024];   /* the mixers in integers: 2.1 million samples a second leave no room for floats */

typedef struct chan chan_t;
struct chan {
    int      mode;              /* 0 off, 1 NFM, 2 AM, 3 WFM */
    uint64_t hz;
    uint32_t ph, ph_inc;        /* the mixer's phase, 2^32 a turn */
    uint32_t i1r, i1i, i2r, i2i, c1r, c1i, c2r, c2i;     /* CIC: modular arithmetic */
    int      cic_n, r1;
    float    hist_r[2 * TAPS], hist_i[2 * TAPS], taps[TAPS];  /* each sample twice: the window in one piece */
    int      hist_at, fir_n;
    float    prev_r, prev_i, deemph, dc, avg_mag;
    bool     raw;               /* the demodulated FM as it is, no audio shaping (SAME's tones) */
    bool     mpx_only;          /* WFM's multiplex only, no audio (RDS's own channel) */
    bool     dmr_on;            /* NFM: DMR looked for in it (dmr.c), its speech in place of the FM while it is there */
    dmr_t   *dmr;
    void   (*out)(float v);     /* each 44.1 kHz sample */
    void   (*mpx)(float v);     /* WFM: each 264.6 kHz sample of the multiplex, NULL none */
};

static void listen_out(float v);
static void watch_out(float v);
static void rds_mpx(float v);
static chan_t lis    = {.r1 = 8, .prev_r = 1, .avg_mag = 0.01f, .dmr_on = true, .out = listen_out};
static chan_t watch  = {.r1 = 8, .prev_r = 1, .avg_mag = 0.01f, .raw = true, .out = watch_out};
static chan_t rdsc   = {.r1 = 8, .prev_r = 1, .avg_mag = 0.01f, .mpx_only = true, .mpx = rds_mpx};
#define l_mode lis.mode
#define l_hz   lis.hz

/* The client, and what it wants */
static pthread_mutex_t out_lock = PTHREAD_MUTEX_INITIALIZER;
static int             client = -1;     /* its socket, -1 none */
static atomic_bool     rows_on;         /* "r 1" */

/* One message to the client, whole or not at all: one not reading (its socket full) misses it, as the stream (the
 * stick's own thread) must never wait */
static void put(const void *b, size_t n) {
    pthread_mutex_lock(&out_lock);
    if (client >= 0) (void)send(client, b, n, MSG_DONTWAIT | MSG_NOSIGNAL);
    pthread_mutex_unlock(&out_lock);
}

/* A low pass of cutoff Hz at 264.6 kHz, Blackman windowed sinc, unity at DC */
static void design(float *taps, float cutoff) {
    float fc = cutoff / (AUD_RATE * R2), sum = 0;

    for (int i = 0; i < TAPS; i++) {
        int   m = i - TAPS / 2;
        float s = m ? sinf(2 * (float)M_PI * fc * m) / ((float)M_PI * m) : 2 * fc;
        float w = 0.42f - 0.5f * cosf(2 * (float)M_PI * i / (TAPS - 1)) + 0.08f * cosf(4 * (float)M_PI * i / (TAPS - 1));
        taps[i] = s * w;
        sum += taps[i];
    }
    for (int i = 0; i < TAPS; i++) taps[i] /= sum;
}

/* What a channel takes, against the centre the stick is tuned to: the mixer's step, the filter; off outside */
static void chan_set(chan_t *c, int mode, uint64_t hz, uint64_t centre, uint32_t rate) {
    double   off = (double)hz - (double)centre;
    uint32_t r1  = (rate + MID_RATE / 2) / MID_RATE;
    /* a whole number of MID_RATE, give or take what a device's own clock makes of the rate asked: the bladeRF's AD9361
     * gives 2116799 for 2116800, and an exact test left it without audio, RDS or SAME (2026-11-16) */
    bool     whole = r1 >= 1 && r1 <= 16 && llabs((long long)rate - (long long)r1 * MID_RATE) <= 50;

    c->mode = (mode >= 1 && mode <= 3 && whole && fabs(off) < rate * 0.45) ? mode : 0;
    c->hz = hz;
    if (!c->mode) return;
    c->r1     = (int)r1;
    c->cic_n  = 0;
    c->ph_inc = (uint32_t)(int64_t)llround(-off / rate * 4294967296.0);
    design(c->taps, c->mode == 2 ? 5000 : c->mode == 1 ? 9000 : 15000);   /* AM, NFM; WFM's: the audio's */
    memset(c->hist_r, 0, sizeof(c->hist_r));
    memset(c->hist_i, 0, sizeof(c->hist_i));
    c->deemph = c->dc = 0;
}

static void listen_set(int mode, uint64_t hz, uint64_t centre, uint32_t rate) {
    if (lis.dmr && (hz != lis.hz || mode != 1)) {       /* another channel: what DMR it had is not this one's */
        dmr_free(lis.dmr);
        lis.dmr = NULL;
    }
    chan_set(&lis, mode, hz, centre, rate);
}

/* ---- DMR on the listened channel (dmr.c): found in any narrow FM channel by its sync, its voice decoded instead of
 * the FM's buzz; each slot's call said to the client as it changes:
 *      "RDMR", u32 length, "<Hz>|<slot>|<cc>|<voice>|<lc>|<flco>|<src>|<dst>|<bs>|<vocoder>"
 * slot 1 or 2 (0 not known), cc the colour code (-1 not known), voice 1 a call going on, lc 1 src and dst known, flco 0
 * a group call (dst a talkgroup) or 3 a private one, bs 1 a repeater's, vocoder 1 its speech heard (a DVSI USB-3000:
 * ambe.c), 0 none here (silence in place of the buzz) ---- */
static int dmr_play;            /* the slot whose voice is heard, 0 none */

static void dmr_call(void *ctx, const dmr_call_t *c) {
    chan_t *ch = ctx;
    char    t[160];
    uint8_t msg[8 + sizeof(t)];
    int     n;

    if (c->slot == dmr_play && !c->voice) dmr_play = 0;
    n = snprintf(t, sizeof(t), "%llu|%d|%d|%d|%d|%d|%u|%u|%d|%d", (unsigned long long)ch->hz, c->slot, c->cc, c->voice,
                 c->lc, c->flco, c->src, c->dst, c->bs, ambe_ready());
    if (n <= 0 || n >= (int)sizeof(t)) return;
    {                                   /* the log: a call's changes only (it is said again each superframe) */
        static char said[3][96];
        char        l[96];

        snprintf(l, sizeof(l), "%.5f MHz TS%d CC%d %s%s", ch->hz / 1e6, c->slot, c->cc, c->voice ? "VOICE" : "END",
                 c->bs ? " (REPEATER)" : "");
        if (c->lc)
            snprintf(l + strlen(l), sizeof(l) - strlen(l), " %s %u FROM %u", c->flco == 3 ? "TO" : "TG", c->dst, c->src);
        if (c->slot >= 0 && c->slot <= 2 && strcmp(said[c->slot], l)) {
            snprintf(said[c->slot], sizeof(said[0]), "%s", l);
            fprintf(stderr, "rtlspec: DMR %s\n", l);
        }
    }
    uint32_t len = (uint32_t)n;

    memcpy(msg, "RDMR", 4);
    memcpy(msg + 4, &len, 4);
    memcpy(msg + 8, t, len);
    put(msg, 8 + len);
}

static void dmr_ambe(void *ctx, int slot, const uint8_t frame[9]) {
    (void)ctx;
    if (!dmr_play) {                    /* the first slot heard holds it till its call ends */
        dmr_play = slot;
        ambe_flush();
    }
    if (slot == dmr_play) ambe_frame(frame);
}

/* A test of DMR (or dsd-fme's protocols) off the air or over a cable: with /tmp/dmr_dump there, the next 20 s of a
 * digital channel's FM (the discriminator, float at 44.1 kHz, +-1 for +-5 kHz) into /tmp/dmr_dump.f32, the flag then
 * taken away. Looked for once a second */
static void dmr_dump(float d, bool active) {
    static FILE    *f;
    static uint32_t n, look;

    if (!f) {
        if (!active || ++look < AUD_RATE) return;
        look = 0;
        if (access("/tmp/dmr_dump", F_OK) || !(f = fopen("/tmp/dmr_dump.f32", "wb"))) return;
        n = 0;
        fprintf(stderr, "rtlspec: the digital channel's FM into /tmp/dmr_dump.f32 for 20 s\n");
    }
    fwrite(&d, sizeof(d), 1, f);
    if (++n >= 20 * AUD_RATE) {
        fclose(f);
        f = NULL;
        unlink("/tmp/dmr_dump");
        fprintf(stderr, "rtlspec: /tmp/dmr_dump.f32 written\n");
    }
}

/* The speech in place of the FM while DMR is there (ambe.c's USB-3000): silence without one */
static float dmr_audio(void) { return ambe_audio(); }

/* ---- dsd-fme on the listened channel (dsd.c): the digital voice dmr.c does not decode, P25, D-STAR, YSF, X2-TDMA:
 *      "RDSD", u32 length, "<Hz>|<protocol>|<slot>|<call>"
 * the protocol ("P25P1", "DSTAR", "YSF"...; "" none now), slot 0 or 1 (a TDMA system's second), the call ("TG 1234
 * FROM 5678901 NAC 293", "TO CQCQCQ FROM N0CALL"; "" no one talking): as it changes, and each second while it is
 * there ---- */
static bool dsd_on;             /* dsd-fme there (/usr/bin/dsd-fme), B's daemon */

static void dsd_heard(const char *proto, int slot, const char *call) {
    static char was[16];
    char        t[320];
    uint8_t     msg[8 + sizeof(t)];
    int         n = snprintf(t, sizeof(t), "%llu|%s|%d|%s", (unsigned long long)lis.hz, proto, slot, call);

    if (strcmp(was, proto)) {
        snprintf(was, sizeof(was), "%s", proto);
        fprintf(stderr, "rtlspec: dsd-fme %s at %.5f MHz\n", *proto ? proto : "no digital voice now", lis.hz / 1e6);
    }
    if (n <= 0 || n >= (int)sizeof(t)) return;
    uint32_t len = (uint32_t)n;

    memcpy(msg, "RDSD", 4);
    memcpy(msg + 4, &len, 4);
    memcpy(msg + 8, t, len);
    put(msg, 8 + len);
}

static void listen_out(float v) {
    static uint8_t msg[8 + AUD_CHUNK * 2];
    static int     aud_n;
    int16_t        s;

    v *= 20000;
    s = (int16_t)(v > 32767 ? 32767 : v < -32767 ? -32767 : v);
    memcpy(msg + 8 + aud_n * 2, &s, 2);
    if (++aud_n == AUD_CHUNK) {
        uint32_t cnt = AUD_CHUNK;

        memcpy(msg, "RAUD", 4);
        memcpy(msg + 4, &cnt, 4);
        put(msg, sizeof(msg));
        aud_n = 0;
    }
}

/* The watched channel's audio into the SAME decoder; a header decided goes out as "RSAM", u32 length, its text */
static same_t *same;

static void watch_out(float v) {
    static float buf[441];
    static int   n;
    char         text[SAME_MAX + 48];
    uint8_t      msg[8 + sizeof(text)];

    buf[n++] = v;
    if (n < (int)(sizeof(buf) / sizeof(buf[0]))) return;
    n = 0;
    if (!same) same = same_new(AUD_RATE);
    if (same && same_feed(same, buf, sizeof(buf) / sizeof(buf[0]), text, sizeof(text) - 40)) {
        /* the event's name after a "|", for the GUI: "ZCZC-WXR-TOR-...-|TORNADO WARNING" */
        const char *name = strlen(text) > 12 ? same_event_name(text + 9) : NULL;

        if (name) snprintf(text + strlen(text), sizeof(text) - strlen(text), "|%s", name);
        uint32_t len = (uint32_t)strlen(text);

        memcpy(msg, "RSAM", 4);
        memcpy(msg + 4, &len, 4);
        memcpy(msg + 8, text, len);
        put(msg, 8 + len);
    }
}

/* RDS (rds.c): the channel decoded, the multiplex in by 441 samples, what changed to the client */
static rds_t   *rds;
static uint64_t rds_hz;

static void rds_say(void) {
    rds_info_t in;
    char       text[160];
    uint8_t    msg[8 + sizeof(text)];

    rds_info(rds, &in);
    if (in.pi) snprintf(text, sizeof(text), "%llu|%04X|%d|%d|%d|%s|%s", (unsigned long long)rds_hz, in.pi, in.pty,
                        in.tp, in.ta, in.ps, in.rt);
    else snprintf(text, sizeof(text), "%llu|||||", (unsigned long long)rds_hz);
    uint32_t len = (uint32_t)strlen(text);

    memcpy(msg, "RRDS", 4);
    memcpy(msg + 4, &len, 4);
    memcpy(msg + 8, text, len);
    put(msg, 8 + len);
}

static void rds_mpx(float v) {
    static float buf[441];
    static int   n;

    buf[n++] = v;
    if (n < (int)(sizeof(buf) / sizeof(buf[0]))) return;
    n = 0;
    if (rds && rds_feed(rds, buf, sizeof(buf) / sizeof(buf[0]))) rds_say();
}

/* ACARS (acars.c): the channels within the span round ACARS_HZ, each one's AM audio into a decoder of its own; a
 * message whose check passed goes out as "RACR". 129.125 MHz lies outside it at these rates (1.07 MHz either side) */
#define ACARS_HZ 130800000u
static const uint64_t acars_hz[] = {131550000, 131725000, 131125000, 130450000, 130425000, 130025000, 129125000};
#define ACARS_N  ((int)(sizeof(acars_hz) / sizeof(acars_hz[0])))
static atomic_bool acars_want;          /* "c 1" */
static bool        acars_on;            /* some channel of it within the span (acars_thread's) */
static chan_t      acrc[ACARS_N];
static acars_t    *acd[ACARS_N];

static void acars_out(int i, float v) {
    static float buf[ACARS_N][441];
    static int   n[ACARS_N];
    acars_msg_t  m[2];
    char         text[400];
    uint8_t      msg[8 + sizeof(text)];

    buf[i][n[i]++] = v;
    if (n[i] < 441) return;
    n[i] = 0;
    if (!acd[i]) acd[i] = acars_new(AUD_RATE);
    if (!acd[i]) return;
    int got = acars_feed(acd[i], buf[i], 441, m, 2);
    for (int k = 0; k < got; k++) {
        for (char *t = m[k].text; *t; t++)
            if (*t == '|') *t = '/';
        snprintf(text, sizeof(text), "%llu|%c|%s|%c|%s|%c|%s|%s|%d|%.0f|%s", (unsigned long long)acars_hz[i], m[k].mode,
                 m[k].reg, m[k].ack, m[k].label, m[k].block, m[k].msn, m[k].flight, m[k].more ? 1 : 0, m[k].level,
                 m[k].text);
        uint32_t len = (uint32_t)strlen(text);

        memcpy(msg, "RACR", 4);
        memcpy(msg + 4, &len, 4);
        memcpy(msg + 8, text, len);
        put(msg, 8 + len);
    }
}
#define ACO(i) \
    static void acars_out##i(float v) { acars_out(i, v); }
ACO(0) ACO(1) ACO(2) ACO(3) ACO(4) ACO(5) ACO(6)
static void (*const acars_outs[ACARS_N])(float) = {acars_out0, acars_out1, acars_out2, acars_out3, acars_out4,
                                                   acars_out5, acars_out6};

/* Each channel set against the centre now (off outside the span, or with ACARS off) */
static void acars_set(bool on, uint64_t centre, uint32_t rate) {
    acars_on = false;
    for (int i = 0; i < ACARS_N; i++) {
        chan_t *c = &acrc[i];

        if (!c->out) {
            c->r1      = 8;
            c->prev_r  = 1;
            c->avg_mag = 0.01f;
            c->out     = acars_outs[i];
        }
        chan_set(c, on ? 2 : 0, acars_hz[i], centre, rate);
        if (c->mode) acars_on = true;
    }
}

/* One sample at 264.6 kHz: into the FIR, every sixth a 44.1 kHz one demodulated (WFM: demodulated here, then the
 * FIR on the audio; its multiplex to RDS when it decodes this channel) */
static void at_264k(chan_t *c, float r, float i) {
    if (c->mode == 3) {
        float zr = r * c->prev_r + i * c->prev_i, zi = i * c->prev_r - r * c->prev_i;

        c->prev_r = r;
        c->prev_i = i;
        r = atan2f(zi, zr) * (264600.0f / (2 * (float)M_PI * 75000));    /* +-75 kHz: +-1 */
        i = 0;
        if (c->mpx) c->mpx(r);
        if (c->mpx_only) return;
    }
    c->hist_r[c->hist_at] = c->hist_r[c->hist_at + TAPS] = r;
    c->hist_i[c->hist_at] = c->hist_i[c->hist_at + TAPS] = i;
    c->hist_at = c->hist_at + 1 == TAPS ? 0 : c->hist_at + 1;
    if (++c->fir_n < R2) return;
    c->fir_n = 0;

    /* the last TAPS samples, oldest first, in one piece: no modulo a tap (it was most of six ACARS channels' time) */
    const float *hr = c->hist_r + c->hist_at, *hi = c->hist_i + c->hist_at;
    float        yr = 0, yi = 0;
    for (int k = 0; k < TAPS; k++) {
        yr += hr[k] * c->taps[k];
        yi += hi[k] * c->taps[k];
    }
    if (c->mode == 1) {                 /* NFM: +-5 kHz full scale, a little low pass and no DC */
        float zr = yr * c->prev_r + yi * c->prev_i, zi = yi * c->prev_r - yr * c->prev_i;
        float d  = atan2f(zi, zr) * (AUD_RATE / (2 * (float)M_PI * 5000));

        c->prev_r = yr;
        c->prev_i = yi;
        if (c->dmr_on) {
            static int off = -1;        /* RTLSPEC_NO_DMR in the environment: NFM alone (a test of what DMR costs) */

            if (off < 0) off = getenv("RTLSPEC_NO_DMR") != NULL;
            if (off) c->dmr_on = false;
        }
        if (c->dmr_on) {
            if (!c->dmr) {
                c->dmr = dmr_new(AUD_RATE, dmr_ambe, dmr_call, c);
                ambe_start();           /* the vocoder looked for (once) */
            }
            if (c->dmr) {
                dmr_feed(c->dmr, &d, 1);
                dmr_dump(d, dmr_active(c->dmr) || (c == &lis && dsd_on && dsd_active()));
                if (dmr_active(c->dmr)) {
                    c->out(dmr_audio());
                    return;
                }
            }
        }
        if (c == &lis && dsd_on) {      /* the other digital voice: dsd-fme's speech (or silence) for its buzz */
            dsd_feed(d);
            if (dsd_active()) {
                c->out(dsd_audio());
                return;
            }
        }
        if (c->raw) {
            c->dc += 0.002f * (d - c->dc);
            c->out(d - c->dc);
            return;
        }
        c->deemph += 0.45f * (d - c->deemph);
        c->dc += 0.002f * (c->deemph - c->dc);
        c->out((c->deemph - c->dc) * 0.7f);
    } else if (c->mode == 2) {          /* AM: the envelope over its average, its DC out */
        float mag = sqrtf(yr * yr + yi * yi);

        c->avg_mag += 0.0005f * (mag - c->avg_mag);
        c->dc += 0.002f * (mag - c->dc);
        c->out((mag - c->dc) / (c->avg_mag + 1e-6f) * 0.6f);
    } else {                            /* WFM: 75 us de-emphasis */
        c->deemph += 0.26f * (yr - c->deemph);
        c->out(c->deemph * 0.8f);
    }
}

/* A sample mixed down (+-16k) into a channel's CIC */
static inline void chan_cic(chan_t *c, int r, int i) {
    c->i1r += (uint32_t)r;
    c->i1i += (uint32_t)i;
    c->i2r += c->i1r;
    c->i2i += c->i1i;
    if (++c->cic_n < c->r1) return;
    c->cic_n = 0;

    uint32_t a = c->i2r - c->c1r, b = c->i2i - c->c1i;      /* the combs */
    c->c1r = c->i2r;
    c->c1i = c->i2i;
    uint32_t cr = a - c->c2r, ci = b - c->c2i;
    c->c2r = a;
    c->c2i = b;
    at_264k(c, (int32_t)cr / (32768.0f * c->r1 * c->r1), (int32_t)ci / (32768.0f * c->r1 * c->r1));
}

/* One stick sample into a channel: mixed down (integers, a 1024-step sine), into its CIC */
static inline void chan_sample(chan_t *c, uint8_t ib, uint8_t qb) {
    int      x = ib - 127, y = qb - 127;
    uint32_t k = c->ph >> 22;
    int      s = sin_lut[k], co = sin_lut[(k + 256) & 1023];
    int      r = (x * co - y * s) >> 8, i = (x * s + y * co) >> 8;     /* +-32k */

    c->ph += c->ph_inc;
    chan_cic(c, r, i);
}

/* ACARS's channels four at a time on NEON, a lane each: every sample mixed down by each lane's phasor and summed over
 * r1 samples (a boxcar, a CIC of order 1, aliases 264.6 kHz off a channel 34 dB down for the FIR after it), the sums
 * into at_264k at 264.6 kHz as the other channels' CIC gives them. chan_sample's mixer and CIC of order 2, sample by
 * sample for each channel, took a whole core of the radio's Cortex-A7 for six channels and fell behind the device */
typedef struct {
    float pr[4], pi[4];                 /* each lane's phasor, length 1 */
    float sr[4], si[4];                 /* its step a sample */
    float ar[4], ai[4];                 /* the sums so far */
} aquad_t;
#define AQ 2                            /* lanes for 8 channels */
static aquad_t aq[AQ];
static int     aq_n;                    /* samples summed since the last output */

static void aq_set(void) {
    memset(aq, 0, sizeof(aq));
    aq_n = 0;
    for (int i = 0; i < ACARS_N && i < AQ * 4; i++) {
        double th = 2 * M_PI * (int32_t)acrc[i].ph_inc / 4294967296.0;
        aquad_t *q = &aq[i / 4];

        q->pr[i % 4] = 1;
        q->sr[i % 4] = (float)cos(th);
        q->si[i % 4] = (float)sin(th);
    }
}

/* m samples (unsigned 8-bit I/Q) into every ACARS channel */
static void acars_run(const uint8_t *b, int m) {
    int   r1 = acrc[0].r1 > 0 ? acrc[0].r1 : 9;
    float sc = 1.0f / (256.0f * r1);
#if defined(__ARM_NEON)
    float32x4_t pr[AQ], pi[AQ], sr[AQ], si[AQ], ar[AQ], ai[AQ];
    for (int g = 0; g < AQ; g++) {
        pr[g] = vld1q_f32(aq[g].pr), pi[g] = vld1q_f32(aq[g].pi), sr[g] = vld1q_f32(aq[g].sr);
        si[g] = vld1q_f32(aq[g].si), ar[g] = vld1q_f32(aq[g].ar), ai[g] = vld1q_f32(aq[g].ai);
    }
#endif
    for (int k = 0; k < m; k++) {
        float x = b[2 * k] - 127.0f, y = b[2 * k + 1] - 127.0f;
#if defined(__ARM_NEON)
        float32x4_t xv = vdupq_n_f32(x), yv = vdupq_n_f32(y);
        for (int g = 0; g < AQ; g++) {
            ar[g] = vaddq_f32(ar[g], vmlsq_f32(vmulq_f32(xv, pr[g]), yv, pi[g]));
            ai[g] = vaddq_f32(ai[g], vmlaq_f32(vmulq_f32(xv, pi[g]), yv, pr[g]));
            float32x4_t nr = vmlsq_f32(vmulq_f32(pr[g], sr[g]), pi[g], si[g]);
            pi[g] = vmlaq_f32(vmulq_f32(pr[g], si[g]), pi[g], sr[g]);
            pr[g] = nr;
        }
#else
        for (int g = 0; g < AQ; g++)
            for (int l = 0; l < 4; l++) {
                aquad_t *q = &aq[g];
                float    p = q->pr[l], qi = q->pi[l];
                q->ar[l] += x * p - y * qi;
                q->ai[l] += x * qi + y * p;
                q->pr[l] = p * q->sr[l] - qi * q->si[l];
                q->pi[l] = p * q->si[l] + qi * q->sr[l];
            }
#endif
        if (++aq_n < r1) continue;
        aq_n = 0;
#if defined(__ARM_NEON)
        for (int g = 0; g < AQ; g++) {
            vst1q_f32(aq[g].ar, ar[g]);
            vst1q_f32(aq[g].ai, ai[g]);
            ar[g] = ai[g] = vdupq_n_f32(0);
        }
#endif
        for (int i = 0; i < ACARS_N && i < AQ * 4; i++) {
            aquad_t *q = &aq[i / 4];

            if (acrc[i].mode) at_264k(&acrc[i], q->ar[i % 4] * sc, q->ai[i % 4] * sc);
#if !defined(__ARM_NEON)
            q->ar[i % 4] = q->ai[i % 4] = 0;
#endif
        }
    }
#if defined(__ARM_NEON)
    for (int g = 0; g < AQ; g++) {
        vst1q_f32(aq[g].pr, pr[g]), vst1q_f32(aq[g].pi, pi[g]);
        vst1q_f32(aq[g].ar, ar[g]), vst1q_f32(aq[g].ai, ai[g]);
    }
#endif
    /* rounding: each phasor back to length 1 once a block */
    for (int g = 0; g < AQ; g++)
        for (int l = 0; l < 4; l++) {
            float n = sqrtf(aq[g].pr[l] * aq[g].pr[l] + aq[g].pi[l] * aq[g].pi[l]);
            if (n > 0) aq[g].pr[l] /= n, aq[g].pi[l] /= n;
        }
}

/* ACARS on a thread of its own, off the stream's: on it, six channels took a core of the radio and the stream fell
 * behind the device (78% of the samples). The stream puts the samples in a ring (acars_put); this thread reads them
 * on another core, its channels set as commands() last said (acars_cfg) */
#define ARING (1u << 22)                /* 4 MB: 0.9 s at 2.38 MS/s */
static uint8_t         aring[ARING];
static atomic_uint     a_wr, a_rd;      /* bytes in and out, mod 2^32 */
static atomic_uint     a_drop;          /* bytes the thread was too late for */
static pthread_mutex_t a_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  a_cond = PTHREAD_COND_INITIALIZER;
static struct {
    bool     on;
    uint64_t centre;
    uint32_t rate;
    uint32_t seq;
} acars_cfg;                            /* under a_lock */
static bool a_thread;

static void acars_put(const uint8_t *b, uint32_t n) {
    uint32_t w = atomic_load(&a_wr), r = atomic_load(&a_rd);

    n &= ~1u;
    if (w - r + n > ARING) {            /* the thread behind: these go */
        atomic_fetch_add(&a_drop, n);
        return;
    }
    uint32_t at = w % ARING, first = ARING - at < n ? ARING - at : n;
    memcpy(aring + at, b, first);
    memcpy(aring, b + first, n - first);
    atomic_store(&a_wr, w + n);
    pthread_mutex_lock(&a_lock);
    pthread_cond_signal(&a_cond);
    pthread_mutex_unlock(&a_lock);
}

static uint32_t now_ms(void);

static void *acars_thread(void *arg) {
    static uint8_t chunk[65536];
    uint32_t       seen = 0;
    (void)arg;

    for (;;) {
        pthread_mutex_lock(&a_lock);
        while (atomic_load(&a_wr) - atomic_load(&a_rd) < sizeof(chunk) && acars_cfg.seq == seen && !stop) {
            struct timespec t;
            clock_gettime(CLOCK_REALTIME, &t);
            t.tv_nsec += 100000000;
            if (t.tv_nsec >= 1000000000) t.tv_sec++, t.tv_nsec -= 1000000000;
            pthread_cond_timedwait(&a_cond, &a_lock, &t);
            if (acars_cfg.seq == seen && atomic_load(&a_wr) - atomic_load(&a_rd) > 0 && atomic_load(&a_wr) - atomic_load(&a_rd) < sizeof(chunk))
                break;                  /* a short tail after 0.1 s: taken too, a multiple of 8 bytes */
        }
        bool     fresh = acars_cfg.seq != seen;
        bool     on    = acars_cfg.on;
        uint64_t c     = acars_cfg.centre;
        uint32_t rt    = acars_cfg.rate;
        seen           = acars_cfg.seq;
        pthread_mutex_unlock(&a_lock);
        if (stop) break;
        if (fresh) {                    /* other channels: what is in the ring was for the ones before */
            acars_set(on, c, rt);
            aq_set();
            atomic_store(&a_rd, atomic_load(&a_wr));
            continue;
        }
        uint32_t r = atomic_load(&a_rd), avail = atomic_load(&a_wr) - r;
        uint32_t n = (avail < sizeof(chunk) ? avail : (uint32_t)sizeof(chunk)) & ~7u;
        if (!n) continue;
        uint32_t at = r % ARING, first = ARING - at < n ? ARING - at : n;
        memcpy(chunk, aring + at, first);
        memcpy(chunk + first, aring, n - first);
        atomic_store(&a_rd, r + n);
        if (acars_on) acars_run(chunk, (int)(n / 2));
        {                               /* behind the device: said at most once a minute */
            static uint32_t last_ms, last_drop;
            uint32_t        d = atomic_load(&a_drop), t = now_ms();
            if (d != last_drop && t - last_ms > 60000) {
                fprintf(stderr, "rtlspec: ACARS behind the device: %u bytes dropped\n", d - last_drop);
                last_drop = d;
                last_ms   = t;
            }
        }
    }
    return NULL;
}

/* commands(): the channels as they should be now, for the thread to set */
static void acars_say(bool on, uint64_t centre, uint32_t rate) {
    pthread_mutex_lock(&a_lock);
    if (on && !a_thread) {
        pthread_t t;
        a_thread = pthread_create(&t, NULL, acars_thread, NULL) == 0;
        if (a_thread) pthread_detach(t);
    }
    acars_cfg.on     = on;
    acars_cfg.centre = centre;
    acars_cfg.rate   = rate;
    acars_cfg.seq++;
    pthread_cond_signal(&a_cond);
    pthread_mutex_unlock(&a_lock);
}

static long per_row_now;        /* samples a row at the rate now */
static int  rows_now = 20;

/* The commands, on a thread of their own: the tuner's calls are USB control transfers, which libusb does not take
 * from inside its own event callback (the stream's, on_samples). What they change for the stream is handed to it
 * through `pend` and taken at the start of its next transfer */
static pthread_mutex_t pend_lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
    bool     centre, rate, listen, watch, rds, acars;
    uint64_t centre_hz, listen_hz, watch_hz, rds_hz;
    uint32_t rate_sps;
    int      mode;
} pend;

static uint32_t now_ms(void) {
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static uint64_t centre_now;
static int      n_bins;
static long     row_left;       /* samples till the next row */
static float    re[MAX_BINS], im[MAX_BINS], win[MAX_BINS], acc[MAX_BINS], wsum, norm;
static uint8_t  frame[24 + MAX_BINS * 4];
static uint8_t  blk[MAX_BINS * 2];
static uint32_t seq;
static bool     fresh = true;
static int      nfft, fill;

/* ---- The device's state, for the client ---- */

enum { S_NOSTICK, S_BUSY, S_OPENING, S_ON, S_STALLED, S_TX, S_NEEDS };
static const char *const s_name[] = {"NOSTICK", "BUSY", "OPENING", "ON", "STALLED", "TX", "NEEDS"};

#define PROTO      2                    /* the hello's first five words as before: a GUI of then reads them */
#define SOCK_NAME  "x6x00-rtlspec"      /* abstract: gone with the daemon, a second one cannot take it */
static const char *sock_name = SOCK_NAME;   /* -S */
#define STALL_MS   3000                 /* open and nothing for so long: STALLED */
#define HUNG_MS    12000                /* pulled out and still held so long: the close hangs, the daemon ends */
#define ADSB_HZ    1090000000u
#define ADSB_RATE  2400000u             /* dump1090's own */

static atomic_int  s_now = S_NOSTICK;
static atomic_uint last_ms;             /* the device's last transfer, either way */
static atomic_bool feeding;             /* AIRSPACE's dump1090 has the samples */
static atomic_bool stick_done;          /* the device's thread ended: the device closed */
static const char *replay;              /* -i */

/* ---- The tuner: as the client asked, or for dump1090 ---- */

static pthread_mutex_t tune_lock = PTHREAD_MUTEX_INITIALIZER;   /* all below; taken before pend_lock */
static bool            dev_open;        /* the device open and streaming or about to (dev_ops) */
static char            tuner_s[16] = "-";
static uint64_t        dev_lo = 1000000, dev_hi = 6000000000ULL;     /* what it tunes */
static uint64_t        want_centre;
static uint32_t        want_rate;
static int             want_gain;
static uint64_t        cur_centre;      /* what the device is set to: 0 (INT_MIN) not set since it opened */
static uint32_t        cur_rate;
static int             cur_gain = INT_MIN;
static atomic_bool     tx_on;           /* transmitting: the device is the transmitter's, the receiver's settings wait */

/* The device set as it should be. On the client's thread, the watchdog's, the device's before it streams: never on
 * the stream's, as libusb does not take control transfers from inside its own event callback. What the stream has to
 * know is handed to it through pend. Nothing while it transmits: what is asked meanwhile is set after */
static void apply_tuning(void) {
    pthread_mutex_lock(&tune_lock);
    bool     ads = atomic_load(&feeding), acr = !ads && atomic_load(&acars_want);
    uint64_t c   = ads ? ADSB_HZ : acr ? ACARS_HZ : want_centre;
    uint32_t r   = ads ? ADSB_RATE : want_rate;
    int      g   = ads || acr ? 999 : want_gain;    /* the tuner's top, as dump1090 itself sets */
    bool     dv  = dev_open && dev_ops && !atomic_load(&tx_on);

    if (c < dev_lo) c = dev_lo;
    if (c > dev_hi) c = dev_hi;
    if (dv) {
        uint32_t got;

        if (r != cur_rate && dev_ops->rate(r, &got) == 0) {
            cur_rate = got;
            pthread_mutex_lock(&pend_lock);
            pend.rate     = true;
            pend.rate_sps = cur_rate;
            pthread_mutex_unlock(&pend_lock);
        }
        if (g != cur_gain) {
            dev_ops->gain(g);
            cur_gain = g;
        }
    }
    if (c != cur_centre && (replay || (dv && dev_ops->freq(c) == 0))) {
        cur_centre = c;
        pthread_mutex_lock(&pend_lock);
        pend.centre    = true;
        pend.centre_hz = c;
        pthread_mutex_unlock(&pend_lock);
    }
    pthread_mutex_unlock(&tune_lock);
}

static void hello(char *s, size_t n) {
    pthread_mutex_lock(&tune_lock);
    const sdr_ops_t *o  = dev_ops;
    int              st = atomic_load(&s_now);

    snprintf(s, n, "RTLSPEC %d %s %s %u %d %s %llu %llu %d %d", PROTO,
             atomic_load(&feeding) && st != S_TX ? "ADSB" : s_name[st], tuner_s, cur_rate ? cur_rate : want_rate,
             n_bins, o ? o->kind : replay ? "REPLAY" : "-", (unsigned long long)dev_lo, (unsigned long long)dev_hi,
             st == S_TX ? 2 : o && o->tx && dev_open ? 1 : 0, o == &sdr_rtl ? sdr_rtl_open_at : -1);
    pthread_mutex_unlock(&tune_lock);
}

/* ---- AIRSPACE's dump1090: the raw samples into its standard input ---- */

static pthread_mutex_t feed_lock = PTHREAD_MUTEX_INITIALIZER;
static int             feed_fd = -1;
static bool            feed_odd;        /* an I/Q pair half written: its other half owed first */
static atomic_bool     feed_gone;       /* dump1090 went (the stream saw it): the tuner back, by the watchdog */

static void feed_set(int fd) {
    pthread_mutex_lock(&feed_lock);
    if (feed_fd >= 0) close(feed_fd);   /* dump1090 at the end of its input: it ends */
    feed_fd  = fd;
    feed_odd = false;
    if (fd >= 0) fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    atomic_store(&feeding, fd >= 0);
    pthread_mutex_unlock(&feed_lock);
    apply_tuning();
}

/* On the stream's thread: never waiting; what does not fit in the pipe is dropped, the pairs kept whole */
static void feed_out(const uint8_t *b, uint32_t n) {
    pthread_mutex_lock(&feed_lock);
    if (feed_fd >= 0) {
        ssize_t k = feed_odd ? write(feed_fd, "\x7f", 1) : 0;

        if (k == 1) feed_odd = false;
        if (!feed_odd) {
            k = write(feed_fd, b, n);
            if (k > 0 && (k & 1)) feed_odd = true;
        }
        if (k < 0 && errno == EPIPE) {
            close(feed_fd);
            feed_fd = -1;
            atomic_store(&feeding, false);
            atomic_store(&feed_gone, true);
        }
    }
    pthread_mutex_unlock(&feed_lock);
}

/* ---- The transmitter (a HackRF, a bladeRF): narrow FM from the client's voice ---- */

#define TX_RATE    2116800u             /* 48 x 44.1 kHz: each voice sample a step of the frequency over 48 */
#define TX_UP      48
#define TX_DEV     4000.0f              /* the deviation at the clipper's level, Hz: under 5 kHz with the filter's
                                         * overshoot after it */
#define TX_AMP     100                  /* the I/Q's level, of 127: the DAC's top kept clear */
#define TX_RAMP_ST 3                    /* the level's step a sample, of 32767: 5 ms up, 5 ms down */
#define TX_TOT_MS  180000               /* an over's longest: a time-out timer, as a handheld's */
#define TX_IDLE_MS 2000                 /* no voice from the client so long: it hung, the transmission ends */
#define TX_RING    (44100 * 2)
#define TX_LUT     4096

/* Where it may transmit at all: the amateur allocations from 2 m to 23 cm that one ITU region or another has (their
 * union, as src/cfg/ham_bands.c's for HF; the licence decides inside them). The GUI narrows it further to the TX
 * ranges set on the radio before it asks; outside these, nothing ever goes, whatever is asked */
static const struct {
    uint64_t lo, hi;
} tx_band[] = {
    {144000000, 148000000},             /* 2 m */
    {220000000, 225000000},             /* 1.25 m (Region 2) */
    {420000000, 450000000},             /* 70 cm */
    {902000000, 928000000},             /* 33 cm (Region 2) */
    {1240000000, 1300000000},           /* 23 cm */
};

static bool tx_band_ok(uint64_t hz) {
    for (size_t i = 0; i < sizeof(tx_band) / sizeof(tx_band[0]); i++)
        if (hz >= tx_band[i].lo && hz <= tx_band[i].hi) return true;
    return false;
}

static pthread_mutex_t tx_lock = PTHREAD_MUTEX_INITIALIZER;     /* the voice waiting, what was asked */
static int16_t         tx_ring[TX_RING];
static int             tx_head, tx_len;
static uint64_t        tx_hz;
static int             tx_gain;
static bool            tx_amp;
static atomic_bool     tx_req;          /* a transmission asked: the stream makes way for it */
static atomic_uint     tx_voice_ms;     /* the client's last voice */
static atomic_uint     tx_from_ms;      /* when it went on the air */
static const char *volatile tx_why;    /* why it is ending: tx_halt's */
static int16_t         tx_lut[TX_LUT];

static void tx_say(const char *word, uint64_t hz, const char *why) {
    char m[96];

    snprintf(m, sizeof(m), "RTX %s %llu %s", word, (unsigned long long)hz, why);
    put(m, strlen(m));
}

/* The voice's filters at 44.1 kHz: biquads from RBJ's cookbook */
typedef struct {
    float b0, b1, b2, a1, a2, z1, z2;
} bq_t;

static void bq_set(bq_t *q, bool high, float f0, float qq) {
    float w = 2 * (float)M_PI * f0 / AUD_RATE, al = sinf(w) / (2 * qq), c = cosf(w), a0 = 1 + al;

    q->b0 = (high ? (1 + c) / 2 : (1 - c) / 2) / a0;
    q->b1 = (high ? -(1 + c) : 1 - c) / a0;
    q->b2 = q->b0;
    q->a1 = -2 * c / a0;
    q->a2 = (1 - al) / a0;
    q->z1 = q->z2 = 0;
}

static float bq_run(bq_t *q, float x) {
    float y = q->b0 * x + q->z1;

    q->z1 = q->b1 * x - q->a1 * y + q->z2;
    q->z2 = q->b2 * x - q->a2 * y;
    return y;
}

/* The modulator, on the device's own thread while it transmits */
static struct {
    bq_t        hp, lp1, lp2;
    float       pre, env, gain;
    int32_t     inc1, dinc, inc;
    uint32_t    ph;
    int         sub, ramp;
    bool        ending;
    const char *why;
} mod;

static void mod_reset(void) {
    memset(&mod, 0, sizeof(mod));
    bq_set(&mod.hp, true, 300, 0.7071f);        /* below the voice: hum, the mic's thumps */
    bq_set(&mod.lp1, false, 3000, 0.5412f);     /* above it, after the clipper: a 4th-order Butterworth */
    bq_set(&mod.lp2, false, 3000, 1.3066f);
    mod.gain = 4;
}

/* The next voice sample (silence when none is waiting), shaped, as the frequency step for TX_UP samples: 300 Hz to
 * 3 kHz, levelled (its peaks to half scale, at most 26 dB up, held under -54 dBFS so the hiss of a pause is not
 * raised), pre-emphasised (6 dB an octave from 350 Hz, 0 dB at 1 kHz), clipped, filtered */
static int32_t mod_next(void) {
    int16_t s = 0;

    pthread_mutex_lock(&tx_lock);
    if (tx_len > AUD_RATE * 3 / 10) {   /* behind (the client's clock a little faster): back to 0.1 s */
        tx_head = (tx_head + tx_len - AUD_RATE / 10) % TX_RING;
        tx_len  = AUD_RATE / 10;
    }
    if (tx_len) {
        s       = tx_ring[tx_head];
        tx_head = (tx_head + 1) % TX_RING;
        tx_len--;
    }
    pthread_mutex_unlock(&tx_lock);

    float x = bq_run(&mod.hp, s / 32768.0f), a = fabsf(x);

    mod.env += (a > mod.env ? 0.05f : 0.00005f) * (a - mod.env);
    if (mod.env > 0.002f) {
        float want = 0.5f / mod.env;

        want = want > 20 ? 20 : want < 0.5f ? 0.5f : want;
        mod.gain += 0.002f * (want - mod.gain);
    }
    x *= mod.gain;
    float y = (x - 0.95f * mod.pre) / 0.1475f;

    mod.pre = x;
    y       = y > 1 ? 1 : y < -1 ? -1 : y;
    y       = bq_run(&mod.lp2, bq_run(&mod.lp1, y));
    return (int32_t)lrintf(y * TX_DEV / TX_RATE * 4294967296.0f);
}

/* The device's next pairs (sdr_tx_fill): the frequency eased from one voice sample's to the next, the level ramped
 * up at the start and down at the end. The end: asked (tx_halt), no voice for TX_IDLE_MS, TX_TOT_MS on the air */
static int mod_fill(int8_t *iq, uint32_t pairs) {
    uint32_t now = now_ms();

    atomic_store(&last_ms, now);
    if (!mod.ending) {
        if (tx_halt) mod.ending = true, mod.why = tx_why ? tx_why : "ASKED";
        else if (now - atomic_load(&tx_from_ms) > TX_TOT_MS) mod.ending = true, mod.why = "TIMEOUT";
        else if (now - atomic_load(&tx_voice_ms) > TX_IDLE_MS) mod.ending = true, mod.why = "IDLE";
    }
    for (uint32_t k = 0; k < pairs; k++) {
        if (mod.sub == 0) {
            int32_t from = mod.inc1;

            mod.inc1 = mod_next();
            mod.dinc = (mod.inc1 - from) / TX_UP;
            mod.inc  = from;
        }
        if (++mod.sub == TX_UP) mod.sub = 0;
        mod.inc += mod.dinc;
        mod.ph += (uint32_t)mod.inc;
        if (mod.ending) {
            if ((mod.ramp -= TX_RAMP_ST) <= 0) {
                memset(iq + 2 * k, 0, (size_t)(pairs - k) * 2);
                return 1;
            }
        } else if (mod.ramp < 32767) {
            mod.ramp = mod.ramp + TX_RAMP_ST > 32767 ? 32767 : mod.ramp + TX_RAMP_ST;
        }
        uint32_t p = mod.ph >> 20;
        int      c = tx_lut[(p + TX_LUT / 4) & (TX_LUT - 1)], sn = tx_lut[p];

        iq[2 * k]     = (int8_t)(((c * mod.ramp) >> 15) * TX_AMP >> 15);
        iq[2 * k + 1] = (int8_t)(((sn * mod.ramp) >> 15) * TX_AMP >> 15);
    }
    return 0;
}

/* Asked to transmit at hz: refused unless a device that transmits is open, AIRSPACE does not have it, and hz is in an
 * amateur band it tunes; then the stream is ended and the device's thread transmits (tx_run) */
static void tx_ask(uint64_t hz, int gain, bool amp) {
    const char *no = NULL;
    int         gmax = 0;

    pthread_mutex_lock(&tune_lock);
    if (!dev_open || !dev_ops) no = "NO-DEVICE";
    else if (!dev_ops->tx) no = "RECEIVE-ONLY";
    else if (atomic_load(&feeding)) no = "AIRSPACE";
    else if (!tx_band_ok(hz) || hz < dev_lo || hz > dev_hi) no = "NOT-AMATEUR";
    else gmax = dev_ops->tx_gain_max;
    pthread_mutex_unlock(&tune_lock);
    if (no) {
        tx_say("NO", hz, no);
        fprintf(stderr, "rtlspec: transmitting on %llu Hz refused: %s\n", (unsigned long long)hz, no);
        return;
    }
    if (atomic_load(&tx_req) || atomic_load(&tx_on)) return;     /* one at a time */
    pthread_mutex_lock(&tx_lock);
    tx_hz   = hz;
    tx_gain = gain < 0 ? 0 : gain > gmax ? gmax : gain;
    tx_amp  = amp;
    tx_head = tx_len = 0;
    pthread_mutex_unlock(&tx_lock);
    atomic_store(&tx_voice_ms, now_ms());
    tx_why  = NULL;
    tx_halt = 0;
    atomic_store(&tx_req, true);
    halt_rx();
}

/* The transmission ends (ramped down): why, for the client */
static void tx_end(const char *why) {
    if (!atomic_load(&tx_req) && !atomic_load(&tx_on)) return;
    tx_why  = why;
    tx_halt = 1;
}

/* The client's voice in: "TAUD", u32 count, s16 samples[count] */
static void tx_voice(const uint8_t *b, size_t n) {
    uint32_t cnt;

    if (n < 8) return;
    memcpy(&cnt, b + 4, 4);
    if (cnt > (n - 8) / 2) return;
    pthread_mutex_lock(&tx_lock);
    for (uint32_t i = 0; i < cnt; i++) {
        int16_t v;

        memcpy(&v, b + 8 + i * 2, 2);
        tx_ring[(tx_head + tx_len) % TX_RING] = v;
        if (tx_len < TX_RING) tx_len++;
        else tx_head = (tx_head + 1) % TX_RING;
    }
    pthread_mutex_unlock(&tx_lock);
    atomic_store(&tx_voice_ms, now_ms());
}

/* On the device's thread, its stream ended for it: the transmission, then the receiver set again as it was */
static void tx_run(const sdr_ops_t *o) {
    uint64_t hz;
    int      g, r;
    bool     amp;

    pthread_mutex_lock(&tx_lock);
    hz  = tx_hz;
    g   = tx_gain;
    amp = tx_amp;
    pthread_mutex_unlock(&tx_lock);
    atomic_store(&tx_req, false);
    if (tx_halt || stop) {              /* ended before it began */
        tx_say("END", hz, tx_why ? tx_why : "ASKED");
        return;
    }
    mod_reset();
    atomic_store(&tx_from_ms, now_ms());
    atomic_store(&tx_on, true);
    atomic_store(&s_now, S_TX);
    tx_say("ON", hz, "-");
    fprintf(stderr, "rtlspec: transmitting FM on %llu Hz, TX gain %d dB, amplifier %s\n", (unsigned long long)hz, g,
            amp ? "on" : "off");
    r = o->tx(hz, TX_RATE, g, amp, mod_fill);
    atomic_store(&tx_on, false);
    tx_say("END", hz, r ? "FAILED" : mod.why ? mod.why : "ASKED");
    fprintf(stderr, "rtlspec: off the air (%s) after %u ms\n", r ? "FAILED" : mod.why ? mod.why : "ASKED",
            now_ms() - atomic_load(&tx_from_ms));
    tx_halt = 0;
    pthread_mutex_lock(&tune_lock);     /* the receiver as the client last asked, all set again */
    cur_centre = 0;
    cur_rate   = 0;
    cur_gain   = INT_MIN;
    pthread_mutex_unlock(&tune_lock);
    apply_tuning();
    atomic_store(&last_ms, now_ms());
    atomic_store(&s_now, S_OPENING);    /* ON at its first samples */
}

static atomic_bool rx_restart;          /* "R" */
static atomic_int  want_dev;            /* "k": kinds[] + 1 of the device to hold, 0 the first by order */
static atomic_bool want_failed;         /* it would not open: the one before kept, till "k" again */

/* ---- The client's commands, on its own thread ---- */

static void command(char *line, int fd) {
    if (line[0] == 'r') {
        atomic_store(&rows_on, atoi(line + 1) != 0);
    } else if (line[0] == 'f') {
        uint64_t hz = strtoull(line + 1, NULL, 10);

        if (hz >= 1000000 && hz <= 6000000000ULL) {     /* within what the device tunes: apply_tuning */
            pthread_mutex_lock(&tune_lock);
            want_centre = hz;
            pthread_mutex_unlock(&tune_lock);
            apply_tuning();
        }
    } else if (line[0] == 's') {
        uint32_t r = (uint32_t)strtoul(line + 1, NULL, 10);

        if (r >= 900001 && r <= 3200000 && !replay) {
            pthread_mutex_lock(&tune_lock);
            want_rate = r;
            pthread_mutex_unlock(&tune_lock);
            apply_tuning();
        }
    } else if (line[0] == 'g') {
        pthread_mutex_lock(&tune_lock);
        want_gain = atoi(line + 1);
        pthread_mutex_unlock(&tune_lock);
        apply_tuning();
    } else if (line[0] == 'l') {
        char    *e;
        uint64_t hz = strtoull(line + 1, &e, 10);

        pthread_mutex_lock(&pend_lock);
        pend.listen    = true;
        pend.listen_hz = hz;
        pend.mode      = hz ? (int)strtol(e, NULL, 10) : 0;
        pthread_mutex_unlock(&pend_lock);
    } else if (line[0] == 'w') {          /* "w <Hz>": the channel watched for SAME alerts, 0 none */
        pthread_mutex_lock(&pend_lock);
        pend.watch    = true;
        pend.watch_hz = strtoull(line + 1, NULL, 10);
        pthread_mutex_unlock(&pend_lock);
    } else if (line[0] == 'd') {          /* "d <Hz>": the FM broadcast channel decoded for RDS, 0 none */
        pthread_mutex_lock(&pend_lock);
        pend.rds    = true;
        pend.rds_hz = strtoull(line + 1, NULL, 10);
        pthread_mutex_unlock(&pend_lock);
    } else if (line[0] == 'c') {          /* "c 1": ACARS; "c 0": no more */
        atomic_store(&acars_want, atoi(line + 1) != 0);
        apply_tuning();
        pthread_mutex_lock(&pend_lock);
        pend.acars = true;
        pthread_mutex_unlock(&pend_lock);
    } else if (line[0] == 'a') {
        feed_set(fd);                   /* -1: "a 0", the samples dump1090's no more */
        fd = -1;
    } else if (line[0] == 't') {          /* "t <Hz> <gain> <amp>": transmit; "t 0": end it */
        char    *e;
        uint64_t hz = strtoull(line + 1, &e, 10);

        if (!hz) {
            tx_end("ASKED");
        } else {
            long g = strtol(e, &e, 10);

            tx_ask(hz, (int)g, strtol(e, NULL, 10) != 0);
        }
    } else if (line[0] == 'k') {          /* "k <KIND>": the device to hold (kinds[]'s order), "k -" any */
        char k[16] = "";

        sscanf(line + 1, "%15s", k);
        bool rtl = !strncmp(k, "RTL", 3);   /* "RTL<n>": RTL n in serial order (sdr_rtl.c); "RTL" RTL0 */

        if (rtl) sdr_rtl_index = atoi(k + 3);
        atomic_store(&want_dev, !strcmp(k, "HACKRF") ? 1 : !strcmp(k, "BLADERF") ? 2 : rtl ? 3 : 0);
        atomic_store(&want_failed, false);
    } else if (line[0] == 'R') {          /* the stream stopped and started, as for a transmission */
        if (!atomic_load(&tx_on) && !atomic_load(&tx_req)) {
            atomic_store(&rx_restart, true);
            halt_rx();
        }
    } else if (line[0] == 'Q') {
        stop    = 1;
        tx_why  = "ASKED";
        tx_halt = 1;
        halt_rx();
    }
    if (fd >= 0) close(fd);
}

static void *client_thread(void *arg) {
    int fd = (int)(intptr_t)arg;

    for (;;) {
        static char line[16384];        /* a line, or the voice: 50 ms of it is 4418 bytes */
        union {
            struct cmsghdr h;
            char           b[CMSG_SPACE(sizeof(int) * 4)];
        } cm;
        struct iovec  io = {.iov_base = line, .iov_len = sizeof(line) - 1};
        struct msghdr m  = {.msg_iov = &io, .msg_iovlen = 1, .msg_control = cm.b, .msg_controllen = sizeof(cm.b)};
        ssize_t       n  = recvmsg(fd, &m, MSG_CMSG_CLOEXEC);
        int           got = -1;

        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        if (n >= 8 && !memcmp(line, "TAUD", 4)) {
            tx_voice((const uint8_t *)line, (size_t)n);
            continue;
        }
        line[n] = 0;
        for (struct cmsghdr *h = CMSG_FIRSTHDR(&m); h; h = CMSG_NXTHDR(&m, h)) {
            if (h->cmsg_level != SOL_SOCKET || h->cmsg_type != SCM_RIGHTS) continue;
            int k = (int)((h->cmsg_len - CMSG_LEN(0)) / sizeof(int));

            for (int i = 0; i < k; i++) {
                int x;

                memcpy(&x, CMSG_DATA(h) + i * sizeof(int), sizeof(int));
                if (got < 0) got = x;
                else close(x);
            }
        }
        command(line, got);
    }
    /* Gone (or replaced): what it asked for ends, its transmission first; the device goes on, tuned as it was */
    tx_end("GONE");
    pthread_mutex_lock(&out_lock);
    if (client == fd) client = -1;
    pthread_mutex_unlock(&out_lock);
    close(fd);
    atomic_store(&rows_on, false);
    pthread_mutex_lock(&pend_lock);
    pend.listen    = true;
    pend.listen_hz = 0;
    pend.mode      = 0;
    pend.watch     = true;
    pend.watch_hz  = 0;
    pend.rds       = true;
    pend.rds_hz    = 0;
    pend.acars     = true;
    pthread_mutex_unlock(&pend_lock);
    if (atomic_exchange(&acars_want, false)) apply_tuning();
    return NULL;
}

/* What the commands changed, taken by the stream */
static void commands(uint64_t *centre, bool *fresh) {
    pthread_mutex_lock(&pend_lock);
    if (pend.rate) {
        rate_now    = pend.rate_sps;
        per_row_now = (long)rate_now / rows_now;
        *fresh      = true;
    }
    if (pend.centre) {
        *centre = pend.centre_hz;
        *fresh  = true;
    }
    if (pend.listen) {
        want_mode = pend.mode;
        listen_set(pend.mode, pend.listen_hz, *centre, rate_now);
        if (!pend.listen_hz) l_hz = 0;
    } else if ((pend.centre || pend.rate) && l_hz) {
        listen_set(l_mode ? l_mode : want_mode, l_hz, *centre, rate_now);
    }
    if (pend.watch || ((pend.centre || pend.rate) && watch.hz))
        chan_set(&watch, (pend.watch ? pend.watch_hz : watch.hz) ? 1 : 0, pend.watch ? pend.watch_hz : watch.hz,
                 *centre, rate_now);
    if (pend.rds && pend.rds_hz != rds_hz) {    /* another station: what came from the last one forgotten */
        rds_hz = pend.rds_hz;
        if (rds_hz && !rds) rds = rds_new(MID_RATE);
        if (rds) rds_reset(rds);
        if (rds_hz) rds_say();
    }
    if (pend.rds || pend.listen || pend.centre || pend.rate) {
        /* RDS off the listened channel's multiplex when it is the same WFM channel, else on a channel of its own */
        bool shared = rds_hz && lis.mode == 3 && lis.hz == rds_hz;

        lis.mpx = shared ? rds_mpx : NULL;
        if (shared || !rds_hz) rdsc.mode = 0;
        else if (pend.rds || pend.centre || pend.rate || rdsc.mode != 3) chan_set(&rdsc, 3, rds_hz, *centre, rate_now);
    }
    if (pend.acars || ((pend.centre || pend.rate) && atomic_load(&acars_want)))
        acars_say(atomic_load(&acars_want), *centre, rate_now);
    pend.centre = pend.rate = pend.listen = pend.watch = pend.rds = pend.acars = false;
    pthread_mutex_unlock(&pend_lock);
}

/* ---- The stream: rows, audio and alerts as asked, or dump1090's samples ---- */

/* Each transfer the device sends (unsigned 8-bit I/Q, whichever it is), on its backend's thread: the commands waiting
 * first */
static atomic_ullong rx_bytes;         /* what the device sent: the watchdog says when it is short of the rate */

static void on_samples(unsigned char *buf, uint32_t n) {
    int  bins = n_bins;
    bool rows;

    atomic_store(&last_ms, now_ms());
    atomic_fetch_add(&rx_bytes, n);
    if (atomic_load(&s_now) != S_ON) atomic_store(&s_now, S_ON);
    if (stop) {
        halt_rx();
        return;
    }
    commands(&centre_now, &fresh);
    if (atomic_load(&feeding)) {
        feed_out(buf, n);
        return;
    }
    if (atomic_load(&acars_want)) acars_put(buf, n);
    rows = atomic_load(&rows_on);
    if (!rows) {                        /* rows again later: from a clean start */
        nfft = fill = 0;
        row_left = per_row_now;
        fresh    = true;
        if (!lis.mode && !watch.mode && !rdsc.mode) return;
    }
    for (uint32_t off = 0; off + 1 < n; off += 2) {
        if (lis.mode) chan_sample(&lis, buf[off], buf[off + 1]);
        if (watch.mode) chan_sample(&watch, buf[off], buf[off + 1]);
        if (rdsc.mode) chan_sample(&rdsc, buf[off], buf[off + 1]);
        if (!rows) continue;
        /* The first BINS * AVG samples of each row's share go into its FFTs, the rest are passed over */
        if (nfft < AVG) {
            blk[fill * 2]     = buf[off];
            blk[fill * 2 + 1] = buf[off + 1];
            if (++fill == bins) {
                for (int i = 0; i < bins; i++) {
                    re[i] = (blk[i * 2] - 127.4f) / 128.0f * win[i];
                    im[i] = (blk[i * 2 + 1] - 127.4f) / 128.0f * win[i];
                }
                fft(re, im, bins);
                for (int i = 0; i < bins; i++) acc[i] += re[i] * re[i] + im[i] * im[i];
                nfft++;
                fill = 0;
            }
        }
        if (--row_left > 0) continue;
        row_left = per_row_now;
        if (nfft == 0) continue;

        /* A row: lowest frequency first (the FFT's second half, then its first), the DC bin from its neighbours */
        uint8_t *p = frame;
        uint16_t nb = (uint16_t)bins, flags = fresh ? 1 : 0;

        memcpy(p, "RSPC", 4); p += 4;
        seq++;
        memcpy(p, &seq, 4); p += 4;
        memcpy(p, &centre_now, 8); p += 8;
        memcpy(p, &rate_now, 4); p += 4;
        memcpy(p, &nb, 2); p += 2;
        memcpy(p, &flags, 2); p += 2;
        acc[0] = (acc[1] + acc[bins - 1]) / 2;
        for (int i = 0; i < bins; i++) {
            float v  = acc[(i + bins / 2) % bins] / nfft;
            float db = 10 * log10f(v + 1e-20f) - norm;

            memcpy(p, &db, 4);
            p += 4;
        }
        for (int i = 0; i < bins; i++) acc[i] = 0;
        nfft  = 0;
        fill  = 0;
        fresh = false;
        put(frame, (size_t)(p - frame));
    }
}

/* -i: a recording in place of the stick, at its own pace and over again */
static void replay_run(void) {
    static uint8_t chunk[READ_LEN];
    FILE          *f = fopen(replay, "rb");

    if (!f) {
        atomic_store(&s_now, S_NOSTICK);
        sleep(2);
        return;
    }
    pthread_mutex_lock(&tune_lock);
    snprintf(tuner_s, sizeof(tuner_s), "REPLAY");
    cur_rate   = want_rate;
    cur_centre = 0;
    pthread_mutex_unlock(&tune_lock);
    apply_tuning();
    while (!stop) {
        size_t n = fread(chunk, 1, sizeof(chunk), f);

        if (n < 2) {
            if (ftell(f) < 2) break;
            rewind(f);
            continue;
        }
        on_samples(chunk, (uint32_t)n);
        usleep((useconds_t)(n / 2 * 1000000.0 / rate_now));
    }
    fclose(f);
    sleep(1);
}

/* The stick held pulled out: its USB device file among this process's open files, its node gone ("(deleted)"). Not
 * the device count: a stick plugged back within a second (or another one) is counted as if it had never gone */
static bool stick_pulled(void) {
    DIR           *d = opendir("/proc/self/fd");
    struct dirent *e;
    bool           gone = false;

    if (!d) return false;
    while (!gone && (e = readdir(d))) {
        char    path[64], to[128];
        ssize_t n;

        if (e->d_name[0] == '.') continue;
        snprintf(path, sizeof(path), "/proc/self/fd/%s", e->d_name);
        if ((n = readlink(path, to, sizeof(to) - 1)) <= 0) continue;
        to[n] = 0;
        gone = !strncmp(to, "/dev/bus/usb/", 13) && strstr(to, " (deleted)");
    }
    closedir(d);
    return gone;
}

/* What is looked for, in this order: those that transmit first, so one plugged in beside a stick is the one used
 * ("k" names another: its index here + 1) */
static const sdr_ops_t *const kinds[] = {&sdr_hackrf, &sdr_bladerf, &sdr_rtl};
static const sdr_ops_t *const test_kinds[] = {&sdr_file};      /* -T: that alone */

/* The device "k" asked for, plugged in, not the one open (o), not while transmitting: o to be closed for it */
static bool switch_due(const sdr_ops_t *o) {
    int w = atomic_load(&want_dev);

    if (!w || !o || sdr_file_path || replay || atomic_load(&want_failed)) return false;
    if (atomic_load(&tx_on) || atomic_load(&tx_req)) return false;
    if (kinds[w - 1] == o)              /* another RTL asked for (RTL<n>), and it is there */
        return o == &sdr_rtl && sdr_rtl_open_at >= 0 && sdr_rtl_index != sdr_rtl_open_at &&
               sdr_rtl_index < sdr_rtl_count();
    return kinds[w - 1]->present();
}

/* The device's stream till it ends: made way for a transmission or a restart ("R"); ended by itself with the
 * device still there, it is left open and alone till it is pulled out (closed and opened again, it would not be
 * mended, and that is what wears the USB host) */
static void streaming(const sdr_ops_t *o) {
    for (;;) {
        rx_halt = 0;                    /* before the look at what asked for it: a halt set after it is not lost */
        if (stop || stick_pulled()) return;
        if (atomic_load(&tx_req)) {
            tx_run(o);
            continue;
        }
        if (atomic_exchange(&rx_restart, false)) fprintf(stderr, "rtlspec: the stream started again (R)\n");
        if (switch_due(o)) return;
        int e = o->rx(on_samples, &rx_halt);

        if (stop || stick_pulled() || switch_due(o)) return;
        if (e == 0 || atomic_load(&tx_req) || atomic_load(&rx_restart)) continue;
        fprintf(stderr, "rtlspec: the stream ended, the device still there: kept as it is till it is pulled out\n");
        atomic_store(&s_now, S_STALLED);
        while (!stop && !stick_pulled() && !switch_due(o)) sleep(1);
        return;
    }
}

/* The device: opened when it is there, streaming till it is pulled out */
static void *stick_thread(void *arg) {
    (void)arg;
    while (!stop) {
        const sdr_ops_t *o = NULL;
        char             tn[16] = "";
        uint64_t         lo = 0, hi = 0;
        int              r;

        if (replay) {
            replay_run();
            continue;
        }
        int w = atomic_load(&want_dev);

        if (sdr_file_path) o = test_kinds[0];
        if (!o && sdr_rtl_exact && sdr_rtl.present()) o = &sdr_rtl;     /* -W: that RTL, nothing else */
        if (!o && !sdr_rtl_exact && w && !atomic_load(&want_failed) && kinds[w - 1]->present()) o = kinds[w - 1];
        for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]) && !o && !sdr_rtl_exact; i++)
            if (kinds[i]->present()) o = kinds[i];
        if (!o) {
            atomic_store(&s_now, S_NOSTICK);
            sleep(1);
            continue;
        }
        if ((r = o->open(tn, sizeof(tn), &lo, &hi)) < 0 || r == 1) {
            if (w && o == kinds[w - 1] && !atomic_load(&want_failed)) {   /* the one asked for: the others then */
                fprintf(stderr, "rtlspec: the %s asked for would not open (%s): the first by order kept\n", o->kind,
                        r == 1 ? "another program has it" : "failed");
                atomic_store(&want_failed, true);
                continue;
            }
            atomic_store(&s_now, r == 1 ? S_BUSY : S_NOSTICK);  /* busy: another program has it, tried again, not
                                                                 * forced; else gone again, or not one it opens */
            sleep(2);
            continue;
        }
        pthread_mutex_lock(&tune_lock);
        dev_ops = o;
        snprintf(tuner_s, sizeof(tuner_s), "%s", tn[0] ? tn : o->kind);
        if (r == 0) {
            dev_lo   = lo;
            dev_hi   = hi;
            dev_open = true;
        }
        cur_centre = 0;
        cur_rate   = 0;
        cur_gain   = INT_MIN;
        pthread_mutex_unlock(&tune_lock);
        bool again = false;             /* closed to be opened again at once, not pulled out */

        if (r == 2) {                   /* there and not usable as it is: said so (tuner), left so till pulled out or
                                         * till what it wants is there (then closed and opened again) */
            atomic_store(&s_now, S_NEEDS);
            fprintf(stderr, "rtlspec: the %s needs %s\n", o->kind, tn);
            while (!stop && o->present() && !stick_pulled() && !switch_due(o)) {
                sleep(2);
                if (o->ready_again && o->ready_again(tn)) {
                    fprintf(stderr, "rtlspec: what the %s needed (%s) changed: opened again\n", o->kind, tn);
                    again = true;
                    break;
                }
            }
        } else {
            apply_tuning();
            atomic_store(&last_ms, now_ms());
            atomic_store(&s_now, S_OPENING);
            fprintf(stderr, "rtlspec: the %s open, %s, %llu-%llu Hz%s\n", o->kind, tuner_s, (unsigned long long)lo,
                    (unsigned long long)hi, o->tx ? ", it can transmit" : "");
            streaming(o);
        }
        pthread_mutex_lock(&tune_lock);
        dev_open = false;
        snprintf(tuner_s, sizeof(tuner_s), "-");        /* no tuner said for a device that is not there */
        pthread_mutex_unlock(&tune_lock);
        o->close();
        pthread_mutex_lock(&tune_lock);
        dev_ops = NULL;
        dev_lo  = 1000000;
        dev_hi  = 6000000000ULL;
        pthread_mutex_unlock(&tune_lock);
        fprintf(stderr, "rtlspec: the %s closed (%s)\n", o->kind, stop ? "the daemon ends" : again ? "to open it again"
                                                                 : switch_due(o) ? "another asked for (k)" : "pulled out");
        atomic_store(&s_now, S_NOSTICK);
    }
    atomic_store(&stick_done, true);
    return NULL;
}

/* ---- A stalled RTL put right as unplugging it would: its hub port disabled for a second (the kernel sees it go and
 * come back; the stick's thread closes it as pulled out and opens it again). The user, 2026-11-16: "can't you bounce
 * the bus on your own?" Tried on the radio: the port disabled while it streamed, the stick back and streaming again
 * in 4 s, nothing in the kernel log but the disconnect and the new device. Only on a kernel whose musb host has the
 * fixes for a disconnect with transfers in flight (9101, 9102: an unpatched one froze, and writing 0 to the device's
 * "authorized" oopsed it): musb_h_disable 0x17c bytes unpatched, 0x348 with them, its size its distance to the next
 * symbol of musb_hdrc in /proc/kallsyms. Once a minute at most, three times in ten minutes ---- */
static bool musb_fixed(void) {
    FILE         *f = fopen("/proc/kallsyms", "r");
    char          l[256], name[128], mod[64];
    unsigned long a, at = 0, next = ~0UL;
    static int    known = -1;

    if (known >= 0) return known;
    if (!f) return false;
    while (fgets(l, sizeof(l), f))
        if (sscanf(l, "%lx %*s %127s %63s", &a, name, mod) == 3 && !strcmp(mod, "[musb_hdrc]") && !strcmp(name, "musb_h_disable"))
            at = a;
    rewind(f);
    while (at && fgets(l, sizeof(l), f))
        if (sscanf(l, "%lx %*s %127s %63s", &a, name, mod) == 3 && !strcmp(mod, "[musb_hdrc]") && a > at && a < next)
            next = a;
    fclose(f);
    known = at && next != ~0UL && next - at > 0x200;
    fprintf(stderr, "rtlspec: the musb host %s (musb_h_disable %#lx bytes): a stalled RTL %s\n",
            known ? "has the disconnect fixes" : "is not known to have the disconnect fixes",
            at && next != ~0UL ? next - at : 0UL, known ? "is bounced on its hub port" : "waits to be pulled out");
    return known;
}

static void *bounce_thread(void *arg) {
    char *loc = arg, path[160], *dot = strrchr(loc, '.'), *dash = strchr(loc, '-');
    FILE *f;

    if (dot)                            /* behind a hub: <hub>:1.0/<hub>-port<N> */
        snprintf(path, sizeof(path), "/sys/bus/usb/devices/%.*s:1.0/%.*s-port%s/disable", (int)(dot - loc), loc,
                 (int)(dot - loc), loc, dot + 1);
    else if (dash)                      /* on a root port: <bus>-0:1.0/usb<bus>-port<N> */
        snprintf(path, sizeof(path), "/sys/bus/usb/devices/%.*s-0:1.0/usb%.*s-port%s/disable", (int)(dash - loc), loc,
                 (int)(dash - loc), loc, dash + 1);
    else
        path[0] = 0;
    if (path[0] && (f = fopen(path, "w"))) {
        fprintf(stderr, "rtlspec: the RTL at USB %s stalled: its port bounced (%s)\n", loc, path);
        fputs("1", f);
        fclose(f);
        sleep(1);
        if ((f = fopen(path, "w"))) {
            fputs("0", f);
            fclose(f);
        }
    } else {
        fprintf(stderr, "rtlspec: the RTL at USB %s stalled: no port to bounce\n", loc);
    }
    free(loc);
    return NULL;
}

static void stall_bounce(void) {
    static uint32_t tries[3];
    static int      next;
    uint32_t        t = now_ms();
    pthread_t       th;
    char           *loc;

    if (!sdr_rtl_open_loc[0] || sdr_file_path || t - tries[(next + 2) % 3] < 60000 ||
        (tries[next] && t - tries[next] < 600000) || !musb_fixed())
        return;
    tries[next] = t;
    next        = (next + 1) % 3;
    if ((loc = strdup(sdr_rtl_open_loc)) && pthread_create(&th, NULL, bounce_thread, loc) == 0) pthread_detach(th);
    else free(loc);
}

/* Each 200 ms: the state told when it changes, a stall seen, dump1090's samples taken back when it went */
static void *watch_thread(void *arg) {
    char     said[128] = "", now[128];
    uint32_t looked = 0, pulled_at = 0, rate_at = 0, rate_said = 0, stalled_at = 0;
    uint64_t rate_from = 0;
    (void)arg;

    while (!stop) {
        int s = atomic_load(&s_now);

        /* Each 10 s streaming: what came against the rate; said when under 95% (once a minute at most), and the first
         * time after an open */
        if (s != S_ON) {
            rate_at = 0;
        } else if (!rate_at) {
            rate_at   = now_ms();
            rate_from = atomic_load(&rx_bytes);
        } else if (now_ms() - rate_at >= 10000) {
            uint64_t got  = atomic_load(&rx_bytes) - rate_from;
            double   want = (double)(cur_rate ? cur_rate : want_rate) * 2 * (now_ms() - rate_at) / 1000.0;
            int      pc   = want > 0 ? (int)(got * 100 / want) : 0;

            if (!rate_said || (pc < 95 && now_ms() - rate_said > 60000)) {
                fprintf(stderr, "rtlspec: %.2f MB/s from the device, %d%% of %u S/s\n",
                        got / ((now_ms() - rate_at) / 1000.0) / 1e6, pc, cur_rate ? cur_rate : want_rate);
                rate_said = now_ms() | 1;
            }
            rate_at   = now_ms();
            rate_from = atomic_load(&rx_bytes);
        }

        if (atomic_exchange(&feed_gone, false)) apply_tuning();
        /* only from the state read: the stick's thread may have said NOSTICK meanwhile */
        if ((s == S_ON || s == S_OPENING) && now_ms() - atomic_load(&last_ms) > STALL_MS &&
            atomic_compare_exchange_strong(&s_now, &s, S_STALLED))
            fprintf(stderr, "rtlspec: nothing from the stick for %d s\n", STALL_MS / 1000);
        if (s == S_ON && !access("/tmp/rtlspec_stall", F_OK) && !unlink("/tmp/rtlspec_stall")) {
            fprintf(stderr, "rtlspec: /tmp/rtlspec_stall: bounced as if stalled (a test of the bounce)\n");
            stall_bounce();             /* at once: a stick that streams is ON again before 5 s */
        }
        if (atomic_load(&s_now) == S_STALLED) {     /* 5 s stalled: its port bounced */
            if (!stalled_at) stalled_at = now_ms() | 1;
            else if (now_ms() - stalled_at > 5000) stall_bounce(), stalled_at = 0;
        } else {
            stalled_at = 0;
        }
        /* Pulled out, its stream not ended by it: ended here, so it is closed and the next one opened. Still held
         * HUNG_MS on, libusb hangs on it: the daemon ends (its end lets go of everything), the GUI starts it again */
        if (now_ms() - looked > 1000) {
            bool gone = stick_pulled();

            looked = now_ms();
            if (gone) {
                tx_end("GONE");
                halt_rx();
            } else if (switch_due(dev_ops)) {
                fprintf(stderr, "rtlspec: the %s asked for (k): the %s's stream ended for it\n",
                        kinds[atomic_load(&want_dev) - 1]->kind, dev_ops->kind);
                halt_rx();
            }
            if (!gone) {
                pulled_at = 0;
            } else if (!pulled_at) {
                pulled_at = looked;
            } else if (looked - pulled_at > HUNG_MS) {
                fprintf(stderr, "rtlspec: the stick pulled out %d s ago and still not closed: the daemon ends\n",
                        HUNG_MS / 1000);
                _exit(2);
            }
        }
        /* A transmission whose device stopped taking samples (a hung stream), or long past its time-out: ended
         * hard, by the backend */
        if (atomic_load(&tx_on) && (now_ms() - atomic_load(&last_ms) > 1500 ||
                                    now_ms() - atomic_load(&tx_from_ms) > TX_TOT_MS + 5000)) {
            const sdr_ops_t *o = dev_ops;

            fprintf(stderr, "rtlspec: the transmission does not end by itself: stopped\n");
            if (o && o->tx_stop) o->tx_stop();
        }
        hello(now, sizeof(now));
        if (strcmp(now, said)) {
            put(now, strlen(now));
            snprintf(said, sizeof(said), "%s", now);
        }
        usleep(200000);
    }
    return NULL;
}

int main(int argc, char **argv) {
    int      bins = 2048, rows = 20, gain = 280, opt, ls;
    bool     front = false;
    uint32_t rate   = 2400000;
    uint64_t centre = 162550000;

    while ((opt = getopt(argc, argv, "Fd:f:s:n:r:g:i:S:T:W")) != -1) {
        switch (opt) {
            case 'F': front = true; break;
            case 'd': sdr_rtl_index = atoi(optarg); break;
            case 'f': centre = strtoull(optarg, NULL, 10); break;
            case 's': rate = (uint32_t)strtoul(optarg, NULL, 10); break;
            case 'n': bins = atoi(optarg); break;
            case 'r': rows = atoi(optarg); break;
            case 'g': gain = atoi(optarg); break;
            case 'i': replay = optarg; break;
            case 'S': sock_name = optarg; break;
            case 'W':                           /* a watch channel's: RTL -d alone (rtl_watch.c); no DMR (6% of a
                                                 * core each, and the one vocoder is B's) */
                sdr_rtl_exact = true;
                lis.dmr_on    = getenv("RTLSPEC_DMR") != NULL;      /* (a test on a spare RTL: DMR all the same) */
                break;
            case 'T': sdr_file_path = optarg; break;
            default: return 1;
        }
    }
    if (bins < 64 || bins > MAX_BINS || (bins & (bins - 1)) || rows < 1 || rows > 50) return 1;

    /* The name taken first: a second daemon stops here. Listened on once in the background, so a client is told this
     * process (SO_PEERCRED), and a GUI that started it connects a moment later */
    struct sockaddr_un a = {.sun_family = AF_UNIX};
    socklen_t          alen = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + strlen(sock_name));

    if (strlen(sock_name) > sizeof(a.sun_path) - 2) return 1;
    memcpy(a.sun_path + 1, sock_name, strlen(sock_name));
    ls = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (ls < 0) return 1;
    if (bind(ls, (struct sockaddr *)&a, alen) != 0) {
        printf("RTLSPEC RUNNING\n");
        return 0;
    }
    if (!front) {                       /* in the background, nobody's child: no GUI's end is its end */
        pid_t p = fork();

        if (p < 0) return 1;
        if (p > 0) _exit(0);
        setsid();
        if ((p = fork()) < 0) return 1;
        if (p > 0) _exit(0);
        if (chdir("/") != 0) return 1;
        int nul = open("/dev/null", O_RDWR);
        if (nul >= 0) dup2(nul, 0), dup2(nul, 1);
    }
    listen(ls, 4);

    struct sigaction sa = {.sa_handler = on_term};
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);           /* a client or dump1090 gone is seen as an error, not the daemon's end */

    want_centre = centre;
    want_rate   = rate;
    want_gain   = gain;
    rate_now    = rate;
    rows_now    = rows;
    per_row_now = (long)rate / rows;
    row_left    = per_row_now;
    n_bins      = bins;
    centre_now  = centre;
    for (int i = 0; i < 1024; i++) sin_lut[i] = (int16_t)lrint(32767 * sin(2 * M_PI * i / 1024));
    for (int i = 0; i < TX_LUT; i++) tx_lut[i] = (int16_t)lrint(32767 * sin(2 * M_PI * i / TX_LUT));
    for (int i = 0; i < bins; i++) {
        win[i] = 0.5f - 0.5f * cosf(2 * (float)M_PI * i / (bins - 1));
        wsum += win[i];
    }
    norm = 20 * log10f(wsum);           /* dB of the window's full-scale sine: a level read as dBFS */

    pthread_t stick, wd, ct;
    bool      ct_on = false;

    pthread_create(&stick, NULL, stick_thread, NULL);
    pthread_create(&wd, NULL, watch_thread, NULL);
    fprintf(stderr, "rtlspec: daemon %d running\n", (int)getpid());
    if (lis.dmr_on) ambe_start();       /* DMR's vocoder looked for from the start (its self-test in the log), not
                                         * by watch channels' daemons */
    if (lis.dmr_on) dsd_on = dsd_init(dsd_heard);      /* dsd-fme: B's daemon's alone, as DMR is */

    while (!stop) {
        struct pollfd p = {.fd = ls, .events = POLLIN};
        char          h[128];
        int           c;

        if (poll(&p, 1, 500) <= 0) continue;
        if ((c = accept4(ls, NULL, NULL, SOCK_CLOEXEC)) < 0) continue;
        int big = 1 << 20;
        setsockopt(c, SOL_SOCKET, SO_SNDBUF, &big, sizeof(big));
        pthread_mutex_lock(&out_lock);
        if (client >= 0) shutdown(client, SHUT_RDWR);   /* the one before: its thread ends */
        pthread_mutex_unlock(&out_lock);
        if (ct_on) pthread_join(ct, NULL);
        pthread_mutex_lock(&out_lock);
        client = c;
        pthread_mutex_unlock(&out_lock);
        hello(h, sizeof(h));
        put(h, strlen(h));
        ct_on = pthread_create(&ct, NULL, client_thread, (void *)(intptr_t)c) == 0;
    }

    /* Told to end: a transmission ended, the stream cancelled, the device closed (6 s for it, then gone anyway) */
    tx_halt = 1;
    halt_rx();
    for (int i = 0; i < 60 && !atomic_load(&stick_done); i++) usleep(100000);
    fprintf(stderr, "rtlspec: daemon ends\n");
    _exit(0);
}
