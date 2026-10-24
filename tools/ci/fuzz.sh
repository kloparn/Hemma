#!/bin/sh
# tools/ci/fuzz.sh TARGET [SECONDS] [JOBS] - libFuzzer over modules/av under ASan + UBSan, from the top of
# the tree after `make build/test` (needs clang with its fuzzer runtime: Ubuntu's libclang-rt-<n>-dev).
#   conv    tools/convfuzz.c: a whole conversion, MKV in, MP4 out (every part at once; slow per input)
#   mkv     tools/avfuzz.c:   the Matroska reader alone
#   ac3     tools/avfuzz.c:   the Dolby (AC-3 / E-AC-3) decoder alone, raw frames
#   dts     tools/avfuzz.c:   the DTS decoder alone, raw core frames
#   mpeg2   tools/avfuzz.c:   the MPEG-2 decoder alone, raw streams
#   pgs     tools/avfuzz.c:   Blu-ray picture subtitles (a .sup) opened, every cue drawn, made a PNG
#   vobsub  tools/avfuzz.c:   DVD picture subtitles (.idx text, a NUL, the .sub) the same
# The corpus grows in build/fuzz-corpus-TARGET - keep it for the next run (the nightly job caches it);
# what it finds lands in build/fuzz-found-TARGET, and then this exits 1. JOBS > 1: that many processes
# side by side (libFuzzer's -fork), carrying on after a finding so one night can find several.
# Used by .github/workflows/build.yml (90 s on every push), fuzz.yml (nightly, hours) and tools/ci/local.sh.
set -eu
t=${1:-}
secs=${2:-90}
jobs=${3:-1}
seeds=build/fuzz-seeds
case $t in
conv) src="tools/convfuzz.c modules/av/conv.c modules/av/mkv.c modules/av/mpeg2.c modules/av/deint.c
           modules/av/h264enc.c modules/av/ac3.c modules/av/dts.c modules/av/aac.c modules/av/mp4.c"
      def= opts="-max_len=65536 -timeout=60" ;;
mkv) src="tools/avfuzz.c modules/av/mkv.c" def=-DAVFUZZ_MKV opts="-max_len=65536 -timeout=10" ;;
ac3) src="tools/avfuzz.c modules/av/ac3.c" def=-DAVFUZZ_AC3 opts="-max_len=32768 -timeout=10" seeds=build/fuzz-raw/ac3 ;;
dts) src="tools/avfuzz.c modules/av/dts.c" def=-DAVFUZZ_DTS opts="-max_len=32768 -timeout=10" seeds=build/fuzz-raw/dts ;;
mpeg2) src="tools/avfuzz.c modules/av/mpeg2.c" def=-DAVFUZZ_MPEG2 opts="-max_len=65536 -timeout=20" seeds=build/fuzz-raw/mpeg2 ;;
pgs) src="tools/avfuzz.c modules/av/subpic.c modules/av/png.c" def=-DAVFUZZ_PGS opts="-max_len=65536 -timeout=20" seeds=build/fuzz-raw/pgs ;;
vobsub) src="tools/avfuzz.c modules/av/subpic.c modules/av/png.c" def=-DAVFUZZ_VOBSUB opts="-max_len=65536 -timeout=20" seeds=build/fuzz-raw/vobsub ;;
*) echo "usage: sh tools/ci/fuzz.sh conv|mkv|ac3|dts|mpeg2|pgs|vobsub [seconds] [processes]"; exit 2 ;;
esac

# the seeds: small files of every kind the converter takes, and picture subtitles, made by their tests
# (never committed)
[ -d build/fuzz-seeds ] && [ -d build/fuzz-raw/ac3 ] && [ -d build/fuzz-raw/dts ] ||
    HEMMA_FUZZ_SEEDS=build/fuzz-seeds HEMMA_FUZZ_RAW=build/fuzz-raw build/test conv_fuzz_seeds >/dev/null
[ -d build/fuzz-raw/pgs ] && [ -d build/fuzz-raw/vobsub ] ||
    HEMMA_FUZZ_RAW=build/fuzz-raw build/test subpic_fuzz_seeds >/dev/null
# shellcheck disable=SC2086 # (word lists on purpose)
clang -g -O1 -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers \
    -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined \
    -DFUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION $def $src -lm -o "build/fuzz-$t"

mkdir -p "build/fuzz-corpus-$t" "build/fuzz-found-$t"
fork=
[ "$jobs" -gt 1 ] && fork="-fork=$jobs -ignore_crashes=1 -ignore_timeouts=1 -ignore_ooms=1"
rc=0
# shellcheck disable=SC2086
"build/fuzz-$t" -max_total_time="$secs" $opts -rss_limit_mb=2048 $fork -print_final_stats=1 \
    -artifact_prefix="build/fuzz-found-$t/" "build/fuzz-corpus-$t" "$seeds" || rc=$?
if [ -n "$(ls "build/fuzz-found-$t")" ]; then
    echo "fuzz $t: found something - replay it with build/fuzz-$t <file>:"
    ls -l "build/fuzz-found-$t"
    exit 1
fi
exit $rc
