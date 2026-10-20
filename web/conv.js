// web/conv.js — a Web Worker that runs Hemma's converter on one file for an
// upload (wire.js starts it). The converter is C (modules/av/conv.c), compiled
// to WebAssembly (/static/av.wasm): this file only feeds it the file's bytes
// and gathers what it writes. Nothing here knows about video.
//   in:  {file, wasm}        a File and the module's URL
//        {put, at}           send the copy, from byte `at`, to that address (a PUT)
//   out: {pm}                progress, 0..1000
//        {size, subs}        the copy is made, this many bytes: it waits here to be sent; subs = the
//                            file's text subtitles as SRT, [{lang, name, forced, text}] (maybe none)
//        {status, body}      the PUT's answer; status 0 = none came (the connection dropped)
//        {lost: true}        the copy can't be read any more (the browser's disk filled up?)
//        {error}             why there is none (the original is sent instead)
// The copy never leaves this worker: wire.js ends the worker once the copy is sent, and the
// browser lets go of it right then. Handed to the page, finished copies stayed until the page's
// memory was next tidied - which a page this small hardly ever needs - and a long upload left
// 28 GB of them on the browser's disk, until it was full and the next copy broke.
'use strict';
let made = null; // the finished copy

async function send(url, at) {
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

self.onmessage = async (e) => {
  if (e.data.put) {
    postMessage(await send(e.data.put, e.data.at));
    return;
  }
  const { file, wasm } = e.data;
  try {
    const reader = new FileReaderSync();
    // The copy grows as a Blob (the browser may keep big ones on disk), fed in 64 MB steps, so
    // memory holds at most that much of it - never the whole copy, let alone twice.
    let copy = new Blob([]), pending = [], held = 0;
    const fold = () => {
      if (!pending.length) return;
      copy = new Blob([copy].concat(pending));
      pending = [];
      held = 0;
    };
    let mem = null, last = -1;
    const bytes = () => new Uint8Array(mem.buffer); // a fresh view: memory can grow
    const hemma = {
      read(off, dst, n) {
        const got = new Uint8Array(reader.readAsArrayBuffer(file.slice(off, off + n)));
        if (got.length !== n) return 0;
        bytes().set(got, dst);
        return 1;
      },
      write(p, n) {
        pending.push(bytes().slice(p, p + n));
        held += n;
        if (held >= 64 << 20) fold();
        return 1;
      },
      write_at(off, p, n) { // overwrite earlier bytes: the MP4 index, once, into the room kept for it
        fold();
        if (off < 0 || off + n > copy.size) return 0;
        copy = new Blob([copy.slice(0, off), bytes().slice(p, p + n), copy.slice(off + n)]);
        return 1;
      },
      progress(pm) { if (pm - last >= 5 || pm === 1000) { last = pm; postMessage({ pm }); } },
    };
    const got = await fetch(wasm);
    if (!got.ok) throw new Error(`the converter is missing (${got.status})`);
    const code = await got.arrayBuffer();
    const { instance } = await WebAssembly.instantiate(code, { hemma });
    const x = instance.exports;
    mem = x.memory;
    x._initialize();
    const rc = x.conv_run(file.size, 0, 0);
    const text = (p) => {
      const b = bytes();
      let end = p;
      while (b[end]) end++;
      return new TextDecoder().decode(b.subarray(p, end));
    };
    fold();
    if (rc === 1) {
      made = new Blob([copy], { type: 'video/mp4' });
      const subs = [];
      for (let i = 0; i < x.conv_subs(); i++) {
        const at = x.conv_sub_text(i);
        subs.push({ lang: text(x.conv_sub_lang(i)), name: text(x.conv_sub_name(i)), forced: !!x.conv_sub_forced(i),
          text: new TextDecoder().decode(bytes().subarray(at, at + x.conv_sub_len(i))) });
      }
      postMessage({ size: made.size, subs });
    } else {
      postMessage({ error: text(x.conv_err()) });
    }
  } catch (err) {
    // no WebAssembly / blocked / out of memory: the original goes instead
    postMessage({ error: 'This browser couldn\'t convert it (' + ((err && err.message) || err) + ')' });
  }
};
