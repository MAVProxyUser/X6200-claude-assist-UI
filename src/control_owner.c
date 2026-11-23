/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - Who controls the radio (control_owner.h)
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#include "control_owner.h"
#include "radio.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define INJECT_MS   150 /* an input event this soon after an injection is the injection, not the operator */
#define INJECT_RING 16  /* the latest injections' times kept: a key's press and release, a knob's turns */

static pthread_mutex_t mux = PTHREAD_MUTEX_INITIALIZER;
static ctl_info_t      owner;
static ctl_info_t      lender;    /* who a takeover took it from (ctl_give_back), kind CTL_NONE: no one (it was free) */
static bool            lent;      /* that takeover is still the last change of holder: lender is to have it back */
static atomic_uint     version;
/* The remote's latest injections, CLOCK_REALTIME in us (the input devices' event clock), under mux. An event read
 * from a device is matched by the time the kernel gave it, not by when the GUI reads it: under load (an FT8 app
 * decoding) the GUI read the remote's keys more than INJECT_MS late and took them for the operator's, and the
 * remote lost control to "the radio's operator" with nobody at the radio (2026-11-23, both radios) */
static int64_t  inj_us[INJECT_RING];
static unsigned inj_n;

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000;
}

static int64_t realtime_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

void ctl_init(void) {
    memset(&owner, 0, sizeof(owner));
    memset(&lender, 0, sizeof(lender));
    lent = false;
}

bool ctl_transmitting(void) {
    return radio_get_state() != RADIO_RX;
}

/* Every change of holder forgets the lender: only ctl_claim's takeover names one, after it */
static void set_owner(ctl_kind_t kind, int id, const char *name) {
    lent = false;
    owner.kind = kind;
    owner.id   = id;
    snprintf(owner.name, sizeof(owner.name), "%s", name ? name : "");
    owner.since_ms = now_ms();
    atomic_fetch_add(&version, 1);
}

bool ctl_claim(ctl_kind_t kind, int id, const char *name, bool take) {
    bool ok;

    pthread_mutex_lock(&mux);
    if (owner.kind == kind && owner.id == id) {
        ok = true;
    } else if (owner.kind == CTL_NONE || (take && !ctl_transmitting())) {
        ctl_info_t was = owner;

        set_owner(kind, id, name);
        lender = was;
        lent   = true;
        ok = true;
    } else {
        ok = false;
    }
    pthread_mutex_unlock(&mux);
    return ok;
}

void ctl_release(ctl_kind_t kind, int id) {
    pthread_mutex_lock(&mux);
    if (owner.kind == kind && owner.id == id)
        set_owner(CTL_NONE, 0, "");
    else if (lent && lender.kind == kind && lender.id == id)
        memset(&lender, 0, sizeof(lender));     /* gone: it goes back free, as its leaving would have left it */
    pthread_mutex_unlock(&mux);
}

void ctl_give_back(ctl_kind_t kind, int id) {
    pthread_mutex_lock(&mux);
    if (owner.kind == kind && owner.id == id && lent) {
        ctl_info_t to = lender;

        set_owner(to.kind, to.id, to.name);     /* CTL_NONE: free again */
    }
    pthread_mutex_unlock(&mux);
}

bool ctl_holds(ctl_kind_t kind, int id) {
    pthread_mutex_lock(&mux);
    bool h = owner.kind == kind && owner.id == id;
    pthread_mutex_unlock(&mux);
    return h;
}

void ctl_get(ctl_info_t *out) {
    pthread_mutex_lock(&mux);
    *out = owner;
    pthread_mutex_unlock(&mux);
}

uint32_t ctl_version(void) {
    return atomic_load(&version);
}

void ctl_injected(void) {
    int64_t t = realtime_us();

    pthread_mutex_lock(&mux);
    inj_us[inj_n++ % INJECT_RING] = t;
    pthread_mutex_unlock(&mux);
}

/* Under mux: t (us) within INJECT_MS after one of the injections, or 5 ms before it (its time read before the
 * write) */
static bool injected_at(int64_t t) {
    for (unsigned i = 0; i < INJECT_RING; i++)
        if (inj_us[i] && t >= inj_us[i] - 5000 && t - inj_us[i] < INJECT_MS * 1000)
            return true;
    return false;
}

static void local_at(int64_t t) {
    pthread_mutex_lock(&mux);
    if (!injected_at(t) && owner.kind != CTL_LOCAL)
        set_owner(CTL_LOCAL, 0, "");
    pthread_mutex_unlock(&mux);
}

void ctl_local_input(void) {
    local_at(realtime_us());
}

void ctl_local_event(int64_t sec, int64_t usec) {
    local_at(sec * 1000000 + usec);
}
