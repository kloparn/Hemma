# Which videos Hemma plays

**In short:** an MP4 with H.264 picture and AAC sound plays everywhere: every browser, phone and TV.
Most other videos Hemma can turn into one. It makes a copy next to the original, called
`<name>.web.mp4`, and plays that instead. A few kinds it can't change yet. They're listed at the end.

## Plays straight away

| The file | Plays on |
|---|---|
| **MP4**, M4V or MOV with H.264 and AAC (or MP3) sound | Everything |
| **WebM** (VP8, VP9 or AV1 picture, Opus or Vorbis sound) | Most browsers |
| **MKV** with H.264, AV1 or VP9, and AAC, Opus, MP3 or FLAC sound | Chrome and Edge, not Safari or iPhone. For H.264 or AV1 with AAC, Hemma makes an MP4 copy (the H.264 one plays everywhere). It takes seconds, since nothing is re-encoded |

## What Hemma converts

Hemma converts **MKV** and **WebM** files. Each part of the file is handled on its own:

| Picture | In the copy |
|---|---|
| H.264 (x264, AVC) | Copied as it is |
| HEVC (x265, H.265), 8- or 10-bit | Copied as it is, into an MP4. Plays on Apple devices (Safari) and on phones, computers and TVs that decode HEVC themselves. The video's page says *Plays on newer devices* |
| AV1, 8- or 10-bit | Copied as it is, into an MP4. Plays in Chrome, Edge and Firefox, on Android phones and most TVs from the last few years, and in Safari on Apple's newest devices (iPhone 15 Pro and later, Macs and iPads with M3 or later). The video's page says *Plays on newer devices* |
| MPEG-2 (DVD rips), MPEG-1 | Made into H.264, with the interlacing (the DVD's combed lines) fixed |

| Sound | In the copy |
|---|---|
| AAC | Copied as it is |
| Dolby Digital (AC-3, "DD5.1"): Chrome and Firefox play it silently | AAC stereo |
| Dolby Digital Plus (E-AC-3, "DDP5.1") | AAC stereo |
| DTS, and the DTS inside DTS-HD MA: no browser plays it | AAC stereo |

Surround sound (5.1) is mixed down to stereo. When a file has several sound tracks, Hemma uses the
first one it can convert, the default one first.

| Subtitles inside the file | Saved next to the video as |
|---|---|
| Text: SRT, ASS/SSA, WebVTT | `.srt`, which every browser shows |
| Blu-ray subtitles (PGS, which are pictures) | `.sup`, drawn by Hemma's player |
| DVD subtitles (VobSub, also pictures) | `.idx` + `.sub`, drawn by Hemma's player |

Up to 32 subtitle tracks per file. Hemma takes them out of the MKVs already in your library by
itself too, a few at a time while nobody is watching.

### Where it happens

- **While uploading** (the box *Make videos play everywhere on this device first*, on by default): the
  computer you upload from makes the copy, and only the copy is sent.
- **On the server:** **Watch → Library settings → Convert** for everything at once, or **Make it play
  everywhere** on one video's page.

Copying a picture is quick, even on a Raspberry Pi: under a minute for an episode, a few minutes for
a film. Making new sound takes a couple of minutes per episode on a Pi. Re-encoding a DVD's picture
is the heavy part: about five minutes per episode on one PC core, and over an hour on a Pi 3. Upload
DVD rips from a computer and let it do the work.

## Reading a file name

What the usual words in a release's name mean for Hemma:

| In the name | It's | Hemma |
|---|---|---|
| x264, H.264, AVC | The picture | Plays everywhere |
| x265, HEVC, H.265, 10bit | The picture | Copied: plays on newer devices |
| AV1 | The picture | Copied: plays on newer devices |
| VP9 | The picture | Plays in Chrome and Edge as it is, not converted |
| XviD, DivX | The picture | Not yet |
| AAC | The sound | Plays everywhere |
| DD, DD5.1, AC3 | Dolby Digital | Made AAC |
| DDP, DD+, EAC3 | Dolby Digital Plus | Made AAC |
| DTS, DTS-HD MA | DTS | Made AAC (from the DTS core) |
| TrueHD, TrueHD Atmos | Dolby TrueHD | Not yet. Another sound track in the file is used if there is one |
| BluRay, WEB-DL, WEBRip, 1080p | Where it came from, and its size | Nothing to do with playing |

## Not yet

- **AVI, WMV, MPEG-TS (`.ts`, `.m2ts`) and MPEG-PS (`.mpg`, `.vob`) files.** Hemma's converter reads
  MKV and WebM only. A `.ts`, `.m2ts`, `.mpg` or `.vob` usually holds H.264 or MPEG-2 with Dolby or
  DTS sound: put it into an MKV first (a free tool such as MKVToolNix does that quickly, without
  losing quality) and Hemma converts it.
- **XviD and DivX** (MPEG-4 Part 2), **VC-1**, **VP9** and **WMV** pictures, and AV1 made into H.264 (for older
  iPhones, iPads and Macs: Hemma copies AV1, it doesn't decode it).
- **Dolby TrueHD** and uncompressed (PCM) sound, when the file has no other track.

Such a video still goes into the library as it is and plays wherever the browser can play it. Its
page in Watch says what's missing. More formats are added when real files need them, each one fuzzed from its first
day ([why](decisions/0006-formats-grow-with-fuzzing.md)).

## Separate subtitle files

Put them next to the video, named after it: `Film (2020).en.srt`, `Show S01E02.sv.forced.srt`.
Hemma reads `.srt`, `.vtt`, `.sup`, and `.idx` with its `.sub`.

## Audiobooks

Audiobooks play as they are: nothing is converted.

| The file | Hemma |
|---|---|
| **M4B** (AAC sound in an MP4, the usual audiobook file) | A book of its own, wherever it is in the library. Its title, author, narrator, cover and chapters come from the file |
| **M4A**, **MP3** | Inside a folder called *Audiobooks* (or *Audio books*, *Ljudböcker*, *Audiolibros*, *Hörbücher*): each folder of them is one book, its files the parts in order, `CD1`/`CD2` folders included. One lying loose in *Audiobooks* is a book of its own. Elsewhere (a show's `theme.mp3`, a film's soundtrack) Hemma leaves them alone |

Chapters come from the file: an M4B's chapter list, or an MP3's ID3 chapters. A book in several files
has each file as a chapter. **Not yet:** Audible's own `.aa`/`.aax` files (they're locked to Audible's
apps), FLAC and Ogg audiobooks.
