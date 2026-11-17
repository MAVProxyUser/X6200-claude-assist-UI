# Five KrakenSDR streams on the X6200: the hub polled (kernel patch 9104)

Status 2026-11-17: **tested on the radio as a hot-installed module, not in any image**. Patch:
`buildroot/br2_external/board/X6100/patches/linux/9104-usb-musb-host-poll-a-hub-over-ep0-instead-of-its-status-endpoint.patch`
(on 9101/9102). 9103 (turns on the shared ep1) is withdrawn: it is in this branch's history (3b106ec) only.

## Why the fifth stream got nothing

The R16's MUSB (sun8i-a33-musb, PIO) has five receive endpoints, ep1-ep5. ep1 is `musb->bulk_ep`, used only by bulk-in
queues that find ep2-ep5 taken, and it changes queues only after 16 ms of NAKs or when a queue empties. The Kraken's
hub (SMSC USB2517, 0424:2517) keeps its status-change URB (interrupt in, 1 byte, 256 ms) queued for good, so it holds
one of ep2-ep5. Three sticks got the other three, the fourth and fifth shared ep1, and the one second on it never
got a turn. usbcore is built in (CONFIG_USB=y), so a hub.c quirk would need a new kernel image. 9104 does the same
inside musb_hdrc.ko: the hub's status URB is linked but never put on the hardware, and a timer gives it back every
`hub_poll_ms` (1000) with every port marked changed. hub_event then reads the hub's and each port's status over ep0.
The module parameters are `hub_poll_ms` (0 = off, at the hub's next status URB) and `hub_poll_id` (0x04242517).

## Measured on the radio (GUI up throughout)

- **Five streams:** B's daemon on 1004 (2.1168 MS/s) plus `df_rate` on 1000-1003 (1.024 MS/s):
  - 60 s: 100.0 / 100.1 / 100.1 / 100.0 % (25.8k musb IRQ/s), with no "share ep1" notice.
  - 219 s of a 30-min run, all full, until the user switched the radio off at 17:01:49 UTC (a clean power-off).
  - 81 s of a 20-min run, all full, stopped for another test.
  - **20 min (17:14:45-17:34:46 UTC): 100.0 % on all four, every one of the 1,200 seconds full** (none at 0, none
    under 900,000). B streamed throughout: the controller averaged 24,931 musb IRQ/s. The hub saw 8.7 URBs/s (the
    polling). Nothing in a live `dmesg -w` for the whole run, and the SoC was at 53.9 C at the end.
  - Nothing in dmesg except the usual "Rx interrupt with no errors or packet!" at teardown. The stock module prints
    those too, one per stick.
- **The hub's status channel** (`hub_poll_ms` 0, the URB on the hardware): the hub's `urbnum` stayed at 312 through a
  5-min capture of B + 3 sticks, all 100.0 %. It never completes in a clean run: the controller polls it every
  256 ms in hardware. It costs only the endpoint it holds. With polling the hub sees about 8 URBs/s (the poll, then the
  hub's and its 7 ports' status over ep0).
- **Port bounce** of idle stick 1003 (`echo 1 > /sys/bus/usb/devices/3-1:1.0/3-1-port2/disable`, 2 s, `echo 0`):
  - back in 0.32 s with the status channel, 0.53 s with polling;
  - **port 5 (stick 1000) dropped with port 2 both times**: this hub switches ports 2 and 5 together, so x6x00-rtlspec's
    stall bounce of one Kraken stick can drop another.
- **Stock module, same 4-stick test:** 100.0 % on all four.
- **Freezes:** the two "freezes" earlier that day (one on 9103, one on the stock module) both came about 3.2 min after
  `S95gui stop`. They were not USB. Never stop the GUI for a test.

## Test setup left on the radio (192.168.0.224)

- `/lib/modules/6.1.82/kernel/drivers/usb/musb/musb_hdrc.ko` = the 9104 test build (sha256 705ede6f...), loaded,
  `hub_poll_ms` 1000. The radio's own module is kept at `/mnt/musb-9103-test/musb_hdrc.ko.orig` (sha256 1d6cc9d5...).
- `/etc/modprobe.d/musb-9103-test.conf` blacklists `sunxi`, so after any boot **the HOST port stays off** until
  `modprobe sunxi`. musb is never unloaded on a running radio: a module swap is a file copy plus `x6x00-reboot`.
- `/mnt/sdr_pool` holds 1000-1004 (the GUI's pool leaves those sticks alone; it was an empty file before). Write it
  while the HOST port is off (B keeps a stick it already streams). Undo it with `: > /mnt/sdr_pool` and one GUI
  restart (`kill $(pidof x6100_gui)`).
- `/mnt/musb-9103-test/`: `df_rate` (radio build), the three modules, run logs.
- Back to stock: `cp /mnt/musb-9103-test/musb_hdrc.ko.orig /lib/modules/6.1.82/kernel/drivers/usb/musb/musb_hdrc.ko;
  rm /etc/modprobe.d/musb-9103-test.conf; : > /mnt/sdr_pool; sync; x6x00-reboot`.

## Building a test module on the Mac (no CI)

The radio's kernel is GCC 12.3 (Buildroot); the Mac has Homebrew arm-linux-gnueabihf-gcc 15.2. The radio's modules
carry no symbol versions (vermagic `6.1.82 SMP preempt mod_unload ARMv7 p2v8`), so a module from the same source and
config loads. Check that before installing:

1. Build the unchanged module the same way.
2. Compare the `[reg, #N]` offsets per function in `objdump -d` against the radio's own .ko. They matched in every hot
   path; the only difference was a loop stride.

Recipe:

1. A linux-6.1.82 tree (tarball sha256 d150d2d9...) with the X6200 config: `make modules_prepare`, with
   `HOSTCFLAGS="-I<hostinc with elf.h> -D_UUID_T -D__GETHOSTUUID_H"` on macOS.
2. Copy `drivers/usb/musb/*` with the patches applied into a directory, and build it as `M=<dir> modules`.
3. Pass `KBUILD_EXTRA_SYMBOLS=<file>`: a Module.symvers made from the module's undefined symbols, with CRC 0. The
   radio's `udc-core.ko` exports go under `drivers/usb/gadget/udc/udc-core`, so modinfo says `depends=udc-core`.
   Leave out the symbols musb_hdrc itself exports, which sunxi.ko imports.

## Left to do, in order

1. Done for 20 min (above); a 30-min or longer run if wanted: `cd /mnt/musb-9103-test && ./df_rate 1800 1024000 1000 1001 1002 1003`,
   with B on 1004. Keep a live `ssh root@<radio> 'dmesg -w'` running.
2. Churn: `df_rate` on 1000-1002 left running, a second one on 1003 stopped and started 10 times.
3. Page I's over-the-air check with five channels: empty `/mnt/sdr_pool`, restart the GUI once.
4. The user decides whether to ship 9104 (main and CI), and the MANUAL change with it. Then remove the test setup above.
</content>
</invoke>
<invoke name="Bash">
<parameter name="command">cd /Users/kfinisterre/Desktop/OpenX6X00/.claude/worktrees/friendly-archimedes-252a82 && git rm -q buildroot/br2_external/board/X6100/patches/linux/9103-usb-musb-host-take-turns-on-the-shared-bulk-in-endpoint.patch && git add buildroot/br2_external/board/X6100/patches/linux/9104-usb-musb-host-poll-a-hub-over-ep0-instead-of-its-status-endpoint.patch docs/MANUAL.md buildroot/local-build.sh docs/plans/musb_hub_poll_9104.md && git status --short && git commit -q -F - <<'EOF'
Kernel: the KrakenSDR's hub polled over ep0 (9104), so all five of its RTLs stream; 9103 withdrawn

The X6200's MUSB (R16) has five receive endpoints; the Kraken's hub kept one for its status URB for good, so the fifth
stick shared ep1 and got nothing. usbcore is built in, so instead of a hub.c quirk (a new kernel image) musb_hdrc
holds that one hub's status URB off the hardware and gives it back every hub_poll_ms (1000) with every port marked
changed; the hub driver reads the ports over ep0 as after a real change. Module parameters hub_poll_ms (0 = off) and
hub_poll_id (0x04242517, the USB2517). Also a notice when a bulk-in queue joins another on ep1.

Tested on the radio as a hot-installed module (GUI up): B at 2.1168 MS/s + four sticks at 1.024 MS/s all 100 % (60 s,
219 s, 81 s); the hub's status channel never completes in a clean 5-min capture (urbnum flat); a port bounce is seen
in 0.53 s polled against 0.32 s; ports 2 and 5 of the Kraken's hub switch together. docs/plans/musb_hub_poll_9104.md
has the results, the test setup left on the radio and how to undo it. 9103 (turns on ep1) is withdrawn from the
patch set; it stays in this branch's history (3b106ec). MANUAL: the "five at once" of 2026-11-16 corrected.

Not merged or pushed: the user decides (once on main, every image and bundle carries it).

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
EOF
git log --oneline -3; git status --short | head