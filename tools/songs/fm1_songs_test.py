#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""tools/songs/fm1_songs.py against a simulated FM-1 (base/tests/backup_tool_test.py's): the bank pulled in 512s as
app/songs.c's sg_load() does, checked, kept; an FM-1 that fails (its code in words, exit 1); one that stops asking
(exit 4).    python3 tools/songs/fm1_songs_test.py"""
import io
import struct
import sys
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(ROOT / "base" / "tests"))
sys.path.insert(0, str(ROOT / "base" / "tools"))
import backup_tool_test as BT  # noqa: E402
import fm1_backup as B  # noqa: E402
import fm1_songs as S  # noqa: E402


class SongFM1(BT.FakeFM1):
    def __init__(self, fail=0, **kw):
        super().__init__(b"T1ST" + bytes(12), **kw)
        self.fail, self.bank = fail, None

    def puller(self, n, crc):
        got = bytearray()
        for off in range(0, n, 512):
            got += (yield (off, min(512, n - off)))
        if self.fail or zlib.crc32(got) != crc:
            yield (B.FAIL_ADDR | (self.fail or 9), 8)
            return
        self.bank = bytes(got)
        yield (B.OK_ADDR, 8)

    def rx(self, link, d):
        f = B.parse_frame(d)
        if f and f[0] == S.SONG_REQ and len(f[1]) == 8:
            self.req = struct.unpack("<II", f[1])
            self.pull = self.puller(*self.req)
            self.next(link, None)
            return
        super().rx(link, d)


def main():
    BT.fast()
    files = [str(ROOT / "songs" / "builtin"), str(ROOT / "app" / "tests" / "songs")]
    want = S.build(S.song_files(files), io.StringIO())[32:]
    bad = 0
    for name, dev, code in (("loaded", SongFM1(), 0), ("refused", SongFM1(fail=8), 1),
                            ("stalled", SongFM1(stall_after=1), 4)):
        out = io.StringIO()
        rc = S.main(files, backend=dev, out=out)
        good = rc == code and (dev.bank == want if code == 0 else dev.bank is None)
        print(("ok  " if good else "FAIL") + f" {name}: exit {rc}")
        bad += not good
    rc = S.main(["--check", "/nonexistent.t1song.json"], out=io.StringIO())
    print(("ok  " if rc == 2 else "FAIL") + f" no song files: exit {rc}")
    bad += rc != 2
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
