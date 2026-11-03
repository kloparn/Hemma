# 0015 - The graphics card makes the picture, through the browser

**Status:** accepted · **Since:** 2026-11-02 (Hemma 2.8) · builds on [0013](0013-heavy-conversions-on-the-device-you-choose.md)
and [0014](0014-power-tiers-and-videos-in-parts.md)

## Context

Making HEVC play everywhere (0013) on the processor costs about 1.7 seconds of a core for each second of 1080p video;
even every core of a 20-thread PC (0014's Full throttle) made four 23-minute episodes in 17 minutes 47 seconds and
kept the computer busy and warm meanwhile. The owner (2026-11-01): "implement the encoding path on the gpu if it exist ...
painfully slow even with a monster cpu". Most computers have a graphics card with fixed-function video blocks: HEVC
decoding and H.264 encoding in hardware, hundreds of pictures a second.

Three ways to reach one were weighed:

- **In Hemma's program** (Media Foundation on Windows, NVENC/NVDEC, VA-API, V4L2): platform code per operating system
  and per vendor, a second way of making copies that the server's queue would need, and none of it helps Hemma's usual
  computer, a Raspberry Pi 3 - it has no HEVC decoder in hardware. Defender quarantines the release hemma.exe besides.
- **FFmpeg** (`h264_nvenc`): Hemma ships no FFmpeg (0003).
- **The browser** that already converts "On this device" (0013): WebCodecs gives a page the device's hardware decoders
  and encoders, whatever the vendor and operating system.

## Decision

- **The browser's WebCodecs, under Hemma's own converter.** The converter (av.wasm, the same C as everywhere) still
  reads the file, makes the sound and the subtitles and writes the MP4; for an HEVC picture made H.264 it hands the
  blocks, as they are, to the browser's `VideoDecoder` and writes what the `VideoEncoder` gives back (`AvConvJob.gpu`).
  The pictures never leave the card. The converter waits for the card through WebAssembly's JSPI (a suspending import),
  so it stays one synchronous C program. Anything the card can't take is made by the processor as before; a card that
  stops part-way has its video made again by the processor.
- **Through a canvas.** A 10-bit picture handed straight from the decoder to the encoder was converted to 8 bits on its
  way in, and that held every job to about 100 pictures a second together; drawn onto an `OffscreenCanvas` first, one
  job made about 315 a second and three about 470, with the copy 46 dB from the original and its colours true.
- **Whole videos, three at a time.** No parts (0014) on the card: it's quicker whole, and the parts would cost the
  server a join.
- **A secure address, by a port forward.** Browsers give WebCodecs only to secure pages: `https://`, or the computer's
  own address. Hemma stays plain http at home (0004): no certificates for households to make and trust. The computer
  that converts opens Hemma through a forwarded port on itself - `http://127.0.0.1:8099` - which browsers count as
  secure; Hemma needs no change for it (its Host guard takes an IP; same-origin compares the page's Origin with its
  Host). docs/graphics-card.md says how; the Converting tab says whether the card is at work, or how to let it.

## Consequences

- On the PC it was built on (an RTX 3060): four episodes in 5 minutes 22 seconds instead of 17 minutes 47 seconds,
  the processor a quarter busy; three jobs fill the card's encoder (95%). Over a home's Wi-Fi the upload of the copies
  to Hemma becomes the slower part.
- Chrome and Edge only (WebCodecs with JSPI); Firefox and Safari convert on the processor, as before.
- The card's copies are H.264 High (ours are Baseline) at the same quantiser: a third smaller, 46.3 dB against the
  original where ours are 47.1 - the same to the eye.
- One more address to know on the converting computer, and one more sign-in there - the browser keeps 127.0.0.1's
  cookies apart from Hemma's usual address.
- CI's browsers have no graphics card: the card's code is held by conv_test.c's stand-in (our own decoder and encoder
  through the same interface: the same samples as the processor's) and by hand on a real card.
