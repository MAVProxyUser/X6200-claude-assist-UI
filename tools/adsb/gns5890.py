#!/usr/bin/env python3
"""Reads a GNS 5890 ADS-B stick (04d8:f8e8) and lists the aircraft it hears. Receive only, Python's standard library
only, so it runs on the X6200 itself.

The stick is a CDC-ACM serial device that streams Mode-S frames as ASCII hex, "*8D4840D6202CC371C32CE0576098;"
(112 bits) or "*5D4840D6F3A1B2;" (56 bits), with no command needed to start (GNS's AdsbTest app, AdsbUsbDriver.java,
only reads). It names itself vendor-specific (device class ff), so Linux's cdc_acm does not take it by its interfaces;
until it does, tell cdc_acm its ID, once per boot or plug:

    echo "04d8 f8e8" > /sys/bus/usb/drivers/cdc_acm/new_id      # then /dev/ttyACM0

Decoded, as in AdsbManager.java: DF17, DF18 (CF 0, 1) and DF19 (CF 0) with a good CRC: the call sign (TC 1-4),
barometric altitude and position (TC 9-18, even/odd CPR, globally from a pair or locally near the receiver), and
speed, track and vertical rate (TC 19). Range and bearing are from --lat/--lon (default the centre of EN80kc).

    gns5890.py [--dev /dev/ttyACM0] [--seconds 60] [--every 5] [--raw] [--lat 40.104 --lon -83.125]
"""
import argparse
import math
import os
import select
import sys
import termios
import time

CHARS = "#ABCDEFGHIJKLMNOPQRSTUVWXYZ##### ###############0123456789######"
POLY = 0xFFF409             # Mode-S CRC-24 generator
CPR_MAX_S = 10.0            # even and odd frames further apart than this are not paired
AGE_OUT_S = 60.0


def crc24(data):
    """Remainder of the Mode-S CRC over all bits of data; 0 for a good DF17/18 (parity in the last 24 bits)."""
    reg = 0
    for byte in data:
        reg ^= byte << 16
        for _ in range(8):
            reg = ((reg << 1) ^ POLY) if reg & 0x800000 else (reg << 1)
            reg &= 0xFFFFFF
    return reg


def nl(lat):
    """Number of longitude zones at a latitude (1090-WP-14-09)."""
    if abs(lat) >= 87.0:
        return 1
    if lat == 0:
        return 59
    a = 1 - math.cos(math.pi / 30)
    b = math.cos(math.pi / 180 * abs(lat)) ** 2
    return int(math.floor(2 * math.pi / math.acos(1 - a / b)))


def cpr_global(even, odd, newest):
    """Airborne position from an even and an odd frame (lat_cpr, lon_cpr as 17-bit fractions); newest 0 or 1."""
    lat0, lon0 = even[0] / 131072.0, even[1] / 131072.0
    lat1, lon1 = odd[0] / 131072.0, odd[1] / 131072.0
    j = math.floor(59 * lat0 - 60 * lat1 + 0.5)
    rlat0 = 360.0 / 60 * ((j % 60) + lat0)
    rlat1 = 360.0 / 59 * ((j % 59) + lat1)
    if rlat0 >= 270:
        rlat0 -= 360
    if rlat1 >= 270:
        rlat1 -= 360
    if nl(rlat0) != nl(rlat1):
        return None                             # the pair straddles a zone edge: wait for the next one
    lat = rlat1 if newest else rlat0
    n = max(nl(lat) - newest, 1)
    m = math.floor(lon0 * (nl(lat) - 1) - lon1 * nl(lat) + 0.5)
    lon = 360.0 / n * ((m % n) + (lon1 if newest else lon0))
    if lon >= 180:
        lon -= 360
    return lat, lon


def cpr_local(frame, odd, ref_lat, ref_lon):
    """Airborne position from one frame near a reference (within ~180 NM)."""
    lat_c, lon_c = frame[0] / 131072.0, frame[1] / 131072.0
    dlat = 360.0 / (59 if odd else 60)
    j = math.floor(ref_lat / dlat) + math.floor(0.5 + (ref_lat % dlat) / dlat - lat_c)
    lat = dlat * (j + lat_c)
    dlon = 360.0 / max(nl(lat) - odd, 1)
    m = math.floor(ref_lon / dlon) + math.floor(0.5 + (ref_lon % dlon) / dlon - lon_c)
    return lat, dlon * (m + lon_c)


def range_bearing(lat1, lon1, lat2, lon2):
    """Great-circle distance (NM) and initial bearing (degrees) from 1 to 2."""
    p1, p2 = math.radians(lat1), math.radians(lat2)
    dl = math.radians(lon2 - lon1)
    a = math.sin((p2 - p1) / 2) ** 2 + math.cos(p1) * math.cos(p2) * math.sin(dl / 2) ** 2
    d = 2 * 3440.065 * math.asin(min(1.0, math.sqrt(a)))
    y = math.sin(dl) * math.cos(p2)
    x = math.cos(p1) * math.sin(p2) - math.sin(p1) * math.cos(p2) * math.cos(dl)
    return d, (math.degrees(math.atan2(y, x)) + 360) % 360


class Plane:
    def __init__(self, icao):
        self.icao = icao
        self.call = ""
        self.alt = None
        self.gs = self.trk = self.vr = None
        self.lat = self.lon = None
        self.cpr = [None, None]                 # (lat_cpr, lon_cpr, time) even, odd
        self.pos_time = 0.0
        self.msgs = 0
        self.seen = 0.0


class Decoder:
    def __init__(self, ref_lat, ref_lon):
        self.ref = (ref_lat, ref_lon)
        self.planes = {}
        self.stats = {"frames": 0, "short": 0, "long": 0, "crc_bad": 0, "adsb": 0, "other_df": 0, "junk": 0}

    def frame(self, hexstr, now):
        try:
            msg = bytes.fromhex(hexstr)
        except ValueError:
            self.stats["junk"] += 1
            return None
        self.stats["frames"] += 1
        if len(msg) == 7:
            self.stats["short"] += 1            # Mode-S short (DF0, 4, 5, 11): parity holds the address, not decoded
            return None
        if len(msg) != 14:
            self.stats["junk"] += 1
            return None
        self.stats["long"] += 1
        df, cf = msg[0] >> 3, msg[0] & 7
        if not (df == 17 or (df == 18 and cf in (0, 1)) or (df == 19 and cf == 0)):
            self.stats["other_df"] += 1         # DF20/21 (Comm-B) and the like: address in the parity
            return None
        if crc24(msg) != 0:
            self.stats["crc_bad"] += 1
            return None
        self.stats["adsb"] += 1
        icao = "%06X" % int.from_bytes(msg[1:4], "big")
        p = self.planes.get(icao) or self.planes.setdefault(icao, Plane(icao))
        p.msgs += 1
        p.seen = now
        me = int.from_bytes(msg[4:11], "big")
        tc = me >> 51
        if 1 <= tc <= 4:
            p.call = "".join(CHARS[(me >> (42 - 6 * i)) & 0x3F] for i in range(8)).replace("#", "").strip()
        elif 9 <= tc <= 18:
            ac = (me >> 36) & 0xFFF
            if ac and ac & 0x10:                # Q = 1: 25 ft steps
                n = ((ac & 0xFE0) >> 1) | (ac & 0x0F)
                p.alt = n * 25 - 1000
            odd = (me >> 34) & 1
            p.cpr[odd] = ((me >> 17) & 0x1FFFF, me & 0x1FFFF, now)
            self.position(p, odd, now)
        elif tc == 19:
            st = (me >> 48) & 7
            if st in (1, 2):
                k = 4 if st == 2 else 1             # supersonic: 4 kt steps
                ew, ns = (me >> 32) & 0x3FF, (me >> 21) & 0x3FF
                if ew and ns:
                    vx = (ew - 1) * k * (-1 if (me >> 42) & 1 else 1)
                    vy = (ns - 1) * k * (-1 if (me >> 31) & 1 else 1)
                    p.gs = math.hypot(vx, vy)
                    p.trk = (math.degrees(math.atan2(vx, vy)) + 360) % 360
                vr = (me >> 10) & 0x1FF
                if vr:
                    p.vr = (vr - 1) * 64 * (-1 if (me >> 19) & 1 else 1)
        return icao, tc, msg.hex().upper()

    def position(self, p, odd, now):
        other = p.cpr[odd ^ 1]
        this = p.cpr[odd]
        pos = None
        if other and abs(this[2] - other[2]) <= CPR_MAX_S:
            even_f, odd_f = (this, other) if odd == 0 else (other, this)
            pos = cpr_global(even_f, odd_f, odd)
        if pos is None:                         # one frame: near the last position, or near the receiver
            ref = (p.lat, p.lon) if p.lat is not None and now - p.pos_time < CPR_MAX_S else self.ref
            pos = cpr_local(this, odd, *ref)
            d, _ = range_bearing(ref[0], ref[1], pos[0], pos[1])
            if d > 180:
                return
        p.lat, p.lon = pos
        p.pos_time = now


def open_stick(dev):
    fd = os.open(dev, os.O_RDONLY | os.O_NOCTTY | os.O_NONBLOCK)
    attr = termios.tcgetattr(fd)
    attr[0] = 0                                                     # iflag: raw
    attr[1] = 0                                                     # oflag
    attr[2] = termios.CS8 | termios.CREAD | termios.CLOCAL          # cflag
    attr[3] = 0                                                     # lflag: no echo, no canonical lines
    attr[4] = attr[5] = termios.B115200                             # the CDC device ignores the rate
    termios.tcsetattr(fd, termios.TCSANOW, attr)
    termios.tcflush(fd, termios.TCIFLUSH)
    return fd


def table(dec, now, out):
    live = [p for p in dec.planes.values() if now - p.seen <= AGE_OUT_S]
    s = dec.stats
    out.write("\n%s  %d aircraft, %d frames (%d long, %d short), %d ADS-B, %d CRC bad, %d other DF\n"
              % (time.strftime("%H:%M:%S", time.gmtime(now)), len(live), s["frames"], s["long"], s["short"],
                 s["adsb"], s["crc_bad"], s["other_df"]))
    out.write("ICAO    CALL      ALT ft   GS kt  TRK  VR fpm   LAT       LON        RNG nm  BRG  MSGS  AGE\n")

    def key(p):
        if p.lat is None:
            return (1, 0)
        return (0, range_bearing(dec.ref[0], dec.ref[1], p.lat, p.lon)[0])

    for p in sorted(live, key=key):
        rng = brg = None
        if p.lat is not None:
            rng, brg = range_bearing(dec.ref[0], dec.ref[1], p.lat, p.lon)
        out.write("%-7s %-8s %7s  %6s  %3s  %6s  %8s  %9s  %6s  %3s  %4d  %3.0f\n" % (
            p.icao, p.call or "-",
            "%d" % p.alt if p.alt is not None else "-",
            "%.0f" % p.gs if p.gs is not None else "-",
            "%03.0f" % p.trk if p.trk is not None else "-",
            "%+d" % p.vr if p.vr is not None else "-",
            "%.4f" % p.lat if p.lat is not None else "-",
            "%.4f" % p.lon if p.lon is not None else "-",
            "%.1f" % rng if rng is not None else "-",
            "%03.0f" % brg if brg is not None else "-",
            p.msgs, now - p.seen))
    out.flush()


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--dev", default="/dev/ttyACM0")
    ap.add_argument("--seconds", type=float, default=60.0, help="how long to listen, 0 for ever")
    ap.add_argument("--every", type=float, default=5.0, help="seconds between tables")
    ap.add_argument("--raw", action="store_true", help="print every frame as it comes")
    ap.add_argument("--lat", type=float, default=40.104)            # EN80kc
    ap.add_argument("--lon", type=float, default=-83.125)
    a = ap.parse_args()

    fd = open_stick(a.dev)
    dec = Decoder(a.lat, a.lon)
    buf, inside, nbytes = "", False, 0
    start = last = time.time()
    try:
        while not a.seconds or time.time() - start < a.seconds:
            r, _, _ = select.select([fd], [], [], 0.5)
            now = time.time()
            if r:
                chunk = os.read(fd, 4096)
                if not chunk:
                    sys.exit("the stick went away")
                nbytes += len(chunk)
                for ch in chunk.decode("ascii", "replace"):
                    if ch in "*@":
                        buf, inside = "", True
                    elif ch == ";" and inside:
                        inside = False
                        hexstr = buf[12:] if len(buf) in (26, 40) else buf     # "@": a 48-bit time stamp first
                        got = dec.frame(hexstr, now)
                        if a.raw:
                            print("%s %s%s" % (time.strftime("%H:%M:%S"), hexstr,
                                               "  %s TC%d" % (got[0], got[1]) if got else ""), flush=True)
                    elif inside:
                        if ch in "0123456789ABCDEFabcdef":
                            buf += ch
                        elif ch not in "\r\n":
                            inside = False
                            dec.stats["junk"] += 1
            if now - last >= a.every:
                last = now
                if not a.raw:
                    table(dec, now, sys.stdout)
                elif not nbytes:
                    print("%s nothing from the stick yet" % time.strftime("%H:%M:%S"), flush=True)
    except KeyboardInterrupt:
        pass
    finally:
        os.close(fd)
    table(dec, time.time(), sys.stdout)
    print("%d bytes from the stick" % nbytes)


if __name__ == "__main__":
    main()
