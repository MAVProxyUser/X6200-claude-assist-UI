#!/usr/bin/env python3
"""
The X6100's base, Xiegu's V1.1.9 (251127001) with GoldCorpHF's fixes, made from Xiegu's own file: decrypted with
Xiegu's key, a few instructions changed where the original bytes are exactly as expected, the version's time field
set to the build's name, encrypted again and checked both ways. Nothing of Xiegu's is in this repository: their
.xgf (on the radio's internal memory, /usr/firmware, or Xiegu's V1.2.0 update) and the key (public in
fventuri/x6100-bbfw common/build_common.sh) are given on the command line.

    x6100_patch.py --xgf X6100_BBFW_V1.1.9_251127001.xgf --key-file KEY --out X6100_BBFW_V1.1.9_251127001_gcfix3.xgf \
                   --patch-out rootfs/usr/share/x6100/base_patches/x6100_V1.1.9_gcfix3.json

The radios never get the key nor a base: --patch-out writes the changed 16-byte blocks of the encrypted file
(Xiegu's and ours, AES-ECB encrypting each block alone) with the SHA-256 and MD5 of Xiegu's file and of the result,
and x6x00-base-patch (rootfs/usr/sbin) applies it on the radio to Xiegu's own file.

The fixes (tools/base_update/README.md "Every base compared", 2026-11-20):

  pll      The codec's PLL (TLV320AIC3204), which clocks the I/Q converter: Xiegu computes P=4 J=15 D=7538 for
           100 kHz but writes the constant 8 into register 8 (D's low byte) and D's low byte into register 9, so
           D is 7432 and the base samples at about 99,932 Hz (-676 ppm). The constant's load becomes the load of D,
           as two lines further on: register 8 gets D's low byte (register 9 gets it too, as before).
  6m       The TX power loop on 6 m: Xiegu's loop only sets a target in dBm and integrates the drive's gain within
           -40..0 dB, then adds +6 dB from band index 20 (29.7 MHz) up. The cap at 0 dB comes after that addition, so
           the +6 dB never raised the most drive; it only lifted 6 m's least from -40 to -34 dB: the owner's X6100
           gave 0.8 W at 0.1, 0.3 and 0.5 W in FM (2026-11-20, through 70 dB of pads). The instructions that choose
           +6 dB become NOPs: 6 m's loop has HF's range; HF is untouched.
  alc      The loop's floor: both -40 dB limits (the integrator's clamp, and the floor test after the band's offset)
           become -60 dB; the start after a band, mode or power change stays -40 dB, as Xiegu has it. With -40 the
           0.1 W setting gave 0.2-0.3 W on HF and 0.2 W on 6 m (FM, 2026-11-20); with -60, 0.1 W on every band.
           (An SSB sweep that lost its tones for seconds was first blamed on this; the same loss came on Xiegu's own
           base: the X6100's TCI transmit stream, not the floor.)
The version text becomes "V1.1.9 Nov 27 2025,gcfix3": no ",rN", so OpenX6X00 and the X6100 library read it as
Xiegu's 1.1.9 with all of its features, and the GUI's Base line says which build it is.
SPDX-License-Identifier: LGPL-2.1-or-later
"""
import argparse, hashlib, json, os, subprocess, sys, tempfile

BASE = 0x08020000
XGF_SHA256 = "87a3e96ea21abf16a14b0fb771aad19a9e0bf8c267f93ec71a09d38e59c25737"     # Xiegu's V1.1.9 .xgf
PLAIN_SHA256 = "3fa11293e06951c21c9cfda04ea08adad85eaadda26b77a8e8bbcca8820c53c4"   # the same, decrypted
NAME = b"gcfix3"

# (address, original bytes, new bytes, what): each applied only where the original is exactly there
PATCHES = {
    "pll": [(0x0802f3d8, bytes.fromhex("0823"), bytes.fromhex("3b6c"),
             "movs r3,#8 -> ldr r3,[r7,#0x40]: register 8 gets D's low byte")],
    "alc": [(0x08027f90, bytes.fromhex("ccf22023"), bytes.fromhex("ccf27023"),
             "movt r3,#0xc220 -> #0xc270: the loop's clamp -40.0 -> -60.0 dB"),
            (0x08027fb6, bytes.fromhex("ccf22023"), bytes.fromhex("ccf27023"),
             "movt r3,#0xc220 -> #0xc270: the floor after the band's offset -40.0 -> -60.0 dB")],
    "6m": [(0x080246aa, bytes.fromhex("28bff0ee667a"), bytes.fromhex("00bf00bf00bf"),
            "it hs; vmovhs.f32 s15,s13 -> nop x3: no +6 dB on the loop's gain from band index 20 (29.7 MHz) up")],
}
TIME_FIELD = (0x0803d3b0, b"10:31:57\x00\x00\x00\x00")       # 12 bytes: up to 11 characters and the NUL


def aes(key_hex, data, decrypt):
    with tempfile.TemporaryDirectory() as d:
        i, o = os.path.join(d, "i"), os.path.join(d, "o")
        open(i, "wb").write(data)
        subprocess.run(["openssl", "enc", "-d" if decrypt else "-e", "-aes-256-ecb", "-nopad", "-K", key_hex,
                        "-in", i, "-out", o], check=True)
        return open(o, "rb").read()


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    ap.add_argument("--xgf", required=True, help="Xiegu's X6100_BBFW_V1.1.9_251127001.xgf")
    ap.add_argument("--key-file", required=True, help="Xiegu's AES-256 key, 64 hex digits")
    ap.add_argument("--out", required=True)
    ap.add_argument("--only", default="pll,6m,alc", help="the fixes to apply (default: all)")
    ap.add_argument("--patch-out", help="also write the block patch the radios apply (x6x00-base-patch)")
    a = ap.parse_args()

    xgf = open(a.xgf, "rb").read()
    if hashlib.sha256(xgf).hexdigest() != XGF_SHA256:
        sys.exit("not Xiegu's V1.1.9 (251127001) .xgf: its SHA-256 differs")
    key = open(a.key_file).read().strip()
    if len(key) != 64 or any(ch not in "0123456789abcdefABCDEF" for ch in key):
        sys.exit("the key file must hold 64 hex digits")
    plain = bytearray(aes(key, xgf, True))
    if hashlib.sha256(plain).hexdigest() != PLAIN_SHA256:
        sys.exit("decrypted, it is not Xiegu's V1.1.9 image: a wrong key?")

    for fix in a.only.split(","):
        for addr, old, new, what in PATCHES[fix]:
            o = addr - BASE
            if bytes(plain[o:o + len(old)]) != old:
                sys.exit(f"{fix} at {addr:#x}: not the expected bytes")
            plain[o:o + len(new)] = new
            print(f"{fix:4s} {addr:#010x} {old.hex()} -> {new.hex()}  {what}")
    o = TIME_FIELD[0] - BASE
    if bytes(plain[o:o + len(TIME_FIELD[1])]) != TIME_FIELD[1]:
        sys.exit("the version's time field is not where it should be")
    plain[o:o + len(TIME_FIELD[1])] = NAME + bytes(len(TIME_FIELD[1]) - len(NAME))
    print(f"name {TIME_FIELD[0]:#010x} 10:31:57 -> {NAME.decode()}")

    out = aes(key, bytes(plain), False)
    if aes(key, out, True) != bytes(plain) or len(out) != len(xgf):
        sys.exit("encrypted again, it does not decrypt to the patched image")
    changed = sum(1 for x, y in zip(xgf, out) if x != y)
    open(a.out, "wb").write(out)
    print(f"{a.out}: {len(out)} bytes, sha256 {hashlib.sha256(out).hexdigest()}, "
          f"plain sha256 {hashlib.sha256(plain).hexdigest()}, {changed} encrypted bytes differ from Xiegu's")

    if a.patch_out:
        # The patch the radios get (x6x00-base-patch): AES-ECB encrypts each 16-byte block alone, so the changed
        # blocks, Xiegu's and ours, are all it takes; no key and no readable code of Xiegu's in it
        blocks = [{"offset": o, "from": xgf[o:o + 16].hex(), "to": out[o:o + 16].hex()}
                  for o in range(0, len(out), 16) if xgf[o:o + 16] != out[o:o + 16]]
        fixes = [f for f in a.only.split(",")]
        doc = {
            "format": "x6x00-base-patch 1",
            "name": NAME.decode(),
            "model": "x6100",
            "about": "Xiegu's X6100 base V1.1.9 (251127001) with GoldCorpHF's fixes: " + ", ".join(
                {"pll": "the codec's PLL fraction written as Xiegu meant it (the I/Q rate 100 kHz, not 99,932 Hz)",
                 "6m": "no +6 dB on the TX power loop from 29.7 MHz up",
                 "alc": "the TX power loop's floor -60 dB, not -40 (0.1 W reached on every band; 6 m gave 0.8 W, HF 0.2-0.3)"}[f]
                for f in fixes),
            "fixes": fixes,
            "source": {"file": "X6100_BBFW_V1.1.9_251127001.xgf", "size": len(xgf),
                       "sha256": hashlib.sha256(xgf).hexdigest(), "md5": hashlib.md5(xgf).hexdigest(),
                       "version": "V1.1.9 Nov 27 2025,10:31:57"},
            "result": {"file": f"X6100_BBFW_V1.1.9_251127001_{NAME.decode()}.xgf", "size": len(out),
                       "sha256": hashlib.sha256(out).hexdigest(), "md5": hashlib.md5(out).hexdigest(),
                       "version": f"V1.1.9 Nov 27 2025,{NAME.decode()}"},
            "blocks": blocks,
        }
        with open(a.patch_out, "w") as f:
            json.dump(doc, f, indent=1)
            f.write("\n")
        print(f"{a.patch_out}: {len(blocks)} blocks of 16 bytes")


if __name__ == "__main__":
    main()
