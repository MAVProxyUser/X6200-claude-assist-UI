/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - what the receivers' backends (sdr.h) share: a device's USB IDs looked for, signed samples made the
 *  RTL's unsigned ones with their DC taken out.
 */

#include "sdr.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>

static bool read_id(const char *dev, const char *what, char *out, size_t n) {
    char  path[300];
    FILE *f;

    snprintf(path, sizeof(path), "/sys/bus/usb/devices/%s/%s", dev, what);
    if (!(f = fopen(path, "r"))) return false;
    bool ok = fgets(out, (int)n, f) != NULL;
    fclose(f);
    if (ok) out[strcspn(out, "\r\n")] = 0;
    return ok;
}

bool sdr_usb_has(const char *vid, const char *pid) {
    DIR           *d = opendir("/sys/bus/usb/devices");
    struct dirent *e;
    bool           found = false;

    if (!d) return false;
    while (!found && (e = readdir(d))) {
        char v[8], p[8];

        if (e->d_name[0] == '.' || strchr(e->d_name, ':')) continue;     /* interfaces have a ':' */
        found = read_id(e->d_name, "idVendor", v, sizeof(v)) && read_id(e->d_name, "idProduct", p, sizeof(p)) &&
                !strcmp(v, vid) && !strcmp(p, pid);
    }
    closedir(d);
    return found;
}

/* Each component: the DC (in 1/256 of a step) taken off, 127.5 added (the middle of the RTL's scale, as dump1090 reads
 * it), rounded, and what the rounding left carried to the next sample: on average nothing of the DC stays, and the
 * rounding's noise goes to the edges of the picture, far under the receiver's own */
void sdr_s8_to_u8(sdr_dc_t *st, uint8_t *buf, uint32_t n) {
    int64_t  si = 0, sq = 0;
    int32_t  di = st->dc_i, dq = st->dc_q, ei = st->err_i, eq = st->err_q;
    uint32_t pairs = n / 2;

    for (uint32_t k = 0; k < pairs; k++) {
        int32_t i = (int8_t)buf[2 * k], q = (int8_t)buf[2 * k + 1];
        int32_t vi = i * 256 - di + 32640 + ei, vq = q * 256 - dq + 32640 + eq;
        int32_t oi = (vi + 128) >> 8, oq = (vq + 128) >> 8;

        si += i;
        sq += q;
        ei = vi - oi * 256;
        eq = vq - oq * 256;
        buf[2 * k]     = (uint8_t)(oi < 0 ? 0 : oi > 255 ? 255 : oi);
        buf[2 * k + 1] = (uint8_t)(oq < 0 ? 0 : oq > 255 ? 255 : oq);
    }
    if (pairs) {                        /* the next transfers' DC: this one's mean, followed over about eight */
        int32_t mi = (int32_t)(si * 256 / (int64_t)pairs), mq = (int32_t)(sq * 256 / (int64_t)pairs);

        if (!st->primed) {
            di = mi;
            dq = mq;
            st->primed = true;
        } else {
            di += (mi - di) / 8;
            dq += (mq - dq) / 8;
        }
    }
    st->dc_i  = di;
    st->dc_q  = dq;
    st->err_i = ei;
    st->err_q = eq;
}
