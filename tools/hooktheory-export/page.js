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

  /* A staff's props hold only the chords and notes it is drawing (a window of the section, when the section is
   * long); the whole section is in the page's store (jotai): its list of chord atoms and of note atoms, kept in a
   * hook of an ancestor of the staff. Found by what they hold: the nearest list of {chordAtom} and of {noteAtom}
   * walking up from the staff (so never another section's), each atom read for its value now. */
  const isAtom = v => v && typeof v === "object" && typeof v.read === "function";
  function storeLists(st) {
    let store = null;
    for (let f = fiberOf(st), d = 0; f && d < 60 && !store; d++, f = f.return)
      if (f.memoizedProps && f.memoizedProps.store && typeof f.memoizedProps.store.get === "function")
        store = f.memoizedProps.store;
    if (!store) return null;
    const best = { chordAtom: null, noteAtom: null };
    const look = v => {
      if (!isAtom(v)) return;
      let r;
      try { r = store.get(v); } catch (e) { return; }
      if (!Array.isArray(r) || !r.length || !r[0] || typeof r[0] !== "object") return;
      for (const k of ["chordAtom", "noteAtom"])
        if (isAtom(r[0][k]) && !best[k]) best[k] = r;
    };
    for (let f = fiberOf(st), d = 0; f && d < 60; d++, f = f.return) {
      for (let h = f.memoizedState, i = 0; h && typeof h === "object" && i < 80; h = h.next, i++) {
        const v = h.memoizedState;
        look(v);
        if (v && typeof v === "object" && !isAtom(v)) for (const k of Object.keys(v).slice(0, 12)) look(v[k]);
      }
    }
    const val = (list, k) => (list || []).map(e => { try { return store.get(e[k]); } catch (x) { return e[k].init; } })
                                       .filter(Boolean);
    return { chords: val(best.chordAtom, "chordAtom"), notes: val(best.noteAtom, "noteAtom") };
  }

  /* the page draws a section only once it is on the screen: bring it there and wait for its data. Some sections
   * have only a melody (an instrumental hook) or only chords: the page's data says which to wait for. A section the
   * site itself has left empty (0% chords and 0% melody, e.g. "Instrumental" on Tiny Dancer) never gets any data:
   * wait only long enough to be sure, then keep it as an empty section */
  const isEmpty = m => !!m && m.pctChord === 0 && m.pctMelody === 0;
  function loaded(el, m) {
    const wantChords = !m || m.pctChord == null || m.pctChord > 0, wantNotes = !!(m && m.pctMelody > 0);
    let chords = 0, notes = 0;
    for (const st of el.querySelectorAll("[data-chord-staff]")) {
      const p = staffProps(st);
      if (p) { chords += p.chordEntries.length; notes += p.noteEntries.length; }
    }
    if (isEmpty(m)) return chords + notes > 0;
    return (wantChords ? chords > 0 : true) && (wantNotes ? notes > 0 : true) && chords + notes > 0;
  }
  async function bringIn(el, m) {
    const tries = isEmpty(m) ? 15 : 100;                  /* 3 s for an empty section, else up to 20 s */
    for (let i = 0; i < tries && !loaded(el, m); i++) {   /* nudging the page every 2 s */
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

  async function readSection(el, m) {
    await bringIn(el, m);
    const chords = new Map(), notes = new Map(), segs = new Map();
    const CH = ["root", "startTick", "durationTicks", "type", "inversion", "applied", "adds", "omits", "alterations",
                "suspensions", "borrowed", "pedal", "isRest"], NT = ["sd", "octave", "startTick", "durationTicks", "isRest", "voice"];
    const first = el.querySelector("[data-chord-staff]"), all = first && storeLists(first);
    if (all) {
      for (const c of all.chords) chords.set(c.id, pick(c, CH));
      for (const n of all.notes) notes.set(n.id, pick(n, NT));
    }
    for (const st of el.querySelectorAll("[data-chord-staff]")) {
      const p = staffProps(st);
      if (!p) continue;
      for (const e of p.chordEntries) {
        const c = e.chordAtom && e.chordAtom.init;
        if (c && !chords.has(c.id)) chords.set(c.id, pick(c, CH));
      }
      for (const e of p.noteEntries) {
        const n = e.noteAtom && e.noteAtom.init;
        if (n && !notes.has(n.id)) notes.set(n.id, pick(n, NT));
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
    for (const [i, el] of els.entries()) {
      const url = el.getAttribute("data-section-url");
      const m = meta.sections.find(s => s.url === url) || {};
      progress("Reading " + (m.name || url) + " (" + (i + 1) + " of " + els.length + ")…");
      const data = await readSection(el, m);
      if (!data.chords.length && !data.melody.length && isEmpty(m)) {
        /* empty on the site too: keep its place in the song, with the numerals the site lists for it */
        sections.push({ name: m.name || url, url, key: m.key, scale: m.scale, bpm: m.tempo, meter: m.meter,
                        numerals: m.rel, empty: true, chords: [],
                        ...(withMelody ? { melody: [] } : {}), keys: data.keys });
        continue;
      }
      if (!data.chords.length && !data.melody.length)
        throw new Error("The section \"" + (m.name || url) + "\" did not load. Scroll through the song once and try again.");
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

  const progress = text => window.postMessage({ t1: "progress", text }, location.origin);

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
