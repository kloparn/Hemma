// web/wire.js — Hemma's browser wire. Deliberately generic: the C server
// renders every page, and this file only
//   1. keeps a WebSocket to the server (reconnecting on its own),
//   2. turns link clicks and form submits into messages,
//   3. morphs the DOM into whatever HTML the server sends back,
//   4. shows toasts and reports media progress ([data-report]).
// Without it everything still works as plain links and POST forms.
// Markup hooks: form[data-a] action · [data-live] live input · [data-auto] submit on change
//               [data-esc] Escape target · [data-key] keyboard shortcut · [data-static] never morphed
//               a[data-native] full page load · form[data-keep] don't reset · form[data-confirm]
//               form[data-upload] send the chosen files (see upload() below); with
//               data-convert/data-wasm the server's converter (C, as WebAssembly) can make
//               play-everywhere copies first, in Workers (web/conv.js)
//               [data-start="id"] open a horizontal scroller at that child
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
        if (next && app) { morph(app, next); settle(); starts(); gaps(); }
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
    if (from.hasAttribute('data-static') && to.hasAttribute('data-static') && from.id === to.id) return;
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

  function kids(from, to) {
    const keyed = new Map(), wanted = new Set();
    for (let c = from.firstChild; c; c = c.nextSibling) if (c.nodeType === 1 && c.id) keyed.set(c.id, c);
    for (let c = to.firstChild; c; c = c.nextSibling) if (c.nodeType === 1 && c.id) wanted.add(c.id);
    let cur = from.firstChild;
    for (let nxt = to.firstChild; nxt;) {
      const after = nxt.nextSibling;
      let match = null;
      if (nxt.nodeType === 1 && nxt.id) {
        match = keyed.get(nxt.id) || null;
        if (match) keyed.delete(nxt.id);
      } else {
        // step over keyed nodes: gone ones are dropped now, moved ones get placed
        // when their turn comes - so the unkeyed node after them still lines up
        while (cur && cur.nodeType === 1 && cur.id) {
          const n = cur.nextSibling;
          if (!wanted.has(cur.id)) cur.remove();
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

  // New nodes: focus [autofocus] (when nothing else has focus) and wire media.
  function settle() {
    const els = fresh;
    fresh = [];
    for (const el of els) {
      if (el.nodeType !== 1) continue;
      const af = el.matches('[autofocus]') ? el : el.querySelector('[autofocus]');
      if (af && (!document.activeElement || document.activeElement === document.body)) af.focus();
      for (const m of el.matches('video,audio') ? [el] : el.querySelectorAll('video,audio')) media(m);
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

  // <video data-resume="123" data-report="media.progress" data-id="..." data-next="/tv/1">
  function media(m) {
    if (m._wired) return;
    m._wired = true;
    const resume = parseFloat(m.dataset.resume || '0');
    if (resume > 0) {
      // "continue watching" (data-report) skips a resume point in the last seconds;
      // live playback (e.g. a TV channel) always joins at the exact offset.
      const guard = m.dataset.report ? 5 : 0;
      m.addEventListener('loadedmetadata', () => {
        if (!m.duration || resume < m.duration - guard) m.currentTime = resume;
      }, { once: true });
    }
    let last = 0;
    const tell = (ended) => m.dataset.report && send({
      t: 'live', a: m.dataset.report, url: here(),
      f: [['id', m.dataset.id || ''], ['pos', String(Math.floor(m.currentTime || 0))],
          ['dur', String(Math.floor(m.duration || 0))], ['ended', ended ? '1' : '']],
    });
    m.addEventListener('timeupdate', () => {
      if (Math.abs(m.currentTime - last) >= 15) { last = m.currentTime; tell(false); }
    });
    m.addEventListener('pause', () => { if (!m.ended) tell(false); });
    m.addEventListener('ended', () => {
      tell(true);
      if (m.dataset.next) send({ t: 'nav', url: m.dataset.next }) || (location.href = m.dataset.next);
    });
  }

  document.querySelectorAll('video,audio').forEach(media);
  starts();
  gaps();
  connect();
})();
