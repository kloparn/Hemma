# 0003 - Our own video codecs, not ffmpeg

**Status:** accepted · **Since:** 2026-10

## Context

A household's films and series are often DVD rips (MPEG-2 picture, Dolby AC-3 sound) or recent
downloads with Dolby Digital Plus sound. Browsers can't play MPEG-2 at all, and Chrome and Firefox
play Dolby sound as silence. Something has to turn such files, once, into what every browser,
phone and TV plays: H.264 picture and AAC sound in an MP4. ffmpeg does that, but it is huge, a
separate install with its own licences and updates, and it can't run in a browser.

## Decision

- `modules/av` is Hemma's own, in plain C with no dependencies: a Matroska reader, MPEG-2 and
  AC-3 / E-AC-3 decoders, a deinterlacer, H.264 and AAC encoders and an MP4 writer.
- A conversion makes a copy next to the original ("convert once"); playing is always direct
  from disk.
- The same code compiles to WebAssembly, so an upload converts on the computer that sends it and
  only the small copy travels. The Pi does no heavy lifting.
- ffmpeg is at most a comparison on a developer's machine, never shipped.

## Consequences

- One maintainer owns code that parses files nobody controls, in the process that serves the
  whole household. That is why it is fuzzed on every push and for hours every night, and why each
  decoder is checked against its specification and real files ([0006](0006-formats-grow-with-fuzzing.md)).
- Formats come one at a time, when real files need them.
- The converter is fast enough where it runs: a 22-minute DVD episode takes under five minutes on
  one PC core, and a film whose picture is already H.264 or HEVC is copied, not converted (a 3.6 GB
  film in under a minute on a PC).
