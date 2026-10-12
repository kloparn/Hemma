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
//               video[data-onair] live: plays at "now", no pausing or winding · [data-mute] its sound
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
          morph(app, next); settle(); starts(); gaps(); arrowsSettle(was);
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

  // <video data-onair data-resume="S">: a live broadcast (a TV channel). It plays where the
  // programme is now - S when it came (m._at, m._t0: see media()), plus the time since - and can't
  // be paused, wound or sped up: it jumps back to now and plays on. Only the sound is the viewer's
  // ([data-mute]); their choice carries over to the next programme. A browser that won't start it
  // with sound starts it muted.
  let onairMuted = false;
  const muteShow = (v) => {
    for (const b of document.querySelectorAll('[data-mute]')) b.setAttribute('aria-pressed', v.muted ? 'true' : 'false');
  };
  function onair(m) {
    const now = () => m._at + (performance.now() - m._t0) / 1000;
    const snap = () => {
      if (m.duration && Math.abs(m.currentTime - now()) > 2) m.currentTime = Math.min(now(), m.duration - 0.5);
    };
    const go = () => {
      const p = m.play();
      if (p && p.catch) p.catch(() => {
        if (m.muted) return;
        m.muted = onairMuted = true; // no sound without a press first: muted, the button says so
        muteShow(m);
        m.play().catch(() => {});
      });
    };
    m.addEventListener('loadedmetadata', () => { snap(); go(); });
    m.addEventListener('pause', () => { if (!m.ended) { snap(); go(); } });
    m.addEventListener('seeked', snap); // its own jump lands within 2 s: no loop
    m.addEventListener('ratechange', () => { if (m.playbackRate !== 1) m.playbackRate = 1; });
    m.addEventListener('volumechange', () => muteShow(m));
    document.addEventListener('visibilitychange', () => {
      if (!document.hidden && m.isConnected) { snap(); go(); }
    });
  }
  document.addEventListener('click', (e) => {
    const b = e.target.closest && e.target.closest('[data-mute]');
    const v = b && document.querySelector('video[data-onair]');
    if (!v) return;
    e.preventDefault();
    v.muted = onairMuted = !v.muted;
    if (!v.muted && v.paused) v.play().catch(() => {});
    muteShow(v);
  });

  // <video data-resume="123" data-report="media.progress" data-id="..." data-next="/tv/1">.
  // Wired once; when a morph brings the same element a new video (data-static="v": the next
  // episode, the next programme) it starts over with that one's data-resume - and the element
  // stays, so a full screen does too. The listeners read the data-* attributes as they are then.
  function media(m) {
    const src = m.getAttribute('src') || '';
    if (m._src === src) return;
    const again = m._src !== undefined;
    m._src = src;
    m._at = parseFloat(m.dataset.resume || '0');
    m._t0 = performance.now();
    m._last = 0;
    m._away = false; // the "next episode" card put away
    if (m.hasAttribute('data-onair')) { m.muted = onairMuted; muteShow(m); }
    if (again) return;
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
    });
    m.addEventListener('pause', () => { if (!m.ended) tell(false); });
    m.addEventListener('ended', () => {
      tell(true);
      if (m.dataset.next) send({ t: 'nav', url: m.dataset.next }) || (location.href = m.dataset.next);
    });
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

  // ------------------------------------------------------------- full screen
  // [data-fullscreen="id"]: #id in full screen - the player with what's drawn over it (the video
  // element alone would hide the card, the sound button). aria-pressed says whether it is.
  const fsOK = !!(document.fullscreenEnabled || document.webkitFullscreenEnabled);
  const fsEl = () => document.fullscreenElement || document.webkitFullscreenElement || null;
  function fsButton(b) {
    if (fsOK && b.hidden) b.hidden = false;
    const f = fsEl();
    b.setAttribute('aria-pressed', f && f.getAttribute('id') === b.dataset.fullscreen ? 'true' : 'false');
  }
  document.addEventListener('click', (e) => {
    const b = e.target.closest && e.target.closest('[data-fullscreen]');
    if (!b) return;
    e.preventDefault();
    const box = fsEl() ? null : document.getElementById(b.dataset.fullscreen);
    const go = box ? box.requestFullscreen || box.webkitRequestFullscreen : document.exitFullscreen || document.webkitExitFullscreen;
    const p = go && go.call(box || document);
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
  arrowsSettle(null);
  connect();
})();
