# The X6200's and the X6100's base firmware, written from OpenX6X00

The X6200 runs two firmwares: OpenX6X00 on the card (Linux, the GUI), and Xiegu's **base** firmware in the radio
board's own processor (DSP, RF, power, keyer). Xiegu's way to update the base needs Xiegu's own system on the
radio's internal memory: a card swap, a menu, another card swap ([README.md, The
baseband](../../README.md#the-baseband)). These tools do it from OpenX6X00, without taking the card out:
from the radio's own screen, or from a computer over SSH. Any release Xiegu has published, up or down.

The X6100's base is written by the same writer, another way into the same kind of bootloader, from a computer over
SSH only: [The X6100](#the-x6100) below.

| Piece | Where | Does |
|---|---|---|
| **Settings > Info > Base firmware** | the radio's screen ([dialog_base_update.c](../../src/dialog_base_update.c)) | the releases, Get, Check, Flash, with everything shown as it happens |
| **x6x00-base-fetch** | [rootfs/usr/sbin](../../rootfs/usr/sbin/x6x00-base-fetch), on the radio and here | fetches a release from xiegu.eu as a stream, keeping only the base file; the check |
| **x6x00-base-flash** | [base_flash.c](base_flash.c), on the radio | writes the base the way Xiegu's own app 1.0.8 does, step for step |
| **S99basecheck** | [rootfs/etc/init.d](../../rootfs/etc/init.d/S99basecheck) | once a day, online, on an X6200: is there a newer base on xiegu.eu? Says so, never more |
| **base_releases.txt** | [rootfs/usr/share/x6100](../../rootfs/usr/share/x6100/base_releases.txt) | the releases known: version, file, size, SHA-256, Xiegu's zip and its address |
| fetch.sh, build.sh, flash.sh, install.sh | here | the same from a computer |

## The releases

All of Xiegu's X6200 base files, as [base_releases.txt](../../rootfs/usr/share/x6100/base_releases.txt) has them.
Files are told apart by their SHA-256, never by name or date (the date is the one in Xiegu's own file name):

| Base | File | Bytes | Where | |
|---|---|---|---|---|
| **1.0.8** | x6200_mcu_20260820141208.xgf | 524288 | xiegu.eu, `x6200udimg-260820001.zip` | fixes the S-meter stuck at -255 dBm (S1, -121.0 on screen), the CW keyer's speed; CW macros back |
| 1.0.7 | x6200_mcu_20260116092100.xgf | 401024 | not on xiegu.eu: the factory build, on the test X6200's internal memory | a later 1.0.7 build than xiegu.eu's: what the test X6200 came with (below) |
| 1.0.7 | x6200_mcu_20250916150221.xgf | 524288 | xiegu.eu, `x6200 FW 1.0.7 (251012).zip` | |
| 1.0.6 | x6200_mcu_20250111134652.xgf | 524288 | xiegu.eu, `Xiegu X6200 update V1.0.6 (240115).zip` | |
| 1.0.3 | x6200_mcu_20240924203603.xgf | 524288 | xiegu.eu, `X6200_UD_Image_1.0.3.zip` | |

**Two 1.0.7 builds.** Xiegu published 1.0.7 once, in October 2025, with a base file built on 16 September 2025. Our
X6200 left the factory in January 2026 with a later build of the same version, which Xiegu never put on its site:

- **It came from the radio, not from a backup.** A base's firmware cannot be read back (its bootloader only takes a
  file in, and the processor is read-protected), so nothing was backed up before the first write. The file is the one
  Xiegu's own system keeps for its UPGRADE screen, `/usr/firmware/x6200_mcu_20260116092100.xgf` on the radio's
  internal memory (`/dev/mmcblk2p2`).
- **Xiegu built that system on 16 January 2026.** The base file is dated 09:18 China time that morning (its name
  says 09:21), the system's last files 10:02; the only newer file there is the 1.0.8 we copied in on 13 November 2026
  ([install.sh](install.sh)). No update on xiegu.eu carries that system, so it came with the radio.
- **The base ran that build.** Before any write, on 7 November 2026, the base reported `fw version: 1.0.7`, `fw date:
  Jan 15 2026,16:35:01`: not xiegu.eu's September 2025 build. Written back in the downgrade below, the base reports the
  same date again. Without a readout, that is as close a match as can be had: same version, same build date.
- **What differs between the two is not known.** The files are encrypted and Xiegu wrote no release note for the later
  one. It is 401,024 bytes; every base file on xiegu.eu is padded to 524,288.

Another X6200 may carry another factory build. The radio's screen copies whatever is in `/usr/firmware` to
`/mnt/base_update` and names it by its SHA-256, or as UNKNOWN when the list does not have it.

Xiegu's files are Xiegu's: they are fetched from Xiegu's own site and nothing of them is in this repository.

## From the radio: Settings > Info > Base firmware

Settings > Info > Base firmware > Open... (an X6200 only). Its window takes the screen; Close goes back.

- **The list**: each release, HERE when its file is in `/mnt/base_update` (whatever it is called: its SHA-256 says
  which it is; the files on Xiegu's own system's `/usr/firmware` are copied there first), ON XIEGU.EU when it can be
  fetched, `<- NOW` on the one the base runs. Any other `.xgf` there is listed as UNKNOWN (one of your own), and a
  release xiegu.eu has that the list does not (the last check found it) as NEW. The MFK chooses.
- **Get** fetches the chosen release from xiegu.eu: Xiegu's update zip streamed, the base file taken out as it goes
  by (the zip's card image, its FAT partition, rootfs.tar, `./usr/firmware/*.xgf`), nothing else kept: about two
  thirds of the 370 MB come down and none of it is stored, so a radio has room for it. Checked against the list;
  4½ minutes on the test X6200's WiFi (255 MB at 0.9 MB/s).
- **Check** reads xiegu.eu's downloads page now, as the daily check does, whether the daily check is on or off.
- **Flash** shows what it is about to do (versions, Xiegu's power conditions and the radio's own supply now), then
  **FLASH NOW** writes it. An unknown file asks once more first (**UNKNOWN: OK?**). The GUI stays up: it lends the
  base to the writer (it stops reading the base's port and sends it nothing, as Xiegu's own app pauses its own
  reading of the port) and shows, as they come: the writer's lines, every kernel message, and the base's serial port
  once a second.
- **After the write**: "the base's firmware is running" once its stream comes back (9 to 12 kB/s on ttyS1, ten
  seconds after the transfer), and, about three minutes later, "the base stopped its stream: it is switching the radio
  off". **Then switch the radio on again**: it runs the new base. Everything also goes to `/mnt/base_update.log`,
  flushed to the card every second, and the window shows its end when opened after the power-on.

### The daily check

**Off by default:** the radio asks xiegu.eu nothing until its owner turns it on, in Settings > Info > **Daily base
check** (`base_daily_check` in `/mnt/params.db`, kept with the other settings). Get and Check work the same either way.

[S99basecheck](../../rootfs/etc/init.d/S99basecheck) runs `x6x00-base-fetch daily` on an X6200 5 to 35 minutes after
the start (a random wait: radios do not all ask xiegu.eu at once). While the setting is off (or `params.db` cannot be
read) it stops there with exit 3, nothing sent, and asks the setting again ten minutes later; so a switch turned on
takes effect within ten minutes, without a restart. On, it is `x6x00-base-fetch check`, then every 24 hours, or every
hour while there is no network. It reads xiegu.eu's X6200 downloads (nothing of the X6100's) and compares them with
the list and with the base's version (the GUI writes it to `/tmp/x6x00-base-version`), into
`/mnt/base_update/check.json`. A newer release is said on the screen once ("Base firmware 1.0.9 is out: Settings >
Info > Base firmware"), and Settings > Info shows it next to Base firmware. It never downloads a base file and never
writes the base. On an X6100 it does nothing.

## From a computer

```bash
tools/base_update/fetch.sh
```

```bash
tools/base_update/build.sh
```

```bash
tools/base_update/flash.sh root@<radio> --check
```

```bash
tools/base_update/flash.sh root@<radio> --flash
```

1. **fetch.sh** does what Get does, into `build/base_update`: `fetch.sh` the latest, `fetch.sh 1.0.6` one release,
   `fetch.sh all` every one on xiegu.eu, `fetch.sh --discover` whatever xiegu.eu's downloads page has that the list
   does not, `fetch.sh --url URL` or `--zip FILE` an update zip by its address or one here. No zip is kept. It needs
   Python 3 and nothing else (no 7-Zip).
2. **build.sh** builds the writer for the radio, statically linked, with any ARM hard-float Linux cross compiler
   (`arm-linux-gnueabihf-gcc`, or another prefix in `CROSS_COMPILE`). The image builds it as `x6x00-base-flash`.
3. **flash.sh --check** reads only, and never asks the base for its bootloader: the file, what holds the base's
   serial port and I2C bus (the GUI, while it runs), and the base's version over I2C.
4. **flash.sh --flash** says what it is about to do and asks for `FLASH` to be typed (`--yes` does not ask). On the
   radio, apart from the SSH session ([run_on_radio.sh](run_on_radio.sh), under `setsid`): the GUI stopped, the lock
   `/etc/init.d/S95gui start` takes held (a GUI start waits), the write, the GUI started again; flash.sh follows the
   log. `--to VERSION` writes another release, its file found in `build/base_update` by its SHA-256 (the newest build
   when a version has two); `--file FILE.xgf` a file of your own; `--recover` again after a write that failed after
   the erase.
5. The base then switches the radio off a few minutes later: switch it on again; `--check` shows the new version.

**install.sh** is the other way, with Xiegu's own system: it puts the 1.0.8 file in `/usr/firmware` on the internal
memory, where Xiegu's MENU > SYSTEM > UPGRADE lists it (switch off, card out, switch on, UPGRADE, card back in).

**Power.** As Xiegu asks for its own update: a 13.8 V supply of 3 A or more and the battery out, for the whole write.
Do not switch the radio off while it writes. Every write here so far was on the radio's 11.8 V external supply with its
battery in and charging: both sources there, and each went through, but that is not Xiegu's condition.

**If it fails.** The writer says where, in Xiegu's own words, and its exit status says what to do:

| Exit | Meaning | Then |
|---|---|---|
| 0 | written | switch the radio on again after it switches itself off |
| 1 | refused: nothing was sent to the base (not a file it writes, something else holds the port, no base on I2C) | what it says |
| 2 | the bootloader did not answer its handshake: nothing erased | switch the radio off and on |
| 3 | failed at or after the erase: the base has no firmware now | **Recover** on the screen, or `flash.sh --recover`; do not switch off |

The bootloader is in the processor's first sectors, which the erase does not touch, so it should take the file
again. That is how Xiegu's design reads; no write here has needed it yet.

## Your own files, and a new release

- **A file of your own**: put it in `/mnt/base_update` on the radio, or `build/base_update` on the computer, as
  `NAME.xgf`. It is offered as UNKNOWN when the list does not have its SHA-256, and written only after one more
  confirmation (`--unknown-ok` from the computer). The writer takes it only if it passes what can be checked of an
  encrypted file: a multiple of 16 bytes, 16 to 512 KiB, and not a plain ARM image (one starting with a stack pointer
  and a reset vector would be decrypted into garbage by the bootloader).
- **A new release from Xiegu**: Check (or the daily check, when it is on) finds it on xiegu.eu's downloads page and
  the screen lists it as NEW;
  Get (or `fetch.sh --discover`) fetches it, keeps it as unverified (`NAME.xgf.json`, with the version from Xiegu's
  release note) and prints its line for `base_releases.txt`. Added to the list and to `known[]` in
  [base_flash.c](base_flash.c) (the test suite checks the two agree), it is a known release like the others.

## Each radio its own base, and its own files

Xiegu encrypts the X6100's, the G90's and the X6200's base files with the same key, so nothing in an encrypted file
says which radio it is for, and the X6100's base is written another way altogether (its Morse key line held low while
the base is reset). Every step keeps the radios apart:

- **The writer knows the radio from its device tree.** Before it opens the base's port or bus, x6x00-base-flash reads
  `/proc/device-tree/compatible`: `xiegu,x6200` is written the X6200's way, `xiegu,x6100` the X6100's, anything else
  is refused with not one byte sent. The writer is in both radios' images; the fetcher, the list of releases, the
  screen, the daily check and the hooks that lend the base are the X6200's only.
- **Only that radio's files are written.** A file is known by its SHA-256 in the writer's `known[]`, each entry for
  one radio, never by its name: an X6200 file is refused on an X6100 and the other way round (tested: the `x6100` and
  `x6100-x6200dt` scenarios below). Any other file needs `--unknown-ok` and a second confirmation, and a file named
  for another radio (X6100, G90, G106, X5105 on an X6200; X6200, G90, G106, X5105 on an X6100) is refused even then.
- **A test build only when asked.** RF.Guru's X6100 test build (`V1.1.9 ... tx1a2-test`) is in `known[]` marked as a
  test: refused without `--test-ok`, and flash.sh asks for `TEST` to be typed.
- **Only the X6200's releases are fetched.** The fetcher and the daily check read only xiegu.eu's links titled
  "X6200 Firmware Upgrade", and take only `./usr/firmware/*.xgf` from those zips.
- **Only an X6200 offers it.** The screen is built for the X6200 only (`BOARD_X6200`), and the daily check runs only
  where the device tree says `xiegu,x6200`.

## How it works

Xiegu's app 1.0.8 (`/usr/app_qt/x6200_ui_v100` in its update) was disassembled to see what its UPGRADE screen does:
`UiFwUpgrade::upgradeFirmware` reads the file whole and hands it to a worker thread, `XMWorker::slot_start`, with the
base's serial port, which its main loop stops reading meanwhile (`BaseComm::pause`). The writer does the same, with
the same timeouts, the same messages and the same error handling:

1. **The port** (`uart_create`, `BaseComm::start`): `/dev/ttyS1`, raw 8N1 at 1152000 baud, `VMIN` and `VTIME` 0 so
   every read returns at once, opened blocking without a controlling terminal, both queues flushed.
2. **The bootloader asked for**: a `c` on the port and host command `0x800a` over I2C, both again every 100 turns of
   a loop that sleeps 1 ms and reads up to 10240 bytes each turn. The host command is one write to the base at
   address `0x72` on `/dev/i2c-0`: register `0xfffe` high byte first, the command low byte first, `FF FE 0A 80`
   (`BaseCommIIC::iicWrite`; the same bytes as `x6200_control_host_cmd` in Aether's library). A one-byte read of `Y`
   or `y` counts; more than 10 end the loop. So do 10000 empty reads (`[ERR] Wait IAP respond timeout`) or 10000
   others (`[ERR] Clear rx buffer timeout`), and Xiegu's app goes on all the same: only the next step decides. The
   X6200 uses no GPIO for this (the X6100's updater pulls the Morse key line low and resets the base).
3. **The handshakes** (`send_cmd`): `c` for `y` (`[ERR] IAP failed, connect timeout`: nothing erased yet), then `r`,
   the erase, for `g` (`[ERR] IAP failed, can't erase flash`), then `p`, program, for `g` (`[ERR] IAP failed, can't
   start xmodem`). Each byte written, then up to 5000 one-byte reads 1 ms apart for the answer, written again up to 10
   more times (`[ERR] no respond to cmd %c, retry %d`).
4. **The file** (`Xmodem::sendFile`), as it is: XMODEM-1K with a CRC. The bootloader decrypts the `.xgf` itself, so
   no key is needed, here or anywhere. A 2-byte read whose second byte is `C` within 10 s, then 1024-byte packets
   (STX, the packet number from 1, its complement, the data with the last block padded with `0xFF`, CRC-16/XMODEM high
   byte first), each answered within 10 s by one byte: ACK the next, CAN, NAK or anything else the end; no retries.
   Then EOT, answered by ACK (`1k xmodem send finished.`).
5. **The end**: Xiegu's app ends every way the same and neither resets the base nor sends it a command.

On the radio's screen the GUI keeps running meanwhile: `radio_base_hold()` ([radio.c](../../src/radio.c)) parks the
radio thread out of its read of the port, and `hw_base_hold()` ([hw_x6200.c](../../src/hw/x6200/hw_x6200.c)) keeps
every write off the base (the library's own entry, the register block, the power-off, the reset and key lines) until
the radio is switched off. The writer runs with `--held-by` the GUI's process, whose open port and bus it then lets
be; anything else holding them is refused.

## How it was checked

- **[test/run.sh](test/run.sh)**, on a Mac or a Linux PC, in about two minutes, and on GitHub for every change to
  the updater ([base_update.yml](../../.github/workflows/base_update.yml), with a stand-in for Xiegu's files):
  - unit tests: SHA-256 against FIPS 180-2's vectors; the packet's CRC against CRC-16/XMODEM's check value and against
    Xiegu's own CRC routine (rewritten from its disassembly) on 2000 random packets; the packet's frame; the writer's
    `known[]` against `base_releases.txt`; Xiegu's files taken, any other refused;
  - the writer itself, its port on a pseudo-terminal, against a simulated base in each of these:

    | Scenario | The simulated base | The writer |
    |---|---|---|
    | ok | streams, then answers as the bootloader | the file whole, byte for byte; exit 0 (also with 1.0.7's 392 packets) |
    | silent | says nothing until the writer has given up waiting | `[ERR] Wait IAP respond timeout`, goes on as Xiegu's app does; exit 0 |
    | stream | keeps streaming until the writer has given up | `[ERR] Clear rx buffer timeout`, goes on; exit 0 |
    | dead | never answers | `[ERR] IAP failed, connect timeout`; exit 2 |
    | noerase | never answers the erase | `[ERR] IAP failed, can't erase flash`; exit 3 |
    | nostart | never sends the `C` | `1k xmodem timeout.`; exit 3 |
    | nak, can, garbage | NAK at packet 100, CAN at 50, an `X` at 10 | `1k xmodem bad packet`, `… transfer canceled.`, `… error, = 88`; exit 3 |
    | eot | no ACK for the EOT | `1k xmodem error, = 0 (after EOT)`; exit 3 |
    | x6100 | an X6200 file on an X6100's device tree | `refused: nothing was sent to the base`; exit 1 |

    and against a simulated X6100 base ([test/sim6100.py](test/sim6100.py)), its key and reset lines a log, which
    streams as its application until the key line is low while the reset is let go, then greets as its bootloader:

    | Scenario | The simulated X6100 base | The writer |
    |---|---|---|
    | ok, zeros | answers as the bootloader (zeros: 0 bytes before its answers) | the file whole; the lines in order: key low, reset pulse, key up; exit 0 |
    | dry | answers | `--dry-run`: no erase, no packet; the application started again (key up, reset pulse); exit 0 |
    | nogreet | never greets | `[ERR] Wait IAP respond timeout`, the application started again; exit 2 |
    | noerase, noprog, nostart | no answer to `r`, to `p`, no `C` | each its error; exit 3 |
    | nak, eot | NAK at packet 100, no ACK for the EOT | `1k xmodem bad packet`, `1k xmodem error, = 0 (after EOT)`; exit 3 |
    | x6200dt | an X6100 file on an X6200's device tree | `refused: nothing was sent to the base`, no line touched; exit 1 |

  - the fetcher on small update zips made as Xiegu's are ([test/synth.py](test/synth.py): a zip written as a stream,
    an MBR, a FAT16 boot partition, a Linux one, a FAT16 one with rootfs.tar), through Python's zlib and through libz
    as on the radio (whose Python has no zlib module): Xiegu's 1.0.8 file out and checked, an unknown one out whole,
    unverified, with its line for the list;
  - the daily check's switch (`x6x00-base-fetch daily`, the network a closed port): no `params.db`, no
    `base_daily_check` row and 0 all stop with exit 3 and ask xiegu.eu nothing; 1 goes on to ask.
- **run_on_radio.sh on the radio** with a stand-in that sends nothing: the GUI stopped, nothing left holding the port
  or the bus, the lock held, the GUI started again.
- **Fetching on the radio**: 1.0.8 from xiegu.eu, the file out and checked after 255 of the zip's 371 MB, in 272 s;
  68 MB of memory in use. On the Mac, from the zip: 0.9 s.

### Three real writes, 13 November 2026

An X6200 on its 11.8 V external supply with its battery in, charging (89 to 90 %):

| | 1.0.7 -> 1.0.8 (flash.sh) | 1.0.8 -> 1.0.7 (flash.sh, the downgrade) | 1.0.7 -> 1.0.8 (the radio's screen) |
|---|---|---|---|
| file | xiegu.eu's 1.0.8 | **the factory 1.0.7** (x6200_mcu_20260116092100.xgf, Jan 2026, 392 packets) | xiegu.eu's 1.0.8 |
| bootloader answers | 4.6 s | 4.6 s | 4.6 s |
| erase (`r` sent twice: it takes more than 5 s, as in Xiegu's app) | 6.7 s | 6.8 s | 6.8 s |
| transfer done, EOT acknowledged | 32.4 s | 27.9 s | 32.4 s |
| new firmware streaming on ttyS1 | (not watched) | yes | 10 s after the transfer |
| radio switches itself off | a few minutes after | about 4 minutes after | 3 min 10 s after its stream began |
| after the power-on | base 1.0.8, Aug 19 2026 | base 1.0.7, Jan 15 2026 | base 1.0.8, Aug 19 2026 |

- **A downgrade works**: 1.0.8's bootloader takes an older file as readily as a newer one, with the same steps and
  the same times.
- **The power-off is the base's own**: nothing on Linux sends it (no command to the base, no kernel message; the GUI
  writes its log to the card when it switches the radio off itself, and there was none). The new firmware starts
  (its stream on the serial port), but its I2C to Linux stays locked (`mv64xxx: I2C bus locked`) and it never gets
  the start-up Linux sends it over I2C; about three minutes on it stops its stream and cuts the power. Switched on
  again, it starts normally.
- **The calibration stays**: the base's calibration block came through every write, its PA bias the radio's own
  (3226/3958) each time.

## The X6100

From a computer over SSH, with a file of your own: Xiegu's X6100 base files come in Xiegu's X6100 updates, and none
is fetched here. The X6100 has no Base firmware screen and no daily check.

```bash
tools/base_update/flash.sh root@<x6100> --check --file X6100_BBFW_V1.1.8_240915003.xgf
```

```bash
tools/base_update/flash.sh root@<x6100> --dry-run --file X6100_BBFW_V1.1.8_240915003.xgf
```

```bash
tools/base_update/flash.sh root@<x6100> --flash --file X6100_BBFW_V1.1.8_240915003.xgf
```

- **The files known**: Xiegu's V1.1.8 (`X6100_BBFW_V1.1.8_240915003.xgf`, 391584 bytes, `8516de0b...`) and V1.1.9
  (`X6100_BBFW_V1.1.9_251127001.xgf`, 392048 bytes, `87a3e96e...`), and RF.Guru's test build
  ([X6100-Baseband v0.1.0-alpha.2](https://github.com/Guru-RF/X6100-Baseband/releases/tag/v0.1.0-alpha.2),
  `--test-ok`). Any other with `--unknown-ok`, as on the X6200.
- **--dry-run** first: the base asked for its bootloader and, once it answers, its application started again. Nothing
  is erased or written: it shows the way in works on this radio.
- **The way in** is the one of Xiegu's own X6100 updater and of Franco Venturi's
  [x6100-base-experiments](https://github.com/fventuri/x6100-base-experiments) flasher (LGPL-2.1-or-later), which wrote
  the bench X6100's base first on 24 October 2026: the Morse key line (GPIO 203 on `/dev/gpiochip1`, the bootloader's
  strap) held low while the base's reset line (204) is pulsed for 100 ms; the port at 1500000 baud; the bootloader's
  greeting `A`; the key line let go; `r` (the erase, up to 60 s) and `p`, each answered by any byte but 0; then
  XMODEM-1K as on the X6200 (steps 4 and 5 above). The X6200's `c` handshake and I2C host command are not used.
- **Checked against Xiegu's own X6100 app** (2026-11-18: APP V1.2.0 of 13 Dec 2025, `/usr/app_qt/x6100_ui_v100` on
  an X6100's internal memory, not stripped): `xradio_start_xmodem()` pauses its serial thread and drains the port,
  sets GPIO 203 (sysfs numbering: our kernel's `/dev/gpiochip1` line 203) out low, 204 high for 100 ms then low and
  lets 204 go, drains the port again, then reads for the greeting (`byte & 0xDF == 'A'`, 51 tries) and lets 203 go
  high; `XMWorker::slot_start` sends `r` with 60 s for an answer ("IAP failed, can't erase flash"), `p` with 1 s
  ("can't start xmodem"), each answered by any byte but 0 (`_comm_send_cmd`), then 1K XMODEM. Its port is
  `/dev/ttyS1` asked for 1152000 baud, which the R16's 24 MHz UART can only make as 1500000 (divisor 1): the writer
  asks 1500000. Step for step the writer's X6100 path; the X6200's way in (I2C host command, `c`/`y`, `g` answers)
  is its own. The base file that system carries, `/usr/firmware/X6100_BBFW_V1.1.9_251127001.xgf`, is the V1.1.9 in
  the writer's list (SHA-256 `87a3e96e...`): an X6100 that left the factory with base V1.1.8 may have V1.1.9 on its
  own internal memory, never written to its base.
- **After the write** the bootloader starts the new firmware by itself: it answered on I2C 4 s after the transfer.
  For about a minute its version text (I2C register 0) then mostly read without its first three bytes
  (`1.8 Sep 15 2024,...`), and empty once; the writer says so, and the GUI reads it again at its start
  ([hw_x6100.c](../../src/hw/x6100/hw_x6100.c), `hw_control_init`), which it did not before: after the first write
  it took the test base for no known base until a restart. The radio does not switch itself off, unlike the X6200.
- **Power** as Xiegu asks: a 13.8 V supply of 3 A or more, the battery out, the radio not switched off while it
  writes. The two writes below were on a 13.6 V supply with the battery in.
- `--recover` takes the same way in; after a failed X6100 write it has not been needed yet.

### Two real writes, 15 November 2026

The bench X6100, OpenX6X00 running, from a computer over SSH:

| | V1.1.8 -> alpha.2 (the test build) | alpha.2 -> V1.1.8 |
|---|---|---|
| bootloader greets | 1.0 s | 1.0 s |
| erase answered (`g`) | 7.1 s | 7.2 s |
| transfer done, EOT acknowledged | 396 packets, 6.9 s | 383 packets, 6.8 s |
| the base answers on I2C | not the same version twice in 60 s (each state not logged then) | 4 s after the transfer |
| the GUI's version | after a restart (the fix above came after) | on its third read |
| the calibration block (I2C 0xF000, 816 bytes) | the same | the same, and the same as on 24 October |

### A third, 18 November 2026: an owner's X6100, V1.1.8 -> V1.1.9

The file from the radio's own internal memory (`/usr/firmware` of Xiegu's system there, copied to
`/mnt/base_update`), the GUI stopped, from a computer over SSH (the writer detached on the radio, its log on DATA),
11.9 V from the supply with the battery in at 99 %. A dry run first (the bootloader answered `a` in
1.0 s, the base back on V1.1.8), then the write: greeting `a` 1.0 s, erase answered 6.6 s later, 383 packets in
6.7 s, the base answering on I2C 4 s after with `V1.1.9 Nov 27 2025,10:31:57`, the writer's exit 0 at 75 s; the
GUI read `V1.1.9` on its start. The radio stayed on. What V1.1.9 changes here: the base zooms the spectrum itself
(the GUI's zoom and IF shift for it, hw_x6100.c), and the IQ's noise still climbs from about 27 kHz either side of
the dial (TODO 12).

## Others' base work, read 2026-11-19

The owner's links, read through GitHub's API and groups.io (nothing cloned, no binaries fetched):

- [strijar/x6100-base](https://github.com/strijar/x6100-base) (R1CBU): Xiegu's X6100 base V1.1.9
  (X6100_BBFW_V1.1.9_251127001, the build on the owner's X6100) mapped from Ghidra, for a clean-room replacement:
  an STM32F427, the CS4272 converter at I2C2 0x10 in double-speed mode clocked from the TLV320AIC3204 at 100 kHz,
  the RX and TX chains, the control registers. No code and no licence: read, not copied. It agrees with what this
  tool and the GUI assume (the bootloader's GPIO 203/204, `A`/`a`, `r`/`p`, XMODEM-1K; i2c-0 0x72 and its 56
  registers); it finds registers 48/49 (RX phase and magnitude correction) in use, which the X6100 control
  library's header calls unused.
- [fventuri/x6100-base, WIP-franco](https://github.com/fventuri/x6100-base/tree/WIP-franco) (K4VZ): the same notes
  plus pins checked on a radio, the libraries in the image identified, and a dump of the 128 KiB IAP bootloader
  (made by flashing a dumper; the application still cannot be read back).
- [fventuri/x6100-bbfw](https://github.com/fventuri/x6100-bbfw) (MIT): a clean-room base built on those notes,
  receive only so far (AM, SSB, FT8, the panadapter, the battery), driven by the stock GUI or ours unchanged.
  Its `common/build_common.sh` makes .xgf files: AES-256-ECB, no padding, with Xiegu's key. The same key opens
  the X6200's: its 2026-08-20 base decrypts to an STM32H7's vector table (stack at 0x24050000, AXI SRAM), the
  X6100's V1.1.9 to an STM32F4's (0x20030000).
- Georgy Dyuldin's patch method (groups.io xiegu-x6100 topic 112186023, April 2025; gdyuldin/x6100_base_patch):
  code built for the STM32 placed past the end of Xiegu's image, jumps into it; his TX chain addresses are for
  1.1.8. The spectrum's edges (topic 108431112, September 2024): strong signals within 10-12 kHz of +-50 kHz show
  on the opposite edge on every X6100, the converter's own filter the only anti-alias filter.

The two radios' current bases side by side (decrypted 2026-11-19 from the radios' own .xgf files; nothing kept in the
repository): the X6100's V1.1.9 (built Nov 27 2025) is 391 KB for an STM32F427 (Cortex-M4, about 91 interrupt
vectors, stack top 0x20030000): an analog quadrature detector fed a 4x LO from the VersaClock (26 MHz reference), the
CS4272 bringing I/Q at 100 kHz, the DSP and audio in the STM32 (debug text for TX power, NR, the digital notch, AGC
time constants, the LO; the BQ24715 charger and MAX17205 fuel gauge; PA bias calibration). The X6200's 1.0.8 (built
Aug 19 2026) fills 512 KiB for an STM32H72x/H73x (Cortex-M7, about 163 vectors, stack top 0x24050000 in AXI SRAM):
an FPGA in front ("FPGA FW Version", "Check FPGA BUS", its own flash written from the base), clocks from a
VersaClock 6 (245.76 and 61.44 MHz in it), selectable I/Q rates of 96, 192 and 384 kHz to the STM32, audio at
48 kHz (12.288/49.152 MHz master clocks); and much more done in the base: a CW decoder, keyer and trainer, a
compressor, 5-band RX, WFM and mic equalisers, FM squelch, line in/out gains, voice record, a serial console with a
login, power-off timers ("host offline, powering off in %d s": what x6200-base-keeper answers), readout protection
set at start and a bootloader it can rewrite. Both use CMSIS-DSP-style FFT twiddle tables.

### Georgy Dyuldin's patched base, studied 2026-11-20

[gdyuldin/x6100_base_patch](https://github.com/gdyuldin/x6100_base_patch) (branch `1.1.6`, no licence: read, not
copied; read through GitHub's API, no binary fetched). Releases r9-r13 (r13 2026-09-30:
`X6100_BBFW_V1.1.6_230307001_r13.xgf`, 465728 bytes, SHA-256 `ed3ee7338401e62f5890599252deed6949f756a7deffcf9829fec5b30d9067b7`),
all built on **Xiegu's V1.1.6** (two builds, 221112001 and 230307001, told apart by the plain image's MD5): there is
no patch for V1.1.8 or V1.1.9. The repository moved on in November 2026 (installers, a desktop/Android uploader),
not the firmware.

- **The method** (`patch.py`, `asm/helper.s`, `Core/`): a binary patch, not a rebuild. Per OEM build, a table of
  addresses (hook points, OEM functions, filter tables, the build-time string). New C (compressor, modulation, ANF,
  NR, NB, IF shift, VOX, CW peak, CESSB, the AIC3204 set-up, the I2C command handler) is built with
  arm-none-eabi-gcc and linked past the end of Xiegu's image; Xiegu's own CMSIS-DSP and helper functions
  (`arm_fir_f32`, `arm_biquad_cascade_df1_f32`, `write_i2c`, `setup_tx`...) are pinned at their OEM addresses so the
  new code calls them. Each hook overwrites 4 bytes of an OEM function with a branch to a wrapper placed right after
  the OEM image: the wrapper saves the core and FPU registers, calls the new function, restores them, runs the OEM
  instructions the branch displaced and jumps back. Data patches replace FIR and IIR coefficient tables in place
  and swap two filter calls (the CW AGC fix); the build-time string becomes `r13`. The GUI drives the new features
  through I2C registers it hijacks (`Core/Inc/comm.h`: 12, 15, 18, 19, 22-25, 35, 36, 38, 44-47; the X6100 control
  library and OpenX6X00's `hw_caps()` know them). Then the image is encrypted to .xgf with Xiegu's key.
- **What r1-r13 add**: TX compressor and gate, true power control (below), fixed AM and FM (10x FM depth, rewritten
  FM, pre/de-emphasis, squelch), TX high-pass 160 Hz and an adjustable TX band-pass, adaptive notch, spectral NR,
  a noise blanker, VOX, IF shift, `fft_span` zoom and a bf16 spectrum flow, RX IQ balancing (AM hum), the ghost RX
  after TX gone, CW AGC, a CW peak filter, CESSB (r12), the AIC PLL at a true 100 kHz and a battery % from a
  discharge curve (r13), TX codec gain and per-band output gain.
- **True power control** (issue #4, 2026-02): Xiegu's base does not change its TX level with the power setting, it
  only moves the ALC's target. The patch scales the I/Q by the power asked (k = 1 at 10 W), times the codec gain and
  the band's output gain. That is what an X6100 lacks where its ALC cannot pull the drive down far enough: the owner's
  gives 0.7-0.9 W at a 0.1 W setting on 6 m (2026-11-19).
- **On radios that shipped with base V1.1.8** (issue #3, 2025-12; and RF.Guru's bench X6100, tried by Joeri Van
  Dooren 2026-10-24, TODO.md: not the owner's X6100, which came 2026-11-18): 1.1.6 code on the newer board shows the
  battery always full (a hard shutdown at 5.8 V), I2C errors and the RTC losing time (#3); on RF.Guru's the external
  supply read 0 V and the first carrier after a band change came with next to no power (6 m nearly always). gdyuldin (#3): porting means finding every address again in the new version and sometimes new assembly.
- **RF.Guru's port to V1.1.9** ([Guru-RF/X6100-Baseband](https://github.com/Guru-RF/X6100-Baseband), GPL-3.0,
  v0.1.0-alpha.2 2026-11-15, TEST ONLY): the same method on V1.1.9 (five guarded hooks, payload at 0x08080000 and
  CCM at 0x10000000, the builder checking the stock image's hashes and every displaced instruction): CESSB, the
  compressor and the TX band-pass, Xiegu's power scaling, ALC, calibration and telemetry kept. Known to `base_flash`
  (`--test-ok`). It does not bring true power control.

### GoldCorpHF's patches to Xiegu's X6100 V1.1.9 (gcfix3, gcfix4), 2026-11-20

`x6100_patch.py` (here, with Xiegu's key, never in the repository) makes them; `rootfs/usr/share/x6100/base_patches/
x6100_V1.1.9_gcfix3.json` and `x6100_V1.1.9_gcfix4.json` are what ships: the 16-byte blocks of the encrypted .xgf that
change, Xiegu's and ours (AES-ECB encrypts each block alone, so no key and nothing of Xiegu's readable), with the SHA-256
and MD5 of Xiegu's file and of the result; gcfix4's blocks past the end of Xiegu's file (format 2) are appended.
`x6x00-base-patch apply gcfix4` (rootfs/usr/sbin) makes the file on the radio from Xiegu's; base_flash knows both
results by their SHA-256 (gcfix3 d34985d0..., gcfix4 fc6edfe5...). gcfix3 is three fixes, 15 bytes of Xiegu's image;
gcfix4 adds the hold, a hook (x6100_hooks/alc_hold.s, assembled by x6100_hooks/build.sh, 4 bytes of Xiegu's changed
and 224 appended):

| Fix | Where | Bytes | Why |
|---|---|---|---|
| pll | 0x0802f3d8 | `movs r3,#8` -> `ldr r3,[r7,#0x40]` | the codec PLL's D low byte written as computed: 100 kHz, not 99,932 Hz |
| 6m | 0x080246aa | `it hs; vmovhs s15,s13` -> 3 x `nop` | no +6 dB on the loop's gain from band index 20: it only raised 6 m's floor |
| alc | 0x08027f90, 0x08027fb6 | `movt r3,#0xc220` -> `#0xc270` | the loop's floor -60 dB, not -40 |
| hold (gcfix4) | 0x08027f82; code at 0x0807fb70 | `vldr s15,[sl,#0x220]` -> `b.w 0x0807fb70` | the loop's raise skipped while the block about to be sent is 30 dB under its recent peak: no climb through pauses |

Measured on the owner's X6100 through 70 dB of pads (bench.py's gates; tools/bench/tones.py): 0.1 W on 6, 10, 20 and 40
m (Xiegu's: 0.8 W on 6 m, 0.2-0.3 W on HF), 0.3, 0.5 and 1.1 W on 6 m as asked, the same to the dB with gcfix4; SSB
tones 300-2900 Hz within 3.5 dB at 0.1 W; the other sideband about 42 dB down in that 20 m sweep, the X6200 on LSB at
the same dial (the X6100's transmit image and the X6200's receive image together; the X6100's own, measured apart
below: -42.5 to -54.5 dBc); the receiver identical to Xiegu's V1.1.9 in the same sweep (passband, image).
At 5 W (2026-11-20 afternoon, the owner's 10 W 30 and 40 dB pads): 4.7-4.8 W on all ten bands 160-10 m and 5.2 W on
6 m by the X6100's meter (Xiegu's V1.1.9: 4.9 W on 20 and 15 m, 5.35 W on 6 m); the X6200's S-meter +16 dB from 0.1 to
5 W on 20 m (+17 expected). 10 W not tested: the bench needs a pad of twice the test power on each jack.
Withdrawn on the way: gcfix1 (floor only) and gcfix2 (no floor change), after a lost-tone run first blamed on the floor.

The hold (gcfix4), measured with `tones.py --bursts` (1 kHz bursts with 2 ms starts after pauses of 50 ms to 3.2 s,
0.1 W, three runs): every burst starts within +1.1 dB of its level, dips 2.6 dB at most, and the transmitter sends
nothing over the receiver's floor in the pauses. gcfix3 (Xiegu's loop): +5 dB after 50 ms, +10 after 100 ms, +18
after the key-down's lead or 0.8 s, each then 6-14 dB under for a fifth of a second, its own noise sent at full drive
10 dB under the tone in long pauses, and 0.7-1.0 W on its meter (the bench's watch ends such an over). Xiegu's loop
runs 781 times a second (128 samples at 100 kHz) and raises its gain by 0.01 x the error (about 50 dB in silence):
half a dB a block. The hook's own state is in CCM RAM (Xiegu's base does not use it; the hook switches its clock on
through the bit-band alias, as RF.Guru's patch_init does): a peak of the block energy, trusted only beside a magic
word, that does not learn in the 100 blocks after a key-down (Xiegu's SWR hold-off count), and no hold lasts over 5 s.
The first transmission after the radio is switched on behaves as Xiegu's until the peak has heard audio.

Two misreadings on the way, both the listening X6200's squelch (at 4: with its AGC off and attenuator in, it shut and
muted its audio whole, a flat -48 dBFS): runs that looked like the hook holding the transmitter quiet, and the other
sideband "101-110 dB down" (it was the muted audio). tones.py opens the listener's squelch now. The X6200 "dropping
changing tones after 2.5 s with its AGC off" was most likely the same.

At 5 W (tones.py --w 5, 2026-11-20): gcfix4 within +0.3 dB after every pause on 20 m (dips -0.1 dB) and +0.6 dB on
6 m; gcfix3 (Xiegu's loop) +0.9 dB on 20 m, and its own noise in long pauses -33 dB against gcfix4's -43. At 5 W
Xiegu's loop has little room to climb: the burst is a low-power problem (QRP, a transverter's drive, an amplifier fed
a low setting).

The codec PLL ("pll"): by the values Xiegu writes the I/Q rate is 99,932 Hz (-676 ppm), but no tone shows it.
Xiegu's base sent a 600 -> 900 Hz step as 299.99 Hz on 20 m and 299.98 on 15 m at 5 W (-676 ppm: 299.80); the X6100
receiving the X6200 measured +50.7 ppm on Xiegu's base and +53.1 on gcfix3. Where it may still show (the spectrum's
scale, the IF shift's NCO) is not measured. The fix stays: the code clearly means the computed byte, and gdyuldin's
r13 made the same change.

### The X6100's transmit image and carrier, measured 2026-11-20

`tools/bench/image.py` (tools/bench/README.md): the X6100 sends USB tones of 600 and 900 Hz at 5 W, the X6200 listens
in USB with its dial 1500 Hz (20 and 80 m also 1200 Hz) below; the image's place is checked against 2 x carrier -
wanted. Every band 160-6 m, gcfix4; Xiegu's V1.1.9 on 20, 15 and 6 m (flashed for it, gcfix4 back after):

| Band | Image, dBc | Mirror error | Carrier, dBc |
|---|---|---|---|
| 160 m | -52.1 to -54.0 | 0.01 Hz | -31.3 to -32.1 |
| 80 m | -51.6 to -54.5 | 0.02 Hz | -30.0 to -30.3 |
| 40 m | -49.0 to -50.8 | 0.09 Hz | -30.2 to -30.5 |
| 30 m | -48.8 to -49.0 | 0.09 Hz | -30.8 to -31.0 |
| 20 m | -45.5 to -49.1 | 0.04 Hz | -34.4 to -34.9 |
| 17 m | -48.1 to -49.0 | 0.05 Hz | -32.9 to -33.0 |
| 15 m | -42.6 to -42.9 | 0.03 Hz | -33.1 to -33.2 |
| 12 m | -42.5 to -43.1 | 0.03 Hz | -34.2 to -34.8 |
| 10 m | -43.6 to -44.3 | 0.09 Hz | -37.4 to -38.2 |
| 6 m | -46.0 to -50.9 | 0.01 Hz | -38.8 to -47.7 |
| Xiegu's, 20 / 15 / 6 m | -46.1 to -46.6 / -45.5 to -45.7 / -45.2 to -45.5 | 0.12 Hz | -34.5 / -32.4 to -32.8 / -38.1 to -40.3 |

The image is the signal mirrored around the dial (the X6100's oscillator): within the audio's 3 kHz of the dial, in
the other sideband; out of a band only right at its edge, out of a channel on channelised 60 m. Xiegu's base has the
same image; no X6100 base we decrypted corrects transmit I/Q. The carrier is a few milliwatts whatever the power (the
DC offsets come after the loop's gain): -11.4 to -13.9 dBc at 0.1 W on HF, -22 dBc on 6 m (Xiegu's at its 0.2 W
floor: -18 to -21 dBc). The fix would be a second hook (Q' = g Q + p I before the DAC, per band) and nulling the
carrier with registers 50/51 (they exist, zero here; not yet tried), set by a calibration on the radio (TODO.md).
Seen, not explained: the tone's second harmonic at the dial + 2 fa, -34 to -38 dBc on both bases; broad humps
38-41 kHz either side on the X6200's panadapter on 80, 10 and 6 m, about -42 dB per 100 Hz bin, the same with the
X6200's attenuator out, no tone when the X6200 is tuned onto them.

### Every base compared, 2026-11-20

Decrypted with the public key (nothing of Xiegu's kept in the repository): Xiegu's X6100 V1.1.6 (230307001, from its
v1.1.7 package; MD5 f19fb85d, the exact image gdyuldin patches), V1.1.8 (240915003, from the v1.1.9 package) and
V1.1.9 (251127001); gdyuldin's r1-r13 and RF.Guru's alpha.2; Xiegu's X6200 1.0.3, 1.0.6, 1.0.7 (both), 1.0.8.
Static analysis only (Ghidra headless); V1.1.7 (only in Xiegu's withdrawn v1.1.8 package) not available.

- **X6100 1.1.6 -> 1.1.8**: the hardware preamp never used (PRE = +15 dB digital), RF gain remapped onto the step
  attenuator, an adaptive RX I/Q image balance, the spectrum zoom in the base (register 15 bits 16-19), NR rewritten,
  a PA bias self-check at every start (idle currents measured, bias ramped and saved; likely Xiegu's "TX problem with
  specific hardware"), the fuel gauge no longer reset at each power-on and its own state of charge used (charging
  90-98 %), a new compiler. **1.1.8 -> 1.1.9**: IF shift (register 35: the LO moved, an NCO back), the AGC rewritten
  (register 28's times), the FM demodulator on atan2f; TX power, PA bias, calibration and filters byte-identical.
- **TX power in every X6100 version**: the setting only sets an ALC target in dBm; a closed loop on the forward
  detector (one polynomial for all bands) integrates its gain within -40..0 dB, faster down (0.5) than up (0.01),
  resets to -40 on a band, mode or power change, runs in every mode but CW; from band index 20 (29.7 MHz up) it
  reads 0.9 x the power and adds +6 dB, which raises 6 m's floor to -34 dB. 1.1.6 read 0.9 x on every band (about
  11 % over on HF); 1.1.8 and 1.1.9 1.0 x. The GUI's meter is the same detector.
- **A Xiegu bug in every X6100 version**: the codec's PLL fraction register is written with a constant, so the base
  samples at about 99,932 Hz, not 100 kHz (-676 ppm). gdyuldin's r13 fixes it.
- **gdyuldin's r13**: 25 hooks into V1.1.6, the code from 0x0807f100, its state in CCM RAM; true power control =
  the I/Q scaled by sqrt(P/10) x codec gain x band gain before Xiegu's loop, which then only limits (P = 0 gives full
  drive). On boards shipped with 1.1.8 the battery and I2C trouble come from 1.1.6 itself (its gauge handling), the
  0 V supply from r13 not updating it for about two minutes, the weak first carrier after a band change from Xiegu's
  loop restarting at -40 dB. Its VOX switches TX from the DMA interrupt.
- **RF.Guru alpha.2**: 25 bytes of V1.1.9 changed: five hooks (start-up, per-block set-up, a compressor before the
  TX band-pass, CESSB and r13's interpolation at the TX interpolator), payload 0x08080000, state in CCM, armed only
  by 0x5458 in register 36's upper half, voice LSB/USB only; Xiegu's power control untouched.
- **X6200**: 1.0.6 added the CW decoder (register 32 = its threshold and switch, not the TX pulse of #53); 1.0.7
  SLIP-encodes the 552-byte packets to Linux, rewrote the S-meter (WFM measured only on the raw wideband stream,
  +70 dB) and the AGC; the Sep 2025 1.0.7 and older rewrite the bootloader from a copy at every start (the Jan 2026
  factory build no longer); 1.0.8 fixed the keyer's timing (20 % fast) and the S-meter's -255. Any I2C transaction
  to 0x72 ending in STOP is the keepalive: 1.2 s without one disables TX, then 10 s + 180 s to power-off.
- **The owner's X6100's calibration block** (read 2026-11-20 at I2C 0x100, 816 bytes, the GUI stopped; a copy in
  app_logs/base_cal_x6100_20261120_0100.bin): magic, two words (605, 720), a checksum, and zeros everywhere else:
  no per-band TX I/Q amplitude, phase or DC offset was ever set, so the GUI's zeros overwrite nothing.

For this tool: any of these bases is an .xgf like Xiegu's and goes on with the same write; a clean-room base
that names itself as a patched one (bbfw says `,r8`) would get capabilities from `hw_caps()` it does not have.

## Files

| File | |
|---|---|
| [base_flash.c](base_flash.c) | the writer: `--check FILE`, `--flash FILE --yes [--recover \| --dry-run] [--held-by PID] [--unknown-ok] [--test-ok]`, `--identify FILE` |
| [build.sh](build.sh) | the writer for the radio, static |
| [fetch.sh](fetch.sh) | releases from xiegu.eu into build/base_update, by x6x00-base-fetch |
| [flash.sh](flash.sh) | from a computer: `--check`, `--flash`, `--recover`, `--dry-run` (X6100), `--to VERSION`, `--file FILE`, `--test-ok` |
| [run_on_radio.sh](run_on_radio.sh) | on the radio, apart from the SSH session: GUI off, lock, write, GUI on |
| [x6100_patch.py](x6100_patch.py) | the X6100 V1.1.9 patches (gcfix4; gcfix3 with `--only pll,6m,alc`) made from Xiegu's file and key: `--patch-out` writes the block patch the radios apply with `x6x00-base-patch` |
| [x6100_hooks/](x6100_hooks) | gcfix4's hook (alc_hold.s) and build.sh, which assembles it (arm-none-eabi binutils) and prints the bytes x6100_patch.py holds |
| [install.sh](install.sh) | the 1.0.8 file to `/usr/firmware` on the internal memory, for Xiegu's own UPGRADE screen |
| [test/](test) | run.sh, the unit tests (units.c), the simulated bases (sim.py, sim6100.py), the synthetic update zips (synth.py) |
