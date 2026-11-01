# 0013 - HEVC is decoded, and a heavy conversion runs where the admin chooses

**Status:** accepted · **Since:** 2026-10-31 (Hemma 2.6) · builds on [0003](0003-own-video-codecs.md) and
[0006](0006-formats-grow-with-fuzzing.md)

## Context

Most of today's smaller releases are HEVC (x265). Hemma copied their picture into an MP4 as it was,
which plays on Apple devices, newer TVs and phones, and browsers with HEVC hardware - not everywhere.
In the owner's library that was about 1,300 of 2,800 videos. Making them play everywhere means
decoding HEVC and encoding H.264: Hemma had no HEVC decoder, and the work is heavy - minutes of a PC
core per episode, hours on a Raspberry Pi 3, where Hemma usually runs.

Uploads already convert in the sending browser (av.wasm), so a strong computer in the home can do the
work. But these videos are in the library already; downloading each one by hand and uploading it again
works, and is a chore nobody does for a whole series.

## Decision

- **Our own HEVC decoder** (modules/av/hevc*.c), Main and Main 10 - what releases use - checked to
  the bit against FFmpeg and fuzzed from its first day, like every format (0006). Other HEVC profiles
  (4:2:2, 4:4:4, 12-bit) are refused by name.
- **Copying stays the default.** HEVC is still copied when a video is converted or uploaded; an H.264
  copy is made only when an admin asks for it - for a whole series, a season, one episode or film, or
  everything - since it costs hours and makes a file a little bigger.
- **The admin picks where it runs:** *On Hemma's computer* (Hemma's own queue) or *On this device*
  (the admin's browser reads the video from Hemma, converts it with the same av.wasm as uploads, and
  sends the copy back through the upload path, with its checks for room and claims). Hemma says how
  heavy it is, given its own number of cores.
- **Only Hemma's own copies are replaced.** The H.264 copy takes the place of the HEVC `.web.mp4`
  Hemma made, or comes beside an HEVC original as its `.web.mp4`. An original is never written over.

## Consequences

- Every library video can be made to play everywhere without leaving Hemma, a bit at a time; Library
  settings lists what's left.
- An admin's browser may read any library video in full (`/media/file/<id>?src=1`, admins only) - the
  same videos an admin could already download.
- A conversion "on this device" lasts only while its page is open; closing it leaves the rest for
  later (or for Hemma's own queue).
- AV1 still can't be made H.264: there's no AV1 decoder yet (a bigger one, for a few files). The same
  two buttons will serve it when one is written.
