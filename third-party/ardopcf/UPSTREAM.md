# ardopcf (vendored)

ARDOP HF modem (TNC) by Rick Muething KN6KB, John Wiseman G8BPQ and Peter
LaRue AI7YN, as ardopcf 1.0.4.1.3. Pat talks to it on its host ports for
Winlink.

Source: https://github.com/pflarue/ardop
Commit: `e27af046c04f973ed8958a219844f655ead6c419` (2026-09-07)
License: MIT (see `LICENSE`), except the libraries under `lib/`, below

Copied from that commit: `src/`, `lib/`, `webgui/`, `LICENSE`, `changelog.md`,
`Makefile` and `README.md` (not `docs/`, `test/` or `host/`).
`CMakeLists.txt` (the `Makefile`'s Linux build, as part of the GUI's), `txt2c.cmake`
(the WebGui files as C strings, in place of `lib/txt2c`) and this file are ours.

Upstream development has stopped (README.md). The README asks that anything
released from ardopcf be given a new name, and that a modem which is not
over-the-air compatible not use "ardop" in its name; ours stays compatible.

## Licences

- ardopcf itself (`src/`, `webgui/`): MIT, Rick Muething, John Wiseman,
  Peter LaRue (`LICENSE`). It does not cover `lib/`; each library there names
  its own author and licence.
- `lib/ws_server` and `lib/txt2c`: MIT, Peter LaRue (in the file headers).
- `lib/zf_log`: MIT, wonder-mice (`lib/zf_log/LICENSE`).
- `lib/rockliff/rrs.c` (the Reed-Solomon coder): Simon Rockliff's own notice,
  in `lib/rockliff/rrs.txt`. It asks that his name stays in the comments at
  the head of the program:

  > This program may be freely modified and/or given to whoever wants it.
  > A condition of such distribution is that the author's contribution be
  > acknowledged by his name being left in the comments heading the program,
  > however no responsibility is accepted for any financial or other loss
  > which may result from some unforseen errors or malfunctioning of the
  > program during use.
  > Simon Rockliff, 26th June 1991

- `lib/hid` (HIDAPI: GPLv3, BSD-style or its original licence, at the user's
  choice) and the PJRC Raw HID part of `lib/rawhid/rawhid.c` (MIT-style):
  Windows only, not in the Linux build. `lib/hid/LICENSE.txt` names
  `LICENSE-gpl3.txt` and `LICENSE-bsd.txt`, which upstream does not ship.
- The tiny_gpio code in `src/linux/ALSASound.c` (Raspberry Pi GPIO PTT, ARM
  builds): public domain.

## Local patches

Each one is its own commit on top of the pristine import.

- `src/linux/SockSound.c` (new) and hooks in `src/linux/ALSASound.c`: the
  capture device `SOCK:<path>` takes the audio and the PTT through the GUI's
  UNIX socket (protocol at the top of `SockSound.c`) instead of ALSA, which
  is then never opened. `Makefile` builds it too.
- `src/common/TCPHostInterface.c`, `lib/ws_server/ws_server.c`: the host
  ports (TCP 8515 commands, 8516 data, UDP 8515) and the WebGui (`-G`) listen
  on 127.0.0.1 only. Upstream binds them to every interface without any
  authentication, so anyone on the radio's network could key it.
- `src/common/ARDOPCommon.c`, `src/common/HostInterface.c`: a capture or
  playback device name of 80 characters or more (on the command line, or in
  the `CAPTURE` / `PLAYBACK` host commands) is refused instead of copied past
  the end of its 80-byte buffer; the port argument is cut to its buffer.
- `lib/rawhid/rawhid.c`: on Linux `CM108Handle` is the file descriptor its
  `read`/`write`/`close` calls take, not a `hid_device *` (an int-conversion
  warning with older gcc, an error with gcc 14 and later).
- `src/common/ARQ.c`, from upstream `2b1e6d5` (develop, after 1.0.4.1.3): a
  call that gets no answer reports `STATUS CONNECT TO <call> FAILED` on the
  host port and the console, not `END ARQ CALL` (the test was inverted).
  Host and log text only.
- `src/common/HostInterface.c`, from upstream `906a38c` (develop, its
  PROTOCOLMODE test only, not its comment typos): `PROTOCOLMODE` with a mode
  other than ARQ, RXO or FEC answers `FAULT Syntax Err`, instead of
  `PROTOCOLMODE now <mode>` with the mode quietly set to ARQ.
- `src/common/ARQ.c`, `src/common/ARDOPC.c`, `src/common/Modulate.c`, from
  upstream `971957f` (develop, its ID part only, not its reconfiguration
  feature): a ConReq or Ping frame (and its repeats) counts as an ID, as it
  carries our call, so no 10 minute ID frame follows a call nobody answered
  (it keyed by itself about 9 minutes after the call).
- Security review of 2026-11-21 (reachable from the air or through Pat):
  - `src/common/ARQ.c`, `src/common/SoundInput.c`: a DISC heard with nothing
    connected (DISC state, or FEC mode) is answered with END, and then our ID
    frame, only when it carried the ID of our own session that ended within
    60 s (protocol rule 1.5, the case it is for). Before, a DISC from anyone,
    at session ID 0xFF or before any session, keyed the idle radio, once for
    each DISC sent, LISTEN FALSE or not.
  - `lib/rockliff/rrs.c`: `elp` one column wider. With rslen 64 (every 16QAM
    frame type, decoded while idle) and syndromes S1-S63 zero, S64 not,
    `elp[u + 1][i + u - q]` wrote one int, of the sender's choosing, past
    the array's end (`elp[65][64]`, the only write that can leave it).
  - `src/common/TCPHostInterface.c`, `src/common/HostInterface.c`: the
    replies to the host are bounded (`snprintf`, the CR kept); ARQCALL and
    PING echo what was parsed, not the host's own text; their attempt count
    is digits only, 1 to 15 (strtol skipped leading spaces, so a call with a
    thousand spaces before its count passed, and its echo overflowed the
    1024-byte reply buffer; Pat passes a connect URL's target and
    `connect_requests` through from its web page).
