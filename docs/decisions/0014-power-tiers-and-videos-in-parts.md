# 0014 - How hard conversions work is the household's choice, and Full throttle makes one video in parts

**Status:** accepted · **Since:** 2026-11-01 (Hemma 2.7) · builds on [0013](0013-heavy-conversions-on-the-device-you-choose.md)

## Context

Making HEVC play everywhere (0013) is the heaviest work Hemma does: about 1.7 seconds of a PC core for each second of
1080p video. Hemma's own computer ran one video on each of its cores but two; a browser on half of its cores, at most
8 - whatever else the machine was doing, and one core per video however many were idle. Converting a season of six
episodes on a 20-thread PC left 14 threads idle; a film used one core for six hours.

## Decision

- **Three tiers, chosen per machine:** *Low power* (about 30% of the cores: the machine stays free for everything
  else), *Medium power* (about 60%: the default, about what Hemma did before) and *Full throttle* (all of them; a phone
  one or two). Hemma's computer has its own (setting `media.power`); each admin has one for their devices, "On this
  device" and the copies uploads make (pref `media.power_here`). The Converting tab says what each means on that
  machine: how many cores, how it goes about it. The rule is the server's (`convert_power_workers`); the browser says
  its core count with its other answers (`media.caps`) and is told how many videos to make at once (`data-workers`).
- **Full throttle splits a video when cores would sit idle.** With fewer videos left than cores, a video being made
  H.264 from HEVC is made in parts side by side - one more per idle core, each at least 5 minutes long, 32 at most.
  Each part is decoded from the keyframe before it and encodes only its own pictures, starting with a keyframe of its
  own (`AvConvJob.piece_from/_to`); the join (`AvConvJob.pieces`) takes the parts' pictures as they are and the sound
  and subtitles from the original, as one conversion of the whole. The pictures come out at the same times as one
  conversion's.
- **The server joins.** On Hemma's computer the worker finishing the last part joins them. On a device, each part is
  an entry of its own in the upload batch, sent to its own file like any copy (room, resuming and retries as ever);
  Hemma joins them once they're all there - it has the original, so the sound isn't fetched again.

## Consequences

- One film uses every core on Full throttle; a season of fewer episodes than cores does too.
- Parts cost a little: the pictures between a part's keyframe and its start are decoded twice, and each part starts
  with a keyframe, so a copy made in parts is a little bigger than one made whole.
- Only HEVC made H.264 is split. MPEG-2 (DVD) re-encodes stay whole: their deinterlacing follows the picture's cadence
  across the cut, and they're quick anyway. Uploads with the H.264 box stay whole too: the original is on the sending
  computer, not Hemma's, so Hemma couldn't join them.
- Lowering the tier while converting lets the workers over it finish their current video first; raising it starts
  more at once.
