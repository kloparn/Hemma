// web/conv.js — a Web Worker that runs Hemma's converter on one file for an
// upload (wire.js starts it). The converter is C (modules/av/conv.c), compiled
// to WebAssembly (/static/av.wasm): this file only feeds it the file's bytes
// and gathers what it writes. Nothing here knows about video.
//   in:  {file, wasm}        a File and the module's URL
//   out: {pm}                progress, 0..1000
//        {blob}              the finished copy
//        {error}             why there is none (the original is sent instead)
'use strict';
self.onmessage = async (e) => {
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
    if (rc === 1) postMessage({ blob: new Blob([copy], { type: 'video/mp4' }) });
    else postMessage({ error: text(x.conv_err()) });
  } catch (err) {
    // no WebAssembly / blocked / out of memory: the original goes instead
    postMessage({ error: 'This browser couldn\'t convert it (' + ((err && err.message) || err) + ')' });
  }
};
