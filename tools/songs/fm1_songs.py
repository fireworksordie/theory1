#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Load your songs into the attached FM-1 over USB-MIDI, for THEORY-1's SONGS: no firmware rebuild, and the songs
never leave your computer and the FM-1.

    python3 tools/songs/fm1_songs.py                     every song in songs/personal/
    python3 tools/songs/fm1_songs.py FILE_OR_FOLDER...   these song files (.t1song.json), or every one in a folder
    python3 tools/songs/fm1_songs.py --check ...         only build the bank and list it: nothing is sent
    options: --port NAME (part of the MIDI port's name)

Each load replaces the songs loaded before (the built-in ones stay). The FM-1 keeps 128 KiB of songs: a few hundred.
How it goes (app/songs.c): the songs become one bank (tools/songs/t1song.py), the FM-1 is told its length and
CRC-32, erases its song area, pulls the bank with read requests (as a restore does: base/tools/fm1_backup.py), checks
it as written and only then writes the header that makes it count; a cable pulled half way leaves no songs, never half.

Exit codes: 0 loaded, 1 failed, 2 bad arguments or song files, 3 FM-1 not found, 4 connection lost or the FM-1
stopped answering (a THEORY-1 without SONGS?).
"""
import argparse
import struct
import sys
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(ROOT / "base" / "tools"))
import t1song as T  # noqa: E402

SONG_REQ = 0x70                            # app/songs.c SONG_REQ
ERRORS = {1: "the bank's size (too many songs)", 2: "the transfer (a read request unanswered)", 3: "the header",
          7: "a flash erase", 8: "a flash write", 9: "the check of what was written"}


def song_files(args):
    files = []
    if not args and not (ROOT / "songs" / "personal").is_dir():
        raise T.SongError("no songs/personal/ folder: put your exported songs there, or name the files")
    for a in args or [str(ROOT / "songs" / "personal")]:
        p = Path(a)
        if p.is_dir():
            files += sorted(p.glob("*.t1song.json"))
        else:
            files.append(p)
    return files


def build(files, out=sys.stdout):
    """the song files -> the region (header + bank), the songs listed"""
    if not files:
        raise T.SongError("no song files (export some into songs/personal/ with tools/hooktheory-export)")
    songs = []
    for f in files:
        try:
            songs.append(T.load(f))
        except T.SongError as e:
            print(f"skipped {f.name}: {e}", file=sys.stderr)
    if not songs:
        raise T.SongError("none of the files is a song THEORY-1 can read")
    songs.sort(key=lambda s: (s["title"].lower(), s["artist"].lower()))
    region = T.region_of(T.pack_bank(songs))
    for s in songs:
        print(f"  {s['title']} - {s['artist']}  ({len(s['sections'])} parts)", file=out)
    print(f"{len(songs)} songs, {len(region)} of {T.REGION_MAX} bytes", file=out)
    return region


def push(dev, region, B, out=sys.stdout):
    payload = region[32:]
    dev.link.drain()
    dev.link.send(B.frame(SONG_REQ, struct.pack("<II", len(payload), zlib.crc32(payload))))
    try:
        return B.serve(dev.link, payload, out)
    finally:
        print(file=out)


def main(argv=None, backend=None, out=sys.stdout):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("files", nargs="*", help="song files or folders (default: songs/personal/)")
    ap.add_argument("--port", metavar="NAME", help="MIDI port to use (part of its name)")
    ap.add_argument("--check", action="store_true", help="only build the bank and list the songs")
    a = ap.parse_args(argv)
    try:
        region = build(song_files(a.files), out)
    except (T.SongError, OSError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 2
    if a.check:
        return 0
    import fm1_backup as B
    import fm1_install as I
    B.DELAY["first"] = max(B.DELAY["first"], 15.0)   # (the FM-1 erases its song area before its first request)
    try:
        if backend is None:
            try:
                backend = I.MidoBackend()
            except ImportError:
                raise I.InstallError("usage", "needs mido and python-rtmidi (pip install mido python-rtmidi)")
        dev = B.find(backend, a.port)
        print(f"device: {dev.id.text}  ({dev.name})", file=out)
        try:
            code = push(dev, region, B, out)
        finally:
            dev.link.close()
        if code:
            print(f"error: songs not loaded: {ERRORS.get(code, 'code %d' % code)}", file=sys.stderr)
            return 1
        print("done: the songs are on the FM-1 (SONGS)", file=out)
        return 0
    except I.InstallError as e:
        print(f"error: {e}", file=sys.stderr)
        return B.EXIT.get(e.code, 1)
    except KeyboardInterrupt:
        print("\ninterrupted (the FM-1 keeps no half-loaded songs: load them again)", file=sys.stderr)
        return 130


if __name__ == "__main__":
    sys.exit(main())
