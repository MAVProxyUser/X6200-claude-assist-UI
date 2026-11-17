#!/usr/bin/env python3
"""The X6200's base keeper (rootfs/usr/sbin/x6200-base-keeper) on the Mac: its own code, the base and the GUI faked.
See the Makefile:

    make          (python3 -I -B keeper_test.py [-v])

1. The block it sends is the GUI's last one with every TX bit and the power-off bit cleared, the rest as it was; a
   file of the wrong size or address is not sent.
2. It sends every period till a GUI runs, then ends, its pid file gone; without a block it only reads the base.
3. A base that does not take a block is tried again the next period; the keeper does not end.
4. start does nothing on an X6100, while a GUI runs, or while a keeper runs already.
"""
import os
import runpy
import shutil
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
KEEPER = os.path.join(HERE, "..", "..", "rootfs", "usr", "sbin", "x6200-base-keeper")
TRX = 12
TX_OFF = 0x40000 | 0x4000 | 0x2000 | 0x10 | 0x100000 | 0x10000


def block_bytes(trx, fill=0x11223344):
    regs = [fill + i for i in range(54)]
    regs[TRX] = trx
    return b"\0\0" + b"".join(r.to_bytes(4, "little") for r in regs)


class Keeper(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(dir=os.environ.get("TMPDIR"))
        self.k = runpy.run_path(KEEPER)["run"].__globals__
        self.block = os.path.join(self.tmp, "block")
        self.sent, self.gui_after, self.calls = [], None, 0

        def gui_running():
            self.calls += 1
            return self.gui_after is not None and self.calls > self.gui_after

        def feed(data):
            self.sent.append(data)
            return True
        self.k.update(BLOCK=self.block, PIDFILE=os.path.join(self.tmp, "pid"), LOG=os.path.join(self.tmp, "log"),
                      PERIOD=0, is_x6200=lambda: True, gui_running=gui_running, feed=feed)

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def write_block(self, data):
        with open(self.block, "wb") as f:
            f.write(data)

    def test_tx_bits_cleared(self):
        kept = 0x00002 | 0x01000 | 0x200000          # sple, atue, bias_drive_off: settings, kept
        self.write_block(block_bytes(kept | TX_OFF))
        b = self.k["block"]()
        self.assertEqual(len(b), 2 + 4 * 54)
        self.assertEqual(int.from_bytes(b[2 + 4 * TRX:6 + 4 * TRX], "little"), kept)
        self.assertEqual(b[:2 + 4 * TRX], block_bytes(0)[:2 + 4 * TRX], "the registers before it as they were")
        self.assertEqual(b[6 + 4 * TRX:], block_bytes(0)[6 + 4 * TRX:], "and after it")

    def test_bad_block_not_sent(self):
        for data in (block_bytes(0)[:-1], block_bytes(0) + b"x", b"\x00\x04" + block_bytes(0)[2:], b""):
            with self.subTest(n=len(data)):
                self.write_block(data)
                self.assertIsNone(self.k["block"]())

    def test_sends_till_a_gui_runs(self):
        self.write_block(block_bytes(0x40000 | 0x2))
        self.gui_after = 5
        self.assertEqual(self.k["run"](), 0)
        self.assertEqual(len(self.sent), 5)
        self.assertTrue(all(s == self.k["block"]() for s in self.sent))
        self.assertEqual(int.from_bytes(self.sent[0][2 + 4 * TRX:6 + 4 * TRX], "little"), 0x2, "TX off")
        self.assertFalse(os.path.exists(self.k["PIDFILE"]))
        with open(self.k["LOG"]) as f:
            self.assertIn("a GUI runs: ended (5 sent)", f.read())

    def test_no_block_reads_only(self):
        self.gui_after = 3
        self.k["run"]()
        self.assertEqual(self.sent, [None, None, None])

    def test_a_block_that_appears_is_taken(self):
        self.gui_after = 4
        calls = []

        def feed(data):
            calls.append(data)
            if len(calls) == 2:
                self.write_block(block_bytes(0))
            return True
        self.k["feed"] = feed
        self.k["run"]()
        self.assertEqual(calls[:2], [None, None])
        self.assertEqual(calls[2:], [block_bytes(0)] * 2)

    def test_base_not_taking_it(self):
        self.write_block(block_bytes(0))
        self.gui_after = 6
        n = []

        def feed(data):
            n.append(1)
            if len(n) <= 3:
                raise OSError(121, "Remote I/O error")
            return True
        self.k["feed"] = feed
        self.k["run"]()
        self.assertEqual(len(n), 6, "tried every period, failures or not")
        with open(self.k["LOG"]) as f:
            log = f.read()
        self.assertIn("the base did not take it", log)
        self.assertIn("the base takes it again after 3 failures", log)

    def test_start_does_nothing_when_it_should_not(self):
        started = []
        popen = self.k["subprocess"].Popen
        self.k["subprocess"].Popen = lambda *a, **kw: started.append(a)
        try:
            self.k["is_x6200"] = lambda: False
            self.k["start"]()
            self.k["is_x6200"] = lambda: True
            self.k["gui_running"] = lambda: True
            self.k["start"]()
            self.k["gui_running"] = lambda: False
            self.k["running_pid"] = lambda: 1234
            self.k["start"]()
            self.assertEqual(started, [])
            self.k["running_pid"] = lambda: None
            self.k["start"]()
            self.assertEqual(len(started), 1)
            self.assertEqual(started[0][0][-1], "run")
        finally:
            self.k["subprocess"].Popen = popen


if __name__ == "__main__":
    unittest.main(verbosity=2 if "-v" in sys.argv else 1, argv=[sys.argv[0]])
