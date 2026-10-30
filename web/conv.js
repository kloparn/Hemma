// web/conv.js — a Web Worker that runs Hemma's converter on one file for an
// upload (wire.js starts it). The converter is C (modules/av/conv.c), compiled
// to WebAssembly (/static/av.wasm): this file only feeds it the file's bytes
// and gathers what it writes. Nothing here knows about video.
//   in:  {file, wasm, ram, part}  a File, the module's URL, and whether the copy waits in memory (else in a
//                            Blob, which the browser may keep on its disk) - sent in pieces of `part` bytes
//        {file, wasm, subs}  only the file's subtitles (a video the library has: it stays behind)
//        {put, at}           send the copy, from byte `at`, to that address (PUTs)
//   out: {pm, fed}           progress, 0..1000, and the original's bytes the converter has taken in so far
//        {size, subs, pics}  the copy is made, this many bytes: it waits here to be sent; subs = the
//                            file's text subtitles as SRT, [{t, lang, name, forced, text}] (maybe none);
//                            pics = its picture subtitles, [{t, kind: 'pgs'|'vobsub', lang, name, forced,
//                            data: ArrayBuffer (the .sup file / VobSub's .sub), idx: VobSub's .idx text}];
//                            t = the track's place among the file's subtitle tracks (they go in that order)
//        {subsOnly, subs, pics}  the same subtitles, of a {subs} job
//        {status, body}      the PUT's answer; status 0 = none came (the connection dropped)
//        {lost: true}        the copy can't be read any more (the browser's disk filled up?)
//        {error}             why there is none (the original is sent instead)
//        {plays: true}       (2.5) an MP4 that plays everywhere as it is: none needed, the original goes quietly
// The copy never leaves this worker: wire.js ends the worker once the copy is sent, and the
// browser lets go of it right then. Handed to the page, finished copies stayed until the page's
// memory was next tidied - which a page this small hardly ever needs - and a long upload left
// 28 GB of them on the browser's disk, until it was full and the next copy broke.
'use strict';
const phone = /Mobi|Android|iPhone|iPad/.test(navigator.userAgent);
// Reading: the converter asks for 1 MB at a time; reading on from where it stopped, the next read is
// twice as big, up to AHEAD - a few big reads instead of many small ones, so several workers on one
// disk take turns in long runs instead of jumping between their files every megabyte.
const AHEAD = (phone ? 8 : 32) << 20;
let PART = 16 << 20; // a copy in memory is kept in parts this big, and sent a part a PUT (?at=&end=: each
// answered with where the file is); {part} says otherwise (the browser tests: tiny copies in several pieces)
let made = null; // the finished copy: a Blob, or (in memory) {size, piece(a, b)}

// The copy in memory: parts filled in order; the MP4's index is written once more into the room kept for it.
function inMemory() {
  const parts = [];
  let size = 0;
  const each = (off, n, fn) => { // fn(part, its offset, how many, how far into the run)
    for (let done = 0; done < n;) {
      const p = Math.floor((off + done) / PART), o = (off + done) % PART, k = Math.min(n - done, PART - o);
      fn(parts[p], o, k, done);
      done += k;
    }
  };
  return {
    get size() { return size; },
    write(src) { // false: no memory for it (nothing written)
      try {
        while (parts.length * PART < size + src.length) parts.push(new Uint8Array(PART));
      } catch (_) {
        return false;
      }
      each(size, src.length, (part, o, k, d) => part.set(src.subarray(d, d + k), o));
      size += src.length;
      return true;
    },
    writeAt(off, src) {
      if (off < 0 || off + src.length > size) return false;
      each(off, src.length, (part, o, k, d) => part.set(src.subarray(d, d + k), o));
      return true;
    },
    piece(a, b) {
      if (Math.floor(a / PART) === Math.floor((b - 1) / PART)) return parts[Math.floor(a / PART)].subarray(a % PART, a % PART + b - a);
      const out = new Uint8Array(b - a);
      each(a, b - a, (part, o, k, d) => out.set(part.subarray(o, o + k), d));
      return out;
    },
    blob() { return new Blob(parts.map((p, i) => p.subarray(0, Math.max(0, Math.min(PART, size - i * PART))))); },
  };
}

async function send(url, at) {
  if (made instanceof Blob) {
    try {
      const r = await fetch(`${url}?at=${at}`, {
        method: 'PUT', headers: { 'Content-Type': 'application/octet-stream' }, body: made.slice(at),
      });
      return { status: r.status, body: await r.text() };
    } catch (_) {
      try {
        await made.slice(at, at + 1).arrayBuffer(); // still readable: the connection, not the copy
      } catch (_) {
        return { lost: true };
      }
      return { status: 0 };
    }
  }
  try { // in memory: piece by piece, each answered with where the file is now
    for (;;) {
      const end = Math.min(made.size, at - (at % PART) + PART); // (to its part's end)
      const r = await fetch(`${url}?at=${at}&end=${end}`, {
        method: 'PUT', headers: { 'Content-Type': 'application/octet-stream' }, body: made.piece(at, end),
      });
      const body = await r.text();
      let next = -1;
      try { next = JSON.parse(body).at; } catch (_) { /* (not JSON: the page reads it) */ }
      if (!r.ok || end >= made.size || typeof next !== 'number') return { status: r.status, body };
      at = next;
    }
  } catch (_) {
    return { status: 0 };
  }
}

self.onmessage = async (e) => {
  if (e.data.put) {
    postMessage(await send(e.data.put, e.data.at));
    return;
  }
  const { file, wasm, ram, part, subs: subsOnly, flaky } = e.data;
  if (part > 0) PART = part;
  // Reading the original can fail for a while - a browser short of memory for the read (copies held in
  // memory; 1.3: ten Simpsons and three South Park episodes went up unconverted, "readAsArrayBuffer ...
  // could not be read") - though the file is fine: the read tries once more, small, and a job that still
  // fails says so ({again}): it goes back in line, not up as the original. flaky (tests: data-flaky) = that
  // many reads fail.
  let readFailed = false, fail = flaky | 0;
  try {
    const reader = new FileReaderSync();
    const slice = (a, b) => {
      if (fail > 0 && fail--) throw new DOMException('The requested file could not be read (test)', 'NotReadableError');
      return new Uint8Array(reader.readAsArrayBuffer(file.slice(a, b)));
    };
    // The copy: in memory when asked (and while there's memory: else what's made goes into a Blob and the
    // rest follows it there), else a Blob that grows in 64 MB steps, so memory holds at most that much of it -
    // never the whole copy, let alone twice. The browser may keep a big Blob on its disk.
    let mem = ram ? inMemory() : null;
    let copy = new Blob([]), pending = [], held = 0;
    const fold = () => {
      if (!pending.length) return;
      copy = new Blob([copy].concat(pending));
      pending = [];
      held = 0;
    };
    let wmem = null, last = -1;
    const bytes = () => new Uint8Array(wmem.buffer); // a fresh view: memory can grow
    let win = null, winAt = 0, grow = 1 << 20; // read ahead (AHEAD)
    // fed: what the converter has taken in so far - with each progress report, and every 2 s while it reads, so
    // the card can say how fast it goes (1.5): a percentage alone sat still for minutes on a DVD episode
    let fed = 0, told = 0;
    const hemma = {
      read(off, dst, n) {
        if (!win || off < winAt || off + n > winAt + win.length) {
          grow = win && off === winAt + win.length ? Math.min(AHEAD, grow * 2) : 1 << 20; // (reading on: more)
          try {
            win = slice(off, Math.min(file.size, off + Math.max(n, grow)));
          } catch (_) { // once more, no bigger than it must be
            win = null;
            grow = 1 << 20;
            try {
              win = slice(off, Math.min(file.size, off + Math.max(n, grow)));
            } catch (err) {
              readFailed = true;
              throw err;
            }
          }
          winAt = off;
        }
        if (off + n > winAt + win.length) return 0;
        bytes().set(win.subarray(off - winAt, off - winAt + n), dst);
        fed += n;
        if (Date.now() - told >= 2000) {
          told = Date.now();
          postMessage({ pm: Math.max(last, 0), fed });
        }
        return 1;
      },
      write(p, n) {
        if (mem && mem.write(bytes().subarray(p, p + n))) return 1;
        if (mem) { // no memory left for it: on in a Blob
          copy = mem.blob();
          mem = null;
        }
        pending.push(bytes().slice(p, p + n));
        held += n;
        if (held >= 64 << 20) fold();
        return 1;
      },
      write_at(off, p, n) { // overwrite earlier bytes: the MP4 index, once, into the room kept for it
        if (mem) return mem.writeAt(off, bytes().subarray(p, p + n)) ? 1 : 0;
        fold();
        if (off < 0 || off + n > copy.size) return 0;
        copy = new Blob([copy.slice(0, off), bytes().slice(p, p + n), copy.slice(off + n)]);
        return 1;
      },
      progress(pm) {
        if (pm - last >= 5 || pm === 1000) {
          last = pm;
          told = Date.now();
          postMessage({ pm, fed });
        }
      },
    };
    const got = await fetch(wasm);
    if (!got.ok) throw new Error(`the converter is missing (${got.status})`);
    const code = await got.arrayBuffer();
    const { instance } = await WebAssembly.instantiate(code, { hemma });
    const x = instance.exports;
    wmem = x.memory;
    x._initialize();
    const rc = subsOnly ? x.conv_subs_only(file.size) : x.conv_run(file.size, 0, 0);
    win = null;
    const text = (p) => {
      const b = bytes();
      let end = p;
      while (b[end]) end++;
      return new TextDecoder().decode(b.subarray(p, end));
    };
    if (rc === 3) { /* (2.5: an MP4 that plays everywhere as it is - nothing made, the original goes quietly) */
      postMessage({ plays: true });
      return;
    }
    if (rc !== 1) {
      postMessage({ error: text(x.conv_err()) });
      return;
    }
    const subs = [], pics = [], raw = (at, n) => new TextDecoder().decode(bytes().subarray(at, at + n));
    for (let i = 0; i < x.conv_subs(); i++) {
      const about = { t: i, lang: text(x.conv_sub_lang(i)), name: text(x.conv_sub_name(i)), forced: !!x.conv_sub_forced(i) };
      const kind = x.conv_sub_kind(i);
      if (kind === 0) {
        subs.push({ ...about, text: raw(x.conv_sub_text(i), x.conv_sub_len(i)) });
        continue;
      }
      const at = x.conv_sub_data(i); // (a copy of its bytes: they leave this memory)
      pics.push({ kind: kind === 1 ? 'pgs' : 'vobsub', ...about,
        data: bytes().slice(at, at + x.conv_sub_size(i)).buffer,
        idx: kind === 2 ? raw(x.conv_sub_idx(i), x.conv_sub_idx_len(i)) : '' });
    }
    if (subsOnly) {
      postMessage({ subsOnly: true, subs, pics }, pics.map((p) => p.data));
      return;
    }
    fold();
    made = mem || new Blob([copy], { type: 'video/mp4' });
    postMessage({ size: made.size, subs, pics }, pics.map((p) => p.data));
  } catch (err) {
    // no WebAssembly / blocked: the original goes instead - but a read that failed, or memory that ran out,
    // is worth another try later ({again})
    const why = (err && err.message) || String(err);
    postMessage({ error: 'This browser couldn\'t convert it (' + why + ')', again: readFailed || /memory|allocat/i.test(why) });
  }
};
