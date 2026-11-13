/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - ADS-B decoding (see adsb.h), as GNS's AdsbTest app decodes its GNS 5890 stick (AdsbManager.java):
 *  DF17, DF18 (CF 0, 1) and DF19 (CF 0) with a good CRC; the call sign (TC 1-4), barometric altitude and airborne
 *  position (TC 9-18: even/odd CPR, globally from a pair or locally near the last position or the receiver), and
 *  speed, track and vertical rate (TC 19, subtypes 1 and 2). Positions as "The 1090 MHz Riddle" (Junzi Sun) gives
 *  them; a jump no aircraft could make is not taken.
 */

#include "adsb.h"

#include <math.h>
#include <pthread.h>
#include <string.h>
#include <time.h>

#define CPR_PAIR_S   10.0       /* even and odd frames further apart are no pair */
#define LOCAL_S      60.0       /* a position this recent is the reference of a one-frame decode */
#define REF_NM       180.0      /* a one-frame decode further than this from its reference is wrong */
#define TRAIL_NM     0.25       /* a track point after this much movement */
#define TRAIL_S      15.0       /* or this long */
#define MAX_KT       1200.0     /* faster than this: a bad position */

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static adsb_plane_t    planes[ADSB_MAX_PLANES];
static int             nplanes;
static adsb_stats_t    stats;
static bool            ref_ok;
static double          ref_lat, ref_lon;
static uint16_t        rate[ADSB_RATE_S];
static int64_t         rate_sec = -1;

static const char CHARS[] = "#ABCDEFGHIJKLMNOPQRSTUVWXYZ##### ###############0123456789######";

double adsb_now(void) {
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

void adsb_lock(void) { pthread_mutex_lock(&lock); }
void adsb_unlock(void) { pthread_mutex_unlock(&lock); }

void adsb_clear(void) {
    nplanes = 0;
    memset(&stats, 0, sizeof(stats));
    memset(rate, 0, sizeof(rate));
    rate_sec = -1;
}

void adsb_set_ref(bool ok, double lat, double lon) {
    ref_ok  = ok;
    ref_lat = lat;
    ref_lon = lon;
}

int adsb_count(void) { return nplanes; }
const adsb_plane_t *adsb_plane(int i) { return i >= 0 && i < nplanes ? &planes[i] : NULL; }
const adsb_stats_t *adsb_stats(void) { return &stats; }

uint32_t adsb_crc24(const uint8_t *data, size_t n) {
    uint32_t reg = 0;

    for (size_t i = 0; i < n; i++) {
        reg ^= (uint32_t)data[i] << 16;
        for (int b = 0; b < 8; b++) reg = (reg & 0x800000u) ? ((reg << 1) ^ 0xFFF409u) : (reg << 1);
        reg &= 0xFFFFFFu;
    }
    return reg;
}

double adsb_range_nm(double lat1, double lon1, double lat2, double lon2, double *bearing) {
    double p1 = lat1 * M_PI / 180, p2 = lat2 * M_PI / 180, dl = (lon2 - lon1) * M_PI / 180;
    double a = pow(sin((p2 - p1) / 2), 2) + cos(p1) * cos(p2) * pow(sin(dl / 2), 2);

    if (bearing) {
        double b = atan2(sin(dl) * cos(p2), cos(p1) * sin(p2) - sin(p1) * cos(p2) * cos(dl)) * 180 / M_PI;
        *bearing = fmod(b + 360.0, 360.0);
    }
    return 2 * 3440.065 * asin(fmin(1.0, sqrt(a)));
}

/* ---- Positions ---- */

/* Longitude zones at a latitude */
static int nl(double lat) {
    if (fabs(lat) >= 87.0) return 1;
    if (lat == 0) return 59;
    double a = 1 - cos(M_PI / 30), b = pow(cos(M_PI / 180 * fabs(lat)), 2);
    return (int)floor(2 * M_PI / acos(1 - a / b));
}

static double pmod(double x, double m) {
    double r = fmod(x, m);
    return r < 0 ? r + m : r;
}

/* From an even and an odd frame, the newest (0 even, 1 odd) giving the position */
static bool cpr_global(const adsb_plane_t *p, int newest, double *lat, double *lon) {
    double lat0 = p->cpr_lat[0] / 131072.0, lon0 = p->cpr_lon[0] / 131072.0;
    double lat1 = p->cpr_lat[1] / 131072.0, lon1 = p->cpr_lon[1] / 131072.0;
    double j = floor(59 * lat0 - 60 * lat1 + 0.5);
    double rlat0 = 360.0 / 60 * (pmod(j, 60) + lat0), rlat1 = 360.0 / 59 * (pmod(j, 59) + lat1);

    if (rlat0 >= 270) rlat0 -= 360;
    if (rlat1 >= 270) rlat1 -= 360;
    if (nl(rlat0) != nl(rlat1)) return false;      /* the pair straddles a zone edge: the next one will do */
    double la = newest ? rlat1 : rlat0;
    int    n  = nl(la) - newest;
    if (n < 1) n = 1;
    double m  = floor(lon0 * (nl(la) - 1) - lon1 * nl(la) + 0.5);
    double lo = 360.0 / n * (pmod(m, n) + (newest ? lon1 : lon0));
    if (lo >= 180) lo -= 360;
    *lat = la;
    *lon = lo;
    return true;
}

/* From one frame, near a reference */
static void cpr_local(uint32_t yz, uint32_t xz, int odd, double rlat, double rlon, double *lat, double *lon) {
    double lat_c = yz / 131072.0, lon_c = xz / 131072.0, dlat = 360.0 / (odd ? 59 : 60);
    double j = floor(rlat / dlat) + floor(0.5 + pmod(rlat, dlat) / dlat - lat_c);

    *lat = dlat * (j + lat_c);
    int n = nl(*lat) - odd;
    double dlon = 360.0 / (n < 1 ? 1 : n);
    double m    = floor(rlon / dlon) + floor(0.5 + pmod(rlon, dlon) / dlon - lon_c);
    *lon = dlon * (m + lon_c);
}

static void trail_add(adsb_plane_t *p, double now) {
    if (p->trail_n) {
        const adsb_point_t *last = &p->trail[(p->trail_head + ADSB_TRAIL - 1) % ADSB_TRAIL];
        if (now - last->t < TRAIL_S && adsb_range_nm(last->lat, last->lon, p->lat, p->lon, NULL) < TRAIL_NM) return;
    }
    adsb_point_t *pt = &p->trail[p->trail_head];
    pt->lat = (float)p->lat;
    pt->lon = (float)p->lon;
    pt->alt = p->alt_ok ? p->alt : INT32_MIN;
    pt->t   = now;
    p->trail_head = (p->trail_head + 1) % ADSB_TRAIL;
    if (p->trail_n < ADSB_TRAIL) p->trail_n++;
}

static void position(adsb_plane_t *p, int odd, double now) {
    double lat, lon;
    bool   got = false;

    if (p->cpr_t[odd ^ 1] > 0 && fabs(p->cpr_t[odd] - p->cpr_t[odd ^ 1]) <= CPR_PAIR_S)
        got = cpr_global(p, odd, &lat, &lon);
    if (!got) {                                 /* one frame: near the last position, or near the receiver */
        double rlat, rlon;
        if (p->pos_ok && now - p->pos_t < LOCAL_S) {
            rlat = p->lat;
            rlon = p->lon;
        } else if (ref_ok) {
            rlat = ref_lat;
            rlon = ref_lon;
        } else {
            return;
        }
        cpr_local(p->cpr_lat[odd], p->cpr_lon[odd], odd, rlat, rlon, &lat, &lon);
        if (adsb_range_nm(rlat, rlon, lat, lon, NULL) > REF_NM) return;
    }
    if (p->pos_ok) {                            /* a jump no aircraft makes: a bad pair */
        double dt = now - p->pos_t, d = adsb_range_nm(p->lat, p->lon, lat, lon, NULL);
        if (d > 5.0 + MAX_KT / 3600.0 * dt) return;
    }
    p->lat    = lat;
    p->lon    = lon;
    p->pos_ok = true;
    p->pos_t  = now;
    trail_add(p, now);
}

/* ---- Messages ---- */

static adsb_plane_t *plane_of(uint32_t icao, double now) {
    for (int i = 0; i < nplanes; i++)
        if (planes[i].icao == icao) return &planes[i];
    int k = nplanes;
    if (nplanes == ADSB_MAX_PLANES) {           /* full: the one heard longest ago goes */
        k = 0;
        for (int i = 1; i < nplanes; i++)
            if (planes[i].seen < planes[k].seen) k = i;
    } else {
        nplanes++;
    }
    memset(&planes[k], 0, sizeof(planes[k]));
    planes[k].icao  = icao;
    planes[k].first = now;
    return &planes[k];
}

static void count_rate(double now) {
    int64_t s = (int64_t)floor(now);

    if (rate_sec < 0 || s - rate_sec >= ADSB_RATE_S) {
        memset(rate, 0, sizeof(rate));
    } else {
        for (int64_t k = rate_sec + 1; k <= s; k++) rate[k % ADSB_RATE_S] = 0;
    }
    if (s > rate_sec) rate_sec = s;
    if (rate[s % ADSB_RATE_S] < UINT16_MAX) rate[s % ADSB_RATE_S]++;
}

void adsb_rate(double now, uint16_t *out, int n) {
    int64_t s = (int64_t)floor(now);

    if (n > ADSB_RATE_S) n = ADSB_RATE_S;
    for (int i = 0; i < n; i++) {
        int64_t k = s - (n - 1 - i);
        out[i] = rate_sec >= 0 && k <= rate_sec && rate_sec - k < ADSB_RATE_S ? rate[k % ADSB_RATE_S] : 0;
    }
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

bool adsb_frame_hex(const char *hex, size_t n, double now) {
    uint8_t msg[14];

    if (n != 14 && n != 28) {
        stats.junk++;
        return false;
    }
    for (size_t i = 0; i < n / 2; i++) {
        int hi = hexval(hex[2 * i]), lo = hexval(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            stats.junk++;
            return false;
        }
        msg[i] = (uint8_t)(hi << 4 | lo);
    }
    stats.frames++;
    if (n == 14) {                              /* short Mode-S: the parity holds the address, not decoded */
        stats.shorts++;
        return false;
    }
    stats.longs++;
    int df = msg[0] >> 3, cf = msg[0] & 7;
    if (!(df == 17 || (df == 18 && cf <= 1) || (df == 19 && cf == 0))) {
        stats.other_df++;
        return false;
    }
    uint32_t syn = adsb_crc24(msg, 14);
    if (syn != 0) {                             /* one bit wrong (not in DF): put right, as dump1090 does */
        static uint32_t table[112];
        static bool     made;
        int             bit = -1;
        if (!made) {
            for (int i = 0; i < 112; i++) {
                uint8_t m[14] = {0};
                m[i / 8] = (uint8_t)(0x80 >> (i % 8));
                table[i] = adsb_crc24(m, 14);
            }
            made = true;
        }
        for (int i = 5; i < 112 && bit < 0; i++)
            if (table[i] == syn) bit = i;
        if (bit < 0) {
            stats.crc_bad++;
            return false;
        }
        msg[bit / 8] ^= (uint8_t)(0x80 >> (bit % 8));
        stats.fixed++;
    }
    stats.adsb++;
    count_rate(now);

    adsb_plane_t *p = plane_of((uint32_t)msg[1] << 16 | (uint32_t)msg[2] << 8 | msg[3], now);
    p->msgs++;
    p->seen = now;

    uint64_t me = 0;
    for (int i = 4; i < 11; i++) me = me << 8 | msg[i];
    int tc = (int)(me >> 51);

    if (tc >= 1 && tc <= 4) {
        char c[9];
        int  k = 0;
        for (int i = 0; i < 8; i++) {
            char ch = CHARS[(me >> (42 - 6 * i)) & 0x3F];
            if (ch != '#' && ch != ' ') c[k++] = ch;
        }
        c[k] = 0;
        if (k) memcpy(p->call, c, sizeof(c));
    } else if (tc >= 9 && tc <= 18) {
        int ac = (int)((me >> 36) & 0xFFF);
        if (ac && (ac & 0x10)) {                /* Q = 1: 25 ft steps (Q = 0, Gillham, is rare and left out) */
            int n25 = ((ac & 0xFE0) >> 1) | (ac & 0x0F);
            p->alt    = n25 * 25 - 1000;
            p->alt_ok = true;
        }
        int odd = (int)((me >> 34) & 1);
        p->cpr_lat[odd] = (uint32_t)((me >> 17) & 0x1FFFF);
        p->cpr_lon[odd] = (uint32_t)(me & 0x1FFFF);
        p->cpr_t[odd]   = now;
        position(p, odd, now);
    } else if (tc == 19) {
        int st = (int)((me >> 48) & 7);
        if (st == 1 || st == 2) {
            int k  = st == 2 ? 4 : 1;           /* supersonic: 4 kt steps */
            int ew = (int)((me >> 32) & 0x3FF), ns = (int)((me >> 21) & 0x3FF);
            if (ew && ns) {
                double vx = (ew - 1) * k * (((me >> 42) & 1) ? -1 : 1);
                double vy = (ns - 1) * k * (((me >> 31) & 1) ? -1 : 1);
                p->gs     = (float)hypot(vx, vy);
                p->trk    = (float)fmod(atan2(vx, vy) * 180 / M_PI + 360.0, 360.0);
                p->vel_ok = true;
            }
            int vr = (int)((me >> 10) & 0x1FF);
            if (vr) {
                p->vr    = (vr - 1) * 64 * (((me >> 19) & 1) ? -1 : 1);
                p->vr_ok = true;
            }
        }
    }
    return true;
}

void adsb_expire(double now, double keep_s, double trail_s) {
    int k = 0;

    for (int i = 0; i < nplanes; i++) {
        adsb_plane_t *p = &planes[i];
        if (keep_s >= 0 && now - p->seen > keep_s) continue;
        if (trail_s >= 0) {                     /* the oldest points past trail_s go */
            while (p->trail_n) {
                int oldest = (p->trail_head + ADSB_TRAIL - p->trail_n) % ADSB_TRAIL;
                if (now - p->trail[oldest].t <= trail_s) break;
                p->trail_n--;
            }
        }
        if (k != i) planes[k] = *p;
        k++;
    }
    nplanes = k;
}
