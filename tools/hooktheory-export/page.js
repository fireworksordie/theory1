/* SPDX-License-Identifier: MPL-2.0
 * THEORY-1 song export: runs in the TheoryTab page itself (injected by content.js), reads the song that is on the
 * screen and hands it back to the content script. It reads only what the page has already loaded: no requests of
 * its own, one song, when the button is pressed. */
(function () {
  if (window.__t1Export) return;
  window.__t1Export = true;

  const TPB = 480;                          /* TheoryTab's ticks per beat (a 4/4 bar is 1920) */
  const sleep = ms => new Promise(r => setTimeout(r, ms));

  /* the page's server data: the song, its sections with key, mode, tempo, meter and numerals */
  function rscText() {
    let s = "";
    for (const sc of document.scripts) {
      const t = sc.textContent;
      if (!t.startsWith("self.__next_f")) continue;
      try {
        const a = JSON.parse(t.slice(t.indexOf("(") + 1, t.lastIndexOf(")")));
        if (typeof a[1] === "string") s += a[1];
      } catch (e) { /* not a data chunk */ }
    }
    return s;
  }
  /* the JSON value that starts at s[i] ({...} or [...]), cut by bracket counting */
  function jsonAt(s, i) {
    let depth = 0, str = false, esc = false;
    for (let j = i; j < s.length; j++) {
      const c = s[j];
      if (str) { if (esc) esc = false; else if (c === "\\") esc = true; else if (c === '"') str = false; continue; }
      if (c === '"') str = true;
      else if (c === "{" || c === "[") depth++;
      else if (c === "}" || c === "]") { if (--depth === 0) return JSON.parse(s.slice(i, j + 1)); }
    }
    return null;
  }
  function songMeta() {
    const s = rscText();
    const out = { song: null, sections: [] };
    const si = s.indexOf('"song":{"title"');
    if (si >= 0) out.song = jsonAt(s, si + 7);
    const ci = s.indexOf('"sections":[{"wikiHash"');
    if (ci >= 0) out.sections = jsonAt(s, ci + 11) || [];
    return out;
  }

  /* React keeps each staff's chords and notes in its props: walk up from the drawn staff to find them */
  function fiberOf(el) { const k = Object.keys(el).find(k => k.startsWith("__reactFiber")); return k ? el[k] : null; }
  function staffProps(el) {
    for (let f = fiberOf(el), d = 0; f && d < 30; d++, f = f.return) {
      const p = f.memoizedProps;
      if (p && typeof p === "object" && p.chordEntries && p.noteEntries && p.line) return p;
    }
    return null;
  }
  const pick = (o, keys) => Object.fromEntries(keys.filter(k => k in o).map(k => [k, o[k]]));

  /* the page draws a section only once it is on the screen: bring it there, and wait until its chords are in */
  function loaded(el) {
    return [...el.querySelectorAll("[data-chord-staff]")].some(st => { const p = staffProps(st); return p && p.chordEntries.length; });
  }
  async function bringIn(el) {
    for (let i = 0; i < 100 && !loaded(el); i++) {         /* up to 20 s, nudging the page every 2 s */
      if (i % 10 === 0) { el.scrollIntoView({ block: "start" }); window.dispatchEvent(new Event("scroll")); }
      await sleep(200);
    }
  }
  /* "All Sections" lays every section out on the page; a single section's tab shows only that one */
  async function showAllSections() {
    const tab = [...document.querySelectorAll("a.tb-section-tab, .tb-section-tab-bar a")]
      .find(a => a.textContent.trim().toLowerCase() === "all sections");
    if (tab && !tab.classList.contains("active")) { tab.click(); await sleep(800); }
  }

  async function readSection(el) {
    await bringIn(el);
    const chords = new Map(), notes = new Map(), segs = new Map();
    for (const st of el.querySelectorAll("[data-chord-staff]")) {
      const p = staffProps(st);
      if (!p) continue;
      for (const e of p.chordEntries) {
        const c = e.chordAtom && e.chordAtom.init;
        if (c) chords.set(c.id, pick(c, ["root", "startTick", "durationTicks", "type", "inversion", "applied", "adds",
          "omits", "alterations", "suspensions", "borrowed", "pedal", "isRest"]));
      }
      for (const e of p.noteEntries) {
        const n = e.noteAtom && e.noteAtom.init;
        if (n) notes.set(n.id, pick(n, ["sd", "octave", "startTick", "durationTicks", "isRest", "voice"]));
      }
      for (const g of p.line.segments || [])
        segs.set(g.startTick, { startTick: g.startTick, endTick: g.endTick, tonic: g.tonic, scale: g.scaleName,
                                beats: g.numBeats, beatUnit: g.beatUnit });
    }
    const byTick = (a, b) => a.startTick - b.startTick;
    return { chords: [...chords.values()].sort(byTick), melody: [...notes.values()].sort(byTick),
             keys: [...segs.values()].sort(byTick) };
  }

  async function exportSong(withMelody) {
    const meta = songMeta();
    await showAllSections();
    const els = [...document.querySelectorAll("[data-section-url]")];
    if (!els.length) throw new Error("No song sections on this page. Open a song's TheoryTab page.");
    /* after moving between songs inside the site, the page's first data is still the old song's */
    if (!els.every(el => meta.sections.some(s => s.url === el.getAttribute("data-section-url"))))
      throw new Error("Reload this page (F5), then press Export again.");
    const y = window.scrollY, sections = [];
    for (const el of els) {
      const url = el.getAttribute("data-section-url");
      const m = meta.sections.find(s => s.url === url) || {};
      const data = await readSection(el);
      if (!data.chords.length) throw new Error("The section \"" + (m.name || url) + "\" did not load. Scroll through the song once and try again.");
      if (!withMelody) delete data.melody;
      sections.push(Object.assign({ name: m.name || url, url, key: m.key, scale: m.scale, bpm: m.tempo,
                                    meter: m.meter, numerals: m.rel }, data));
    }
    window.scrollTo(0, y);
    return {
      format: "theory1-hooktheory/1",
      title: meta.song ? meta.song.title : document.title,
      artist: meta.song ? meta.song.artist : "",
      source: location.href.split("#")[0],
      exported: new Date().toISOString().slice(0, 10),
      ticksPerBeat: TPB,
      personalUse: true,
      sections
    };
  }

  window.addEventListener("message", async ev => {
    if (ev.source !== window || !ev.data || ev.data.t1 !== "export") return;
    try {
      const song = await exportSong(!!ev.data.melody);
      window.postMessage({ t1: "result", song }, location.origin);
    } catch (e) {
      window.postMessage({ t1: "error", message: String(e && e.message || e) }, location.origin);
    }
  });
})();
