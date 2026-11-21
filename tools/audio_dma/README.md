# The receive audio's 2-second repeat (sun6i-dma position read)

Both radios read their receive audio through the R16's DMA engine (sun6i-dma), PulseAudio's ALSA source holding a
2.000 s ring (88200 frames at 44.1 kHz, periods of 14700). On 6.1.82 the engine's position was read with two
register reads, LLI_ADDR then CUR_CNT; when the engine moved to its next descriptor between them, the residue took
one period too many and the position stepped back a period. ALSA (`snd_pcm_update_hw_ptr0`) takes any step back as
the ring wrapping, so hw_ptr jumped a whole buffer ahead, and PulseAudio read the ring's last 2 s again: every client
(the GUI's decoders, TCI) got the previous 2.000 s twice and ran 2 s late from then on. Mainline fixed the read in
c90b6973daa3 (Aug 2026, Cc stable); kernel patches 9105/9106 (`buildroot/br2_external/board/X6100/patches/linux`)
bring it and 9096bdc8d930 to our 6.1.82.

Measured 2026-11-21 (receive only):

- X6100, `hammer.py` 300 s (15.6k pointer reads a second): one jump of +73504 frames (a buffer less a period);
  `ptrmon.py` saw the net +88213 (1.00 buffer) step, and `rep2s.py`'s 10-minute TCI recording had 602.8 s of audio in
  600.9 s, its blocks at 397.0-398.5 s equal (correlation 1.0000) to the audio exactly 2.00000 s before.
- X6200, `ab.sh` 600 s per side, two hammers (31k reads a second), `x6x00_dmafix` in place of the built-in read:
  fix=0 (6.1.82's read): 19.07 M calls, 6 descriptor changes between the reads, **5 jumps**; fix=1 (mainline's):
  19.11 M calls, 11 retries, **0 jumps**.
- Without the hammer the radio reads the position on the order of a hundred times a second: about one jump in
  4 million reads, so once in many hours (none on either radio in 2.6-3.6 h of capture before the test).

## The tools

- `hammer.py SECONDS` (on a radio): reads `/proc/asound/card0/pcm0c/sub0/status` as fast as one core can (each read
  runs the driver's pointer callback) and prints every hw_ptr jump over half a buffer. Two at once double the rate.
- `ptrmon.py HOST SECONDS` (on the Mac, `ptrmon.sh` on the radio): the capture's hw_ptr against its timestamp every
  5 s; a jump is a step of the offset by one buffer.
- `rep2s.py HOST SECONDS [NAME]` (on the Mac): records the radio's TCI audio and finds blocks equal to the audio
  2.000 s before.
- `x6x00_dmafix/`: a TEST module for a 6.1.82 kernel without 9105. It points the controller's `device_tx_status` at
  a copy of 6.1.82's function whose position read is chosen by its `fix` parameter (1: mainline's, 0: the old one)
  and counts the descriptor changes it meets (`retries`, `crossed`); unloading it puts the built-in function back.
  The structs are 6.1.82's sun6i-dma.c, the headers its drivers/dma/virt-dma.h and dmaengine.h. Built by
  `.github/workflows/rtw88.yml` against the X6200's kernel kit:
  `gh workflow run rtw88.yml -f kit_run=<a kit run> -f extra_patch="$(sh tools/audio_dma/mkpatch.sh)"`; the
  artifact's `kit/x6x00_dmafix.ko`.
- `ab.sh HOST SECONDS KO`: the A/B above.

On a kernel with 9105, `hammer.py 600` (two at once) should show no jump.
