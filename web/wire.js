// web/wire.js — Hemma's browser wire. Deliberately generic: the C server
// renders every page, and this file only
//   1. keeps a WebSocket to the server (reconnecting on its own),
//   2. turns link clicks and form submits into messages,
//   3. morphs the DOM into whatever HTML the server sends back,
//   4. shows toasts and reports media progress ([data-report]).
// Without it everything still works as plain links and POST forms.
// Markup hooks: form[data-a] action · [data-live] live input · [data-auto] submit on change
//               [data-esc] Escape target · [data-key] keyboard shortcut · [data-static] never morphed;
//               [data-static="v"] never while v stays, a new v morphs it in place (the element stays:
//               a player in full screen stays there into the next episode)
//               a[data-native] full page load · form[data-keep] don't reset · form[data-confirm]
//               form[data-upload] send the chosen files (see upload() below); with
//               data-convert/data-wasm the server's converter (C, as WebAssembly) can make
//               play-everywhere copies first, in Workers (web/conv.js)
//               [data-start="id"] open a horizontal scroller at that child
//               video[data-onair] live: plays at "now", no pausing or winding
//               [data-player="<video id>"] a video's own controls (play, seek, time, subtitles,
//               sound, full screen - see "the player" below), instead of the browser's
//               [data-arrows] a remote control's arrow keys move focus inside it, to the nearest
//               focusable thing that way (Left/Right along its row, Up/Down to the next row,
//               whatever is closest across); Enter then opens it as usual. With nothing focused
//               (a page load, a morph that took it away) [autofocus] - or the first focusable
//               inside it, when it's a container - else the first focusable gets the focus
//               [data-fullscreen="id"] a button: #id (a player and what's over it) full screen and
//               back; double-clicking the picture too. Arrives hidden, shown where the browser can
//               [data-upnext="S"] the card for the page's video[data-next]: shown in its last S
//               seconds, [data-count] counting them down; a link in it goes on now, [data-hide]
//               puts it away; an [autofocus] in it takes the focus from the video (a remote's OK)
(() => {
  'use strict';
  const root = document.documentElement;
  const VERSION = root.dataset.v || '';
  const here = () => location.pathname + location.search;
  const pending = new Map();
  let ws = null, open = false, tries = 0, seq = 0, fresh = [], leaving = 0, uploading = false;

  // While the browser is navigating away (plain POST, full page load) the old page
  // must not reconnect or reload: that would cancel the navigation in flight.
  const leave = () => { leaving = Date.now(); };
  const isLeaving = () => leaving && Date.now() - leaving < 4000;
  addEventListener('beforeunload', (e) => {
    if (uploading) { e.preventDefault(); e.returnValue = ''; return; } // "leave and stop the upload?"
    leave();
  });
  addEventListener('pageshow', () => { leaving = 0; });

  function connect() {
    ws = new WebSocket((location.protocol === 'https:' ? 'wss://' : 'ws://') + location.host + '/ws');
    ws.onopen = () => {
      open = true;
      tries = 0;
      root.dataset.ws = 'on';
      // the page this load brought (its hash): unchanged, the server needn't send it again. Only
      // once - after a morph or on a reconnect it would be stale.
      const h = root.dataset.h;
      if (h) delete root.dataset.h;
      send(h ? { t: 'hello', url: here(), v: VERSION, h } : { t: 'hello', url: here(), v: VERSION });
      while (later.length) send({ t: 'log', url: here(), m: later.shift(), ua: navigator.userAgent });
    };
    ws.onmessage = (e) => {
      let ops;
      try { ops = JSON.parse(e.data); } catch (_) { return; }
      for (const op of ops) apply(op);
    };
    ws.onclose = () => {
      const was = open;
      open = false;
      for (const f of pending.values()) busy(f, false);
      pending.clear();
      const delay = isLeaving() ? 4000 : was ? 300 : Math.min(5000, 300 * 2 ** tries++);
      if (!isLeaving()) root.dataset.ws = 'off';
      setTimeout(connect, delay);
    };
  }
  const send = (msg) => (open && ws.send(JSON.stringify(msg)), open);
  // Trouble goes to Hemma's log (the System page): a TV's browser has no console anyone can read.
  // A script error here, a video that errs or doesn't start (see media()). The server takes 20
  // lines per person an hour and logs the user agent (ua) once per browser; lines from before the
  // socket opened wait for it (10 at most).
  const later = [];
  const report = (what) => {
    const m = String(what).slice(0, 400);
    if (!send({ t: 'log', url: here(), m, ua: navigator.userAgent }) && later.length < 10) later.push(m);
  };
  addEventListener('error', (e) => report(`script error: ${e.message} (${String(e.filename || '').split('/').pop()}:${e.lineno})`));
  addEventListener('unhandledrejection', (e) => report(`script error: ${e.reason && e.reason.message ? e.reason.message : e.reason}`));
  const pairs = (fd) => [...fd].filter(([, v]) => typeof v === 'string');
  const busy = (f, on) => (on ? f.setAttribute('aria-busy', 'true') : f.removeAttribute('aria-busy'));

  // Older engines (2020 Samsung TVs run Chromium 69) lack a few things; stand-ins:
  // requestSubmit (Chrome 76), SubmitEvent.submitter (81) and `gap` in flexbox (84).
  const submit = (f) => {
    if (f.requestSubmit) { f.requestSubmit(); return; }
    if (f.dispatchEvent(new Event('submit', { bubbles: true, cancelable: true }))) f.submit();
  };
  let pressed = null; // the submit button last clicked, for engines without e.submitter
  document.addEventListener('click', (e) => {
    const b = e.target.closest && e.target.closest('button, input[type=submit]');
    pressed = b && b.form ? b : null;
  }, true);
  const flexGap = (() => {
    const d = document.createElement('div');
    d.style.cssText = 'display:flex;flex-direction:column;row-gap:1px;position:absolute';
    d.append(document.createElement('div'), document.createElement('div'));
    document.body.append(d);
    const ok = d.scrollHeight === 1;
    d.remove();
    return ok;
  })();
  // Without flex gap, space flex items with margins (re-applied after every morph,
  // since the server's markup carries no such inline styles).
  // All the reading first, then all the writing: interleaved, every write forced the next
  // read to lay the page out again (~150 ms a morph on an old TV; ~40 ms this way).
  function gaps() {
    if (flexGap) return;
    const writes = [];
    for (const el of document.querySelectorAll('#app *, #toasts')) {
      const s = getComputedStyle(el);
      if (s.display !== 'flex' && s.display !== 'inline-flex') continue;
      const cg = parseFloat(s.columnGap) || 0, rg = parseFloat(s.rowGap) || 0;
      if (!cg && !rg) continue;
      const kids = [...el.children].filter((k) => {
        const ks = getComputedStyle(k);
        return ks.display !== 'none' && ks.position !== 'absolute' && ks.position !== 'fixed';
      });
      const col = s.flexDirection.startsWith('column'), wrap = s.flexWrap !== 'nowrap';
      kids.forEach((k, i) => {
        if (wrap) writes.push([k, 'marginRight', cg], [k, 'marginBottom', rg]);
        else if (i < kids.length - 1) writes.push([k, col ? 'marginBottom' : 'marginRight', col ? rg : cg]);
      });
    }
    for (const [k, prop, px] of writes) k.style[prop] = px + 'px';
  }

  // ------------------------------------------------------------- outgoing
  document.addEventListener('click', (e) => {
    const a = e.target.closest('a[href]');
    if (!a || !open || e.defaultPrevented || e.button !== 0 || e.metaKey || e.ctrlKey || e.shiftKey || e.altKey) return;
    if (a.target || a.hasAttribute('download') || a.hasAttribute('data-native')) return;
    const u = new URL(a.href, location.href);
    if (u.origin !== location.origin) return;
    e.preventDefault();
    if (u.hash && u.pathname + u.search === here()) { location.hash = u.hash; return; }
    send({ t: 'nav', url: u.pathname + u.search });
  });
  addEventListener('popstate', () => { if (!send({ t: 'nav', url: here(), pop: 1 })) location.reload(); });

  document.addEventListener('submit', (e) => {
    const f = e.target, a = f.dataset.a;
    if (f.getAttribute('aria-busy')) { e.preventDefault(); return; }
    // ask first, whichever way the form then travels (socket or plain POST)
    if (f.dataset.confirm && !confirm(f.dataset.confirm)) { e.preventDefault(); return; }
    if (f.dataset.upload) { e.preventDefault(); upload(f); return; }
    if (!a || !open) { if (!e.defaultPrevented) leave(); return; } // a plain POST: the page is leaving
    e.preventDefault();
    const fd = new FormData(f);
    const sub = e.submitter || (pressed && pressed.form === f ? pressed : null);
    if (sub && sub.name) fd.append(sub.name, sub.value);
    const id = ++seq;
    pending.set(id, f);
    busy(f, true);
    send({ t: 'act', a, id, url: here(), f: pairs(fd) });
  });

  let liveTimer = 0;
  document.addEventListener('input', (e) => {
    const el = e.target.closest('[data-live]');
    if (!el) return;
    clearTimeout(liveTimer);
    liveTimer = setTimeout(() => {
      const fd = el.form ? new FormData(el.form) : new FormData();
      if (!el.form && el.name) fd.append(el.name, el.value);
      send({ t: 'live', a: el.dataset.live, url: here(), f: pairs(fd) });
    }, +(el.dataset.delay || 120));
  });
  document.addEventListener('change', (e) => {
    const el = e.target.closest('[data-auto]');
    if (el && el.form) submit(el.form);
  });

  document.addEventListener('keydown', (e) => {
    if (e.key === 'Escape') {
      if (fsEl()) return; // the browser leaves full screen; going back too would be one step too many
      if (menus(null)) { e.preventDefault(); return; } // an open subtitles menu closes first
      const x = document.querySelector('[data-esc]');
      if (x) { e.preventDefault(); x.click(); }
      return;
    }
    const t = e.target;
    if (e.ctrlKey || e.metaKey || e.altKey || /^(INPUT|TEXTAREA|SELECT)$/.test(t.tagName) || t.isContentEditable) return;
    const k = document.querySelector(`[data-key="${CSS.escape(e.key)}"]`);
    if (!k) return;
    e.preventDefault();
    k.focus();
    if (k.tagName === 'A' || k.tagName === 'BUTTON') k.click();
  });

  // ------------------------------------------------------------ remote control
  // [data-arrows]: a TV remote has arrows and OK (Enter), nothing to Tab with. Focus moves by what
  // is on screen: Left/Right only along the focused thing's row (what overlaps it vertically),
  // Up/Down to the nearest row that way, then to what's closest across it - so rows that scroll
  // sideways keep their place. Nothing that way: the key does what it always does.
  const FOCUSABLE = 'a[href],button:not([disabled]),input:not([type=hidden]):not([disabled]),' +
    'select:not([disabled]),textarea:not([disabled]),[tabindex]:not([tabindex="-1"])';
  const ARROWS = { ArrowLeft: 'l', ArrowRight: 'r', ArrowUp: 'u', ArrowDown: 'd', Left: 'l', Right: 'r', Up: 'u', Down: 'd' };
  const ARROW_CODES = { 37: 'l', 38: 'u', 39: 'r', 40: 'd' }; // engines without e.key
  let pageUrl = here(); // the address the page on screen belongs to (a morph follows its "url" op)
  const focusables = (box) => [...box.querySelectorAll(FOCUSABLE)].filter((el) => {
    const r = el.getBoundingClientRect();
    return r.width > 0 && r.height > 0;
  });
  const across = (a, b) => b.top < a.bottom && b.bottom > a.top; // on the same row
  function nearest(area, from, dir) {
    const a = from.getBoundingClientRect(), x = a.left + a.width / 2;
    const tx = a.width * 0.1, ty = a.height * 0.1; // the focused one may be drawn a little larger
    let best = null, bestD = Infinity, bestS = Infinity, row = null;
    const below = [];
    for (const el of focusables(area)) {
      if (el === from) continue;
      const b = el.getBoundingClientRect();
      let d;
      if (dir === 'l' || dir === 'r') {
        if (!across(a, b)) continue;
        d = dir === 'r' ? b.left - (a.right - tx) : (a.left + tx) - b.right;
        const s = Math.abs(b.top - a.top);
        if (d >= 0 && (d < bestD || (d === bestD && s < bestS))) { best = el; bestD = d; bestS = s; }
        continue;
      }
      d = dir === 'd' ? b.top - (a.bottom - ty) : (a.top + ty) - b.bottom;
      if (d < 0) continue;
      below.push([el, b]);
      if (d < bestD) { bestD = d; row = b; }
    }
    if (dir === 'l' || dir === 'r') return best;
    for (const [el, b] of below) { // the nearest row, then what's closest across it
      const s = Math.abs(b.left + b.width / 2 - x);
      if (across(row, b) && s < bestS) { best = el; bestS = s; }
    }
    return best;
  }
  function firstIn(area) {
    const af = area.querySelector('[autofocus]');
    const inside = af ? (af.matches(FOCUSABLE) ? [af] : focusables(af)) : [];
    return inside[0] || focusables(area)[0] || null;
  }
  const moveTo = (el, smooth) => {
    el.focus({ preventScroll: true });
    el.scrollIntoView({ block: 'nearest', inline: 'nearest', behavior: smooth ? 'smooth' : 'auto' });
  };
  document.addEventListener('keydown', (e) => playerKeys(e)); // a player's keys come first (see below)
  document.addEventListener('keydown', (e) => {
    const dir = ARROWS[e.key] || ARROW_CODES[e.keyCode];
    if (!dir || e.defaultPrevented || e.ctrlKey || e.metaKey || e.altKey || e.shiftKey) return;
    const t = document.activeElement, idle = !t || t === document.body;
    const area = idle ? document.querySelector('[data-arrows]') : t.closest('[data-arrows]');
    if (!area) return;
    if (idle) { // nothing focused yet: the first press lands somewhere
      const f = firstIn(area);
      if (f) { e.preventDefault(); moveTo(f, false); }
      return;
    }
    if (t.isContentEditable || t.tagName === 'TEXTAREA' || t.tagName === 'SELECT' ||
        (t.tagName === 'INPUT' && !/^(button|submit|reset|checkbox|image)$/.test(t.type))) return; // the caret's keys
    const next = nearest(area, t, dir);
    if (!next) return;
    e.preventDefault();
    moveTo(next, true);
  });
  // Where the remote is, just before a morph: {area, url, x, y} or null.
  function focusSpot() {
    const t = document.activeElement, area = t && t.closest ? t.closest('[data-arrows]') : null;
    if (!area) return null;
    const r = t.getBoundingClientRect();
    return { area, url: pageUrl, x: r.left + r.width / 2, y: r.top + r.height / 2 };
  }
  // After a load or a morph a [data-arrows] area always has something focused: when the morph
  // took the focused thing away (a tile that went), whatever is nearest to where it was; on a
  // fresh page (or area) its start.
  function arrowsSettle(was) {
    const area = document.querySelector('[data-arrows]');
    if (!area || !unfocused()) return;
    let pick = null;
    if (was && was.area === area && was.url === here()) {
      let bestD = Infinity;
      for (const el of focusables(area)) {
        const r = el.getBoundingClientRect();
        const d = Math.hypot(r.left + r.width / 2 - was.x, r.top + r.height / 2 - was.y);
        if (d < bestD) { bestD = d; pick = el; }
      }
    }
    pick = pick || firstIn(area);
    if (pick) pick.focus({ preventScroll: true });
  }

  document.addEventListener('visibilitychange', () => {
    if (document.visibilityState !== 'visible') return;
    if (open) send({ t: 'nav', url: here(), pop: 1 }); // catch up after sleep
    else if (ws && ws.readyState !== WebSocket.CONNECTING) ws.close();
  });

  // -------------------------------------------------------------- uploads
  // <form data-upload="/media/upload"> with <input type=file> fields: ask the
  // server for a plan (what to skip, where to resume), then send each file as
  // one PUT streamed from disk. The server renders the progress; this only
  // moves bytes - and keeps going while you browse other pages.
  const pause = (ms) => new Promise((ok) => setTimeout(ok, ms));
  const json = (r) => r.json().catch(() => ({ error: `Unexpected answer (${r.status})` }));
  // The server's converter, built to WebAssembly, can run here (web/conv.js in a Worker).
  // It needs SIMD (Chrome 91, Firefox 89, Safari 16.4): a 30-byte module that uses it tells.
  const canConvert = (() => {
    try {
      return !!(self.Worker && self.Blob && self.WebAssembly) && WebAssembly.validate(new Uint8Array(
        [0, 97, 115, 109, 1, 0, 0, 0, 1, 5, 1, 96, 0, 1, 123, 3, 2, 1, 0, 10, 10, 1, 8, 0, 65, 0, 253, 15, 253, 98, 11]));
    } catch (_) { return false; }
  })();

  async function upload(f) {
    if (uploading) { toast(1, 'An upload is already running'); return; }
    const files = [];
    for (const el of f.querySelectorAll('input[type=file]')) files.push(...el.files);
    if (!files.length) { toast(2, f.dataset.empty || 'Choose a folder or some files first'); return; }
    const base = f.dataset.upload, body = {};
    for (const [k, v] of pairs(new FormData(f))) body[k] = v;
    const use = { worker: f.dataset.convert, wasm: f.dataset.wasm }; // the page says where (if it has them)
    if (!canConvert || !use.worker || !use.wasm) delete body.conv;
    body.files = files.map((x) => [x.webkitRelativePath || x.name, x.size]);
    uploading = true;
    busy(f, true);
    let made = null;
    try {
      const plan = await fetch(base + '/plan', {
        method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body),
      }).then(json);
      if (plan.error) throw new Error(plan.error);
      // [i, at] = file i from byte `at`; [i, at, end] = only up to `end` (the server needs no more);
      // {i, f} = convert file f here first, then send the copy as entry i (the server says how)
      const copies = plan.todo.filter((t) => !Array.isArray(t));
      made = converter(base, plan.batch, files, copies, use);
      for (const t of plan.todo) {
        if (Array.isArray(t)) { await put(`${base}/${plan.batch}/${t[0]}`, files[t[0]], t[1], t[2]); continue; }
        const url = `${base}/${plan.batch}/${t.i}`, m = await made.take(copies.indexOf(t));
        if (m.stop) throw new Error(m.stop);
        const s = await ask(url, m.blob ? `size=${m.blob.size}` : `original=1&why=${encodeURIComponent(m.error || '')}`);
        if (!s.skip) await put(url, m.blob || files[t.f], s.at || 0);
      }
    } catch (err) {
      toast(2, err.message || 'The upload stopped');
    } finally {
      if (made) made.stop();
      uploading = false;
      busy(f, false);
    }
  }

  // Copies made here, a few at a time (half the cores, at most 8; a phone one) and only a few
  // ahead of the one being sent, so finished copies never pile up in memory. Progress goes to
  // the server (?pm=), which renders it like everything else; when it says stop, everything stops.
  function converter(base, batch, files, jobs, use) {
    const phone = /Mobi|Android|iPhone|iPad/.test(navigator.userAgent);
    const width = phone ? 1 : Math.max(1, Math.min(8, Math.floor((navigator.hardwareConcurrency || 2) / 2)));
    const results = new Map(), done = new Map(), workers = new Map(); // a copy is let go once it's taken
    const seen = new Map(); // job -> [progress, when the server last heard it]
    let next = 0, sending = 0, halted = '';
    const tell = (k, pm) => {
      seen.set(k, [pm, Date.now()]);
      fetch(`${base}/${batch}/${jobs[k].i}?pm=${pm}`, { method: 'POST' }).then(json)
        .then((s) => { if (s.stop) halt(s.error); }, () => {});
    };
    // a heartbeat: a long stretch without progress (a big file's first look) isn't "the browser left"
    const beat = setInterval(() => {
      for (const k of workers.keys()) {
        const [pm, at] = seen.get(k) || [0, 0];
        if (Date.now() - at >= 10000) tell(k, pm);
      }
    }, 5000);
    const finish = (k, m) => {
      const w = workers.get(k);
      if (w) { w.terminate(); workers.delete(k); }
      seen.delete(k);
      if (done.has(k)) { done.get(k)(m); done.delete(k); }
      pump();
    };
    function start(k) {
      const job = jobs[k];
      results.set(k, new Promise((ok) => done.set(k, ok)));
      const w = new Worker(use.worker);
      workers.set(k, w);
      w.onmessage = (e) => {
        const m = e.data;
        if (m.pm === undefined) { finish(k, m); return; }
        const [, at] = seen.get(k) || [0, 0];
        if (Date.now() - at >= 3000) tell(k, m.pm);
        else seen.set(k, [m.pm, at]);
      };
      w.onerror = (e) => { e.preventDefault(); finish(k, { error: 'The converter stopped' }); };
      w.postMessage({ file: files[job.f], wasm: use.wasm });
    }
    function pump() {
      while (!halted && workers.size < width && next < jobs.length && next <= sending + width + 1) start(next++);
    }
    function halt(why) {
      halted = why || 'The upload was stopped';
      clearInterval(beat);
      for (const w of workers.values()) w.terminate();
      workers.clear();
      for (const ok of done.values()) ok({ stop: halted });
      done.clear();
    }
    pump();
    return {
      take(k) {
        sending = k;
        pump();
        const r = results.get(k) || Promise.resolve({ stop: halted || 'The upload was stopped' });
        results.delete(k);
        return r;
      },
      stop() { if (!halted) halt(''); },
    };
  }

  // A small POST with retries; {stop} answers end the upload.
  async function ask(url, query) {
    for (let tries = 0; ; tries++) {
      const s = await fetch(`${url}?${query}`, { method: 'POST' }).then(json).catch(() => ({}));
      if (s.stop) throw new Error(s.error);
      if (!s.error && (s.skip || typeof s.at === 'number')) return s;
      if (tries >= 5) throw new Error(s.error || 'The connection keeps dropping');
      await pause(1000 * 2 ** tries);
    }
  }

  // One file; after a dropped connection, ask the server how far it got and go on from there.
  async function put(url, file, at, end) {
    for (let tries = 0; ; tries++) {
      let r = null;
      try {
        r = await fetch(`${url}?at=${at}`, {
          method: 'PUT', headers: { 'Content-Type': 'application/octet-stream' }, body: file.slice(at, end),
        });
      } catch (_) { /* network hiccup */ }
      if (r && r.ok) return;
      const s = r ? await json(r) : {};
      if (s.stop) throw new Error(s.error); // stopped, not allowed, disk full: no point retrying
      if (s.error === 'busy') { // other uploads are writing: wait for a turn, as long as it takes
        await pause(3000);
        tries--;
        continue;
      }
      if (tries >= 8) throw new Error(s.error || 'The connection keeps dropping');
      if (typeof s.at !== 'number') {
        await pause(Math.min(15000, 1000 * 2 ** tries));
        Object.assign(s, await fetch(url).then(json).catch(() => ({})));
        if (s.stop) throw new Error(s.error);
      }
      if (typeof s.at === 'number') at = s.at;
    }
  }

  // ------------------------------------------------------------- incoming
  function apply(op) {
    switch (op[0]) {
      case 'morph': {
        const t = document.createElement('template');
        t.innerHTML = op[1];
        const next = t.content.firstElementChild, app = document.getElementById('app');
        if (next && app) {
          const was = focusSpot();
          morph(app, next); settle(); starts(); gaps(); fits(); arrowsSettle(was);
          pageUrl = here();
        }
        break;
      }
      case 'url':
        if (op[1] !== here()) history[op[3] ? 'pushState' : 'replaceState'](null, '', op[1]);
        document.title = op[2];
        if (op[3]) scrollTo(0, 0);
        break;
      case 'ack': {
        const f = pending.get(op[1]);
        pending.delete(op[1]);
        if (f) { busy(f, false); if (op[2] && !f.hasAttribute('data-keep')) f.reset(); }
        break;
      }
      case 'toast': toast(op[1], op[2]); break;
      case 'reload': if (!isLeaving()) location.reload(); break;
    }
  }

  function toast(kind, text) {
    const box = document.getElementById('toasts');
    if (!box) return;
    const el = document.createElement('div');
    el.className = 'toast t-' + (['ok', 'info', 'err'][kind] || 'info');
    el.textContent = text;
    el.onclick = () => el.remove();
    box.append(el);
    setTimeout(() => { el.classList.add('out'); setTimeout(() => el.remove(), 400); }, kind === 2 ? 6000 : 3500);
  }

  // -------------------------------------------------------------- morphing
  // Attribute-level sync keeps whatever the user typed: changing an input's
  // `value` attribute only moves inputs the user hasn't touched yet.
  function morph(from, to) {
    if (from.nodeType !== to.nodeType || from.nodeName !== to.nodeName) {
      from.replaceWith(to);
      fresh.push(to);
      return;
    }
    if (from.nodeType === 3 || from.nodeType === 8) {
      if (from.nodeValue !== to.nodeValue) from.nodeValue = to.nodeValue;
      return;
    }
    if (from.nodeType !== 1) return;
    if (from.hasAttribute('data-static') && to.hasAttribute('data-static') && key(from) === key(to)) {
      if (from.getAttribute('data-static') === to.getAttribute('data-static')) return;
      fresh.push(from); // the same element with something new in it (a video's next one): settled again
    }
    attrs(from, to);
    if (from.nodeName === 'TEXTAREA') {
      if (from.defaultValue !== to.textContent) from.defaultValue = to.textContent;
      return;
    }
    if (from.hasAttribute('data-auto') && 'checked' in from) from.checked = to.hasAttribute('checked');
    kids(from, to);
  }

  function attrs(a, b) {
    for (const { name, value } of b.attributes) if (a.getAttribute(name) !== value) a.setAttribute(name, value);
    for (const { name } of [...a.attributes]) {
      if (b.hasAttribute(name) || name === 'aria-busy' || (name === 'open' && a.nodeName === 'DETAILS')) continue;
      a.removeAttribute(name);
    }
  }

  // A node's key: its id attribute. (Not el.id: a form holding <input name="id"> answers el.id
  // with that input, so such forms never matched and were rebuilt - typing lost - every morph.)
  const key = (n) => (n.nodeType === 1 && n.getAttribute('id')) || '';
  function kids(from, to) {
    const keyed = new Map(), wanted = new Set();
    for (let c = from.firstChild; c; c = c.nextSibling) if (key(c)) keyed.set(key(c), c);
    for (let c = to.firstChild; c; c = c.nextSibling) if (key(c)) wanted.add(key(c));
    let cur = from.firstChild;
    for (let nxt = to.firstChild; nxt;) {
      const after = nxt.nextSibling;
      let match = null;
      if (key(nxt)) {
        match = keyed.get(key(nxt)) || null;
        if (match) keyed.delete(key(nxt));
      } else {
        // step over keyed nodes: gone ones are dropped now, moved ones get placed
        // when their turn comes - so the unkeyed node after them still lines up
        while (cur && key(cur)) {
          const n = cur.nextSibling;
          if (!wanted.has(key(cur))) cur.remove();
          cur = n;
        }
        if (cur && cur.nodeType === nxt.nodeType && cur.nodeName === nxt.nodeName) match = cur;
      }
      if (match) {
        if (match === cur) cur = cur.nextSibling;
        else from.insertBefore(match, cur);
        morph(match, nxt);
      } else {
        from.insertBefore(nxt, cur);
        fresh.push(nxt);
      }
      nxt = after;
    }
    while (cur) {
      const n = cur.nextSibling;
      cur.remove();
      cur = n;
    }
  }

  // Nothing has the focus - or what has it is no longer drawn (a morph just hid it: the browser
  // only lets go of it at the next frame).
  const unfocused = () => {
    const t = document.activeElement;
    return !t || t === document.body || !t.getClientRects().length;
  };
  // New nodes: focus [autofocus] (when nothing else has focus), wire media, show full-screen buttons.
  const within = (el, sel) => (el.matches(sel) ? [el] : el.querySelectorAll(sel));
  function settle() {
    const els = fresh;
    fresh = [];
    for (const el of els) {
      if (el.nodeType !== 1) continue;
      const af = el.matches('[autofocus]') ? el : el.querySelector('[autofocus]');
      if (af && unfocused()) af.focus();
      for (const m of within(el, 'video,audio')) media(m);
      for (const b of within(el, '[data-fullscreen]')) fsButton(b);
    }
  }

  // [data-start="id"] on a horizontal scroller: open it at that child (today's column on a
  // phone). Once per page URL, so live updates never undo the user's own scrolling.
  function starts() {
    for (const box of document.querySelectorAll('[data-start]')) {
      const key = box.dataset.start + '@' + here(), t = document.getElementById(box.dataset.start);
      if (box._start === key || !t) continue;
      box._start = key;
      const pad = parseFloat(getComputedStyle(box).scrollPaddingLeft) || 0;
      box.scrollLeft += t.getBoundingClientRect().left - box.getBoundingClientRect().left - pad;
    }
  }

  // [data-fit]: a box of cards (its children, the admin's order) that must fit the screen it's on - a
  // wall tablet nobody scrolls (issue #77). Its CSS bounds its height. While a card spills over the
  // edges: 1. the longest cards give up their last rows (li), down to FIT_ROWS each; 2. whole cards go,
  // from the end; 3. a card left alone gets shorter still; then rows come back while they fit.
  // "+N more" says how many went (or adds to the server's own [data-fit-more="N"]); [data-fit-keep]
  // rows stay; a [data-fit-head] (a day's heading) goes when nothing under it is left. From the start
  // after every morph and resize: the server's HTML always has everything.
  const FIT_ROWS = 4;
  const fitCut = (el) => { el.hidden = true; el.setAttribute('data-fit-cut', ''); };
  const fitShow = (el) => { el.hidden = false; el.removeAttribute('data-fit-cut'); };
  function fit(box) {
    for (const x of box.querySelectorAll('[data-fit-cut]')) fitShow(x);
    for (const x of box.querySelectorAll('.fit-more')) x.remove();
    for (const x of box.querySelectorAll('[data-fit-more]')) x.textContent = `+${x.dataset.fitMore} more`;
    const shown = () => [...box.children].filter((c) => !c.hidden);
    /* (per card: in CSS columns a card too tall for its column doesn't count in the box's scroll size) */
    const spills = () => {
      const b = box.getBoundingClientRect();
      return shown().some((card) => {
        const r = card.getBoundingClientRect();
        return r.bottom > b.bottom + 1 || r.right > b.right + 1;
      });
    };
    const rows = (card) => [...card.querySelectorAll('li:not([data-fit-cut]):not([data-fit-keep]):not([data-fit-head])')];
    /* the card again as it now is: its headings, and how many rows went */
    const settle = (card) => {
      let head = null, under = 0;
      const end = () => { if (head) (under ? fitShow : fitCut)(head); };
      for (const el of card.querySelectorAll('[data-fit-head], li:not([data-fit-keep])')) {
        if (el.hasAttribute('data-fit-head')) { end(); head = el; under = 0; }
        else if (!el.hasAttribute('data-fit-cut')) under++;
      }
      end();
      const n = card.querySelectorAll('li[data-fit-cut]:not([data-fit-head])').length, own = card.querySelector('[data-fit-more]');
      let p = card.querySelector('.fit-more');
      if (own) own.textContent = `+${n + (parseInt(own.dataset.fitMore, 10) || 0)} more`;
      else if (n) {
        if (!p) { p = document.createElement('p'); p.className = 'fit-more'; card.append(p); }
        p.textContent = `+${n} more`;
      } else if (p) p.remove();
    };
    const cuts = [];
    const shorten = (card) => { const r = rows(card); fitCut(r[r.length - 1]); cuts.push(r[r.length - 1]); settle(card); };
    for (let g = 0; g < 400 && spills(); g++) { /* 1. the longest cards, down to FIT_ROWS rows */
      let best = null, most = FIT_ROWS;
      for (const card of shown()) {
        const n = rows(card).length;
        if (n > most) { most = n; best = card; }
      }
      if (!best) break;
      shorten(best);
    }
    while (spills() && shown().length > 1) { const s = shown(); fitCut(s[s.length - 1]); } /* 2. cards, from the end */
    for (const card of [...box.children].filter((c) => c.hasAttribute('data-fit-cut'))) { /* ...back where room's left */
      fitShow(card);
      while (spills() && rows(card).length > FIT_ROWS) shorten(card);
      if (spills()) fitCut(card);
    }
    for (let g = 0; g < 400 && spills(); g++) { /* 3. the one card left */
      const card = shown()[0];
      if (!card || rows(card).length <= 1) break;
      shorten(card);
    }
    while (cuts.length) { /* rows back where they fit (a card that went may have made room) */
      const r = cuts.pop(), card = [...box.children].find((c) => c.contains(r));
      if (card.hasAttribute('data-fit-cut')) continue;
      fitShow(r);
      settle(card);
      if (spills()) { fitCut(r); settle(card); }
    }
    /* data-fit="<id>": that element names the cards that didn't fit (by their headings) */
    const say = box.dataset.fit && document.getElementById(box.dataset.fit);
    if (say) {
      const gone = [...box.children].filter((c) => c.hasAttribute('data-fit-cut'));
      say.textContent = gone.length ? `Didn't fit: ${gone.map((c) => (c.querySelector('h2,h3') || c).textContent.trim()).join(', ')}` : '';
    }
  }
  const fits = () => document.querySelectorAll('[data-fit]').forEach(fit);
  let fitLater;
  addEventListener('resize', () => { clearTimeout(fitLater); fitLater = setTimeout(fits, 150); });

  // <video data-onair data-resume="S">: a live broadcast (a TV channel). It plays where the
  // programme is now - S when it came (m._at, m._t0: see media()), plus the time since - and can't
  // be paused, wound or sped up: it jumps back to now and plays on. Only the sound is the viewer's
  // (its player's sound button); the element stays from programme to programme, and so does that.
  // A browser that won't start it with sound starts it muted (the player then says "Sound off").
  function onair(m) {
    const now = () => m._at + (performance.now() - m._t0) / 1000;
    const snap = () => {
      if (m.duration && Math.abs(m.currentTime - now()) > 2) m.currentTime = Math.min(now(), m.duration - 0.5);
    };
    // (only while it's on the page: leaving it pauses it, and playing on there - unseen - would add one
    // more voice for every visit)
    const go = () => {
      if (!m.isConnected) return;
      const p = m.play();
      if (p && p.catch) p.catch(() => {
        if (m.muted || !m.isConnected) return;
        m.muted = true; // no sound without a press first
        m.play().catch(() => {});
      });
    };
    m.addEventListener('loadedmetadata', () => { snap(); go(); });
    m.addEventListener('pause', () => { if (!m.ended && m.isConnected) { snap(); go(); } });
    m.addEventListener('seeked', snap); // its own jump lands within 2 s: no loop
    m.addEventListener('ratechange', () => { if (m.playbackRate !== 1) m.playbackRate = 1; });
    document.addEventListener('visibilitychange', () => {
      if (!document.hidden && m.isConnected) { snap(); go(); }
    });
  }

  // <video data-resume="123" data-report="media.progress" data-id="..." data-next="/tv/1">.
  // Wired once; when a morph brings the same element a new video (data-static="v": the next
  // episode, the next programme) it starts over with that one's data-resume - and the element
  // stays, so a full screen does too. The listeners read the data-* attributes as they are then.
  function media(m) {
    const src = (m.getAttribute('data-static') || '') + ' ' + (m.getAttribute('src') || ''); // (a channel airs a file again)
    if (m._src === src) return;
    const again = m._src !== undefined;
    m._src = src;
    m._at = parseFloat(m.dataset.resume || '0');
    m._t0 = performance.now();
    m._last = 0;
    m._away = false; // the "next episode" card put away
    m._played = false;
    m._nativeOk = false; /* (a new video: Hemma's player again - see "error") */
    clearTimeout(m._dog);
    if (m.autoplay || m.hasAttribute('data-onair')) watch(m); /* (others: once someone presses play) */
    plWire(m);
    if (again) return;
    // its player shows how it's doing; "waiting" (the spinner) ends with anything that says the
    // picture can go on - an engine that never says "playing" mustn't leave it spinning
    const onward = { playing: 1, pause: 1, seeked: 1, canplay: 1, loadeddata: 1 };
    for (const t of ['play', 'pause', 'playing', 'waiting', 'seeked', 'canplay', 'loadeddata', 'volumechange',
      'durationchange', 'progress', 'ended']) {
      m.addEventListener(t, () => {
        if (t === 'playing') { m._played = true; clearTimeout(m._dog); }
        if (t === 'play' && !m._played) watch(m);
        const pl = plOf(m);
        if (!pl) return;
        if (t === 'waiting' || onward[t]) flag(pl, 'data-wait', t === 'waiting');
        plSync(pl, m);
        if (t === 'play' || t === 'pause' || t === 'volumechange') plWake(pl);
      });
    }
    m.addEventListener('error', () => {
      const e = m.error;
      if (!e || !m.isConnected) return; /* (one taken off the page: not the next page's player's business) */
      clearTimeout(m._dog);
      const id = m.dataset.id || m.id, why = `error ${e.code}${e.message ? ` (${e.message})` : ''}`;
      const pl = plOf(m), native = m.hasAttribute('data-native');
      // Hemma's player couldn't play it: the browser's own player takes over (nativePlayer).
      if (pl && !native) {
        report(`video ${id}: ${why} in Hemma's player - switching to the browser's own player`);
        nativePlayer(m, pl);
        return;
      }
      if (pl) { pl.hidden = false; flag(pl, 'data-err', true); flag(pl, 'data-wait', false); plShow(pl, true); }
      report(`video ${id}: ${why}${native ? " - in the browser's own player too" : ''}`);
    });
    m.addEventListener('playing', () => { /* (the browser's own player plays it: say so once) */
      if (m.hasAttribute('data-native') && !m._nativeOk) {
        m._nativeOk = true;
        report(`video ${m.dataset.id || m.id} plays in the browser's own player`);
      }
    });
    if (m.hasAttribute('data-onair')) onair(m);
    else {
      // "continue watching" (data-report) skips a resume point in the last seconds
      m.addEventListener('loadedmetadata', () => {
        const at = m._at, guard = m.dataset.report ? 5 : 0;
        m._at = 0;
        if (at > 0 && (!m.duration || at < m.duration - guard)) m.currentTime = at;
      });
    }
    const tell = m._tell = (ended) => m.dataset.report && m.duration && send({
      t: 'live', a: m.dataset.report, url: here(),
      f: [['id', m.dataset.id || ''], ['pos', String(Math.floor(m.currentTime || 0))],
          ['dur', String(Math.floor(m.duration || 0))], ['ended', ended ? '1' : '']],
    });
    m.addEventListener('timeupdate', () => {
      if (Math.abs(m.currentTime - m._last) >= 15) { m._last = m.currentTime; tell(false); }
      upnext(m);
      const pl = plOf(m);
      if (!pl) return;
      if (!m.paused) flag(pl, 'data-wait', false); /* (time moves: not waiting) */
      plSync(pl, m);
    });
    m.addEventListener('pause', () => {
      if (!m.ended) tell(false);
      // taken off the page (another page came): let go of the stream too - after the report above
      if (!m.isConnected) setTimeout(() => { if (!m.isConnected) { m.removeAttribute('src'); m.load(); } });
    });
    m.addEventListener('ended', () => {
      tell(true);
      if (m.dataset.next) send({ t: 'nav', url: m.dataset.next }) || (location.href = m.dataset.next);
    });
  }

  // Two players (owner, 2026-10-13): Hemma's first; a video it can't play gets the browser's own -
  // a NEW <video> with the browser's controls in its place, Hemma's overlay hidden (a TV that
  // gave a decode error under Hemma's player may refuse that element after, and some TVs decode
  // HEVC only in a plain player). From the same spot (live: where the channel is now); the old
  // element lets go of its stream first (a TV has one decoder). Progress, the next episode and live
  // TV's rules carry on (media()); the next video tries Hemma's player again.
  function nativePlayer(m, pl) {
    const n = document.createElement('video'), live = m.hasAttribute('data-onair'), focused = document.activeElement === m;
    for (let i = 0; i < m.attributes.length; i++) n.setAttribute(m.attributes[i].name, m.attributes[i].value);
    for (const t of m.querySelectorAll('track')) n.append(t.cloneNode(true));
    const at = live ? m._at + (performance.now() - m._t0) / 1000
      : m.currentTime > 1 ? m.currentTime : parseFloat(m.dataset.resume || '0');
    n.setAttribute('data-resume', String(Math.floor(at)));
    n.setAttribute('data-native', '');
    n.setAttribute('autoplay', '');
    if (!live) n.setAttribute('controls', ''); /* (a live channel stays without: nothing to pause or wind) */
    n.setAttribute('controlslist', 'nofullscreen'); /* full screen stays the box's: the card shows, the next episode keeps it */
    n.muted = m.muted;
    n.volume = m.volume;
    m.removeAttribute('src');
    m.load();
    m.replaceWith(n);
    media(n); /* (plWire: the overlay keeps only its full-screen button - data-native) */
    if (focused) n.focus();
    const p = n.play();
    if (p && p.catch) p.catch(() => {});
  }

  // A video meant to play (autoplay, live, or play pressed) that hasn't started 25 s later says how
  // it looks, in the log: ready/network state, paused, buffered, the browser.
  function watch(m) {
    clearTimeout(m._dog);
    m._dog = setTimeout(() => {
      if (m._played || !m.isConnected) return;
      let buf = '';
      for (let i = 0; i < m.buffered.length; i++) buf += `${m.buffered.start(i).toFixed(0)}-${m.buffered.end(i).toFixed(0)} `;
      report(`video ${m.dataset.id || m.id} hasn't started after 25 s: readyState ${m.readyState}, networkState ` +
        `${m.networkState}, paused ${m.paused}, muted ${m.muted}, at ${(m.currentTime || 0).toFixed(1)} s, buffered ` +
        `${buf || 'nothing'}, error ${m.error ? m.error.code : 'none'}`);
    }, 25000);
  }

  // [data-upnext="S"]: the page's "next episode" card. It shows in the video's last S seconds and
  // counts them down; at the end the video goes on as always (data-next). With [autofocus] in it
  // (a TV) it takes the focus from the video, so OK goes on now; put away, the focus goes back.
  function upnext(m) {
    const card = m.dataset.next && document.querySelector('[data-upnext]');
    if (!card) return;
    const s = parseFloat(card.dataset.upnext) || 0, left = m.duration - m.currentTime;
    const show = !m._away && m.duration > s * 3 && left <= s;
    if (show) for (const n of card.querySelectorAll('[data-count]')) n.textContent = String(Math.max(0, Math.ceil(left)));
    if (show === !card.hidden) return;
    const t = document.activeElement;
    card.hidden = !show;
    if (show) {
      const af = card.querySelector('[autofocus]');
      if (af && (!t || t === document.body || t === m)) af.focus();
    } else if (card.contains(t)) m.focus();
  }
  // (capturing: "this one is watched" has to go before the link's nav - the server takes a live
  // message's url as where the screen is, and the old page's url after the nav would undo it)
  document.addEventListener('click', (e) => {
    const card = e.target.closest && e.target.closest('[data-upnext]');
    const m = card && document.querySelector('video[data-next]');
    if (!m) return;
    if (e.target.closest('[data-hide]')) { e.preventDefault(); m._away = true; upnext(m); }
    else if (e.target.closest('a[href]') && m._tell) m._tell(true); // on to the next one: this one is watched
  }, true);

  // ------------------------------------------------------------- the player
  // [data-player="<video id>"] (ui_player): a video's own controls, drawn over it - the same in every
  // browser and for a TV's remote. Without this file the video keeps the browser's controls; wired,
  // those go and the overlay shows. Its state, for CSS: data-on (the bar shows: a move, a key, a
  // pause - 3 still seconds of playing put it away), data-paused, data-wait, data-muted. Parts
  // ([data-p]): play back fwd seek at dur mute vol subs, and [data-sub="<srclang>"] in the subtitles
  // menu ("" = off; the choice goes on to the next episode). Keys while the player has the focus (or
  // nothing has): Space/K play, Left/Right 10 s, M sound, F full screen; a remote's Up/Down on the
  // picture go to the bar, Up on the seek bar back to the picture.
  const plOf = (v) => (v && v.id ? document.querySelector(`[data-player="${CSS.escape(v.id)}"]`) : null);
  const plVideo = (pl) => document.getElementById(pl.dataset.player);
  const players = () => document.querySelectorAll('[data-player]:not([hidden])');
  const flag = (el, name, on) => {
    if (on !== el.hasAttribute(name)) { if (on) el.setAttribute(name, ''); else el.removeAttribute(name); }
  };
  const two = (n) => (n < 10 ? '0' : '') + n;
  const clock = (s) => {
    s = Math.max(0, Math.floor(s || 0));
    return s >= 3600 ? `${Math.floor(s / 3600)}:${two(Math.floor(s / 60) % 60)}:${two(s % 60)}` : `${Math.floor(s / 60)}:${two(s % 60)}`;
  };
  const say = (pl, part, text) => {
    const el = pl.querySelector(`[data-p="${part}"]`);
    if (el && el.textContent !== text) el.textContent = text;
  };
  // A video came (a page, or the next episode in the same element): ours instead of the browser's.
  function plWire(v) {
    const pl = plOf(v);
    if (!pl) return;
    pl.hidden = false;
    flag(pl, 'data-native', v.hasAttribute('data-native'));
    if (v.hasAttribute('data-native')) return; /* (the browser's own player for this video: see nativePlayer) */
    v.controls = false;
    pl._drag = false;
    flag(pl, 'data-err', false); /* (a new video: whatever the last one couldn't do) */
    if (v._sub !== undefined) subPick(pl, v, v._sub);
    plSync(pl, v);
    plWake(pl);
  }
  function plSync(pl, v) {
    flag(pl, 'data-paused', v.paused); /* (a live channel too: one that never started) */
    flag(pl, 'data-muted', v.muted || v.volume === 0);
    const d = v.duration, seek = pl.querySelector('[data-p="seek"]');
    if (seek && d > 0 && isFinite(d)) {
      const at = pl._drag ? pl._to : v.currentTime;
      if (!pl._drag) {
        if (+seek.max !== d) seek.max = d;
        seek.value = at;
      }
      pl.querySelector('.pl-fill').style.width = (100 * at / d) + '%';
      let end = 0;
      for (let i = 0; i < v.buffered.length; i++) if (v.buffered.start(i) <= v.currentTime + 1) end = Math.max(end, v.buffered.end(i));
      pl.querySelector('.pl-buf').style.width = (100 * Math.min(end, d) / d) + '%';
      say(pl, 'at', clock(at));
      say(pl, 'dur', clock(d));
    }
    const vol = pl.querySelector('[data-p="vol"]');
    if (vol && document.activeElement !== vol) vol.value = v.muted ? 0 : v.volume;
  }
  let pointer = 'mouse'; // what pressed last: a tap on the picture shows the bar instead of pausing
  // The bar shows or goes; while it shows, subtitles move up above it (as a browser's own controls
  // make them).
  function plShow(pl, on) {
    if (on === pl.hasAttribute('data-on')) return;
    flag(pl, 'data-on', on);
    const v = plVideo(pl);
    for (let i = 0; v && i < v.textTracks.length; i++) {
      const t = v.textTracks[i];
      for (let j = 0; t.mode === 'showing' && t.cues && j < t.cues.length; j++) {
        const c = t.cues[j];
        if (on) {
          if (c._line === undefined) c._line = c.line;
          c.line = -4;
        } else if (c._line !== undefined) c.line = c._line;
      }
    }
  }
  function plWake(pl) {
    plShow(pl, true);
    clearTimeout(pl._t);
    pl._t = setTimeout(() => plSleep(pl), 3000);
  }
  function plSleep(pl) {
    const v = plVideo(pl);
    if (!v || !pl.isConnected) return;
    const live = v.hasAttribute('data-onair');
    if (v.paused) return; // paused: it stays until playing again
    if ((live && v.muted) || pl._drag || pl.querySelector('[role="menu"]:not([hidden])') ||
        (pointer === 'mouse' && pl.querySelector('.pl-bar:hover'))) {
      pl._t = setTimeout(() => plSleep(pl), 3000);
      return;
    }
    plShow(pl, false);
  }
  const play = (v) => {
    if (!v.paused) { v.pause(); return; }
    if (v.hasAttribute('data-onair')) v.muted = false; /* (a channel that never started: a press may have sound) */
    const p = v.play();
    if (p && p.catch) p.catch(() => {});
  };
  const seekBy = (v, s) => { if (v.duration > 0) v.currentTime = Math.max(0, Math.min(v.duration - 0.25, v.currentTime + s)); };
  const mute = (v) => {
    v.muted = !v.muted;
    if (!v.muted && v.volume === 0) v.volume = 1;
    if (!v.muted && v.paused && v.hasAttribute('data-onair')) v.play().catch(() => {});
  };
  // Subtitles: the first track in that language shows, the rest don't; the menu ticks what shows.
  function subPick(pl, v, lang) {
    v._sub = lang;
    let done = false;
    for (let i = 0; i < v.textTracks.length; i++) {
      const t = v.textTracks[i];
      if (t.kind !== 'subtitles' && t.kind !== 'captions') continue;
      const on = !!lang && !done && t.language === lang;
      done = done || on;
      t.mode = on ? 'showing' : 'disabled';
    }
    subTicks(pl, v);
  }
  function subTicks(pl, v) {
    let now = '';
    for (let i = 0; i < v.textTracks.length; i++) if (v.textTracks[i].mode === 'showing') { now = v.textTracks[i].language; break; }
    for (const b of pl.querySelectorAll('[data-sub]')) b.setAttribute('aria-checked', b.dataset.sub === now ? 'true' : 'false');
  }
  // Close the open subtitle menus but `keep`; true when one was open. The focus, if it was in one,
  // goes back to its button.
  function menus(keep) {
    let was = false;
    for (const m of document.querySelectorAll('[data-player] [role="menu"]:not([hidden])')) {
      if (m === keep) continue;
      if (m.contains(document.activeElement) && m.previousElementSibling) m.previousElementSibling.focus();
      m.hidden = true;
      was = true;
    }
    return was;
  }
  addEventListener('pointerdown', (e) => { pointer = e.pointerType || 'mouse'; }, true);
  document.addEventListener('click', (e) => {
    const t = e.target;
    if (!t.closest) return;
    const part = t.closest('[data-player] [data-p], [data-player] [data-sub]');
    const pic = part ? null : t.closest('video');
    const pl = part ? part.closest('[data-player]') : plOf(pic);
    const v = pl && !pl.hidden && !pl.hasAttribute('data-native') && plVideo(pl); /* (the browser's own player: its own clicks) */
    if (!v) { if (!t.closest('[data-player] [role="menu"]')) menus(null); return; }
    const was = pl.hasAttribute('data-on');
    plWake(pl);
    if (!part) { // the picture: a click plays or pauses, a tap shows or hides the bar
      menus(null);
      if (pointer !== 'touch') { if (!v.hasAttribute('data-onair')) play(v); }
      else if (was && !v.paused) { clearTimeout(pl._t); plShow(pl, false); }
      return;
    }
    if (part.tagName === 'INPUT') return;
    e.preventDefault();
    if (part.hasAttribute('data-sub')) { subPick(pl, v, part.dataset.sub); menus(null); return; }
    const what = part.dataset.p, menu = pl.querySelector('[role="menu"]');
    menus(what === 'subs' ? menu : null);
    if (what === 'play') play(v);
    else if (what === 'back') seekBy(v, -10);
    else if (what === 'fwd') seekBy(v, 10);
    else if (what === 'mute') mute(v);
    else if (what === 'subs' && menu) {
      subTicks(pl, v);
      menu.hidden = !menu.hidden;
      const c = !menu.hidden && !e.detail && menu.querySelector('[aria-checked="true"]'); // (a key, not a pointer)
      if (c) c.focus();
    }
  });
  // The seek bar follows the finger (one seek at a time) and lands where it's let go.
  const dragEnd = (pl) => {
    if (!pl._drag) return;
    pl._drag = false;
    const v = plVideo(pl);
    if (v) { v.currentTime = pl._to; plSync(pl, v); }
  };
  document.addEventListener('input', (e) => {
    const r = e.target, pl = r.closest && r.closest('[data-player]'), v = pl && plVideo(pl);
    if (!v || !r.dataset.p) return;
    plWake(pl);
    if (r.dataset.p === 'vol') {
      v.volume = +r.value;
      v.muted = +r.value === 0;
    } else if (r.dataset.p === 'seek') {
      pl._drag = true;
      pl._to = +r.value;
      plSync(pl, v);
      if (!v.seeking) v.currentTime = pl._to;
    }
  });
  document.addEventListener('change', (e) => {
    const pl = e.target.closest && e.target.dataset.p === 'seek' && e.target.closest('[data-player]');
    if (pl) dragEnd(pl);
  });
  addEventListener('pointerup', () => { for (const pl of players()) dragEnd(pl); }, true);
  let moved = 0; // (a mouse's moves only: a tap's made-up mouse events would undo the tap)
  addEventListener('pointermove', (e) => {
    if (e.pointerType === 'touch' || Date.now() - moved < 150) return;
    moved = Date.now();
    for (const pl of players()) if (pl.parentNode.contains(e.target)) plWake(pl);
  }, true);
  const KEY_CODES = {
    13: 'Enter', 32: ' ', 37: 'ArrowLeft', 38: 'ArrowUp', 39: 'ArrowRight', 40: 'ArrowDown', 70: 'f', 75: 'k', 77: 'm',
    415: 'MediaPlay', 19: 'MediaPause', 10252: 'MediaPlayPause', 412: 'MediaRewind', 417: 'MediaFastForward',
  };
  // (registered before [data-arrows]: on a player the arrows are its keys first)
  function playerKeys(e) {
    if (e.defaultPrevented || e.ctrlKey || e.metaKey || e.altKey) return;
    const t = document.activeElement, none = unfocused();
    let pl = null;
    for (const p of players()) if (none || p.parentNode.contains(t)) { pl = p; break; }
    const v = pl && !pl.hasAttribute('data-native') && plVideo(pl); /* (the browser's own player: its own keys) */
    if (!v) return;
    if (!none && (t.isContentEditable || /^(TEXTAREA|SELECT)$/.test(t.tagName) || (t.tagName === 'INPUT' && t.type !== 'range'))) return;
    plWake(pl);
    // (a TV's browser may not name its keys: by their numbers then - OK, Space, arrows, the remote's
    // play/pause/rewind/forward buttons)
    const k = e.key && e.key !== 'Unidentified' ? e.key : KEY_CODES[e.keyCode] || '';
    const live = v.hasAttribute('data-onair'), onSeek = !none && t.dataset.p === 'seek';
    if (!live && /^Media(Play|Pause|PlayPause|Rewind|FastForward)$/.test(k)) {
      e.preventDefault();
      if (k === 'MediaRewind' || k === 'MediaFastForward') seekBy(v, k === 'MediaRewind' ? -10 : 10);
      else if (k === 'MediaPlayPause' || (k === 'MediaPlay') === v.paused) play(v);
      return;
    }
    // the picture, or one of its buttons (a computer's: Space and the arrows stay the video's, as on
    // any player); a TV's bar ([data-arrows]) moves the remote's focus between them instead
    const bare = none || t === v || (t.tagName === 'BUTTON' && pl.contains(t) && !t.closest('[data-arrows], [role="menu"]'));
    const up = k === 'ArrowUp' || k === 'Up', down = k === 'ArrowDown' || k === 'Down';
    const item = !none && t.closest('[role="menu"]') && t; // up and down the subtitles menu
    let act = null;
    if (item && (up || down)) {
      const s = up ? item.previousElementSibling : item.nextElementSibling;
      if (s) act = () => s.focus();
    } else if (!live && (k === 'k' || k === 'K' || (k === ' ' && (bare || onSeek)) || (k === 'Enter' && (t === v || none)))) act = () => play(v);
    else if (live && k === 'Enter' && none) act = () => { // (OK with nothing focused: the sound - or the picture, if it never started)
      if (!v.paused) mute(v);
      else {
        v.muted = false;
        const p = v.play();
        if (p && p.catch) p.catch(() => {});
      }
    };
    else if (!live && (bare || onSeek) && (k === 'ArrowLeft' || k === 'Left')) act = () => seekBy(v, -10);
    else if (!live && (bare || onSeek) && (k === 'ArrowRight' || k === 'Right')) act = () => seekBy(v, 10);
    else if (k === 'm' || k === 'M') act = () => mute(v);
    else if (k === 'f' || k === 'F') {
      const b = pl.querySelector('[data-fullscreen]');
      if (b && !b.hidden) act = () => b.click();
    } else if ((up || down) && t === v) {
      const b = pl.querySelector(live ? '[data-p="mute"]' : '.pl-bar [data-p="play"]');
      if (b) act = () => b.focus();
    } else if (up && onSeek) act = () => v.focus();
    else if (down && onSeek) {
      const b = pl.querySelector('.pl-bar [data-p="play"]');
      if (b) act = () => b.focus();
    }
    if (!act) return;
    e.preventDefault();
    act();
  }

  // ------------------------------------------------------------- full screen
  // [data-fullscreen="id"]: #id in full screen - the player with what's drawn over it (the video
  // element alone would hide the card and the controls). aria-pressed says whether it is. Where a
  // page can't go full screen (an iPhone) the video can: Apple's own player then.
  const fsOK = !!(document.fullscreenEnabled || document.webkitFullscreenEnabled);
  const fsEl = () => document.fullscreenElement || document.webkitFullscreenElement || null;
  const fsVideo = (b) => {
    const box = document.getElementById(b.dataset.fullscreen);
    return box && box.querySelector('video');
  };
  function fsButton(b) {
    const v = fsVideo(b);
    if (b.hidden && (fsOK || (v && v.webkitEnterFullscreen))) b.hidden = false;
    const f = fsEl();
    b.setAttribute('aria-pressed', f && f.getAttribute('id') === b.dataset.fullscreen ? 'true' : 'false');
  }
  document.addEventListener('click', (e) => {
    const b = e.target.closest && e.target.closest('[data-fullscreen]');
    if (!b) return;
    e.preventDefault();
    let p = null;
    if (fsEl()) p = (document.exitFullscreen || document.webkitExitFullscreen).call(document);
    else if (fsOK) {
      const box = document.getElementById(b.dataset.fullscreen);
      if (box) p = (box.requestFullscreen || box.webkitRequestFullscreen).call(box);
    } else {
      const v = fsVideo(b);
      if (v && v.webkitEnterFullscreen) v.webkitEnterFullscreen();
    }
    if (p && p.catch) p.catch(() => {});
  });
  document.addEventListener('dblclick', (e) => { // on the picture, like a player's own
    const v = e.target.closest && e.target.closest('video');
    if (!v) return;
    for (const b of document.querySelectorAll('[data-fullscreen]')) {
      const box = document.getElementById(b.dataset.fullscreen);
      if (b.hidden || !box || !box.contains(v)) continue;
      e.preventDefault(); // (not the video's own full screen as well)
      b.click();
      return;
    }
  });
  // Full screen and a few seconds without the mouse or a key: <html data-idle> (the buttons over
  // the picture fade, the pointer hides) until the next move.
  let idle = 0;
  const wake = () => {
    clearTimeout(idle);
    if (root.hasAttribute('data-idle')) root.removeAttribute('data-idle');
    if (fsEl()) idle = setTimeout(() => { if (fsEl()) root.setAttribute('data-idle', ''); }, 3000);
  };
  const fsChange = () => { document.querySelectorAll('[data-fullscreen]').forEach(fsButton); wake(); };
  document.addEventListener('fullscreenchange', fsChange);
  document.addEventListener('webkitfullscreenchange', fsChange);
  for (const t of ['mousemove', 'mousedown', 'touchstart', 'keydown']) addEventListener(t, wake, true);

  document.querySelectorAll('video,audio').forEach(media);
  document.querySelectorAll('[data-fullscreen]').forEach(fsButton);
  starts();
  gaps();
  fits();
  arrowsSettle(null);
  connect();
})();
