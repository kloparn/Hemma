# FFmpeg's DTS vector tables (two arrays of numbers)

- What: `dts_vq.h` — the DTS Coherent Acoustics core's ADPCM prediction coefficients (4096 vectors of 4)
  and high-frequency subband vectors (1024 of 32), which `modules/av/dts.c` needs to decode DTS sound.
- Why from FFmpeg: the DTS specification (ETSI TS 102 114, D.10.1 and D.10.2) leaves both out of its text
  in every edition since 2002 ("due to its extensive size, this table is not included here"). Everything
  else the decoder uses comes from the specification itself (`modules/av/dts_tables.h`). The maintainer
  chose this (2026-10-24) over leaving DTS sound unconverted.
- License: **GNU LGPL 2.1 or later** (`COPYING.LGPLv2.1`, from the same FFmpeg release), not Hemma's
  Apache 2.0. Hemma's full source is public, so anyone can rebuild Hemma with a changed copy of this
  file - what the LGPL asks of a program that includes it.
- Source: `libavcodec/dcadata.c` of FFmpeg release `n7.1`,
  https://raw.githubusercontent.com/FFmpeg/FFmpeg/n7.1/libavcodec/dcadata.c
  (SHA-256 of that file: `f8d2667d306792cf7dc1a61dd1d85d0f8a8c862b14335a532eae8d251ed9dfc5`) — the arrays
  `ff_dca_adpcm_vb` and `ff_dca_high_freq_vq`, numbers unchanged, renamed `DTS_ADPCM_VQ` and `DTS_HF_VQ`,
  under that file's copyright notice (Gildas Bazin 2004, Benjamin Larsson 2006).
- `COPYING.LGPLv2.1`: https://raw.githubusercontent.com/FFmpeg/FFmpeg/n7.1/COPYING.LGPLv2.1
  (SHA-256 `b634ab5640e258563c536e658cad87080553df6f34f62269a21d554844e58bfe`).
- Checked: the first prediction entry is the specification's own example (9928, D.10.1); Hemma's decoder
  with these tables gives the same PCM as FFmpeg's on real DTS tracks to within float rounding (Hemma's
  `dts_real` test, with `HEMMA_TEST_DTS=<file.mkv>`).
