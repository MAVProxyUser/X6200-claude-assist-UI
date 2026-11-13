# The X6200's base firmware, updated from OpenX6X00

The X6200 runs two firmwares: OpenX6X00 on the card (Linux, the GUI), and Xiegu's **base** firmware in the radio
board's own processor (DSP, RF, power, keyer). The base is updated with Xiegu's own system, which stays on the
radio's internal memory: a card swap, a menu, another card swap ([README_original.md, The
baseband](../../README_original.md#the-baseband)). These tools do it from OpenX6X00 instead, over SSH, without
taking the card out:

- **fetch.sh** downloads Xiegu's update from xiegu.eu and takes the base file out of it;
- **build.sh** builds **base_flash**, which writes it into the base the way Xiegu's own app does, step for step;
- **flash.sh** puts both on the radio and runs the write there (`--check` first: it reads only);
- **install.sh**, as an alternative, puts the file where Xiegu's own UPGRADE screen lists it.

Base **1.0.8** (19 August 2026) fixes the S-meter that sticks at -255 dBm on 1.0.7 (OpenX6X00 shows that as S1,
-121.0), corrects the CW keyer's speed and brings back the CW keyer macros. It was written into this fork's X6200
with these tools on 13 November 2026 ([below](#the-first-real-write-13-november-2026)).

## Using it

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

1. **fetch.sh**: Xiegu's update for 1.0.8, `x6200udimg-260820001.zip` (371 MB), into `build/base_update`, checked
   by size and SHA-256. It holds a card image; its third partition (FAT) holds `rootfs.tar`, the system Xiegu's update
   writes to the internal memory, with the base's file in `/usr/firmware`: `x6200_mcu_20260820141208.xgf`, 524288
   bytes, SHA-256 `9a19f97a…f9daa23`. The intermediate files are removed after. Needs 7-Zip (`7zz` or `7z`;
   `brew install sevenzip`).
2. **build.sh**: base_flash for the radio, statically linked, so any ARM hard-float Linux cross compiler builds it
   and it runs whatever the radio's C library is (`arm-linux-gnueabihf-gcc`, or another prefix in `CROSS_COMPILE`).
3. **flash.sh --check** reads only, and never asks the base for its bootloader: the file (only Xiegu's 1.0.8 and
   1.0.7 files are written, known by their SHA-256), what holds the base's serial port and I2C bus (the GUI, while
   it runs), and the base's own version over I2C:

   ```
   it is Xiegu's x6200_mcu_20260820141208.xgf (base 1.0.8)
   /dev/ttyS1 is open in x6100_gui (pid 863)
   /dev/i2c-0 is open in x6100_gui (pid 863)
   the base answers on I2C: ready 0x01, version "1.0.7"
   check done: nothing was sent to the base
   ```

4. **flash.sh --flash** says what it is about to do and asks for `FLASH` to be typed (`--yes` does not ask). On the
   radio, by itself (run_on_radio.sh under `setsid`, so a lost connection does not stop it): the GUI stopped, the
   lock `/etc/init.d/S95gui start` takes held (a GUI start waits for the end of the write), base_flash, the GUI
   started again. flash.sh follows the log; the radio keeps it in `/tmp/base_update/flash.log` and adds it to
   `/mnt/base_update.log`. The write itself takes about half a minute.
5. **Then switch the radio on again.** The base does not start the new firmware by itself: the I2C bus stays locked
   (`mv64xxx: I2C bus locked` in `dmesg`) and the radio switches itself off a minute or two later. Switched on, its
   base runs the new firmware; **flash.sh --check** shows the version, and Settings > Info > Baseband too.

**Power.** Xiegu's own instructions: a 13.8 V supply of 3 A or more and the battery out, for the whole update. Do not
switch the radio off while it writes. (The first real write was done on the radio's 11.8 V external supply with its
battery in and charging, both sources there; it went through, but that is not Xiegu's condition.)

**If it fails.** base_flash says where, with Xiegu's own messages, and its exit status tells flash.sh what to do:

| Exit | Meaning | Then |
|---|---|---|
| 0 | written | switch the radio on again when it switches itself off |
| 1 | refused: nothing was sent to the base (not Xiegu's file, the GUI still running, the base not answering) | fix what it says |
| 2 | the bootloader did not answer its handshake: nothing erased | the GUI is started again; switch the radio off and on |
| 3 | failed at or after the erase: the base has no application now | the GUI is left off; **flash.sh --recover** (do not switch the radio off) |

`--recover` writes the same again with the I2C check left out (a base without its application has nothing to answer
with); the bootloader is asked as before. The bootloader sits in the processor's first sectors, which the erase
(`r`) does not touch, so it should still be there to take the file again. That is how Xiegu's design reads; on an
X6200 it has not been needed yet.

**Back to 1.0.7**: `flash.sh --to 1.0.7`, with Xiegu's 1.0.7 file (`x6200_mcu_20260116092100.xgf`, 401024 bytes,
SHA-256 `9baa46b8…a1e2690`) in `build/base_update`. It is on every X6200's internal memory, in `/usr/firmware` on
`/dev/mmcblk2p2`.

**Or with Xiegu's own screen**: `install.sh root@<radio>` mounts the internal memory, copies the 1.0.8 file into its
`/usr/firmware` (mode 600, as Xiegu's own) and unmounts it. Then: switch off, take the OpenX6X00 card out, switch on
(Xiegu's system starts), MENU > SYSTEM > UPGRADE, choose `x6200_mcu_20260820141208.xgf`, UPGRADE, QUIT, switch off,
put the card back.

## How it works

Xiegu's app 1.0.8 (`/usr/app_qt/x6200_ui_v100` in the same update) was disassembled to find what its UPGRADE screen
does: `UiFwUpgrade::upgradeFirmware` reads the file whole and hands it to a worker thread, `XMWorker::slot_start`,
with the file descriptor of the base's serial port, which its main loop stops reading meanwhile (`BaseComm::pause`).
base_flash does the same, with the same timeouts, the same messages and the same error handling:

1. **The port** (`uart_create`, `BaseComm::start`): `/dev/ttyS1`, raw 8N1 at 1152000 baud, `VMIN` and `VTIME` 0 so
   every read returns at once, opened blocking without a controlling terminal, both queues flushed.
2. **The bootloader asked for**: a `c` on the port and host command `0x800a` over I2C, both again every 100 turns of
   a loop that sleeps 1 ms and reads up to 10240 bytes each turn. The host command is one write to the base at
   address `0x72` on `/dev/i2c-0`: register `0xfffe` high byte first, the command low byte first, `FF FE 0A 80`
   (`BaseCommIIC::iicWrite`; the same bytes as `x6200_control_host_cmd` in Aether's library, which sends `0x8002`
   and `0x8003` at every start). A one-byte read of `Y` or `y` counts; more than 10 end the loop. So do 10000 empty
   reads (`[ERR] Wait IAP respond timeout`) or 10000 others (`[ERR] Clear rx buffer timeout`), and Xiegu's app goes
   on all the same: only the next step decides. The X6200's app uses no GPIO for this (the X6100's updater pulls the
   Morse key line low and resets the base; on the X6200 that line is only the keyer).
3. **The handshakes** (`send_cmd`): `c` for `y` (`[ERR] IAP failed, connect timeout`: nothing erased yet), then `r`,
   the erase, for `g` (`[ERR] IAP failed, can't erase flash`), then `p`, program, for `g` (`[ERR] IAP failed, can't
   start xmodem`). Each byte written, then up to 5000 one-byte reads 1 ms apart for the answer (other bytes passed
   over), written again up to 10 more times (`[ERR] no respond to cmd %c, retry %d`).
4. **The file** (`Xmodem::sendFile`), as it is: XMODEM-1K with a CRC. The bootloader decrypts the `.xgf` itself, so
   no key is needed, here or anywhere. A 2-byte read whose second byte is `C` within 10 s (`1k xmodem timeout.`),
   then 1024-byte packets: STX, the packet number from 1, its complement, the data (the last block padded with
   `0xFF`), CRC-16/XMODEM high byte first. Each answered within 10 s by one byte: ACK the next, CAN `1k xmodem
   transfer canceled.`, NAK `1k xmodem bad packet`, anything else `1k xmodem error`; no retries. Then EOT, answered
   by ACK (`1k xmodem send finished.`).
5. **The end**: Xiegu's app ends every way the same ("finished.", `civ start`, its reading of the port resumed) and
   neither resets the base nor sends it a command. base_flash then looks for the base on I2C for a minute.

What base_flash adds to Xiegu's app, none of it on the wire: the read-only `--check`; only Xiegu's two files written;
the refusal when anything else holds the port or the bus; the hang-up, interrupt and terminate signals ignored once
the base is asked for its bootloader; an exit status for each way it can end; the minute's look for the base after.

## How it was checked

- **tools/base_update/test/run.sh**, on a Mac or a Linux PC, in about two minutes:
  - unit tests: SHA-256 against FIPS 180-2's vectors; the packet's CRC against CRC-16/XMODEM's check value and against
    Xiegu's own CRC routine, rewritten from its disassembly, on 2000 random packets; the packet's frame; Xiegu's two
    files taken, any other refused;
  - base_flash itself, its port on a pseudo-terminal, against a simulated base in each of these:

    | Scenario | The simulated base | base_flash |
    |---|---|---|
    | ok | streams, then answers as the bootloader | the file whole, byte for byte; exit 0 (also with 1.0.7's 392 packets) |
    | silent | says nothing until base_flash has given up waiting | `[ERR] Wait IAP respond timeout`, goes on as Xiegu's app does; exit 0 |
    | stream | keeps streaming until base_flash has given up | `[ERR] Clear rx buffer timeout`, goes on; exit 0 |
    | dead | never answers | `[ERR] IAP failed, connect timeout`; exit 2 |
    | noerase | never answers the erase | `[ERR] IAP failed, can't erase flash`; exit 3 |
    | nostart | never sends the `C` | `1k xmodem timeout.`; exit 3 |
    | nak, can, garbage | NAK at packet 100, CAN at 50, an `X` at 10 | `1k xmodem bad packet`, `… transfer canceled.`, `… error, = 88`; exit 3 |
    | eot | no ACK for the EOT | `1k xmodem error, = 0 (after EOT)`; exit 3 |

- **run_on_radio.sh on the radio**, with a stand-in for base_flash that sends nothing: the GUI stopped, nothing left
  holding the port or the bus, the lock held, the GUI started again.

### The first real write, 13 November 2026

An X6200 on base 1.0.7, its 11.8 V external supply and its battery (89 %, charging) both in. From flash.sh's log:

| Time | |
|---|---|
| 0 | the GUI stopped |
| 4.6 s | the bootloader answers (`Y` 11 times; the I2C command taken twice, then no more answers on I2C as the base left its application) |
| 4.6 s | `c` answered, the erase asked for |
| 9.9 s | `[ERR] no respond to cmd r, retry 10`: the erase takes longer than 5 s, so `r` goes again, as in Xiegu's app |
| 11.3 s | erased (`g`), `p` answered |
| 11.5 s | `1k xmodem start.` |
| 32.4 s | 512 packets (about 41 ms each), EOT acknowledged: `1k xmodem send finished.` |

The base did not answer on I2C after it (`mv64xxx: I2C bus locked`), the GUI was started again, and the radio
switched itself off about two minutes later. Switched on by hand: `the base answers on I2C: ready 0x01, version
"1.0.8"`, and the GUI's own log `BASE fw version: 1.0.8`, `BASE fw date: Aug 19 2026,16:13:08`. The base's
calibration block came through: the PA bias read from it is the radio's own, 3226/3958, as before.

## Files

| File | |
|---|---|
| [fetch.sh](fetch.sh) | Xiegu's 1.0.8 update downloaded and checked, the base file out of it |
| [build.sh](build.sh) | base_flash for the radio, static |
| [base_flash.c](base_flash.c) | the writer, on the radio: `--check FILE`, `--flash FILE --yes [--recover]` |
| [flash.sh](flash.sh) | from a computer: both to the radio, `--check`, `--flash`, `--recover`, `--to 1.0.7` |
| [run_on_radio.sh](run_on_radio.sh) | on the radio, apart from the SSH session: GUI off, lock, write, GUI on |
| [install.sh](install.sh) | the file to `/usr/firmware` on the internal memory, for Xiegu's own UPGRADE screen |
| [test/](test) | run.sh, the unit tests (units.c) and the simulated base (sim.py) |

The X6200 only: the X6100's base is updated another way (its Morse key line held low while the base is reset).
Xiegu's files are Xiegu's: fetch.sh downloads them from Xiegu's site and nothing of them is in this repository.
