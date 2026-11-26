#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""THEORY-1's songs: from song files (.t1song.json) to the bank the FM-1 keeps in its flash.

    python3 tools/songs/t1song.py show FILE...          the songs as THEORY-1 will show them: sections, chords, numerals
    python3 tools/songs/t1song.py bank OUT.bin FILE...  the bank for tools/songs/fm1_songs.py (or the simulator's "data")
    python3 tools/songs/t1song.py header OUT.h FILE...  the same bank as a C array (the firmware's built-in songs)

A song file is what tools/hooktheory-export saves (format "theory1-hooktheory/1"): sections with a key, a mode, a
tempo and a meter; chords as scale degrees with their type, inversion, added notes, suspensions, secondary (applied)
and borrowed chords; and the melody as scale degrees; all timed in ticks (480 to a beat). The theory is done here,
once: each chord becomes its notes, its symbol and its roman numeral, relative to the section's key, so the firmware
only spells and plays them, in whatever key the player turns to.

The bank (little-endian; every count and offset checked by the firmware, app/songs.c):
  region   32-byte header: "T1SG", u32 payload length, u32 CRC-32 of the payload, 4 x u32 0, u32 CRC-32 of the
           28 bytes before (written last on the FM-1: the commit record); then the payload
  payload  "T1SB", u16 version (1), u16 songs, u32 offset of each song (from the payload's start), the songs
  song     str title, str artist, u8 sections, the sections
  section  str name, u8 tonic letter (0..6, C..B), s8 its accidental, str mode ("major", "minor"...), u16 BPM,
           u8 beats a bar, u8 beat unit, u16 start, u16 end (in units), u16 chords, u16 notes, the chords, the notes
  chord    u16 start, u16 length (units), u8 root (letter steps << 4 | semitones above the tonic), u8 bass (the same),
           u8 function (FN_*), u8 tones, tones x u8 (semitones above the tonic, root first), str suffix ("m7",
           "sus4"), str numeral ("V7/vi"); a rest: no tones, empty strings
  note     u16 start, u16 length (units), s8 semitones above the tonic's C4-octave note (60 + its pitch class)
  str      u8 length, then UTF-8 (Latin-1 letters only: the FM-1 draws those accents)
  unit     40 ticks, a twelfth of a beat: sixteenths and triplets both land on it
"""
import json
import re
import struct
import sys
import unicodedata
import zlib
from pathlib import Path

TPB = 480
UNIT = 40
MAGIC_REGION, MAGIC_BANK, VERSION = b"T1SG", b"T1SB", 1
REGION_MAX = 32 * 4096                     # the FM-1's song region (app/songs.c SONG_SECTORS)

LETTERS = "CDEFGAB"
LETTER_PC = [0, 2, 4, 5, 7, 9, 11]
MODES = {                                  # TheoryTab's mode names -> the scale's semitones
    "major": [0, 2, 4, 5, 7, 9, 11], "ionian": [0, 2, 4, 5, 7, 9, 11],
    "minor": [0, 2, 3, 5, 7, 8, 10], "aeolian": [0, 2, 3, 5, 7, 8, 10],
    "dorian": [0, 2, 3, 5, 7, 9, 10], "phrygian": [0, 1, 3, 5, 7, 8, 10], "lydian": [0, 2, 4, 6, 7, 9, 11],
    "mixolydian": [0, 2, 4, 5, 7, 9, 10], "locrian": [0, 1, 3, 5, 6, 8, 10],
    "harmonicminor": [0, 2, 3, 5, 7, 8, 11], "phrygiandominant": [0, 1, 4, 5, 7, 8, 10],
    "melodicminor": [0, 2, 3, 5, 7, 9, 11],
}
MAJOR = MODES["major"]
HARMONIC_MINOR = MODES["harmonicminor"]
# what a chord does, in the firmware's words (app/songs.c FN_*)
FN_HOME, FN_AWAY, FN_TENSION, FN_COLOR, FN_BORROWED, FN_APPLIED, FN_REST = range(7)
DEGREE_FN = {1: FN_HOME, 2: FN_AWAY, 3: FN_COLOR, 4: FN_AWAY, 5: FN_TENSION, 6: FN_COLOR, 7: FN_TENSION}


class SongError(Exception):
    pass


def mode_of(name):
    key = re.sub(r"[^a-z]", "", (name or "major").lower())
    if key not in MODES:
        raise SongError(f"unknown mode {name!r}")
    return key, MODES[key]


def parse_tonic(s):
    """'F#' -> (letter 3, accidental +1)"""
    m = re.fullmatch(r"\s*([A-Ga-g])\s*(#{1,2}|b{1,2}|♯|♭)?\s*", s or "")
    if not m:
        raise SongError(f"unknown key {s!r}")
    acc = m.group(2) or ""
    return LETTERS.index(m.group(1).upper()), (len(acc) if acc.startswith(("#", "♯")) else -len(acc))


def parse_degree(sd):
    """a melody note's scale degree: '3', 'b5', '#4' -> (degree 1..7, accidental)"""
    m = re.fullmatch(r"([#b]*)([1-7])", str(sd))
    if not m:
        raise SongError(f"unknown scale degree {sd!r}")
    return int(m.group(2)), m.group(1).count("#") - m.group(1).count("b")


def fold_text(s, limit):
    """Latin-1 letters kept (the FM-1 draws their accents), the rest folded to ASCII"""
    out = []
    for ch in s or "":
        if ord(ch) < 256 and (ch.isprintable() or ch == " "):
            out.append(ch)
        else:
            base = unicodedata.normalize("NFKD", ch).encode("ascii", "ignore").decode()
            out.append(base or {"’": "'", "‘": "'", "“": '"', "”": '"', "–": "-", "—": "-", "…": "..."}.get(ch, ""))
    t = re.sub(r"\s+", " ", "".join(out)).strip()
    while len(t.encode()) > limit:
        t = t[:-1]
    return t


def stack(scale, degree, count):
    """count notes in thirds from degree (1-based) of the scale: [(letter steps, semitones)] from the scale's tonic"""
    out = []
    for i in range(count):
        d = degree - 1 + 2 * i
        out.append((d, scale[d % 7] + 12 * (d // 7)))
    return out


TYPE_TONES = {5: 3, 7: 4, 9: 5, 11: 6, 13: 7}


def chord_tones(c, scale, mode_name):
    """a TheoryTab chord -> its tones as (letter steps, semitones) from the key's tonic, root first; the index of the
    bass among them; and what it does (FN_*)"""
    degree, applied = int(c.get("root", 1)), int(c.get("applied", 0) or 0)
    ctype = int(c.get("type", 5))
    if ctype not in TYPE_TONES or not 1 <= degree <= 7 or not 0 <= applied <= 7:
        raise SongError(f"unknown chord (root {degree}, type {ctype}, applied {applied})")
    borrowed = c.get("borrowed") or ""
    fn = DEGREE_FN[degree]
    if applied:
        # V/x, vii/x, IV/x...: TheoryTab's root is the function (V) and `applied` the target (x): degree `root` of the
        # major scale built on the target (vii7/x: diminished, from the harmonic minor)
        src, off_steps, off_semis, r0 = (HARMONIC_MINOR if degree == 7 and ctype >= 7 else MAJOR,
                                         applied - 1, scale[applied - 1], degree - 1)
        fn = FN_APPLIED
    else:
        src, off_steps, off_semis, r0 = scale, 0, 0, degree - 1
        if isinstance(borrowed, str) and borrowed:
            try:
                src = mode_of(borrowed)[1]
                fn = FN_BORROWED
            except SongError:
                pass                       # an unknown borrowing: the key's own chord
        elif isinstance(borrowed, list) and len(borrowed) == 7:
            src = [int(x) % 12 for x in borrowed]   # a custom scale, as semitones
            fn = FN_BORROWED

    def tone(n):                           # the chord's n-th (1, 3, 5, 7, 9...) from its own scale
        d = r0 + n - 1
        return (off_steps + d, off_semis + src[d % 7] + 12 * (d // 7))

    by_num = {n: tone(n) for n in range(1, 2 * TYPE_TONES[ctype], 2)}
    if fn == FN_BORROWED and [by_num[n][1] % 12 for n in by_num] == \
            [(scale[(degree - 1 + n - 1) % 7]) % 12 for n in by_num]:
        fn = DEGREE_FN[degree]             # "borrowed" yet the same chord as the key's own
    sus = [int(x) for x in c.get("suspensions") or []]
    if sus:
        by_num.pop(3, None)
        for s_ in sus:
            by_num[s_] = tone(s_)
    for a in c.get("adds") or []:
        by_num[int(a)] = tone(int(a))
    for o in c.get("omits") or []:
        if int(o) != 1:
            by_num.pop(int(o), None)
    for alt in c.get("alterations") or []:
        m = re.fullmatch(r"([#b])(\d+)", str(alt))
        if m and int(m.group(2)) in by_num:
            n = int(m.group(2))
            st, t = by_num[n]
            by_num[n] = (st, t + (1 if m.group(1) == "#" else -1))
    if fn == FN_TENSION and degree == 7 and by_num[1][1] % 12 == 10:
        fn = FN_AWAY                       # the subtonic (bVII, VII in minor): a step away, no leading tone
    nums = sorted(by_num)
    tones = [by_num[n] for n in nums]
    # the bass: the inversion counts root, third (or its suspension), fifth, seventh
    order = [n for n in (1, 3 if 3 in by_num else 4 if 4 in by_num else 2, 5, 7) if n in by_num]
    inv = int(c.get("inversion", 0) or 0)
    bass = nums.index(order[inv]) if inv < len(order) else 0
    return tones, bass, fn


def suffix_of(semis):
    """a chord's symbol after its root, from its tones' semitones above the root"""
    s = {x % 12 for x in semis}
    third = "M" if 4 in s else "m" if 3 in s else ""
    fifth = 7 if 7 in s else 6 if 6 in s and third == "m" and 4 not in s else 8 if 8 in s and third == "M" else None
    sus = "" if third else ("sus4" if 5 in s else "sus2" if 2 in s else "")
    seventh = 11 if 11 in s else 10 if 10 in s else 9 if (third == "m" and fifth == 6 and 9 in s) else None
    used = {0, 4 if third == "M" else 3 if third == "m" else None, fifth, seventh, 5 if sus == "sus4" else None,
            2 if sus == "sus2" else None}
    if third == "m" and fifth == 6:
        base = "dim7" if seventh == 9 else "m7b5" if seventh == 10 else "dim" if seventh is None else "m(maj7)b5"
    elif third == "M" and fifth == 8:
        base = "aug" if seventh is None else "+7" if seventh == 10 else "+maj7"
    else:
        q = "m" if third == "m" else ""
        if seventh == 11:
            base = q + "maj7" if q == "" else "m(maj7)"
        elif seventh == 10:
            base = q + "7"
        elif 9 in s and seventh is None and third:
            base, used = q + "6", used | {9}
        else:
            base = q
        base += sus
        if fifth is None and third:
            base += "(no5)" if 6 not in s and 8 not in s else ("b5" if 6 in s else "#5")
            used |= {6, 8}
        elif not third and not sus:
            base += "5" if fifth == 7 and seventh is None else "(no3)" if fifth == 7 else "(no3,no5)"
        elif fifth is None:
            base += "(no5)"
    rest = sorted(s - {u for u in used if u is not None})
    ext = {1: "b9", 2: "9", 3: "#9", 5: "11", 6: "#11", 8: "b13", 9: "13"}
    if rest and seventh is not None and "7" in base and not base.startswith(("dim7", "m7b5")):
        top = 7                            # stacked thirds: 7 -> 9 -> 11 -> 13, the rest as tensions
        for semi, n in ((2, 9), (5, 11), (9, 13)):
            if semi not in rest:
                break
            rest.remove(semi)
            top = n
        base = base.replace("7", str(top), 1)
        if rest:
            base += "(" + ",".join(ext.get(r, str(r)) for r in rest) + ")"
    elif rest:
        base += "(add" + ",".join(ext.get(r, str(r)) for r in rest) + ")"
    return base


ROMAN = ["I", "II", "III", "IV", "V", "VI", "VII"]


def numeral_of(c, tones, bass, scale):
    """a roman numeral for a chord, when the song file gives none (the export's are TheoryTab's own)"""
    degree, applied = int(c.get("root", 1)), int(c.get("applied", 0) or 0)
    semis = [t - tones[0][1] for _, t in tones]
    sfx = suffix_of(semis)
    minor = sfx.startswith("m") and not sfx.startswith("maj")
    dim = sfx.startswith("dim") or "b5" in sfx
    r = ROMAN[degree - 1]
    if "sus" in sfx or "no3" in sfx or sfx.startswith("5"):     # no third: the key's own chord says the case
        minor = (scale[(degree + 1) % 7] - scale[degree - 1]) % 12 == 3 and not applied
    r = r.lower() if minor or dim else r
    acc = ""
    if not applied:
        diff = (tones[0][1] - scale[degree - 1]) % 12     # against the key's own scale, as TheoryTab writes them
        acc = {1: "#", 11: "b", 10: "bb", 2: "##"}.get(diff, "")
    q = "o" if sfx.startswith("dim") else "/o" if "m7b5" in sfx else "+" if sfx.startswith(("aug", "+")) else ""
    seven = "7" if re.search(r"7|9|11|13", re.sub(r"\(.*?\)", "", sfx)) else ""
    inv = ["", "6", "64", "42"][min(bass, 3)] if not seven else ["7", "65", "43", "42"][min(bass, 3)]
    out = acc + r + q + (inv if seven else inv)
    if "sus" in sfx:
        out += sfx[sfx.index("sus"):sfx.index("sus") + 4]
    if applied:
        t = ROMAN[applied - 1]
        tq = suffix_of([x - scale[applied - 1] for x in (scale[(applied - 1 + 2 * i) % 7] + 12 * ((applied - 1 + 2 * i) // 7)
                                                          for i in range(3))])
        out += "/" + (t.lower() if tq.startswith("m") or tq.startswith("dim") else t)
    return out


ASCII_NUM = {"°": "o", "ø": "/o", "♭": "b", "♯": "#", "Δ": "maj", "⁶": "6", "⁴": "4", "⁷": "7"}


def clean_numeral(s):
    return "".join(ASCII_NUM.get(ch, ch) for ch in s)[:23]


def ticks_units(t):
    return int(round(int(t) / UNIT))


def merge_keys(keys):
    """TheoryTab starts a new segment at a meter change too (one 2/4 bar in a 4/4 verse): only a change of key or
    mode makes a part of its own. A merged run takes the meter that covers most of it."""
    out = []
    for k in sorted(keys, key=lambda k: k.get("startTick", 0)):
        if out and (out[-1]["tonic"], out[-1]["scale"]) == (k.get("tonic"), k.get("scale")):
            m = out[-1]
            m["_runs"].append(k)
            if "endTick" in k:
                m["endTick"] = k["endTick"]
            else:
                m.pop("endTick", None)
            continue
        out.append(dict(k, _runs=[k]))
    for m in out:
        runs = m.pop("_runs")
        def span(k):
            return (k.get("endTick") or 0) - k.get("startTick", 0)
        best = max(runs, key=span)
        m["beats"], m["beatUnit"] = best.get("beats"), best.get("beatUnit")
    return out


def convert(doc, where="song"):
    """a song file's JSON -> the song as the bank holds it (a dict, for show and pack)"""
    if doc.get("format") != "theory1-hooktheory/1":
        raise SongError(f"{where}: not a THEORY-1 song file (format {doc.get('format')!r})")
    tpb = int(doc.get("ticksPerBeat", TPB))
    scale_t = TPB / tpb
    song = {"title": fold_text(doc.get("title"), 48) or "?", "artist": fold_text(doc.get("artist"), 40),
            "sections": []}
    for sec in doc.get("sections") or []:
        keys = sec.get("keys") or [{"startTick": 0, "tonic": sec.get("key"), "scale": sec.get("scale"),
                                    "beats": sec.get("meter", 4), "beatUnit": 4}]
        keys = merge_keys(keys)
        chords = sorted(sec.get("chords") or [], key=lambda c: c["startTick"])
        melody = sorted(sec.get("melody") or [], key=lambda n: n["startTick"])
        tokens = (sec.get("numerals") or "").split()
        real = [c for c in chords if not c.get("isRest")]
        use_tokens = len(tokens) == len(real)
        ti, first_part = 0, len(song["sections"])
        for ki, k in enumerate(keys):     # a key change inside a section: a part of its own
            k0 = k.get("startTick", 0)
            k1 = keys[ki + 1]["startTick"] if ki + 1 < len(keys) else 1 << 30
            tl, ta = parse_tonic(k.get("tonic") or sec.get("key"))
            mname, scale = mode_of(k.get("scale") or sec.get("scale"))
            tonic_pc = (LETTER_PC[tl] + ta) % 12
            part = {"name": fold_text(sec.get("name") or "Section", 24) + (f" ({ki + 1})" if len(keys) > 1 else ""),
                    "tonic": (tl, ta), "mode": mname if mname != "harmonicminor" else "harmonic minor",
                    "bpm": max(20, min(300, int(round(float(sec.get("bpm") or 100))))),
                    "beats": int(k.get("beats") or sec.get("meter") or 4), "unit": int(k.get("beatUnit") or 4),
                    "chords": [], "notes": []}
            if part["mode"] == "phrygiandominant":
                part["mode"] = "phrygian dominant"
            if part["mode"] == "melodicminor":
                part["mode"] = "melodic minor"
            for c in chords:
                if not (k0 <= c["startTick"] < k1):
                    continue
                st, ln = ticks_units(c["startTick"] * scale_t), max(1, ticks_units(c["durationTicks"] * scale_t))
                if c.get("isRest"):
                    part["chords"].append({"start": st, "len": ln, "root": (0, 0), "bass": (0, 0), "fn": FN_REST,
                                           "tones": [], "suffix": "", "numeral": ""})
                    continue
                tones, bass, fn = chord_tones(c, scale, mname)
                num = clean_numeral(tokens[ti]) if use_tokens else numeral_of(c, tones, bass, scale)
                ti += 1
                semis = [t - tones[0][1] for _, t in tones]
                part["chords"].append({
                    "start": st, "len": ln, "fn": fn,
                    "root": (tones[0][0] % 7, tones[0][1] % 12), "bass": (tones[bass][0] % 7, tones[bass][1] % 12),
                    "tones": [t % 12 for _, t in tones], "suffix": suffix_of(semis), "numeral": num})
            for n in melody:
                if n.get("isRest") or not (k0 <= n["startTick"] < k1):
                    continue
                d, acc = parse_degree(n["sd"])
                rel = scale[d - 1] + acc + 12 * int(n.get("octave", 0))
                part["notes"].append({"start": ticks_units(n["startTick"] * scale_t),
                                      "len": max(1, ticks_units(n["durationTicks"] * scale_t)),
                                      "rel": max(-60, min(60, rel))})
            if not part["chords"] and not part["notes"]:
                continue                   # an empty part (a melody-only section exported without its melody)
            bar = part["beats"] * 12 * 4 // part["unit"]
            spans = [(c["start"], c["start"] + c["len"]) for c in part["chords"]] + \
                    [(n["start"], n["start"] + n["len"]) for n in part["notes"]]
            first, last = min(a for a, _ in spans), max(b for _, b in spans)
            if "endTick" in k:             # the bars the page drew, from the part's start
                part["from"] = ticks_units(k0 * scale_t)
                part["to"] = ticks_units(k["endTick"] * scale_t)
            else:
                part["from"] = first - first % bar
                part["to"] = 1 << 30
            if ki + 1 == len(keys):        # the section's end: no bars left empty after what is written (TheoryTab
                end = last + (-(last - part["from"]) % bar)   # sections often end in blank bars: silence, then on)
                part["to"] = min(part["to"], end)
            elif part["to"] >= 1 << 30:
                part["to"] = last + (-(last - part["from"]) % bar)
            song["sections"].append(part)
        if len(song["sections"]) == first_part + 1 and len(keys) > 1:   # only one key's part had anything in it
            song["sections"][-1]["name"] = fold_text(sec.get("name") or "Section", 24)
    if not song["sections"]:
        raise SongError(f"{where}: no chords or melody in any section")
    if len(song["sections"]) > 40:
        raise SongError(f"{where}: more than 40 sections")
    return song


def load(path):
    try:
        doc = json.loads(Path(path).read_text(encoding="utf-8"))
    except (OSError, ValueError) as e:
        raise SongError(f"{path}: {e}")
    return convert(doc, str(path))


def pstr(s):
    b = s.encode("utf-8")
    if len(b) > 255:
        raise SongError("text too long")
    return bytes([len(b)]) + b


def u16(v, what):
    if not 0 <= v <= 0xFFFF:
        raise SongError(f"{what} out of range ({v})")
    return struct.pack("<H", v)


def pack_song(song):
    out = bytearray(pstr(song["title"]) + pstr(song["artist"]) + bytes([len(song["sections"])]))
    for p in song["sections"]:
        tl, ta = p["tonic"]
        out += pstr(p["name"]) + struct.pack("<Bb", tl, ta) + pstr(p["mode"])
        out += u16(p["bpm"], "bpm") + bytes([p["beats"], p["unit"]])
        out += u16(p["from"], "start") + u16(p["to"], "end")
        out += u16(len(p["chords"]), "chords") + u16(len(p["notes"]), "notes")
        for c in p["chords"]:
            out += u16(c["start"], "chord start") + u16(min(c["len"], 0xFFFF), "chord length")
            out += bytes([c["root"][0] << 4 | c["root"][1], c["bass"][0] << 4 | c["bass"][1], c["fn"], len(c["tones"])])
            out += bytes(c["tones"]) + pstr(c["suffix"]) + pstr(c["numeral"])
        for n in p["notes"]:
            out += u16(n["start"], "note start") + u16(min(n["len"], 0xFFFF), "note length") + struct.pack("<b", n["rel"])
    return bytes(out)


def pack_bank(songs):
    head = MAGIC_BANK + struct.pack("<HH", VERSION, len(songs))
    blobs = [pack_song(s) for s in songs]
    off = len(head) + 4 * len(blobs)
    table = b""
    for b in blobs:
        table += struct.pack("<I", off)
        off += len(b)
    return head + table + b"".join(blobs)


def region_of(payload):
    h = MAGIC_REGION + struct.pack("<II", len(payload), zlib.crc32(payload)) + bytes(16)
    region = h + struct.pack("<I", zlib.crc32(h)) + payload
    if len(region) > REGION_MAX:
        raise SongError(f"the songs take {len(region)} bytes: the FM-1 keeps {REGION_MAX} (fewer songs)")
    return region


SPELL_ACC = {-2: "bb", -1: "b", 0: "", 1: "#", 2: "##"}


def spell(tonic, steps, semis):
    tl, ta = tonic
    letter = (tl + steps) % 7
    target = (LETTER_PC[tl] + ta + semis) % 12
    d = (target - LETTER_PC[letter]) % 12
    d = d - 12 if d > 6 else d
    return LETTERS[letter] + SPELL_ACC.get(d, "?")


def show(song, out=sys.stdout):
    print(f"{song['title']}" + (f" - {song['artist']}" if song["artist"] else ""), file=out)
    for p in song["sections"]:
        tonic = spell(p["tonic"], 0, 0)
        print(f"  [{p['name']}] {tonic} {p['mode']}, {p['bpm']} BPM, {p['beats']}/{p['unit']}, "
              f"{len(p['chords'])} chords, {len(p['notes'])} melody notes", file=out)
        row = []
        for c in p["chords"]:
            if not c["tones"]:
                row.append("-")
                continue
            sym = spell(p["tonic"], *c["root"]) + c["suffix"]
            if c["bass"] != c["root"]:
                sym += "/" + spell(p["tonic"], *c["bass"])
            row.append(f"{sym}({c['numeral']})")
        print("    " + " ".join(row), file=out)


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if len(argv) < 2 or argv[0] not in ("show", "bank", "header"):
        print(__doc__, file=sys.stderr)
        return 2
    try:
        if argv[0] == "show":
            for f in argv[1:]:
                show(load(f))
            return 0
        out, files = argv[1], argv[2:]
        songs = [load(f) for f in files]
        songs.sort(key=lambda s: (s["title"].lower(), s["artist"].lower()))
        region = region_of(pack_bank(songs))
        if argv[0] == "bank":
            Path(out).write_bytes(region)
        else:
            body = ",\n".join("    " + ", ".join(f"0x{b:02x}" for b in region[i:i + 16]) for i in range(0, len(region), 16))
            Path(out).write_text("/* generated by tools/songs/t1song.py header from songs/builtin: the built-in songs */\n"
                                 f"static const uint8_t SONGS_BUILTIN[{len(region)}] = {{\n{body}\n}};\n")
        print(f"{len(songs)} songs, {len(region)} bytes -> {out}")
        return 0
    except SongError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
