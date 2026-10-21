# 0006 - New video formats when real files need them, each fuzzed

**Status:** accepted · **Since:** 2026-10-21 (issue #50)

## Context

Issue #50 proposed freezing the converter's formats: the codec code is the riskiest part of
Hemma - hand-written decoders parsing files nobody controls - and HEVC decoding or DTS would
add more of it. On the other hand households keep meeting files the converter can't take yet
(0.24.0 added Dolby Digital Plus because a whole series played silently in Chrome), and "convert
it on a desktop first" is exactly what Hemma exists to spare them.

## Decision

- Formats keep growing when real files need them, each as a minor version.
- A new format is never added without its safety net, in the same pull request:
  - it is listed in `AV_CONV_CODECS` (`modules/av/conv.h`) - the one table the converter's checks
    consult - and `conv_fuzz_seeds` (`modules/av/conv_test.c`) fails until a seed file carries it,
    so the fuzzing covers it from its first day;
  - a new decoder gets its own fast fuzz target (`tools/avfuzz.c`) and tests against its
    specification, and where possible against real files and another decoder (as E-AC-3 was
    checked against a test encoder built from the specification and against FFmpeg);
  - the household-facing words say what it plays where (the watch page's "What plays where").
- Every push fuzzes the whole converter for 90 seconds; every night `.github/workflows/fuzz.yml`
  fuzzes it and each part alone for about three hours, carrying on from what earlier nights
  found. A finding fails the run and keeps the input that breaks it.

## Consequences

- The codec code keeps growing, so its fuzzing does too: a format without a seed can't be merged.
- Nightly findings need someone to look at them. A crashing input becomes a test once fixed.
- Household features still get their releases; this record only says codecs aren't frozen.
