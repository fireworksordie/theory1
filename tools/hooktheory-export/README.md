# THEORY-1 song export (Firefox)

A small Firefox add-on that saves the TheoryTab song you are looking at on hooktheory.com as a THEORY-1 song
file (`.t1song.json`), so the FM-1's SONGS mode can walk through it and play it back.

The file keeps, for every section (Verse, Chorus, …):

- the key and mode, the tempo (BPM) and the meter
- every chord as a scale degree with its type, inversion, added notes, suspensions, secondary (applied) and
  borrowed chords, and where it starts and how long it lasts (480 ticks to a beat)
- the melody, if **melody** is ticked: each note as a scale degree, octave, start and length
- a section that is empty on TheoryTab itself (0% chords and melody, like *Instrumental* on Tiny Dancer) is
  kept in its place with `"empty": true`, no chords, and the numerals the site lists for it
- the numerals as TheoryTab writes them (`i7 III VIIsus4 iv7sus4`), the song's title, artist and page address

## Install

**For a quick try** (gone when Firefox restarts):

1. Open `about:debugging#/runtime/this-firefox` in Firefox.
2. Click **Load Temporary Add-on…** and choose `manifest.json` in this folder.

**To keep it installed:** Firefox only keeps signed add-ons. Sign it once for yourself, unlisted (it is not
published anywhere), with Mozilla's free tool:

```sh
npm install --global web-ext
cd tools/hooktheory-export
web-ext sign --channel=unlisted --api-key=YOUR_JWT_ISSUER --api-secret=YOUR_JWT_SECRET
```

The keys come from your addons.mozilla.org account, under *Developer Hub → Manage API Keys*. The signed `.xpi`
lands in `web-ext-artifacts/`; drag it onto a Firefox window to install it. (Firefox Developer Edition or Nightly
can instead run it unsigned with `xpinstall.signatures.required` set to `false` in `about:config`.)

## Use

1. Open a song on TheoryTab, such as `https://www.hooktheory.com/theorytab/view/<artist>/<song>`.
2. Press **Export to THEORY-1** at the bottom right. It switches the page to *All Sections*, scrolls through them
   so each one loads, and saves the file. This takes a few seconds; the message under the button names the
   section it is reading.
3. Put the file in `songs/personal/` in your copy of this repository.

## Your songs stay yours

TheoryTab's analyses are written by its community and Hooktheory's
[terms](https://www.hooktheory.com/terms) don't allow copying them in bulk or sharing them. So the add-on is built
for one person learning their own songs:

- It reads one song, the one on your screen, only when you press the button. It makes no requests of its own and
  never visits other pages.
- Each file records the page it came from and is marked `"personalUse": true`.
- `songs/personal/` and `*.t1song.json` are in `.gitignore`, so the files are never committed or published with the
  firmware.

## If it stops working

The add-on reads what the page has already drawn, so a redesign of TheoryTab can break it. The message under the
button then says what failed; the code to update is `page.js`.
