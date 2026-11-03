// web/conv.js — a Web Worker that runs Hemma's converter on one file for an
// upload (wire.js starts it). The converter is C (modules/av/conv.c), compiled
// to WebAssembly (/static/av.wasm): this file only feeds it the file's bytes
// and gathers what it writes. Nothing here knows about video.
//   in:  {file, wasm, ram, part}  a File, the module's URL, and whether the copy waits in memory (else in a
//                            Blob, which the browser may keep on its disk) - sent in pieces of `part` bytes;
//                            (2.6) file = {remote, size, h264}: a library file read from the server (Range
//                            requests), its HEVC made H.264 - "convert on this device"; (2.7) h264: an upload's too
//                            (the form's "Make HEVC videos play everywhere too" box); (2.7) cut = [from, to] ms: only
//                            that part of its picture (conv_part: Full throttle, the server joins the parts)
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
//        {error, again, net, at} why there is none (the original is sent instead); again = a read failed,
//                            worth another try; net = (2.6) a library file's read got no answer at all (the
//                            network), at = how much the converter had taken in when it failed
//        {plays: true}       (2.5) an MP4 that plays everywhere as it is: none needed, the original goes quietly
// (2.8) in: gpu = this job may use the graphics card - an HEVC picture made H.264 by WebCodecs (card() below); out: the
//        {size ...} message says gpu: true when it did, and cardFailed: why, when the card stopped and the CPU made it
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

// (2.8) THE GRAPHICS CARD: an HEVC picture made H.264 by the browser's WebCodecs - decoded and encoded on the device's
// graphics card, the pictures never leaving it - while the converter (av.wasm) does everything else: it hands over the
// HEVC blocks as they are (gpu_feed) and writes the H.264 that comes back (gpu_take). The calls that wait suspend the
// converter (JSPI: WebAssembly.Suspending / promising). Browsers allow WebCodecs only on a secure page - https, or
// 127.0.0.1 / localhost - so on Hemma's plain http address there's none (docs: how to open Hemma at 127.0.0.1).
const CARD_AHEAD = 4; // blocks waiting in the decoder, and pictures in the encoder, at most
const cardCan = () => !!(self.VideoDecoder && self.VideoEncoder && self.EncodedVideoChunk && typeof WebAssembly.Suspending === 'function'
  && typeof WebAssembly.promising === 'function');

// hvcC -> the codec string WebCodecs wants (ISO/IEC 14496-15 Annex E): hvc1.<space><profile>.<compat, bits reversed>.<tier><level>[.<constraints>]
function hevcCodec(c) {
  const space = ['', 'A', 'B', 'C'][c[1] >> 6], tier = c[1] & 0x20 ? 'H' : 'L', profile = c[1] & 31;
  let compat = 0;
  for (let i = 0; i < 32; i++) if (c[2 + (i >> 3)] & (0x80 >> (i & 7))) compat |= 1 << i; // (reversed: the first bit lowest)
  const cons = Array.from(c.subarray(6, 12));
  while (cons.length && !cons[cons.length - 1]) cons.pop();
  return `hvc1.${space}${profile}.${(compat >>> 0).toString(16).toUpperCase()}.${tier}${c[12]}` +
    cons.map((b) => '.' + b.toString(16).toUpperCase()).join('');
}

// H.264 High at the lowest level that holds this size and rate (H.264 table A-1): 4.0 for 1080p30, 5.1 for 4K
function avcCodec(w, h, fps) {
  const mbs = Math.ceil(w / 16) * Math.ceil(h / 16), rate = mbs * fps;
  const levels = [[0x1E, 1620, 40500], [0x1F, 3600, 108000], [0x20, 5120, 216000], [0x28, 8192, 245760], [0x2A, 8704, 522240],
    [0x32, 22080, 589824], [0x33, 36864, 983040], [0x34, 36864, 2073600]];
  const l = levels.find(([, size, perS]) => mbs <= size && rate <= perS) || levels[levels.length - 1];
  return 'avc1.6400' + l[0].toString(16).toUpperCase().padStart(2, '0');
}

function card(memory) {
  let dec = null, enc = null, setup = null, avcc = null, failed = '', first = true, flushed = false, made = 0;
  const out = [];
  let waiting = null; // the converter waiting (a resolve) - for a picture, or for room
  const wake = () => { if (waiting) { const w = waiting; waiting = null; w(); } };
  const oops = (where) => (e) => { failed = failed || `${where}: ${(e && e.message) || e}`; wake(); };
  const bytes = () => new Uint8Array(memory().buffer);
  const busy = () => (dec ? dec.decodeQueueSize : 0) > CARD_AHEAD || (enc ? enc.encodeQueueSize : 0) > CARD_AHEAD;
  // Each picture goes from the decoder to the encoder through a canvas, drawn there on the card: handed over straight, a
  // 10-bit picture (x265's) went at ~100 a second for all the jobs together - its conversion to 8 bits on the way into
  // the encoder held them up (2.8, an RTX 3060: ~315 a second this way for one job, ~470 for three; 46 dB, colours true)
  let canvas = null, draw = null;
  const decoded = (frame) => {
    if (failed || (setup.end >= 0 && frame.timestamp >= setup.end)) { frame.close(); return; } // (past an edit list's end)
    let pic = null;
    try {
      if (!canvas) {
        canvas = new OffscreenCanvas(setup.width, setup.height);
        draw = canvas.getContext('2d', { alpha: false });
      }
      draw.drawImage(frame, 0, 0, setup.width, setup.height);
      pic = new VideoFrame(canvas, { timestamp: frame.timestamp });
      enc.encode(pic, { keyFrame: made++ % setup.keyint === 0, avc: { quantizer: setup.qp } });
    } catch (e) {
      oops('encoding')(e);
    }
    if (pic) pic.close();
    frame.close();
  };
  const encoded = (chunk, meta) => {
    if (!avcc && meta && meta.decoderConfig && meta.decoderConfig.description) {
      const d = meta.decoderConfig.description;
      avcc = new Uint8Array(ArrayBuffer.isView(d) ? d.buffer.slice(d.byteOffset, d.byteOffset + d.byteLength) : d.slice(0));
    }
    const data = new Uint8Array(chunk.byteLength);
    chunk.copyTo(data);
    out.push({ data, t: chunk.timestamp, key: chunk.type === 'key' });
    wake();
  };
  return {
    // hemma.gpu_open: 1 = the card takes this picture (both configurations supported here), 0 = it doesn't
    open: async (cfg, n, width, height, depth, fpsNum, fpsDen, qp, keyint, endUs) => {
      try {
        const hvcc = bytes().slice(cfg, cfg + n);
        const fps = fpsDen > 0 ? fpsNum / fpsDen : 25;
        const dc = { codec: hevcCodec(hvcc), description: hvcc, hardwareAcceleration: 'prefer-hardware' };
        const ec = { codec: avcCodec(width, height, fps), width, height, framerate: fps, bitrateMode: 'quantizer',
          hardwareAcceleration: 'prefer-hardware', latencyMode: 'quality', avc: { format: 'avc' } };
        const [ds, es] = await Promise.all([VideoDecoder.isConfigSupported(dc), VideoEncoder.isConfigSupported(ec)]);
        if (!ds.supported || !es.supported || typeof OffscreenCanvas !== 'function') return 0;
        setup = { qp, keyint: Math.max(1, keyint), end: endUs, depth, width, height };
        enc = new VideoEncoder({ output: encoded, error: oops('the encoder') });
        enc.configure(ec);
        dec = new VideoDecoder({ output: decoded, error: oops('the decoder') });
        dec.configure(dc);
        dec.ondequeue = enc.ondequeue = wake;
        return 1;
      } catch (_) {
        return 0;
      }
    },
    // hemma.gpu_feed: a block, in file order; waits while the card is busy (a promise suspends the converter)
    feed: (p, n, ptsUs, key) => {
      if (failed) return 0;
      if (first && !key) return 1; // (the decoder starts at a keyframe)
      first = false;
      try {
        dec.decode(new EncodedVideoChunk({ type: key ? 'key' : 'delta', timestamp: ptsUs, data: bytes().subarray(p, p + n) }));
      } catch (e) {
        oops('decoding')(e);
        return 0;
      }
      if (!busy()) return 1;
      return (async () => {
        while (busy() && !failed) await new Promise((ok) => { waiting = ok; });
        return failed ? 0 : 1;
      })();
    },
    // hemma.gpu_end: no more blocks - what's in the card comes out
    end: () => {
      if (!dec || failed) { flushed = true; return; }
      dec.flush().then(() => enc.flush()).then(() => { flushed = true; wake(); }, oops('finishing'));
    },
    // hemma.gpu_take: the next H.264 picture into memory[buf] (see avconv.c); waits only when asked to
    take: (wait, buf, cap, meta) => {
      const give = () => {
        if (failed) return -2;
        if (out.length) {
          const o = out[0], m = new DataView(memory().buffer, meta, 24);
          if (o.data.length > cap) { m.setFloat64(16, o.data.length, true); return -3; }
          bytes().set(o.data, buf);
          m.setFloat64(0, o.t, true);
          m.setFloat64(8, o.key ? 1 : 0, true);
          out.shift();
          return o.data.length;
        }
        return flushed ? -1 : null;
      };
      const r = give();
      if (r !== null || !wait) return r === null ? 0 : r;
      return (async () => {
        for (;;) {
          await new Promise((ok) => { waiting = ok; });
          const got = give();
          if (got !== null) return got;
        }
      })();
    },
    avcc: (o, cap) => {
      if (!avcc || avcc.length > cap) return 0;
      bytes().set(avcc, o);
      return avcc.length;
    },
    close: () => {
      for (const c of [dec, enc]) try { if (c && c.state !== 'closed') c.close(); } catch (_) { /* (gone) */ }
    },
    get failed() { return failed; },
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
  const { file, wasm, ram, part, subs: subsOnly, flaky, h264, cut, gpu } = e.data;
  if (part > 0) PART = part;
  // Reading the original can fail for a while - a browser short of memory for the read (copies held in
  // memory; 1.3: ten Simpsons and three South Park episodes went up unconverted, "readAsArrayBuffer ...
  // could not be read") - though the file is fine: the read tries once more, small, and a job that still
  // fails says so ({again}): it goes back in line, not up as the original. flaky (tests: data-flaky) = that
  // many reads fail.
  let readFailed = false, netFailed = false, failedAt = 0, fail = flaky | 0;
  try {
    const reader = file.remote ? null : new FileReaderSync();
    // (2.6) a library file: each read a Range request - synchronous, as the converter asks for its bytes (a
    // worker may); anything but those very bytes stops it. No answer at all is the network ({net}: waited out
    // like a PUT's - a Wi-Fi gone at 4 am mustn't skip a night's files as unreadable)
    const fetched = (a, b) => {
      const x = new XMLHttpRequest();
      x.open('GET', file.remote, false);
      x.setRequestHeader('Range', `bytes=${a}-${b - 1}`);
      x.responseType = 'arraybuffer';
      try {
        x.send();
      } catch (err) {
        netFailed = true;
        throw err;
      }
      if (!x.status) netFailed = true;
      if (x.status !== 206 || !x.response || x.response.byteLength !== b - a) throw new Error(`reading it failed (${x.status})`);
      netFailed = false;
      return new Uint8Array(x.response);
    };
    const slice = (a, b) => {
      if (fail > 0 && fail--) throw new DOMException('The requested file could not be read (test)', 'NotReadableError');
      return file.remote ? fetched(a, b) : new Uint8Array(reader.readAsArrayBuffer(file.slice(a, b)));
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
              failedAt = fed;
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
    // (2.8) the graphics card, when this job may use it and the browser has what it takes (else the six imports are
    // there, never called)
    const toH264 = file.h264 || h264 ? 1 : 0;
    const onCard = !subsOnly && !cut && !!gpu && !!toH264 && cardCan() ? card(() => wmem) : null;
    const cardIn = onCard ? { gpu_open: new WebAssembly.Suspending(onCard.open), gpu_feed: new WebAssembly.Suspending(onCard.feed),
      gpu_end: onCard.end, gpu_take: new WebAssembly.Suspending(onCard.take), gpu_avcc: onCard.avcc, gpu_close: onCard.close }
      : { gpu_open: () => 0, gpu_feed: () => 0, gpu_end: () => {}, gpu_take: () => -2, gpu_avcc: () => 0, gpu_close: () => {} };
    const got = await fetch(wasm);
    if (!got.ok) throw new Error(`the converter is missing (${got.status})`);
    const code = await got.arrayBuffer();
    const { instance } = await WebAssembly.instantiate(code, { hemma: { ...hemma, ...cardIn } });
    const x = instance.exports;
    wmem = x.memory;
    x._initialize();
    const text = (p) => {
      const b = bytes();
      let end = p;
      while (b[end]) end++;
      return new TextDecoder().decode(b.subarray(p, end));
    };
    let rc = subsOnly ? x.conv_subs_only(file.size)
      : cut ? x.conv_part(file.size, 0, cut[0], cut[1]) // (2.7) one part of it, made side by side (Full throttle)
      : onCard ? await WebAssembly.promising(x.conv_run)(file.size, 0, 0, toH264, 1)
      : x.conv_run(file.size, 0, 0, toH264, 0);
    const madeOnCard = !!onCard && rc === 1 && !!x.conv_on_gpu();
    let cardFailed = '';
    if (onCard && rc !== 1 && x.conv_gpu_failed()) { // the card stopped part-way: made again without it, from the start
      cardFailed = onCard.failed || text(x.conv_err());
      mem = ram ? inMemory() : null;
      copy = new Blob([]), pending = [], held = 0;
      win = null, fed = 0, last = -1;
      rc = x.conv_run(file.size, 0, 0, toH264, 0);
    }
    win = null;
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
    postMessage({ size: made.size, subs, pics, gpu: madeOnCard, cardFailed }, pics.map((p) => p.data));
  } catch (err) {
    // no WebAssembly / blocked: the original goes instead - but a read that failed, or memory that ran out,
    // is worth another try later ({again})
    const why = (err && err.message) || String(err);
    postMessage({ error: 'This browser couldn\'t convert it (' + why + ')', again: readFailed || /memory|allocat/i.test(why),
      net: readFailed && netFailed, at: failedAt });
  }
};
