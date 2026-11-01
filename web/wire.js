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
//               play-everywhere copies first, in Workers (web/conv.js) - kept in memory instead of on
//               this device's disk when its "ram" box is ticked. A [data-drop] area in it:
//               files and folders dropped (anywhere on the page) or picked are kept on the form,
//               each choice adding to them ([data-drop-clear] empties the list), and with
//               data-preview="<live action>" the server is told them to draw what Start will do
//               input[type=file][data-shrink="N"] a photo goes in its form shrunk (N px, a JPEG data: URL)
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
  let ws = null, open = false, tries = 0, seq = 0, fresh = [], leaving = 0, uploading = false, reloadDue = false;

  // While the browser is navigating away (plain POST, full page load) the old page
  // must not reconnect or reload: that would cancel the navigation in flight.
  const leave = () => { leaving = Date.now(); };
  const isLeaving = () => leaving && Date.now() - leaving < 4000;
  addEventListener('beforeunload', (e) => {
    if (uploading) { e.preventDefault(); e.returnValue = ''; return; } // "leave and stop the upload?"
    leave();
  });
  addEventListener('pageshow', () => { leaving = 0; });

  // wire.js's own words in the page's language (issue #46): the server puts them, translated, in
  // <script type="application/json" id="wire-tr"> (core/ui.c WIRE_TEXTS - a new T() text goes there
  // too; core/i18n_test.c checks). English is the key and stays when there's none; %s and %d take
  // the values in order.
  const TR = (() => { try { return JSON.parse(document.getElementById('wire-tr').textContent) || {}; } catch (_) { return {}; } })();
  const T = (en, ...v) => { let i = 0; return (TR[en] || en).replace(/%[sd]/g, () => String(v[i++])); };

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
      if (subLater) { send(subLater); subLater = null; } // (a subtitles pick made while the socket was down)
      capsTell();
      // a new socket is a new screen to the server: an upload page's preview is asked for again
      for (const f of document.querySelectorAll('form[data-preview]')) if (f._files && f._files.length && !uploading) preview(f, true);
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
  let subLater = null;
  let capsSent = false; // (this load's answer to [data-caps] went: capsTell)
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
    if (u.hash && u.pathname + u.search === here()) { // (an in-page link takes the focus along: "Skip to content", #68)
      location.hash = u.hash;
      const t = document.getElementById(decodeURIComponent(u.hash.slice(1)));
      if (t) t.focus();
      return;
    }
    if (a.hasAttribute('data-back')) { goBack(a); return; }
    send({ t: 'nav', url: u.pathname + u.search });
  });
  // [data-back] goes back without growing the history (issue #98: every Back pushed one more entry,
  // video -> series -> video -> series ...). Each pushed entry remembers the page it came from
  // (history.state.prev, the 'url' op below): when that's where Back leads, the browser's own back;
  // else that page replaces this one (a nav the server answers with replaceState, as for a pop). A
  // TV's browser may also go back by itself on top of ours: a second popstate right after ours is
  // undone (history.forward) - no timer to guess how slow the TV is.
  let backGuard = null;
  function goBack(x) {
    const to = x.getAttribute('href'), st = history.state;
    if (st && st.prev === to) {
      backGuard = { until: Date.now() + 1500, pops: 1, undone: false };
      history.back();
    } else if (!send({ t: 'nav', url: to, pop: 1 })) location.replace(to);
  }
  addEventListener('popstate', () => {
    const g = backGuard;
    if (g && Date.now() < g.until) {
      if (g.pops > 0) g.pops--;
      else if (!g.undone) { g.undone = true; history.forward(); return; } /* (the browser's own back, one too many) */
    }
    if (!send({ t: 'nav', url: here(), pop: 1 })) location.reload();
  });

  document.addEventListener('submit', (e) => {
    const f = e.target, a = f.dataset.a;
    if (f.getAttribute('aria-busy')) { e.preventDefault(); return; }
    // ask first, whichever way the form then travels (socket or plain POST)
    if (f.dataset.confirm && !confirm(f.dataset.confirm)) { e.preventDefault(); return; }
    if (f.dataset.upload) { e.preventDefault(); upload(f); return; }
    const shots = [...f.querySelectorAll('input[type=file][data-shrink]')].filter((x) => x.files && x.files.length);
    if (shots.length && !(a && open)) { // a plain POST can't carry it: say so instead of losing it
      e.preventDefault();
      toast(2, T('Photos need the connection to Hemma - wait a moment and send it again'));
      return;
    }
    if (!a || !open) { if (!e.defaultPrevented) leave(); return; } // a plain POST: the page is leaving
    e.preventDefault();
    const fd = new FormData(f);
    const sub = e.submitter || (pressed && pressed.form === f ? pressed : null);
    if (sub && sub.name) fd.append(sub.name, sub.value);
    const go = () => {
      const id = ++seq;
      pending.set(id, f);
      busy(f, true);
      errsOff(f);
      send({ t: 'act', a, id, url: here(), f: pairs(fd) });
    };
    if (!shots.length) { go(); return; }
    busy(f, true);
    Promise.all(shots.map((x) => shrink(x.files[0], +x.dataset.shrink || 1600).then((u) => fd.set(x.name, u))))
      .then(go, (err) => { busy(f, false); toast(2, err.message); });
  });

  // input[type=file][data-shrink="1600"] (a photo for a note): the picture goes in the form shrunk to at
  // most that many pixels on its longest side, as a JPEG data: URL small enough for one socket message
  // (under 600 KB). The photo's own orientation is applied; the rest of its EXIF (where it was taken)
  // stays behind. The server checks it's a JPEG and keeps it.
  const PHOTO_BYTES = 600 * 1024;
  function picture(file) {
    const viaImg = () => new Promise((ok, no) => {
      const u = URL.createObjectURL(file), i = new Image();
      i.onload = () => { URL.revokeObjectURL(u); ok(i); };
      i.onerror = () => { URL.revokeObjectURL(u); no(new Error(T("That doesn't look like a photo"))); };
      i.src = u;
    });
    return self.createImageBitmap ? createImageBitmap(file, { imageOrientation: 'from-image' }).catch(viaImg) : viaImg();
  }
  async function shrink(file, max) {
    const img = await picture(file);
    for (const [side, q] of [[max, 0.85], [max, 0.7], [Math.round(max * 0.8), 0.7], [Math.round(max * 0.6), 0.6]]) {
      const w = img.naturalWidth || img.width, h = img.naturalHeight || img.height;
      const k = Math.min(1, side / Math.max(w, h));
      const c = document.createElement('canvas');
      c.width = Math.max(1, Math.round(w * k));
      c.height = Math.max(1, Math.round(h * k));
      c.getContext('2d').drawImage(img, 0, 0, c.width, c.height);
      const blob = await new Promise((ok) => c.toBlob(ok, 'image/jpeg', q));
      if (blob && blob.size <= PHOTO_BYTES) {
        return new Promise((ok, no) => {
          const r = new FileReader();
          r.onload = () => ok(r.result);
          r.onerror = () => no(new Error(T("The photo couldn't be read")));
          r.readAsDataURL(blob);
        });
      }
    }
    throw new Error(T('That photo is too big to send - try another'));
  }

  // An error stays by its field until the field changes; a form's own error until any field does.
  const errEdit = (e) => {
    const x = e.target;
    if (x._err) errOff(x._err);
    else if (x.form && x.form._err) errOff(x.form._err);
  };
  document.addEventListener('input', errEdit);
  document.addEventListener('change', errEdit);

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

  // input[data-show="id"]: #id shows the field's value - a slider's number as it's dragged, and
  // again after every morph (the server draws the value it last saw)
  const showValue = (el) => {
    const out = document.getElementById(el.dataset.show);
    if (out && out.textContent !== el.value) out.textContent = el.value;
  };
  const showValues = () => { for (const el of document.querySelectorAll('input[data-show]')) showValue(el); };
  document.addEventListener('input', (e) => { if (e.target.dataset && e.target.dataset.show) showValue(e.target); });

  // A TV remote's Back key (issue #57: remotes rarely send Escape): Samsung's says 10009, LG's 461,
  // others BrowserBack / GoBack; Backspace too, outside a text field. On a page with [data-back]
  // (TV mode's Back) it goes there - through goBack(), which keeps a TV browser that also goes back
  // by itself from taking a second step.
  const BACK_KEYS = { Backspace: 1, BrowserBack: 1, GoBack: 1, XF86Back: 1 }, BACK_CODES = { 8: 1, 166: 1, 461: 1, 10009: 1 };
  const typing = (t) => !!t && (t.isContentEditable || /^(TEXTAREA|SELECT)$/.test(t.tagName) ||
    (t.tagName === 'INPUT' && !/^(button|submit|reset|checkbox|radio|range|image)$/.test(t.type)));
  function remoteBack(e) {
    const x = document.querySelector('[data-back]');
    if (!x || e.ctrlKey || e.metaKey || e.altKey || typing(e.target)) return false;
    e.preventDefault();
    if (fsEl()) { // full screen first, as Escape does
      const p = (document.exitFullscreen || document.webkitExitFullscreen).call(document);
      if (p && p.catch) p.catch(() => {});
      return true;
    }
    if (menus(null)) return true;
    x.click(); /* (the link's click goes through goBack) */
    return true;
  }

  document.addEventListener('keydown', (e) => {
    if (e.key === 'Escape') {
      if (fsEl()) return; // the browser leaves full screen; going back too would be one step too many
      if (menus(null)) { e.preventDefault(); return; } // an open subtitles menu closes first
      const x = document.querySelector('[data-esc]');
      if (x) { e.preventDefault(); x.click(); }
      return;
    }
    if ((BACK_KEYS[e.key] || BACK_CODES[e.keyCode]) && remoteBack(e)) return;
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
  const lessMotion = () => !!(window.matchMedia && matchMedia('(prefers-reduced-motion: reduce)').matches); // (#70)
  const moveTo = (el, smooth) => {
    el.focus({ preventScroll: true });
    el.scrollIntoView({ block: 'nearest', inline: 'nearest', behavior: smooth && !lessMotion() ? 'smooth' : 'auto' });
  };
  // [data-back] (TV mode's Back) is in reach of the arrows too (issue #57: a playing video had no
  // way out on a remote): Up from the picture, Left or Up past the edge of a [data-arrows] area;
  // Down or Right from it returns to where the focus came from (else the page's [autofocus]).
  let backFrom = null;
  const onScreen = (el) => { const r = el.getBoundingClientRect(); return r.width > 0 && r.height > 0; };
  function toBack(from) {
    const x = document.querySelector('[data-back]'), f = fsEl();
    if (!x || x === from || (f && !f.contains(x)) || !onScreen(x)) return false; // (full screen hides what's outside it)
    backFrom = from;
    moveTo(x, false);
    return true;
  }
  function fromBack() {
    let to = backFrom && backFrom.isConnected && onScreen(backFrom) ? backFrom : document.querySelector('[autofocus]');
    if (to && !to.matches(FOCUSABLE)) to = focusables(to)[0] || null;
    if (!to || !onScreen(to)) to = firstIn(document.querySelector('[data-arrows]') || document.body);
    if (to) moveTo(to, false);
    return !!to;
  }
  document.addEventListener('keydown', (e) => playerKeys(e)); // a player's keys come first (see below)
  document.addEventListener('keydown', (e) => {
    const dir = ARROWS[e.key] || ARROW_CODES[e.keyCode];
    if (!dir || e.defaultPrevented || e.ctrlKey || e.metaKey || e.altKey || e.shiftKey) return;
    const t = document.activeElement, idle = !t || t === document.body;
    const area = idle ? document.querySelector('[data-arrows]') : t.closest('[data-arrows]');
    if (!area && !idle && t.matches('[data-back]')) {
      if ((dir === 'd' || dir === 'r') && fromBack()) e.preventDefault();
      return;
    }
    if (!area) return;
    if (idle) { // nothing focused yet: the first press lands somewhere
      const f = firstIn(area);
      if (f) { e.preventDefault(); moveTo(f, false); }
      return;
    }
    if (t.isContentEditable || t.tagName === 'TEXTAREA' || t.tagName === 'SELECT' ||
        (t.tagName === 'INPUT' && !/^(button|submit|reset|checkbox|image)$/.test(t.type))) return; // the caret's keys
    const next = nearest(area, t, dir);
    if (!next) {
      if ((dir === 'l' || dir === 'u') && toBack(t)) e.preventDefault();
      return;
    }
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
  // Dialogs (ui_modal: .modal[role=dialog]; issue #63 - focus stayed on the button behind, and Shift+Tab
  // reached "Sign out"): a dialog that appears takes the focus (its first field, else its title), Tab
  // and Shift+Tab go round inside it, and everything outside is inert (set again after every morph:
  // the server's markup doesn't carry it). When it goes, the focus goes back to what opened it - or,
  // that gone, near where it was (keepSpot's rules). A TV's engine without inert still gets the rest.
  let dlg = null, dlgFrom = null, inerted = [];
  function dialogs(opener) {
    const d = document.querySelector('#app .modal[role="dialog"]');
    for (const el of inerted) el.inert = false;
    inerted = [];
    for (let n = d; n && n.parentElement && n.getAttribute('id') !== 'app'; n = n.parentElement)
      for (const s of n.parentElement.children)
        if (s !== n && 'inert' in s && !s.inert) { s.inert = true; inerted.push(s); }
    if (d) {
      if (!dlg && opener && opener.el && opener.el !== document.body) dlgFrom = opener; // (what had the focus before it)
      const fresh = d !== dlg;
      dlg = d;
      if (fresh || unfocused() || !d.contains(document.activeElement)) {
        const fields = focusables(d.querySelector('.modal-b') || d).filter((el) => el.matches('input,select,textarea'));
        const h = d.querySelector('.modal-h h2');
        if (fields[0]) fields[0].focus();
        else if (h) { h.tabIndex = -1; h.focus(); }
      }
    } else if (dlg) {
      dlg = null;
      const from = dlgFrom;
      dlgFrom = null;
      if (from && unfocused()) {
        if (from.el.isConnected && focusables(document).indexOf(from.el) >= 0) from.el.focus();
        else keepSettle(from.keep);
      }
    }
  }
  document.addEventListener('keydown', (e) => {
    if (e.key !== 'Tab' || !dlg || !dlg.isConnected) return;
    const f = focusables(dlg).filter((el) => el.tabIndex >= 0); // (not the backdrop's close link)
    if (!f.length) return;
    const t = document.activeElement, first = f[0], last = f[f.length - 1];
    if (e.shiftKey && (t === first || !dlg.contains(t))) { e.preventDefault(); last.focus(); }
    else if (!e.shiftKey && (t === last || !dlg.contains(t))) { e.preventDefault(); first.focus(); }
  });

  // Everywhere else (issue #68: ticking a to-do off sent the focus to the top of the page): where the
  // focus was - its item (nearest ancestor with an id), which of the item's controls, and the items
  // either side of it - so that a morph that takes it away puts it back: on the same item if it's still
  // in the same list, else the next item there (ticking several in a row), else the one before, else
  // the item wherever it went. A dialog's own focus is the dialog's business (dialogs below).
  function keepSpot() {
    const t = document.activeElement;
    if (!t || t === document.body || !t.closest || !t.closest('#app') || t.closest('[data-arrows]')) return null;
    if (t.closest('.modal')) return null;
    const item = t.parentElement && t.parentElement.closest('[id]'), iid = item && item.getAttribute('id');
    if (!item || iid === 'app' || iid === 'main') return null;
    const sib = (el, dir) => {
      for (let s = el[dir]; s; s = s[dir]) if (s.nodeType === 1 && s.getAttribute('id')) return s.getAttribute('id');
      return '';
    };
    return { url: pageUrl, id: iid, list: item.parentElement, index: focusables(item).indexOf(t),
             next: sib(item, 'nextElementSibling'), prev: sib(item, 'previousElementSibling') };
  }
  function keepSettle(k) {
    if (!k || k.url !== here() || !unfocused()) return;
    const here_ = (id) => { const el = id && document.getElementById(id); return el && el.parentElement === k.list ? el : null; };
    const item = here_(k.id) || here_(k.next) || here_(k.prev) || document.getElementById(k.id);
    if (!item) return;
    const f = focusables(item);
    const el = f[Math.max(0, Math.min(k.index, f.length - 1))];
    if (el) el.focus({ preventScroll: true });
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
  const json = (r) => r.json().catch(() => ({ error: T('Unexpected answer (%d)', r.status) }));
  // No answer at all is the network, not the server (1.4: a PC's Wi-Fi dropped for ten minutes at 4 am, and a
  // night's upload stopped with "The connection keeps dropping" after a minute of tries): it's waited out -
  // asked again every NET_PAUSE, for up to NET_WAIT - and isn't a try. Only the server's answers count.
  // down() = true after a wait (go on asking), false once the network has been gone NET_WAIT.
  const NET_WAIT = 30 * 60 * 1000;
  let netPause = 5000; // (form[data-netpause]: the browser tests' shorter wait)
  const down = async (since) => {
    if (Date.now() - since >= NET_WAIT) return false;
    await pause(netPause);
    return true;
  };
  // The server's converter, built to WebAssembly, can run here (web/conv.js in a Worker).
  // It needs SIMD (Chrome 91, Firefox 89, Safari 16.4): a 30-byte module that uses it tells.
  const canConvert = (() => {
    try {
      return !!(self.Worker && self.Blob && self.WebAssembly) && WebAssembly.validate(new Uint8Array(
        [0, 97, 115, 109, 1, 0, 0, 0, 1, 5, 1, 96, 0, 1, 123, 3, 2, 1, 0, 10, 10, 1, 8, 0, 65, 0, 253, 15, 253, 98, 11]));
    } catch (_) { return false; }
  })();

  // What's chosen: a form with a [data-drop] area keeps its files itself (f._files: [{file, path}]) -
  // dropped (folders too, walked with their paths) or picked, each choice adding to the list;
  // [data-drop-clear] empties it. Others read their file inputs at submit (a picture's form).
  const keeps = (f) => !!f.querySelector('[data-drop]');
  const chosen = (f) => {
    if (keeps(f)) return f._files || [];
    const out = [];
    for (const el of f.querySelectorAll('input[type=file]')) for (const file of el.files) out.push({ file, path: file.webkitRelativePath || file.name });
    return out;
  };
  function keep(f, list) {
    const have = f._files || (f._files = []), seen = new Set(have.map((x) => x.path));
    for (const x of list) if (x.path && !seen.has(x.path)) { seen.add(x.path); have.push(x); }
    preview(f, true);
  }
  // form[data-preview="<live action>"]: the server sorts the chosen files and draws what Start will do
  // (where each goes, what's converted here, what's skipped) - asked again when an option changes. A
  // socket message has room for about 1 MB: a long list goes cut ("all" says how many there are).
  const PREVIEW_BYTES = 900 * 1024;
  let previewTimer = 0;
  function preview(f, now) {
    if (!f.dataset.preview) return;
    clearTimeout(previewTimer);
    previewTimer = setTimeout(() => {
      const all = chosen(f), base = pairs(new FormData(f)).filter(([k]) => k !== 'files' && k !== 'all');
      if (!canConvert || !f.dataset.convert) for (let i = base.length - 1; i >= 0; i--) if (base[i][0] === 'conv') base.splice(i, 1);
      let list = all.map((x) => [x.path, x.file.size]), msg;
      for (;;) { // (the files first: a form's fields beyond 64 are dropped)
        msg = { t: 'live', a: f.dataset.preview, url: here(), f: [['files', list.length ? JSON.stringify(list) : ''], ['all', String(all.length)], ...base] };
        if (list.length <= 1 || new TextEncoder().encode(JSON.stringify(msg)).length <= PREVIEW_BYTES) break;
        list = list.slice(0, Math.floor(list.length / 2));
      }
      send(msg);
    }, now ? 0 : 400);
  }
  const previewing = (t) => t && t.form && t.form.dataset.upload && keeps(t.form) ? t.form : null;
  document.addEventListener('change', (e) => {
    const f = previewing(e.target);
    if (!f) return;
    if (e.target.type === 'file') { // picked: onto the list, and the input is ready for the next pick
      keep(f, [...e.target.files].map((file) => ({ file, path: file.webkitRelativePath || file.name })));
      e.target.value = '';
    } else if (e.target.type !== 'range') preview(f, true); // (the room slider changes nothing there)
  });
  document.addEventListener('input', (e) => { // a series' new name, typed
    const f = previewing(e.target);
    if (f && e.target.type === 'text') preview(f);
  });
  document.addEventListener('keydown', (e) => { // Enter in a name field asks again; it doesn't start the upload
    const f = previewing(e.target);
    if (f && e.key === 'Enter' && e.target.tagName === 'INPUT' && e.target.type === 'text') { e.preventDefault(); preview(f, true); }
  });
  document.addEventListener('click', (e) => {
    const x = e.target.closest && e.target.closest('[data-drop-clear]'), f = x && x.form;
    if (!f) return;
    f._files = [];
    preview(f, true);
  });
  // Dropping: anywhere on a page with a [data-drop] area (a near miss shouldn't open the video in the
  // tab instead); the area shows [data-over] meanwhile. A dropped folder is walked: its files keep
  // their paths ("Show/Season 1/x.mkv"), as a picked folder's do.
  const dropZone = () => document.querySelector('form[data-upload] [data-drop]');
  const hasFiles = (e) => !!e.dataTransfer && [...(e.dataTransfer.types || [])].indexOf('Files') >= 0;
  let dragDepth = 0;
  const over = (z, on) => (on ? z.setAttribute('data-over', '') : z.removeAttribute('data-over'));
  document.addEventListener('dragenter', (e) => {
    const z = dropZone();
    if (!z || !hasFiles(e)) return;
    e.preventDefault();
    dragDepth++;
    over(z, true);
  });
  document.addEventListener('dragover', (e) => {
    const z = dropZone();
    if (!z || !hasFiles(e)) return;
    e.preventDefault();
    e.dataTransfer.dropEffect = 'copy';
    over(z, true); // (a morph meanwhile took the mark away)
  });
  document.addEventListener('dragleave', () => {
    const z = dropZone();
    if (z && --dragDepth <= 0) { dragDepth = 0; over(z, false); }
  });
  document.addEventListener('drop', (e) => {
    const z = dropZone();
    if (!z || !hasFiles(e)) return;
    e.preventDefault();
    dragDepth = 0;
    over(z, false);
    const f = z.closest('form');
    dropped(e.dataTransfer).then((list) => keep(f, list), (err) => report(`drop: ${err && err.message}`));
  });
  async function dropped(dt) {
    // (entries are read now: the drop's items are gone once this event is over)
    const entries = [...(dt.items || [])].map((it) => (it.kind === 'file' && it.webkitGetAsEntry ? it.webkitGetAsEntry() : null));
    if (!entries.some(Boolean)) return [...dt.files].map((file) => ({ file, path: file.name }));
    const out = [];
    const fileOf = (en) => new Promise((ok) => en.file(ok, () => ok(null)));
    const children = (dir) => new Promise((ok) => {
      const r = dir.createReader(), all = [];
      const more = () => r.readEntries((got) => { if (!got.length) ok(all); else { all.push(...got); more(); } }, () => ok(all));
      more();
    });
    const walk = async (en) => {
      if (en && en.isFile) {
        const file = await fileOf(en);
        if (file) out.push({ file, path: en.fullPath.replace(/^\/+/, '') });
      } else if (en && en.isDirectory) for (const c of await children(en)) await walk(c);
    };
    for (const en of entries) await walk(en);
    return out;
  }
  // A folder picker where the browser has one (phones: none - the page shows only the video picker there)
  if (!('webkitdirectory' in document.createElement('input')) || /iPhone|iPad|iPod|Android/.test(navigator.userAgent))
    root.dataset.nodir = '1';

  async function upload(f) {
    if (uploading) { toast(1, T('An upload is already running')); return; }
    // (2.6) form[data-here]: no files of this device's - the server's plan names library files ({remote}), which the
    // converter here reads from the server and makes H.264 ("convert on this device"); the copies go back as an
    // upload's do
    const library = f.dataset.here !== undefined; /* (not `here`: that's the page's address, below) */
    const picked = library ? [] : chosen(f);
    let files = picked.map((x) => x.file);
    if (!library && !files.length) { toast(2, f.dataset.empty || T('Choose a folder or some files first')); return; }
    const base = f.dataset.upload, body = {};
    for (const [k, v] of pairs(new FormData(f))) body[k] = v;
    // the page says where (if it has them); data-part: the pieces a copy in memory goes in (tests: small ones);
    // data-workers (2.7): how many at once - the server's, by this device's cores and its person's power tier
    // data-flaky (tests only): each job's first try fails that many reads, as a browser short of memory did
    const use = { worker: f.dataset.convert, wasm: f.dataset.wasm, part: Number(f.dataset.part) || 0,
      flaky: Number(f.dataset.flaky) || 0, workers: Number(f.dataset.workers) || 0 };
    netPause = Number(f.dataset.netpause) || 5000;
    if (!canConvert || !use.worker || !use.wasm) {
      if (library) { toast(2, T('This browser can\'t convert videos')); return; }
      delete body.conv;
    }
    if (!library) body.files = picked.map((x) => [x.path, x.file.size]);
    uploading = true;
    busy(f, true);
    let made = null, sent = false;
    try {
      const plan = await fetch(base + '/plan', {
        method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body),
      }).then(json);
      if (plan.error) throw new Error(plan.error);
      if (library) {
        files = (plan.remote || []).map((r) => ({ remote: r.u, size: r.s, h264: !!r.h264 }));
        toast(1, T('Converting on this device - keep this page open until it\'s done'));
      }
      // under way: the page shows its progress instead of the preview (the list stays here until it's done:
      // Start again after a failure carries on)
      if (f.dataset.preview) send({ t: 'live', a: f.dataset.preview, url: here(), f: [['files', '']] });
      // [i, at] = file i from byte `at`; [i, at, end] = only up to `end` (the server needs no more);
      // {i, f} = convert file f here first, then send the copy as entry i (the server says how);
      // {i, f, subs: 1} = only read file f's subtitles, and send those (it stays behind)
      const copies = plan.todo.filter((t) => !Array.isArray(t)), plain = plan.todo.filter((t) => Array.isArray(t));
      // the room the copies under way may take: the server's (2.7: the person's setting, or what their power tier's
      // workers need) - else the form's "room" (GB)
      const room = plan.room > 0 ? plan.room : Math.min(256, Math.max(2, Number(body.room) || 8)) * 2 ** 30;
      made = converter(base, plan.batch, files, copies, { ...use, ram: body.ram === '1', h264: body.h264 === '1' }, room);
      // One file at a time, never idle while something can go (1.3; owner: "be always at that limit"): what
      // the converters finished first, then the files that go as they are (in the plan's order: a VobSub's
      // .sub before its .idx), else wait for the next copy. Copies used to go in the list's order, and the
      // line stood still while the next one in it was still being made.
      for (let p = 0; ;) {
        let got = made.take();
        if (!got && p < plain.length) {
          const t = plain[p++];
          await put(`${base}/${plan.batch}/${t[0]}`, files[t[0]], t[1], t[2]);
          continue;
        }
        if (!got && !made.left()) break;
        if (!got) got = await made.next();
        if (got.stop) throw new Error(got.stop);
        await sendMade(`${base}/${plan.batch}/${copies[got.k].i}`, copies[got.k], got.m, files);
      }
      sent = true;
      if (library && copies.length) toast(0, T('Done: the new copies play everywhere')); // (the card says what was skipped)
    } catch (err) {
      toast(2, err.message || T('The upload stopped'));
    } finally {
      if (made) made.stop();
      uploading = false;
      busy(f, false);
      if (keeps(f)) { // done: the list goes; else it stays, shown again - Start carries on
        if (sent) f._files = [];
        else preview(f, true);
      }
      // (a stopped upload keeps its picked files for Start - a library one has none: it loads again either way)
      if (reloadDue && (sent || library) && !isLeaving()) location.reload();
    }
  }

  // What a converter finished: a copy (m.send: its worker sends it; m.drop once it's gone), "it couldn't"
  // (m.error: the original goes), or a file's subtitles alone (job.subs). The file's own subtitles go next to
  // the video - text as .srt, pictures as .sup / .idx + .sub - in the file's order (a second English track is
  // ".2" as the server's converter would name it); for a subtitles-only job, ?pm=1000 says it's done.
  async function sendMade(url, job, m, files) {
    try {
      if (!job.subs) {
        /* (m.plays, 2.5: an MP4 that plays everywhere as it is - the original goes, and nothing is said about it) */
        const s = await ask(url, m.send ? `size=${m.size}` : m.plays ? 'original=1&plays=1' : `original=1&why=${encodeURIComponent(m.error || '')}`);
        if (!s.skip) await put(url, m.send ? m : files[job.f], s.at || 0);
      }
      const tracks = (m.subs || []).concat(m.pics || []).sort((a, b) => (a.t || 0) - (b.t || 0));
      for (const sub of tracks) await (sub.data ? subPictures(url, sub) : subtitle(url, sub));
      if (job.subs) {
        for (let tries = 0, since = 0; tries < 3; tries++) {
          const s = await fetch(`${url}?pm=1000`, { method: 'POST' }).then(json).catch(() => null);
          if (!s && await down(since = since || Date.now())) { tries--; continue; }
          if (!s) break;
          since = 0;
          if (s.stop) throw new Error(s.error);
          if (s.ok) break;
          await pause(1000 * 2 ** tries);
        }
      }
    } finally {
      if (m.drop) m.drop(); // sent (or not needed): the browser lets go of the copy now
    }
  }

  // Copies made here, a few at a time (2.7: data-workers, by the power tier; else half the cores): what's being made and
  // what waits to be sent come from originals of `room` bytes at most (the form's choice; one always
  // may) - copies are no bigger, so that's what this device may hold of them: on its disk, or with
  // use.ram in its memory (conv.js). A finished copy stays in its worker, which sends it (m.send) and
  // is ended once it's sent (m.drop): the browser lets go of it right then - a copy handed to the page
  // stayed until the page's memory was next tidied, and a long upload filled the browser's disk with
  // them. take() hands over whatever finished first ({k, m}), next() waits for it, left() = jobs not
  // handed over yet. Progress goes to the server (?pm=), which renders it like everything else; when it
  // says stop, everything stops ({stop}).
  // 1.3: copies in memory stay under RAM_MAX however much room is set (all of a tab's workers share one
  // process, and past ~16 GB its reads failed: episodes went up unconverted) - beyond it a copy waits on the
  // device's disk, the old way. A job whose read failed ({again}) goes back in line, AGAIN times at most.
  const RAM_MAX = 8 * 2 ** 30, AGAIN = 2;
  function converter(base, batch, files, jobs, use, room) {
    const phone = /Mobi|Android|iPhone|iPad/.test(navigator.userAgent);
    // (2.7) the server's number when it knows this device (data-workers), else half the cores, at most 8; a phone one
    const width = use.workers > 0 ? use.workers
      : phone ? 1 : Math.max(1, Math.min(8, Math.floor((navigator.hardwareConcurrency || 2) / 2)));
    const workers = new Map(), finished = []; // finished: {k, m}, in the order they came
    const ready = new Map(); // job -> the worker holding its finished copy, until it's sent
    const seen = new Map(); // job -> [progress, when the server last heard it]
    const inRam = new Map(); // job -> what its copy keeps in memory (its original's size)
    const tries = new Map(), again = []; // job -> tries so far; jobs back in line
    // (2.6) job -> since when its reads of a library file get no answer (the network), and how far in the furthest of
    // them was: a job that reads past it again was cut off by a passing outage - the next one is waited out afresh; one
    // failing at the same read every time (a disk's bad spot cutting the stream) gives up after NET_WAIT, as before
    const netSince = new Map(), netAt = new Map();
    let next = 0, taken = 0, halted = '', waiting = null;
    // (2.7: a part of a video, its share of it - the server says: Full throttle makes one video in parts side by side)
    const source = (k) => (jobs[k].subs ? 0 : jobs[k].part ? jobs[k].share || 0 : files[jobs[k].f].size); // (reading subtitles keeps nothing)
    const ahead = () => [...workers.keys(), ...ready.keys()].reduce((n, k) => n + source(k), 0);
    const held = () => [...inRam.values()].reduce((n, b) => n + b, 0);
    const wake = () => { if (waiting) { const w = waiting; waiting = null; w(); } };
    // the worker sends from its copy; one PUT at a time (put() waits for each)
    const sender = (w) => (url, at) => new Promise((ok) => {
      w.onmessage = (e) => ok(e.data);
      w.onerror = (e) => { e.preventDefault(); ok({ status: 0 }); };
      w.postMessage({ put: url, at });
    });
    const drop = (k) => {
      const w = ready.get(k);
      if (w) { w.terminate(); ready.delete(k); }
      inRam.delete(k);
      pump();
    };
    // (fed: what its converter has taken in of the original - the card shows how fast, 1.5)
    const report = (k, pm, fed) => fetch(`${base}/${batch}/${jobs[k].i}?pm=${pm}${fed >= 0 ? `&fed=${fed}` : ''}`,
      { method: 'POST' }).then(json)
      .then((s) => { if (s.stop) halt(s.error); }, () => {});
    const tell = (k, pm, fed) => {
      seen.set(k, [pm, Date.now(), fed]);
      // (999 at most: 1000 = a copy made, waiting its turn - finish() says it; a subtitles-only job's 1000 is
      // sendMade's, once its subtitles are there)
      report(k, Math.min(pm, 999), fed);
    };
    // a heartbeat: a long stretch without progress (a big file's first look) isn't "the browser left"
    const beat = setInterval(() => {
      for (const k of workers.keys()) {
        const [pm, at, fed] = seen.get(k) || [0, 0, -1];
        if (Date.now() - at >= 10000) tell(k, pm, fed);
      }
    }, 5000);
    const finish = (k, m) => {
      const w = workers.get(k);
      workers.delete(k);
      seen.delete(k);
      if (w && m.error && m.net && !halted) { // (2.6) the network, not the file: waited out (down()), not a try
        const since = netSince.get(k) || Date.now();
        netSince.set(k, since);
        netAt.set(k, Math.max(netAt.get(k) || 0, m.at || 0));
        inRam.delete(k);
        w.terminate();
        down(since).then((on) => {
          if (halted) return;
          if (on) { again.push(k); pump(); } else { netSince.delete(k); netAt.delete(k); finish(k, { error: m.error }); }
        });
        return;
      }
      netSince.delete(k);
      netAt.delete(k);
      if (w && m.error && m.again && !halted && (tries.get(k) || 0) < AGAIN) { // back in line, not up as it is
        tries.set(k, (tries.get(k) || 0) + 1);
        inRam.delete(k);
        w.terminate();
        again.push(k);
        pump();
        return;
      }
      if (m.size === undefined) inRam.delete(k);
      if (w && m.size !== undefined && !halted) { // made: it waits in its worker to be sent
        ready.set(k, w);
        report(k, 1000); // (1.5: the card says "Ready" until it goes, not "Processing 99%")
        m = { size: m.size, subs: m.subs, pics: m.pics, send: sender(w), drop: () => drop(k) };
      } else if (w) {
        w.terminate();
      }
      if (!halted) finished.push({ k, m });
      wake();
      pump();
    };
    function start(k) {
      const job = jobs[k];
      const w = new Worker(use.worker);
      workers.set(k, w);
      w.onmessage = (e) => {
        const m = e.data;
        if (m.pm === undefined) { finish(k, m); return; }
        if (netAt.has(k) && m.fed > netAt.get(k)) { netSince.delete(k); netAt.delete(k); } // (past where it was cut off)
        const [, at] = seen.get(k) || [0, 0];
        if (Date.now() - at >= 3000) tell(k, m.pm, m.fed);
        else seen.set(k, [m.pm, at, m.fed]);
      };
      w.onerror = (e) => { e.preventDefault(); finish(k, { error: T('The converter stopped') }); };
      const ram = !!use.ram && !job.subs && held() + source(k) <= RAM_MAX;
      if (ram) inRam.set(k, source(k));
      w.postMessage({ file: files[job.f], wasm: use.wasm, ram, part: use.part, subs: !!job.subs, h264: !!use.h264, cut: job.part,
        flaky: tries.has(k) ? 0 : use.flaky });
    }
    function pump() {
      for (;;) { // the jobs back in line first, then the next new one - while there's room
        const k = again.length ? again[0] : next < jobs.length ? next : -1;
        if (halted || k < 0 || workers.size >= width || !((!workers.size && !ready.size) || ahead() + source(k) <= room)) return;
        if (again.length) again.shift();
        else next++;
        start(k);
      }
    }
    function halt(why) {
      halted = why || T('The upload was stopped');
      clearInterval(beat);
      for (const w of workers.values()) w.terminate();
      workers.clear();
      for (const w of ready.values()) w.terminate();
      ready.clear();
      finished.length = 0;
      wake();
    }
    pump();
    return {
      take() {
        if (halted) return { stop: halted };
        const got = finished.shift();
        if (got) taken++;
        return got || null;
      },
      next() { return new Promise((ok) => { waiting = ok; }).then(() => this.take() || this.next()); },
      left() { return halted ? 1 : jobs.length - taken; },
      stop() { if (!halted) halt(''); },
    };
  }

  // A subtitle track the converter took out of a file: the server saves it next to the copy. A few
  // tries; past them the video still counts as sent (subtitles are a bonus, not the upload).
  const subQuery = (sub) => `lang=${encodeURIComponent(sub.lang)}&name=${encodeURIComponent(sub.name)}` +
    `&forced=${sub.forced ? 1 : 0}${sub.t === undefined ? '' : `&t=${sub.t}`}`;
  async function subtitle(url, sub) {
    for (let tries = 0, since = 0; tries < 3; tries++) {
      const s = await fetch(`${url}?sub=1&${subQuery(sub)}`, { method: 'POST',
        headers: { 'Content-Type': 'text/plain; charset=utf-8' }, body: sub.text }).then(json).catch(() => null);
      if (!s && await down(since = since || Date.now())) { tries--; continue; }
      if (!s) return;
      since = 0;
      if (s.stop) throw new Error(s.error);
      if (s.ok || s.skip) return;
      await pause(1000 * 2 ** tries);
    }
  }
  // A picture subtitle track (Blu-ray PGS, DVD VobSub - up to 64 MB): one PUT, streamed to the server's disk
  // like a file - a VobSub's .idx text first (idx= bytes of it), then its .sub. A turn's wait ("busy": a
  // file or an earlier try of this one is being written) isn't a try, for a few minutes.
  async function subPictures(url, sub) {
    const idx = sub.kind === 'vobsub' ? new TextEncoder().encode(sub.idx || '') : null;
    const q = `pic=${sub.kind}&${subQuery(sub)}${idx ? `&idx=${idx.length}` : ''}`;
    const body = new Blob(idx ? [idx, sub.data] : [sub.data]);
    for (let tries = 0, waits = 0, since = 0; tries < 4 && waits < 90;) {
      const s = await fetch(`${url}?${q}`, { method: 'PUT', headers: { 'Content-Type': 'application/octet-stream' }, body })
        .then(json).catch(() => null);
      if (!s && await down(since = since || Date.now())) continue;
      if (!s) return;
      since = 0;
      if (s.stop) throw new Error(s.error);
      if (s.ok || s.skip) return;
      if (s.error === 'busy') { waits++; await pause(2000); continue; }
      await pause(1000 * 2 ** tries++);
    }
  }

  // A small POST with retries; {stop} answers end the upload.
  async function ask(url, query) {
    for (let tries = 0, since = 0; ; tries++) {
      let s = await fetch(`${url}?${query}`, { method: 'POST' }).then(json).catch(() => null);
      if (!s && await down(since = since || Date.now())) { tries--; continue; }
      if (s) since = 0;
      s = s || {};
      if (s.stop) throw new Error(s.error);
      if (!s.error && (s.skip || typeof s.at === 'number')) return s;
      if (tries >= 5) throw new Error(s.error || T('The connection keeps dropping'));
      await pause(1000 * 2 ** tries);
    }
  }

  // One file - or a copy its converter holds (src.send) - after a dropped connection, ask the server
  // how far it got and go on from there.
  const LOST = T(
    'The browser lost the copy it made - is its disk full? Make room and start the upload again: what was sent stays');
  async function put(url, src, at, end) {
    const send = src.send
      ? (a) => src.send(url, a).then((x) => {
          if (x.lost) throw new Error(LOST);
          return x.status ? new Response([204, 205, 304].includes(x.status) ? null : x.body, { status: x.status }) : null;
        })
      : (a) => fetch(`${url}?at=${a}`, {
          method: 'PUT', headers: { 'Content-Type': 'application/octet-stream' }, body: src.slice(a, end),
        });
    for (let tries = 0, since = 0; ; tries++) {
      let r = null;
      try {
        r = await send(at);
      } catch (err) {
        if (err && err.message === LOST) throw err; /* else a network hiccup */
      }
      if (r && r.ok) return;
      if (!r && await down(since = since || Date.now())) { // the network: wait, then ask how far it got
        tries--;
        const g = await fetch(url).then(json).catch(() => null);
        if (g && g.stop) throw new Error(g.error);
        if (g && typeof g.at === 'number') at = g.at;
        continue;
      }
      if (r) since = 0;
      const s = r ? await json(r) : {};
      if (s.stop) throw new Error(s.error); // stopped, not allowed, disk full: no point retrying
      if (s.error === 'busy') { // other uploads are writing: wait for a turn, as long as it takes
        await pause(3000);
        tries--;
        continue;
      }
      if (tries >= 8) throw new Error(s.error || T('The connection keeps dropping'));
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
        if (next && app && next.dataset.lang && next.dataset.lang !== root.lang && !uploading) {
          if (!isLeaving()) location.reload(); /* another language: the head (lang, wire-tr) comes with a load */
        } else if (next && app) {
          const was = focusSpot(), kept = keepSpot(), opener = dlg ? null : { el: document.activeElement, keep: kept };
          morph(app, next); settle(); starts(); gaps(); fits(); arrowsSettle(was); keepSettle(kept); dialogs(opener); says();
          showValues();
          pageUrl = here();
        }
        break;
      }
      case 'url':
        if (op[1] !== here()) { /* (a new entry remembers where it came from: goBack) */
          if (op[3]) history.pushState({ prev: here() }, '', op[1]);
          else history.replaceState(history.state, '', op[1]);
        }
        document.title = op[2];
        if (op[3]) scrollTo(0, 0);
        break;
      case 'ack': {
        const f = pending.get(op[1]);
        pending.delete(op[1]);
        if (f) { busy(f, false); if (op[2] && !f.hasAttribute('data-keep')) f.reset(); }
        if (op[2] === false && op[3]) formErr(f, op[3], op[4]); // (an error rides on the ack: issue #60)
        break;
      }
      case 'toast': toast(op[1], op[2], op[3]); break;
      // (not while uploading: the "leave and stop the upload?" question froze the page - and a night's converting
      // with it - until someone answered; a new Hemma's reload came at the socket's return. It comes after.)
      case 'reload': if (uploading) reloadDue = true; else if (!isLeaving()) location.reload(); break;
    }
  }

  // more: {undo: token} - the action kept what it removed (ctx_undo): an Undo button puts it back.
  function toast(kind, text, more) {
    const box = document.getElementById('toasts');
    if (!box) return;
    const el = document.createElement('div');
    el.className = 'toast t-' + (['ok', 'info', 'err'][kind] || 'info');
    el.textContent = text;
    const undo = more && more.undo;
    if (undo) {
      const u = document.createElement('button');
      u.type = 'button';
      u.className = 'toast-undo';
      u.textContent = T('Undo');
      u.onclick = (e) => {
        e.stopPropagation();
        el.remove();
        send({ t: 'act', a: 'core.undo', id: ++seq, url: here(), f: [['k', undo]] });
      };
      el.append(u);
    }
    el.onclick = () => el.remove();
    box.append(el);
    dodge();
    setTimeout(() => { el.classList.add('out'); setTimeout(() => el.remove(), 400); }, undo ? 8000 : kind === 2 ? 6000 : 3500);
  }

  // Toasts never sit on the field being typed in (issue #60: an error covered the very field it was
  // about): over the focused field - a phone's keyboard pushes fields down to them - they go to the top.
  function dodge() {
    const box = document.getElementById('toasts'), t = document.activeElement;
    if (!box) return;
    box.classList.remove('toasts-top');
    if (!box.firstChild || !t || !t.matches('input,textarea,select,[contenteditable]')) return;
    const a = box.getBoundingClientRect(), b = t.getBoundingClientRect();
    if (a.left < b.right && b.left < a.right && a.top < b.bottom && b.top < a.bottom) box.classList.add('toasts-top');
  }
  document.addEventListener('focusin', dodge);

  // ------------------------------------------------------------ form errors
  // An action's error beside the field it names (aria-describedby: a screen reader reads it with the
  // field), or - unnamed, in a form of class "stack" - above the form's buttons; else a toast. They are
  // the browser's own nodes ([data-client]): morphs leave them, the field's change takes them away.
  let errSeq = 0;
  function describe(el, id, on) {
    const ids = (el.getAttribute('aria-describedby') || '').split(' ').filter((x) => x && x !== id);
    if (on) ids.push(id);
    if (ids.length) el.setAttribute('aria-describedby', ids.join(' '));
    else el.removeAttribute('aria-describedby');
  }
  const shown = (el) => el.type !== 'hidden' && !!el.getClientRects().length;
  function formErr(f, text, field) {
    let at = null;
    if (f && f.isConnected && field && f.elements) {
      at = f.elements.namedItem(field);
      if (at && !at.nodeType) at = at[0]; // (radio buttons: the first)
      if (at && !shown(at)) at = null;
    }
    if (!at && !(f && f.isConnected && f.classList.contains('stack'))) { toast(2, text); return; }
    const e = document.createElement('small');
    e.className = 'form-err';
    e.id = 'form-err-' + ++errSeq;
    e.setAttribute('role', 'alert');
    e.setAttribute('data-client', '');
    e.textContent = text;
    if (at) {
      const wrap = at.closest('.field');
      if (wrap && f.contains(wrap) && !wrap.querySelector('.field')) wrap.append(e);
      else at.after(e);
      at._err = e;
      e._for = at;
      errMark(at);
    } else {
      const foot = f.querySelector('.modal-f');
      if (foot && foot.parentNode === f) f.insertBefore(e, foot);
      else f.append(e);
      f._err = e;
      e._for = null;
    }
    e._form = f;
    (f._errs = f._errs || []).push(e);
    if (at && unfocused()) at.focus();
  }
  function errMark(at) {
    at.setAttribute('aria-invalid', 'true');
    describe(at, at._err.id, true);
  }
  function errOff(e) {
    e.remove();
    const at = e._for, f = e._form;
    if (at && at._err === e) {
      at._err = null;
      at.removeAttribute('aria-invalid');
      describe(at, e.id, false);
    }
    if (f) {
      if (f._err === e) f._err = null;
      if (f._errs) f._errs = f._errs.filter((x) => x !== e);
    }
  }
  function errsOff(f) {
    for (const e of f._errs || []) errOff(e);
  }

  // [data-say]: a line a screen reader should hear when it changes - the quick bars' previews ("Car ·
  // Today 20:00-22:00 · Free", a clash; issue #69). Not at every keystroke: once typing pauses, its text
  // goes into ONE polite live region (#wire-say), which is what gets read.
  let sayTimer = 0, said = '';
  function announce(text) {
    let live = document.getElementById('wire-say');
    if (!live) {
      live = document.createElement('div');
      live.id = 'wire-say';
      live.className = 'sr-only';
      live.setAttribute('aria-live', 'polite');
      document.body.append(live);
    }
    live.textContent = text;
  }
  // What a screen reader would read of el: aria-hidden parts left out (an avatar beside its name),
  // the "·" between parts a pause.
  function spoken(el) {
    let s = '';
    const walk = (n) => {
      if (n.nodeType === 3) s += n.nodeValue;
      else if (n.nodeType === 1 && n.getAttribute('aria-hidden') !== 'true' && !n.hidden) {
        const p = n !== el && n.previousElementSibling; // (a line of its own: it starts below the one before)
        const line = p && n.getBoundingClientRect().top >= p.getBoundingClientRect().bottom - 2;
        if (line && /\S\s*$/.test(s) && !/[.,:;!?]\s*$/.test(s)) s += '. ';
        for (let c = n.firstChild; c; c = c.nextSibling) walk(c);
        if (line) s += ' ';
      }
    };
    walk(el);
    return s.replace(/\s*·\s*/g, ', ').replace(/\s+/g, ' ').trim();
  }
  function says() {
    const text = [...document.querySelectorAll('[data-say]')].map(spoken).join(' ').trim();
    clearTimeout(sayTimer);
    if (text === said) return;
    sayTimer = setTimeout(() => { said = text; announce(text); }, 700);
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
    if (from._err) errMark(from); // (the server's markup doesn't know about the error beside it)
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
  // The browser's own nodes ([data-client]: a form error) aren't the server's to take away: morphs
  // step over them.
  const client = (n) => n.nodeType === 1 && n.hasAttribute('data-client');
  const key = (n) => (n.nodeType === 1 && !n.hasAttribute('data-client') && n.getAttribute('id')) || '';
  function kids(from, to) {
    const keyed = new Map(), wanted = new Set();
    for (let c = from.firstChild; c; c = c.nextSibling) if (key(c)) keyed.set(key(c), c);
    for (let c = to.firstChild; c; c = c.nextSibling) if (key(c)) wanted.add(key(c));
    let cur = from.firstChild;
    for (let nxt = to.firstChild; nxt;) {
      const after = nxt.nextSibling;
      while (cur && client(cur)) cur = cur.nextSibling;
      let match = null;
      if (key(nxt)) {
        match = keyed.get(key(nxt)) || null;
        if (match) keyed.delete(key(nxt));
      } else {
        // step over keyed nodes: gone ones are dropped now, moved ones get placed
        // when their turn comes - so the unkeyed node after them still lines up
        while (cur && (key(cur) || client(cur))) {
          const n = cur.nextSibling;
          if (key(cur) && !wanted.has(key(cur))) cur.remove();
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
      if (!client(cur)) cur.remove();
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
    letGo();
    capsTell();
  }

  // What this browser plays (2.5, media/devices.c): a page carrying [data-caps="<type>|<type>|..."] is answered once a
  // load - canPlayType for each, y (probably), m (maybe) or n - as the live media.caps. The list and what it means are
  // the server's.
  function capsTell() {
    const el = capsSent ? null : document.querySelector('[data-caps]');
    if (!el) return;
    const v = document.createElement('video');
    const said = (t) => { const r = v.canPlayType(t); return r === 'probably' ? 'y' : r === 'maybe' ? 'm' : 'n'; };
    // (2.7) with its logical cores: how many videos "On this device" converts at once is the server's to say per tier
    capsSent = send({ t: 'live', a: 'media.caps', url: here(),
      f: [['caps', el.dataset.caps.split('|').map(said).join('')], ['cores', String(navigator.hardwareConcurrency || 0)]] });
  }

  // A video or a book taken off the page (another page came) lets go of its file - a paused one too: it kept it,
  // and a keyboard's play key, the computer's media controls or a phone's lock screen played it on, out of sight
  // (1.6). One still playing is paused first: its pause handler reports the place, then lets go (media(), book()).
  const wired = new Set();
  function letGo() {
    for (const m of wired) {
      if (m.isConnected) continue;
      wired.delete(m);
      if (m._book) sessionDrop(m);
      if (!m.paused) { m.pause(); continue; }
      m.removeAttribute('src');
      m.load();
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
  // edges: 1. the longest cards give up their last rows (li), down to FIT_ROWS each, 1b. then down to
  // FIT_MIN; 2. whole cards go, from the end; 2b. they come back where room is left; 3. a card left
  // alone gets shorter still; then rows come back while they fit. "+N more" says how many went (or adds
  // to the server's own [data-fit-more="N"]); [data-fit-keep] rows stay; a [data-fit-head] (a day's
  // heading) goes when nothing under it is left. From the start after every morph and resize: the
  // server's HTML always has everything.
  // A bounded box (overflow hidden) is laid out here, not by its CSS columns (issue #97: columns keep
  // the order, so a card can't use a gap in an earlier column - cards went while a third of the screen
  // was empty): each card, in order, goes to the shortest column ([data-fit-laid], app.css). As many
  // columns as fit at --fit-col (300px) wide with --fit-gap between, at most [data-fit-cols] and never
  // more than there are cards.
  const FIT_ROWS = 4, FIT_MIN = 2;
  const fitCut = (el) => { el.hidden = true; el.setAttribute('data-fit-cut', ''); };
  const fitShow = (el) => { el.hidden = false; el.removeAttribute('data-fit-cut'); };
  function fit(box) {
    for (const x of box.querySelectorAll('[data-fit-cut]')) fitShow(x);
    for (const x of box.querySelectorAll('.fit-more')) x.remove();
    for (const x of box.querySelectorAll('[data-fit-more]')) x.textContent = T('+%d more', x.dataset.fitMore);
    const shown = () => [...box.children].filter((c) => !c.hidden);
    const css = getComputedStyle(box), laid = css.overflowY === 'hidden';
    if (laid) box.setAttribute('data-fit-laid', '');
    else box.removeAttribute('data-fit-laid');
    const gap = parseFloat(css.getPropertyValue('--fit-gap')) || 18, colW = parseFloat(css.getPropertyValue('--fit-col')) || 300;
    const layout = () => {
      if (!laid) return;
      const cards = shown(), W = box.clientWidth;
      const n = Math.max(1, Math.min(parseInt(box.dataset.fitCols, 10) || 4, cards.length, Math.floor((W + gap) / (colW + gap))));
      const w = (W - gap * (n - 1)) / n, h = new Array(n).fill(0);
      for (const card of cards) {
        let col = 0;
        for (let i = 1; i < n; i++) if (h[i] < h[col] - 1) col = i;
        card.style.width = `${w}px`;
        card.style.left = `${col * (w + gap)}px`;
        card.style.top = `${h[col]}px`;
        h[col] += card.offsetHeight + gap;
      }
    };
    /* (per card: in CSS columns a card too tall for its column doesn't count in the box's scroll size) */
    const spills = () => {
      layout();
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
      if (own) own.textContent = T('+%d more', n + (parseInt(own.dataset.fitMore, 10) || 0));
      else if (n) {
        if (!p) { p = document.createElement('p'); p.className = 'fit-more'; card.append(p); }
        p.textContent = T('+%d more', n);
      } else if (p) p.remove();
    };
    const shorten = (card) => { const r = rows(card); fitCut(r[r.length - 1]); settle(card); };
    for (const floor of [FIT_ROWS, FIT_MIN]) { /* 1. the longest cards, down to FIT_ROWS rows, then FIT_MIN */
      for (let g = 0; g < 400 && spills(); g++) {
        let best = null, most = floor;
        for (const card of shown()) {
          const n = rows(card).length;
          if (n > most) { most = n; best = card; }
        }
        if (!best) break;
        shorten(best);
      }
    }
    while (spills() && shown().length > 1) { const s = shown(); fitCut(s[s.length - 1]); } /* 2. cards, from the end */
    for (const card of [...box.children].filter((c) => c.hasAttribute('data-fit-cut'))) { /* ...back where room's left */
      fitShow(card);
      while (spills() && rows(card).length > FIT_MIN) shorten(card);
      if (spills()) fitCut(card);
    }
    for (let g = 0; g < 400 && spills(); g++) { /* 3. the one card left */
      const card = shown()[0];
      if (!card || rows(card).length <= 1) break;
      shorten(card);
    }
    /* rows back where they fit (a card that went may have made room) - fairly: the card showing the
       fewest rows gets the next one, its first hidden row, until no card can take one more */
    const full = new Set();
    for (let g = 0; g < 400; g++) {
      let best = null, fewest = Infinity;
      for (const card of shown()) {
        if (full.has(card) || !card.querySelector('li[data-fit-cut]:not([data-fit-head])')) continue;
        const n = rows(card).length;
        if (n < fewest) { fewest = n; best = card; }
      }
      if (!best) break;
      const r = best.querySelector('li[data-fit-cut]:not([data-fit-head])');
      fitShow(r);
      settle(best);
      if (spills()) { fitCut(r); settle(best); full.add(best); }
    }
    layout(); /* (as the cards finally are) */
    /* data-fit="<id>": that element names the cards that didn't fit (by their headings) */
    const say = box.dataset.fit && document.getElementById(box.dataset.fit);
    if (say) {
      const gone = [...box.children].filter((c) => c.hasAttribute('data-fit-cut'));
      say.textContent = gone.length ? T("Didn't fit: %s", gone.map((c) => (c.querySelector('h2,h3') || c).textContent.trim()).join(', ')) : '';
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
        m._forcedMute = true; /* (the browser's doing, not the viewer's: not remembered - see sound) */
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
  // The sound - volume and muted - is the viewer's on this device: remembered (localStorage) and given
  // to every video, so a new page, the next episode or a reload no longer starts at full volume (it
  // reset every time). Any change counts (the player's bar, M, the browser's own controls) except
  // the muting a browser forces when it won't start a channel with sound.
  const SOUND = 'hemma.sound';
  const soundSave = (m) => { try { localStorage.setItem(SOUND, JSON.stringify({ v: m.volume, m: m.muted })); } catch (_) {} };
  function soundApply(m) {
    let s = null;
    try { s = JSON.parse(localStorage.getItem(SOUND) || 'null'); } catch (_) {}
    if (!s || typeof s.v !== 'number' || !(s.v >= 0 && s.v <= 1)) return;
    m.volume = s.v;
    if (!m.hasAttribute('muted')) m.muted = !!s.m;
  }

  function media(m) {
    wired.add(m);
    if (m.hasAttribute('data-book')) { book(m); return; } // (an audiobook: the listener, below)
    const src = (m.getAttribute('data-static') || '') + ' ' + (m.getAttribute('src') || ''); // (a channel airs a file again)
    if (m._src === src) return;
    const again = m._src !== undefined;
    m._src = src;
    soundApply(m);
    m._at = parseFloat(m.dataset.resume || '0');
    m._t0 = performance.now();
    m._last = 0;
    m._away = false; // the "next episode" card put away
    m._played = false;
    m._nativeOk = false; /* (a new video: Hemma's player again - see "error") */
    clearTimeout(m._dog);
    if (m.autoplay || m.hasAttribute('data-onair')) watch(m); /* (others: once someone presses play) */
    m._spLists = null; /* (a new video: its own picture subtitles - the pick, v._sub, goes on) */
    if (m._sp) spShow(m, '', '');
    plWire(m);
    if (again) return;
    // its player shows how it's doing; "waiting" (the spinner) ends with anything that says the
    // picture can go on - an engine that never says "playing" mustn't leave it spinning
    const onward = { playing: 1, pause: 1, seeked: 1, canplay: 1, loadeddata: 1 };
    for (const t of ['play', 'pause', 'playing', 'waiting', 'seeked', 'canplay', 'loadeddata', 'volumechange',
      'durationchange', 'progress', 'ended']) {
      m.addEventListener(t, () => {
        if (t === 'playing') { m._played = true; clearTimeout(m._dog); }
        if (t === 'volumechange') { if (m._forcedMute) m._forcedMute = false; else soundSave(m); }
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
    if (m._sub !== undefined) { /* the subtitles picked go on there too: a picture track is still Hemma's to draw */
      n._sub = m._sub;
      subHold(n);
    }
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
    const t = document.activeElement, pl = plOf(m);
    card.hidden = !show;
    if (show) {
      const head = card.querySelector('.media-upnext-t'); // read out once, not each second (issue #64)
      announce(spoken(head || card));
      const af = card.querySelector('[autofocus]');
      if (af && (!t || t === document.body || t === m || (pl && pl.contains(t)))) af.focus();
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
  // ([data-p]): play back fwd seek at dur mute vol subs, and [data-sub="<key>"] in the subtitles
  // menu ("" = off; the choice goes on to the next episode; with data-subpic="<cue list>" a picture
  // track, which the player draws itself - see "picture subtitles"). Keys while the player has the focus (or
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
    subWatch(v);
    if (v._sub === undefined) { /* the viewer's language as a picture track: drawn from the start (a text one is <track default>) */
      const on = pl.querySelector('[data-sub][data-subpic][aria-checked="true"]');
      if (on) v._sub = on.dataset.sub;
    }
    if (v._sub !== undefined) subPick(pl, v, v._sub);
    plSync(pl, v);
    plWake(pl);
  }
  // (attributes only when they change: plSync runs at every timeupdate)
  const attr = (el, name, value) => { if (el && el.getAttribute(name) !== value) el.setAttribute(name, value); };
  function plSync(pl, v) {
    flag(pl, 'data-paused', v.paused); /* (a live channel too: one that never started) */
    flag(pl, 'data-muted', v.muted || v.volume === 0);
    // what the buttons do now, in words (issue #64: "Play" and "Sound" never said paused or muted)
    for (const b of pl.querySelectorAll('[data-p="play"]')) {
      const l = v.paused ? (b.title ? T('Play (Space)') : T('Play')) : b.title ? T('Pause (Space)') : T('Pause');
      attr(b, 'aria-label', l);
      if (b.title) attr(b, 'title', l);
    }
    attr(pl.querySelector('[data-p="mute"]'), 'aria-pressed', v.muted || v.volume === 0 ? 'true' : 'false');
    const d = v.duration, seek = pl.querySelector('[data-p="seek"]');
    if (seek && d > 0 && isFinite(d)) {
      const at = pl._drag ? pl._to : v.currentTime;
      if (!pl._drag) {
        if (+seek.max !== d) seek.max = d;
        seek.value = at;
      }
      pl.querySelector('.pl-fill').style.width = (100 * at / d) + '%';
      attr(seek, 'aria-valuetext', T('%s of %s', clock(at), clock(d))); /* (not a bare "12": #64) */
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
  // make them) - picture ones too (spPlace).
  function plShow(pl, on) {
    if (on === pl.hasAttribute('data-on')) return;
    flag(pl, 'data-on', on);
    const v = plVideo(pl);
    if (v) spPlace(v);
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
    const bar = pl.querySelector('.pl-bar');
    const keyed = bar && !bar.hasAttribute('data-arrows') && bar.contains(document.activeElement) && Date.now() - lastKey < 30000;
    if ((live && v.muted) || pl._drag || keyed || pl.querySelector('[role="menu"]:not([hidden])') ||
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
  // Subtitles by key ([data-sub="<key>"] = the <track id="sub-<key>">: "en", "en.sdh", "en.2" - two
  // tracks in one language are two choices; "" = off): that one shows, the rest don't; the menu ticks
  // what shows. A picture track (Blu-ray, DVD: [data-subpic], no <track>) is drawn by the player itself
  // (see "picture subtitles") - never beside a text one. The pick goes on to the next episode (v._sub):
  // the same key, else the first choice in its language (the key's start; in the menu's order, text and
  // pictures alike), else none.
  function subPick(pl, v, key) {
    v._sub = key;
    subHold(v);
    subTicks(pl, v);
  }
  // The viewer's own pick (the menu, or C) goes to the server too (media.sub, 1.3): the series' next episode
  // starts with it however it's opened - the same key, else its language, else off.
  const subNow = (pl) => { const c = pl.querySelector('[data-sub][aria-checked="true"]'); return c ? c.dataset.sub : ''; };
  function subChoose(pl, v, key) {
    const was = subNow(pl);
    if (was && !key) v._subPrev = was;
    subPick(pl, v, key);
    const msg = { t: 'live', a: 'media.sub', url: here(), f: [['id', v.dataset.id || ''], ['key', key]] };
    if (v.dataset.id && !send(msg)) subLater = msg; /* (sent when the socket's back) */
  }
  // C: subtitles off - or on again: the last shown in this player, else the viewer's ([data-subpref]: their last
  // pick in the series, else their language), else the first
  function subToggle(pl, v) {
    const items = [...pl.querySelectorAll('[data-sub]')].filter((b) => b.dataset.sub);
    if (!items.length) return;
    subTicks(pl, v);
    if (subNow(pl)) { subChoose(pl, v, ''); return; }
    const pick = (v._subPrev && items.find((b) => b.dataset.sub === v._subPrev)) || pl.querySelector('[data-sub][data-subpref]') || items[0];
    subChoose(pl, v, pick.dataset.sub);
  }
  function subHold(v) {
    const key = v._sub, lang = key ? key.split('.')[0] : '', tracks = [], pl = plOf(v);
    for (let i = 0; i < v.textTracks.length; i++) {
      const t = v.textTracks[i];
      if (t.kind === 'subtitles' || t.kind === 'captions') tracks.push(t);
    }
    const items = pl ? [...pl.querySelectorAll('[data-sub]')].filter((b) => b.dataset.sub) : [];
    const item = !key ? null : items.find((b) => b.dataset.sub === key) || items.find((b) => b.dataset.sub.split('.')[0] === lang);
    const pic = (item && item.getAttribute('data-subpic')) || '';
    const on = pic || !key ? null : item ? tracks.find((t) => t.id === `sub-${item.dataset.sub}`) || null
      : tracks.find((t) => t.id === `sub-${key}`) || tracks.find((t) => t.language === lang) || null;
    for (const t of tracks) {
      const mode = t === on ? 'showing' : 'disabled';
      if (t.mode !== mode) t.mode = mode;
    }
    spShow(v, pic ? item.dataset.sub : '', pic);
  }
  // The pick holds: a browser may turn a track on by itself when tracks come and go (Chrome does - the
  // first one, beside the one showing - when the next episode's <track>s take their places).
  function subWatch(v) {
    if (v._subWatch) return;
    v._subWatch = true;
    v.textTracks.addEventListener('change', () => {
      const pl = plOf(v);
      if (v._sub === undefined || !pl || v.hasAttribute('data-native')) return;
      subHold(v);
      subTicks(pl, v);
    });
  }
  function subTicks(pl, v) {
    let now = v._sp && v._sp.url ? v._sp.key : '';
    for (let i = 0; !now && i < v.textTracks.length; i++) {
      const t = v.textTracks[i];
      if (t.mode === 'showing') now = t.id.replace(/^sub-/, '');
    }
    for (const b of pl.querySelectorAll('[data-sub]')) b.setAttribute('aria-checked', b.dataset.sub === now ? 'true' : 'false');
  }

  // The LANGUAGE menu (2.2, [data-snd="<n>"]): the same video in another of its sound tracks - the item's data-src is
  // its address with ?a=<n>, which the server answers with an MP4 holding just that sound (no browser but Safari
  // switches between several). The same element goes on from the same place, playing or not; the pick goes to the
  // server (media.sound), which keeps its language for the series or the film.
  function sndChoose(pl, v, item) {
    for (const b of pl.querySelectorAll('[data-snd]')) b.setAttribute('aria-checked', b === item ? 'true' : 'false');
    const src = item.dataset.src;
    if (src && v.getAttribute('src') !== src) {
      const go = !v.paused;
      v.dataset.resume = String(Math.floor(v.currentTime > 1 ? v.currentTime : parseFloat(v.dataset.resume || '0')));
      v.setAttribute('src', src);
      /* the key the server's next page gives the player ("<id>-a<n>": media_video_key) - so that morph finds it as it
         is and leaves it be, instead of starting it over from the server's resume point */
      for (const el of [v, pl]) {
        const k = el.getAttribute('data-static') || '';
        if (/-a\d+$/.test(k)) el.setAttribute('data-static', k.replace(/-a\d+$/, `-a${item.dataset.snd}`));
      }
      media(v); /* (a new address: it starts at data-resume once it knows its length) */
      if (go) { const p = v.play(); if (p && p.catch) p.catch(() => {}); }
    }
    send({ t: 'live', a: 'media.sound', url: here(), f: [['id', v.dataset.id || ''], ['n', item.dataset.snd]] });
  }

  // ------------------------------------------------------ picture subtitles
  // A Blu-ray (PGS) or DVD (VobSub) track is pictures, which no browser shows as a <track>. Its menu item
  // carries data-subpic="<its cue list>": GET .../<id>.json?v=.. -> {"w":720,"h":480,"cues":[[start_ms,
  // end_ms,x,y,w,h],...]} (places on a w x h frame, times on the video's clock), and cue n's picture is the
  // PNG at the list's address with ".json" -> "/<n>.png" (its query kept). The player draws the cue showing
  // now in a layer of its own in the player's box (.pl-subpic, [data-client]: morphs leave it), over the
  // video's SHOWN picture - its element less the bars object-fit leaves - scaled from the frame, and lifted
  // above the bar while that's up (where it would reach under it). The list is fetched once per video
  // element, the next cue's picture ahead of time; it's looked at on timeupdate/seeked and, while playing,
  // every frame (a lookup; the picture changes only with the cue). The layer is aria-hidden: pictures of
  // words, which a screen reader can't read (a text track, where there is one, is the one for it).
  function spLayer(v, make) {
    const box = v.parentNode;
    if (!box) return null;
    for (const c of box.children) if (c.classList.contains('pl-subpic')) return c;
    if (!make) return null;
    const el = document.createElement('div'), img = document.createElement('img'), pl = plOf(v);
    el.className = 'pl-subpic';
    el.setAttribute('data-client', '');
    el.setAttribute('aria-hidden', 'true');
    el.hidden = true;
    img.alt = '';
    el.append(img);
    box.insertBefore(el, pl && pl.parentNode === box ? pl : v.nextSibling); /* (under the player's bar) */
    return el;
  }
  const spPng = (url, n) => {
    const u = new URL(url, location.href);
    u.pathname = u.pathname.replace(/\.json$/, `/${n}.png`);
    return u.pathname + u.search;
  };
  // the cue showing at t ms (the last to start by then that hasn't ended; -1 none), and the next to come
  function cueAt(cues, t) {
    let lo = 0, hi = cues.length;
    while (lo < hi) {
      const mid = (lo + hi) >> 1;
      if (cues[mid][0] <= t) lo = mid + 1;
      else hi = mid;
    }
    for (let i = lo - 1; i >= 0 && i >= lo - 4; i--) if (t < cues[i][1]) return { at: i, next: lo };
    return { at: -1, next: lo };
  }
  // The track to draw (key, its list's address; '' = none): its list once fetched, the cue of now.
  function spShow(v, key, url) {
    const sp = v._sp || (v._sp = { key: '', url: '', data: null, at: -1 });
    if (sp.url === url) return;
    Object.assign(sp, { key, url, data: null, at: -1, pre: -1 });
    spWire(v);
    spDraw(v);
    if (!url) return;
    const lists = v._spLists || (v._spLists = new Map());
    if (!lists.has(url)) {
      lists.set(url, fetch(url, { credentials: 'same-origin' }).then((r) => (r.ok ? r.json() : null)).catch(() => null)
        .then((d) => (d && Array.isArray(d.cues) && d.w > 0 && d.h > 0 ? d : null)));
    }
    lists.get(url).then((d) => {
      if (sp.url !== url || !v.isConnected) return; /* (another pick, or another video, meanwhile) */
      if (!d) { report(`subtitles ${url.split('?')[0]}: no pictures to draw`); return; } /* (no address key in the log) */
      sp.data = d;
      spDraw(v);
      if (!v.paused) spLoop(v);
    });
  }
  function spDraw(v) {
    const sp = v._sp, el = v.isConnected && spLayer(v, !!(sp && sp.data));
    if (!el) return;
    if (!sp || !sp.data) {
      el.hidden = true;
      if (sp) sp.at = -1;
      return;
    }
    const d = sp.data, { at, next } = cueAt(d.cues, (v.currentTime || 0) * 1000), img = el.firstChild;
    if (next !== sp.pre && next < d.cues.length) { /* the next one's picture, fetched ahead */
      sp.pre = next;
      new Image().src = spPng(sp.url, next);
    }
    if (at === sp.at) return;
    sp.at = at;
    if (at < 0) { el.hidden = true; return; }
    const c = d.cues[at];
    img.style.visibility = 'hidden'; /* (until it's there: not the last cue's picture in this one's place) */
    img.onload = () => { if (sp.at === at) img.style.visibility = ''; };
    img.src = spPng(sp.url, at);
    if (img.complete && img.naturalWidth) img.style.visibility = '';
    img.style.left = `${100 * c[2] / d.w}%`;
    img.style.top = `${100 * c[3] / d.h}%`;
    img.style.width = `${100 * c[4] / d.w}%`;
    img.style.height = `${100 * c[5] / d.h}%`;
    el.hidden = false;
    spPlace(v, true);
  }
  // The layer over the shown picture: object-fit (contain unless said otherwise) leaves bars when the
  // element's shape isn't the video's - pillarbox, letterbox, a full screen of another shape. The cue
  // list's frame (w x h) is mapped onto that, whatever its own shape: MakeMKV's .idx says 720x576 for an
  // NTSC DVD's 720x480 too (spDraw places the picture in % of the frame, never by the video's size).
  // jump: a new cue - in its place at once (only the bar coming and going slides it)
  function spPlace(v, jump) {
    const sp = v._sp, el = sp && sp.data && sp.at >= 0 && v.isConnected && spLayer(v, false);
    if (!el || el.hidden) return;
    const box = el.parentNode, vr = v.getBoundingClientRect(), br = box.getBoundingClientRect();
    const W = v.clientWidth, H = v.clientHeight, nw = v.videoWidth || W, nh = v.videoHeight || H; /* (not known yet: the box) */
    if (!W || !H) return;
    const fit = getComputedStyle(v).objectFit;
    let sx = W / nw, sy = H / nh;
    if (fit !== 'fill') sx = sy = fit === 'cover' ? Math.max(sx, sy) : Math.min(sx, sy);
    const w = nw * sx, h = nh * sy;
    const left = vr.left - br.left - box.clientLeft + v.clientLeft + (W - w) / 2;
    const top = vr.top - br.top - box.clientTop + v.clientTop + (H - h) / 2;
    el.style.left = `${left}px`;
    el.style.top = `${top}px`;
    el.style.width = `${w}px`;
    el.style.height = `${h}px`;
    // the bar up: a picture reaching under it goes up above it (a text track's lines move up too)
    const pl = plOf(v), bar = pl && !pl.hidden && pl.hasAttribute('data-on') && !pl.hasAttribute('data-native') &&
      pl.querySelector('.pl-bar'), edgeOf = bar && (bar.firstElementChild || bar);
    const c = sp.data.cues[sp.at], ctop = c[3] / sp.data.h * h;
    let lift = 0;
    if (edgeOf && edgeOf.getClientRects().length) {
      const edge = edgeOf.getBoundingClientRect().top - br.top - box.clientTop - 8;
      lift = Math.max(0, Math.min(top + (c[3] + c[5]) / sp.data.h * h - edge, ctop));
    }
    const lifted = lift ? `translateY(${-Math.round(lift)}px)` : '';
    const img = el.firstChild;
    if (img.style.transform === lifted) return;
    if (jump) img.style.transition = 'none';
    img.style.transform = lifted;
    if (jump) {
      void img.offsetWidth; /* (that place taken first: no slide from the last cue's) */
      img.style.transition = '';
    }
  }
  function spLoop(v) {
    if (v._spRaf || !v._sp || !v._sp.data) return;
    const step = () => {
      v._spRaf = 0;
      if (!v.isConnected || v.paused || v.ended || !v._sp || !v._sp.data) return;
      spDraw(v);
      v._spRaf = requestAnimationFrame(step);
    };
    v._spRaf = requestAnimationFrame(step);
  }
  function spWire(v) {
    if (v._spWired) return;
    v._spWired = true;
    for (const t of ['timeupdate', 'seeking', 'seeked', 'pause', 'emptied']) v.addEventListener(t, () => spDraw(v));
    for (const t of ['play', 'playing']) v.addEventListener(t, () => spLoop(v));
    for (const t of ['loadedmetadata', 'resize']) v.addEventListener(t, () => spPlace(v)); /* (the video's own size known) */
    if (window.ResizeObserver) new ResizeObserver(() => spPlace(v)).observe(v); /* (full screen, a turned phone) */
  }
  addEventListener('resize', () => { for (const v of document.querySelectorAll('video')) spPlace(v); });
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
  // The keyboard on a player's bar keeps it (issue #64: the focused button faded away with it), for
  // 30 s after the last key; and focus arriving there by keyboard (Tab) shows it.
  let lastKey = 0;
  addEventListener('keydown', () => { lastKey = Date.now(); }, true);
  document.addEventListener('focusin', (e) => {
    const pl = e.target.closest && e.target.closest('[data-player]');
    if (pl && !pl.hidden && !pl.hasAttribute('data-native') && Date.now() - lastKey < 1000) plWake(pl);
  });
  document.addEventListener('click', (e) => {
    const t = e.target;
    if (!t.closest) return;
    const part = t.closest('[data-player] [data-p], [data-player] [data-sub], [data-player] [data-snd]');
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
    if (part.hasAttribute('data-sub')) { subChoose(pl, v, part.dataset.sub); menus(null); return; }
    if (part.hasAttribute('data-snd')) { sndChoose(pl, v, part); menus(null); return; }
    const what = part.dataset.p, menu = part.parentNode.querySelector('[role="menu"]'); /* (its own: subtitles, language) */
    menus(what === 'subs' || what === 'snd' ? menu : null);
    if (what === 'play') play(v);
    else if (what === 'back') seekBy(v, -10);
    else if (what === 'fwd') seekBy(v, 10);
    else if (what === 'mute') mute(v);
    else if ((what === 'subs' || what === 'snd') && menu) {
      if (what === 'subs') subTicks(pl, v);
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
    13: 'Enter', 32: ' ', 37: 'ArrowLeft', 38: 'ArrowUp', 39: 'ArrowRight', 40: 'ArrowDown', 67: 'c', 70: 'f', 75: 'k', 77: 'm',
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
    else if (live && bare && (k === 'ArrowLeft' || k === 'Left') && document.querySelector('[data-back]'))
      act = () => toBack(none ? v : t); /* (live can't be wound: Left on the picture reaches Back - issue #98) */
    else if (!live && (bare || onSeek) && (k === 'ArrowRight' || k === 'Right')) act = () => seekBy(v, 10);
    else if (k === 'm' || k === 'M') act = () => mute(v);
    else if ((k === 'c' || k === 'C') && pl.querySelector('[data-sub]')) act = () => subToggle(pl, v);
    else if (k === 'f' || k === 'F') {
      const b = pl.querySelector('[data-fullscreen]');
      if (b && !b.hidden) act = () => b.click();
    } else if (up && t === v && document.querySelector('[data-back]')) {
      act = () => { if (!toBack(v)) { const b = pl.querySelector(live ? '[data-p="mute"]' : '.pl-bar [data-p="play"]'); if (b) b.focus(); } };
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

  // ------------------------------------------------------------ the listener
  // <audio data-book> (an audiobook, modules/media/books.c; 1.6) with its controls [data-listen="<audio id>"]:
  // the chapter that's on (the page's #media-chapters a[data-at] - seconds in the whole book), a seek bar over the
  // whole book, back 15 s / forward 30 s, previous and next chapter, speed (remembered on this device) and a sleep
  // timer. A book in parts (data-parts = [[address, start, length], ...]) plays them one after the other in the
  // same element; its place is always in the whole book - what's reported (media.progress, every 15 s of it, at a
  // pause, when the page goes out of sight, at the end), what the seek bar and the lock screen show. The LOCK SCREEN
  // (owner: "listen to it while the phone is locked") is the Media Session API: title, author, cover, the chapter;
  // play/pause, 15/30 s and the chapters from there. While the socket is down - a locked phone may close it - the
  // reports go as plain POSTs (keepalive). Without this file the element keeps the browser's own controls.
  const SPEED = 'hemma.speed';
  const lsOf = (a) => document.querySelector(`[data-listen="${CSS.escape(a.id)}"]`);
  const bookAt = (a) => (a._parts[a._part] ? a._parts[a._part].at : 0) + (a.currentTime || 0);
  const partAt = (a, t) => { /* the part holding second t of the book */
    let k = 0;
    while (k + 1 < a._parts.length && t >= a._parts[k + 1].at) k++;
    return k;
  };
  const chapters = () => [...document.querySelectorAll('#media-chapters a[data-at]')]
    .map((el) => ({ at: parseFloat(el.dataset.at) || 0, el, title: (el.querySelector('.media-ch-t') || el).textContent }));
  const chapterAt = (list, t) => { let k = -1; for (let i = 0; i < list.length; i++) if (list[i].at <= t + 0.5) k = i; return k; };

  function book(a) {
    if (a._book) return;
    a._book = true;
    const len = parseFloat(a.dataset.length) || 0;
    let parts;
    try { parts = JSON.parse(a.dataset.parts || 'null'); } catch (_) { parts = null; }
    a._parts = Array.isArray(parts) && parts.length
      ? parts.map((p) => ({ src: String(p[0]), at: +p[1] || 0, len: +p[2] || 0 }))
      : [{ src: (a.getAttribute('src') || '').split('#')[0], at: 0, len }];
    a._len = len;
    const resume = parseFloat(a.dataset.resume || '0');
    a._part = partAt(a, resume);
    a._seekTo = resume > 0 ? resume - a._parts[a._part].at : null; /* (the address's #t= says the same) */
    a._last = resume;
    a._sleepAt = 0;
    a._sleepCh = -1;
    soundApply(a);
    let rate = 1;
    try { rate = parseFloat(localStorage.getItem(SPEED) || '1') || 1; } catch (_) {}
    a._rate = rate >= 0.5 && rate <= 3 ? rate : 1;
    const ls = lsOf(a);
    if (ls) {
      ls.hidden = false;
      a.controls = false;
    }
    const lengthKnown = () => { /* (a book of one file whose length the server couldn't read: the element's) */
      if (a._len > 0 || a._parts.length !== 1 || !(a.duration > 0) || !isFinite(a.duration)) return;
      a._len = a._parts[0].len = a.duration;
    };
    a.addEventListener('durationchange', lengthKnown);
    a.addEventListener('loadedmetadata', () => {
      lengthKnown();
      if (a._seekTo !== null && a._seekTo > 0 && (!a.duration || a._seekTo < a.duration)) a.currentTime = a._seekTo;
      a._seekTo = null;
      a.playbackRate = a._rate; /* (a new part may start at 1) */
      lsSync(a);
    });
    for (const t of ['play', 'pause', 'playing', 'seeked', 'durationchange', 'ratechange', 'volumechange'])
      a.addEventListener(t, () => {
        if (t === 'playing') { a._played = true; clearTimeout(a._dog); }
        if (t === 'play' && !a._played) watch(a);
        if (t === 'play') a.playbackRate = a._rate;
        if (t === 'volumechange') soundSave(a);
        lsSync(a);
        session(a, t === 'play' || t === 'playing' || t === 'pause');
      });
    a.addEventListener('timeupdate', () => {
      const t = bookAt(a);
      if (Math.abs(t - a._last) >= 15) { a._last = t; bookTell(a, false); }
      if ((a._sleepAt && Date.now() >= a._sleepAt) || (a._sleepCh >= 0 && t >= a._sleepCh)) {
        sleepSet(a, '0');
        a.pause();
      }
      lsSync(a);
    });
    a.addEventListener('pause', () => {
      if (!a.ended) bookTell(a, false);
      if (!a.isConnected) { /* (another page came: the lock screen lets go, and so does the stream) */
        sessionDrop(a);
        setTimeout(() => { if (!a.isConnected) { a.removeAttribute('src'); a.load(); } });
      }
    });
    a.addEventListener('ended', () => {
      if (a._part + 1 < a._parts.length) { partGo(a, a._part + 1, 0, true); return; } /* on to the next part */
      bookTell(a, true);
    });
    a.addEventListener('error', () => {
      const e = a.error;
      if (!e || !a.isConnected) return;
      if (ls) flag(ls, 'data-err', true);
      report(`audiobook ${a.dataset.id || a.id}: error ${e.code}${e.message ? ` (${e.message})` : ''}`);
    });
    if (a.readyState >= 1) { /* (it had its length before this file came: the address's #t= placed it, or not) */
      lengthKnown();
      if (a._seekTo !== null && Math.abs(a.currentTime - a._seekTo) > 2) a.currentTime = a._seekTo;
      a._seekTo = null;
    }
    a.playbackRate = a._rate;
    lsSync(a);
  }

  // the place in the book to the server: over the socket, else (a locked phone's socket gone, the page going out
  // of sight) a plain POST that outlives the page; redirect "manual": its answer is a 303 to the page, not wanted
  function bookTell(a, ended, sure) {
    /* (one that let go of its file has nothing to say: its time went back to 0 - "timeupdate" - which isn't the place) */
    if (!a.dataset.report || !(a._len > 0) || !a.hasAttribute('src')) return;
    const f = [['id', a.dataset.id || ''], ['pos', String(Math.floor(bookAt(a)))], ['dur', String(Math.floor(a._len))],
      ['ended', ended ? '1' : '']];
    if (!sure && send({ t: 'live', a: a.dataset.report, url: here(), f })) return;
    try {
      fetch(`/a/${a.dataset.report}`, { method: 'POST', body: new URLSearchParams(f), keepalive: true, redirect: 'manual',
        credentials: 'same-origin' }).catch(() => {});
    } catch (_) {}
  }
  document.addEventListener('visibilitychange', () => {
    if (document.visibilityState !== 'hidden') return;
    for (const a of document.querySelectorAll('audio[data-book]')) if (a._book && !a.paused) bookTell(a, false, true);
  });
  addEventListener('pagehide', () => { for (const a of document.querySelectorAll('audio[data-book]')) if (a._book && a._played) bookTell(a, false, true); });

  // To part k at its second `at` (playing on when `go`, or when it played)
  function partGo(a, k, at, go) {
    const play = go || !a.paused;
    if (k === a._part && a.readyState >= 1) {
      a.currentTime = Math.max(0, at);
      if (play && a.paused) a.play().catch(() => {});
      return;
    }
    a._part = k;
    a._seekTo = at > 0 ? at : null;
    a.src = a._parts[k].src;
    if (play) a.play().catch(() => {});
    lsSync(a);
  }
  const bookSeek = (a, t) => {
    t = Math.max(0, Math.min(a._len > 1 ? a._len - 0.5 : t, t));
    const k = partAt(a, t);
    partGo(a, k, t - a._parts[k].at, false);
    a._last = t;
  };
  const bookBy = (a, s) => bookSeek(a, bookAt(a) + s);
  function chapterGo(a, dir) {
    const list = chapters(), t = bookAt(a), k = chapterAt(list, t);
    if (!list.length) return;
    if (dir < 0) bookSeek(a, k >= 0 && t - list[k].at > 3 ? list[k].at : list[Math.max(0, k - 1)].at);
    else if (k + 1 < list.length) bookSeek(a, list[k + 1].at);
  }

  // The controls, the chapter list and the lock screen as the book is now (timeupdate: often - attributes and text
  // only when they change)
  function lsSync(a) {
    const ls = lsOf(a), t = bookAt(a), len = a._len || 0;
    const list = chapters(), k = chapterAt(list, t);
    for (let i = 0; i < list.length; i++) {
      const on = i === k;
      if (on !== (list[i].el.getAttribute('aria-current') === 'true')) {
        if (on) list[i].el.setAttribute('aria-current', 'true');
        else list[i].el.removeAttribute('aria-current');
      }
    }
    a._chapter = k >= 0 ? list[k].title : '';
    if (!ls) return;
    flag(ls, 'data-paused', a.paused);
    const p = ls.querySelector('[data-b="play"]');
    attr(p, 'aria-label', a.paused ? T('Play') : T('Pause'));
    const ch = ls.querySelector('[data-b="chapter"]');
    if (ch && a._chapter && ch.textContent !== a._chapter) ch.textContent = a._chapter;
    const seek = ls.querySelector('[data-b="seek"]');
    if (seek && len > 0) {
      const at = ls._drag ? ls._to : t;
      if (!ls._drag) { if (+seek.max !== len) seek.max = len; seek.value = at; }
      const fill = ls.querySelector('.media-ab-fill');
      if (fill) fill.style.width = (100 * Math.min(at, len) / len) + '%';
      attr(seek, 'aria-valuetext', T('%s of %s', clock(at), clock(len)));
      const at1 = ls.querySelector('[data-b="at"]'), left = ls.querySelector('[data-b="left"]');
      if (at1 && at1.textContent !== clock(at)) at1.textContent = clock(at);
      if (left && left.textContent !== '-' + clock(len - at)) left.textContent = '-' + clock(len - at);
    }
    for (const b of ls.querySelectorAll('[data-speed]')) attr(b, 'aria-checked', +b.dataset.speed === a._rate ? 'true' : 'false');
    const sp = ls.querySelector('[data-speed][aria-checked="true"]'), spt = ls.querySelector('[data-b="speed-t"]');
    if (sp && spt && spt.textContent !== sp.textContent) spt.textContent = sp.textContent;
    const sl = ls.querySelector('[data-b="sleep-t"]');
    if (sl) {
      if (sl._was === undefined) sl._was = sl.textContent; /* ("Sleep timer", as the server said it) */
      const on = ls.querySelector('[data-sleep][aria-checked="true"]');
      const text = a._sleepAt ? clock(Math.max(0, (a._sleepAt - Date.now()) / 1000))
        : a._sleepCh >= 0 && on ? on.textContent : sl._was;
      if (sl.textContent !== text) sl.textContent = text;
    }
    sessionPlace(a);
  }
  function sleepSet(a, v) {
    a._sleepAt = 0;
    a._sleepCh = -1;
    if (v === 'ch') {
      const list = chapters(), k = chapterAt(list, bookAt(a));
      a._sleepCh = k + 1 < list.length ? list[k + 1].at : a._len;
    } else if (+v > 0) a._sleepAt = Date.now() + v * 60000;
    const ls = lsOf(a);
    if (ls) for (const b of ls.querySelectorAll('[data-sleep]')) attr(b, 'aria-checked', b.dataset.sleep === String(v) ? 'true' : 'false');
    lsSync(a);
  }
  setInterval(() => { /* (the sleep timer's minutes count down while nothing else moves) */
    for (const a of document.querySelectorAll('audio[data-book]')) if (a._book && a._sleepAt) lsSync(a);
  }, 1000);

  // The lock screen: what plays (the chapter as its title), and its buttons - once per book element, when it plays.
  // THE APP (2.0): the Hemma app for Android gives its pages window.HemmaApp - a WebView shows nothing of the Media
  // Session on the lock screen, so the app has its own (apps/android PlayerService): it's told what plays
  // (HemmaApp.media(json): state playing | paused | none, title, artist, album, art, duration, position, rate - at
  // every change and every 4 s) and its buttons come back through window.hemmaAction(what, seconds), the same
  // actions as the Media Session's.
  const hemmaApp = window.HemmaApp && typeof window.HemmaApp.media === 'function' ? window.HemmaApp : null;
  let lockBook = null;
  const lockActs = {};
  window.hemmaAction = (what, arg) => { const f = lockBook && lockActs[what]; if (f) f(typeof arg === 'number' ? { seekTime: arg } : {}); };
  function appTell(a, state) {
    if (!hemmaApp) return;
    const art = a.dataset.art ? new URL(a.dataset.art, location.href).href : '';
    try {
      hemmaApp.media(JSON.stringify(state === 'none' ? { state } : {
        state, title: a._chapter || a.dataset.title || '', artist: a.dataset.author || '', album: a.dataset.title || '', art,
        duration: a._len || 0, position: Math.min(bookAt(a), a._len || Infinity), rate: a.playbackRate || 1,
      }));
    } catch (_) {}
  }
  function session(a, now) {
    if (!now) return;
    const ms = 'mediaSession' in navigator ? navigator.mediaSession : null;
    if (lockBook !== a) {
      lockBook = a;
      const on = (what, fn) => { lockActs[what] = fn; try { if (ms) ms.setActionHandler(what, fn); } catch (_) {} };
      on('play', () => a.play().catch(() => {}));
      on('pause', () => a.pause());
      on('stop', () => a.pause());
      on('seekbackward', (d) => bookBy(a, -((d && d.seekOffset) || 15)));
      on('seekforward', (d) => bookBy(a, (d && d.seekOffset) || 30));
      on('seekto', (d) => { if (d && typeof d.seekTime === 'number') bookSeek(a, d.seekTime); });
      on('previoustrack', () => chapterGo(a, -1));
      on('nexttrack', () => chapterGo(a, 1));
    }
    if (ms) ms.playbackState = a.paused ? 'paused' : 'playing';
    a._metaFor = null; /* (the app hears every change of state) */
    sessionMeta(a);
  }
  // (a book taken off the page: the lock screen forgets it, and its buttons go - they'd play it on, unseen)
  function sessionDrop(a) {
    if (lockBook !== a) return;
    lockBook = null;
    for (const k of Object.keys(lockActs)) delete lockActs[k];
    appTell(a, 'none');
    const ms = 'mediaSession' in navigator ? navigator.mediaSession : null;
    if (!ms) return;
    ms.metadata = null;
    ms.playbackState = 'none';
    for (const what of ['play', 'pause', 'stop', 'seekbackward', 'seekforward', 'seekto', 'previoustrack', 'nexttrack'])
      try { ms.setActionHandler(what, null); } catch (_) {}
  }
  function sessionMeta(a) {
    const ms = 'mediaSession' in navigator ? navigator.mediaSession : null, title = a._chapter || a.dataset.title || '';
    if (lockBook !== a || a._metaFor === title) return;
    a._metaFor = title;
    appTell(a, a.paused ? 'paused' : 'playing');
    if (!ms || !window.MediaMetadata) return;
    const art = a.dataset.art ? [{ src: new URL(a.dataset.art, location.href).href }] : [];
    try {
      ms.metadata = new MediaMetadata({ title, artist: a.dataset.author || '', album: a.dataset.title || '', artwork: art });
    } catch (_) {}
  }
  function sessionPlace(a) {
    if (lockBook !== a) return;
    sessionMeta(a);
    const now = Date.now(), ms = 'mediaSession' in navigator ? navigator.mediaSession : null;
    if (now - (a._placed || 0) < 4000 || !(a._len > 0)) return;
    a._placed = now;
    appTell(a, a.paused ? 'paused' : 'playing');
    if (!ms || !ms.setPositionState) return;
    try { ms.setPositionState({ duration: a._len, playbackRate: a.playbackRate || 1, position: Math.min(bookAt(a), a._len) }); } catch (_) {}
  }

  // The controls: their buttons, menus (speed, sleep timer), the seek bar; a chapter in the list goes there
  const lsAudio = (ls) => document.getElementById(ls.dataset.listen);
  function lsMenus(keep) {
    for (const m of document.querySelectorAll('[data-listen] [role="menu"]:not([hidden])')) if (m !== keep) m.hidden = true;
  }
  // (capturing: before the links' own handler, which would load the chapter's address - it's for a page without
  // this file)
  document.addEventListener('click', (e) => {
    const jump = e.target.closest && e.target.closest('#media-chapters a[data-at]'), a = jump && document.querySelector('audio[data-book]');
    if (!a || !a._book || e.button !== 0 || e.metaKey || e.ctrlKey || e.shiftKey || e.altKey) return;
    e.preventDefault(); /* a chapter: there, playing */
    bookSeek(a, parseFloat(jump.dataset.at) || 0);
    if (a.paused) a.play().catch(() => {});
  }, true);
  document.addEventListener('click', (e) => {
    const t = e.target;
    if (!t.closest) return;
    /* (buttons only: the seek bar is an input, and a button's own label may be a [data-b] too - speed-t, sleep-t) */
    const part = t.closest('[data-listen] button[data-b], [data-listen] [data-speed], [data-listen] [data-sleep]');
    const ls = part && part.closest('[data-listen]'), a = ls && lsAudio(ls);
    if (!a || !a._book) { if (!t.closest('[data-listen] [role="menu"]')) lsMenus(null); return; }
    e.preventDefault();
    if (part.hasAttribute('data-speed')) {
      a._rate = +part.dataset.speed || 1;
      a.playbackRate = a._rate;
      try { localStorage.setItem(SPEED, String(a._rate)); } catch (_) {}
      lsMenus(null);
      lsSync(a);
      return;
    }
    if (part.hasAttribute('data-sleep')) { sleepSet(a, part.dataset.sleep); lsMenus(null); return; }
    const what = part.dataset.b;
    if (what === 'speed' || what === 'sleep') {
      const menu = part.parentNode.querySelector('[role="menu"]');
      lsMenus(menu);
      if (menu) {
        menu.hidden = !menu.hidden;
        const c = !menu.hidden && !e.detail && menu.querySelector('[aria-checked="true"]'); // (a key, not a pointer)
        if (c) c.focus();
      }
      return;
    }
    lsMenus(null);
    if (what === 'play') { if (a.paused) a.play().catch(() => {}); else a.pause(); }
    else if (what === 'back') bookBy(a, -15);
    else if (what === 'fwd') bookBy(a, 30);
    else if (what === 'prev') chapterGo(a, -1);
    else if (what === 'next') chapterGo(a, 1);
  });
  document.addEventListener('input', (e) => {
    const r = e.target, ls = r.dataset && r.dataset.b === 'seek' && r.closest('[data-listen]'), a = ls && lsAudio(ls);
    if (!a || !a._book) return;
    ls._drag = true;
    ls._to = +r.value;
    lsSync(a);
  });
  document.addEventListener('change', (e) => {
    const r = e.target, ls = r.dataset && r.dataset.b === 'seek' && r.closest('[data-listen]'), a = ls && lsAudio(ls);
    if (!a || !a._book || !ls._drag) return;
    ls._drag = false;
    bookSeek(a, +r.value);
  });
  // Keys while nothing else wants them (or the focus is on the controls): Space/K play, Left/Right 15 s back and 30
  // forward (the seek bar keeps its own arrows), Escape closes a menu
  addEventListener('keydown', (e) => {
    if (e.defaultPrevented || e.ctrlKey || e.metaKey || e.altKey) return;
    const a = document.querySelector('audio[data-book]');
    if (!a || !a._book) return;
    const t = document.activeElement, ls = lsOf(a), inside = ls && ls.contains(t);
    if (e.key === 'Escape' && document.querySelector('[data-listen] [role="menu"]:not([hidden])')) {
      e.preventDefault();
      lsMenus(null);
      return;
    }
    if (!unfocused() && !inside) return;
    if (t && (t.tagName === 'INPUT' || t.closest('[role="menu"]'))) return;
    const k = e.key;
    let act = null;
    if (k === ' ' || k === 'k' || k === 'K') act = () => { if (a.paused) a.play().catch(() => {}); else a.pause(); };
    else if (k === 'ArrowLeft') act = () => bookBy(a, -15);
    else if (k === 'ArrowRight') act = () => bookBy(a, 30);
    if (!act) return;
    e.preventDefault();
    act();
  });

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
  dialogs(null);
  connect();
})();
