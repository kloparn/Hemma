# 0016 - Intros are found by their sound, on Hemma's computer, while nobody watches

**Status:** accepted · **Since:** 2026-11-04 (Hemma 2.9) · builds on [0003](0003-own-video-codecs.md) and
[0006](0006-formats-grow-with-fuzzing.md)

## Context

The owner (2026-11-03): "Shows have intros, like all series, are we able to automatically determine when the intro is?
And if so can we add a 'skip' button for it?" Nobody marks intros in the files: release groups don't, and an MKV's
chapters rarely say "Intro". Plex and Jellyfin find them by comparing the sound of a season's episodes: the theme
is the one stretch every episode has, while the rest of each differs.

Where to compare, and what to compare, were the choices:

- **The picture** varies where the sound doesn't (The Simpsons' couch gag, a title card over a new scene), and Hemma
  has no H.264 decoder - most of the library is H.264 or HEVC copies.
- **The sound** needs a decoder for what the library carries. Dolby and DTS were there (0003); AAC - the sound of every
  copy that plays everywhere, our encoder's or the original's copied as it was - only as a test decoder that read what
  our own encoder writes. The owner: "if we can make our own true AAC decoder, even on the already encoded AAC, then
  we could make it for everything".
- **Where:** a browser could do it (as conversions on the device do, 0013), but only while someone has a page open;
  Hemma's computer has every file and hours a day when nobody watches. The owner: "the PI should do it slow, there
  will be hours upon hours where no one checks the software ... it's perfect when no-one is watching we process stuff".

## Decision

- **A real AAC-LC decoder** (modules/av/aacdec.c), from the standard: every sample rate, up to 7.1, every AAC-LC tool;
  HE-AAC as its AAC core (the lower half of the sound - all a fingerprint looks at). Checked against FFmpeg to float
  rounding, and fuzzed every night like the other decoders (0006). The server's only: not in av.wasm.
- **Sound fingerprints** (modules/av/intro.c): each episode's first minutes as a 32-bit print 16 times a second, which
  bands grew louder than their neighbours since the frame before (Haitsma and Kalker's robust hash, 300-2,000 Hz) -
  it survives other encoders, levels and rates. Two episodes are compared at every offset (equal pieces of the prints
  point to the offsets worth scanning); a season's intro is, for each episode, the stretch it shares with the most of
  its neighbours, 15 seconds to 2.5 minutes long.
- **On Hemma's computer, a season at a time, only while nobody watches** (modules/media/intros.c): the same rules as
  the subtitles taken out of MKVs (1.2) - background priority, one thread, waiting for streams, conversions, the
  subtitles' job and a disk that's away; what was read stays in memory when it has to stop. The results are rows in
  the database; the player's *Skip intro* button reads them.
- **The admin has the last word:** Manage shows each episode's intro and lets an admin set it, say there's none, or
  give it back to Hemma; detection never replaces those.

## Consequences

- Every episode is read once - up to ten minutes of its start, less than half of it: on a Raspberry Pi a whole library
  takes many hours, spread over the quiet ones. A season gaining an episode is done again (its other episodes' prints
  are in memory while Hemma runs).
- Episodes whose sound Hemma can't decode yet (MP3, Opus) get no button; the Library settings card counts them. An MP3
  decoder would close that, if the count says it's worth it.
- Intros that change every episode, and short title cards, are never found; a pilot without the season's theme has
  none - Manage is where to set one.
- Finding end credits ("Skip credits", the next-episode card earlier) uses the same comparison, but sitcoms play scenes
  over their credits: not done.
