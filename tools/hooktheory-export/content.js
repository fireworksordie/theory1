/* SPDX-License-Identifier: MPL-2.0
 * THEORY-1 song export: puts an "Export to THEORY-1" button on a TheoryTab song page. Pressing it reads the song on
 * the screen (page.js, which runs in the page so it can see what the page has drawn) and saves it as one
 * .t1song.json file: sections, key, tempo, meter, chords and, if ticked, the melody, with their timing. */
(function () {
  if (document.getElementById("t1-export")) return;
  const api = typeof browser !== "undefined" ? browser : chrome;

  /* page.js runs in the page's own world: content scripts can't see React's state through Firefox's wrappers */
  const s = document.createElement("script");
  s.src = api.runtime.getURL("page.js");
  s.onload = () => s.remove();
  (document.head || document.documentElement).appendChild(s);

  const box = document.createElement("div");
  box.id = "t1-export";
  box.innerHTML =
    '<button type="button" id="t1-go">Export to THEORY-1</button>' +
    '<label><input type="checkbox" id="t1-mel" checked> melody</label>' +
    '<span id="t1-msg" role="status"></span>';
  document.documentElement.appendChild(box);
  const go = box.querySelector("#t1-go"), msg = box.querySelector("#t1-msg"), mel = box.querySelector("#t1-mel");

  const say = (t, bad) => { msg.textContent = t; msg.className = bad ? "bad" : ""; };
  const fileName = song => ((song.artist ? song.artist + " - " : "") + song.title)
    .replace(/[\\/:*?"<>|]+/g, "").replace(/\s+/g, " ").trim().slice(0, 100) + ".t1song.json";

  function save(song) {
    const blob = new Blob([JSON.stringify(song, null, 1)], { type: "application/json" });
    const a = document.createElement("a");
    a.href = URL.createObjectURL(blob);
    a.download = fileName(song);
    document.body.appendChild(a);
    a.click();
    setTimeout(() => { URL.revokeObjectURL(a.href); a.remove(); }, 2000);
  }

  window.addEventListener("message", ev => {
    if (ev.source !== window || !ev.data || !ev.data.t1) return;
    if (ev.data.t1 === "result") {
      const song = ev.data.song, n = song.sections.length;
      save(song);
      say("Saved " + n + (n === 1 ? " section" : " sections") + ".");
      go.disabled = false;
    } else if (ev.data.t1 === "error") {
      say(ev.data.message, true);
      go.disabled = false;
    }
  });

  go.addEventListener("click", () => {
    go.disabled = true;
    say("Reading the song…");
    window.postMessage({ t1: "export", melody: mel.checked }, location.origin);
  });
})();
