#!/bin/bash
# tools/ci/local.sh — CI's Linux jobs (.github/workflows/build.yml) on your own machine, in a minute or
# so: any Linux, or WSL on Windows (./build.ps1 ci - docs/development.md). It works on a copy of the
# source tree in a RAM disk (/dev/shm), a fresh folder each run, with the compiled objects of the last
# run of the same job brought along, so only what changed compiles again. Nothing is deleted:
# /dev/shm empties when the machine (or WSL) shuts down.
#
#   bash tools/ci/local.sh test       make + make test with the system compiler     (CI: test)
#   bash tools/ci/local.sh asan       the suite under ASan + UBSan with clang         (CI: sanitizers)
#   bash tools/ci/local.sh fuzz [S]   libFuzzer over the converter for S s (90)       (CI: fuzz)
#
# The tree to test: a tar on stdin (git ls-files -co --exclude-standard | tar -cf - -T -), else
# --tar FILE, else this checkout's own files (git ls-files). Needs make, gcc, and for asan/fuzz clang
# with its sanitizer and libFuzzer runtimes (Ubuntu: build-essential clang libclang-rt-18-dev).
set -u
# The suite in 4 processes, as GitHub's runners split it: tests sharing a process meet what earlier ones
# left (a test module stays registered), and 20 processes once hid such a meeting CI then had (0.25.0).
export HEMMA_TEST_JOBS=${HEMMA_TEST_JOBS:-4}
job=${1:-test}
secs=90
tarfile=
shift || true
while [ $# -gt 0 ]; do
    case $1 in
    --tar) tarfile=$2; shift 2 ;;
    [0-9]*) secs=$1; shift ;;
    *) echo "unknown argument $1"; exit 2 ;;
    esac
done
case $job in test | asan | fuzz) ;; *) echo "usage: bash tools/ci/local.sh test | asan | fuzz [seconds] [--tar FILE]"; exit 2 ;; esac

base=/dev/shm
[ -d "$base" ] && [ -w "$base" ] || base=${TMPDIR:-/tmp}
cache=$base/hemma-ci/cache/$job
W=$base/hemma-ci/runs/$(date +%Y%m%d-%H%M%S)-$$-$job
mkdir -p "$cache" "$W" || exit 1

# the tree
if [ -n "$tarfile" ]; then
    tar -xf "$tarfile" -C "$W" || exit 1
elif [ ! -t 0 ]; then
    tar -xf - -C "$W" || exit 1
else
    repo=$(cd "$(dirname "$0")/../.." && pwd)
    (cd "$repo" && git ls-files -co --exclude-standard | tar -cf - -T -) | tar -xf - -C "$W" || exit 1
fi
cd "$W" || exit 1
# the last run's objects: kept times, so unchanged files stay compiled (git keeps no commit here:
# the binary says just its version)
[ -d "$cache/build" ] && cp -a "$cache/build" .

jobs=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)
now() { date +%s%N; }
ms() { echo $((($(now) - $1) / 1000000)); }
t0=$(now)
rc=0
case $job in
test)
    make -j"$jobs" all build/test >make.log 2>&1 || { tail -40 make.log; exit 1; }
    built=$(ms "$t0"); t1=$(now)
    make test >test.log 2>&1 || rc=$?
    ;;
asan)
    export ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1
    S=(CC=clang "CFLAGS=-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined"
       "LDFLAGS=-fsanitize=address,undefined")
    make -j"$jobs" "${S[@]}" build/test >make.log 2>&1 || { tail -40 make.log; exit 1; }
    built=$(ms "$t0"); t1=$(now)
    make "${S[@]}" test >test.log 2>&1 || rc=$?
    ;;
fuzz)
    export ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1
    { make -j"$jobs" build/test && HEMMA_FUZZ_SEEDS=build/fuzz-seeds build/test conv_fuzz_seeds &&
      clang -g -O1 -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers \
        -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined \
        -DFUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION tools/convfuzz.c \
        modules/av/conv.c modules/av/mkv.c modules/av/mpeg2.c modules/av/deint.c modules/av/h264enc.c \
        modules/av/ac3.c modules/av/aac.c modules/av/mp4.c -lm -o build/convfuzz; } >make.log 2>&1 ||
        { tail -40 make.log; exit 1; }
    built=$(ms "$t0"); t1=$(now)
    mkdir -p build/fuzz-corpus build/fuzz-found
    build/convfuzz -max_total_time="$secs" -max_len=65536 -rss_limit_mb=2048 -timeout=60 -print_final_stats=1 \
        -artifact_prefix=build/fuzz-found/ build/fuzz-corpus build/fuzz-seeds >test.log 2>&1 || rc=$?
    ;;
esac
ran=$(ms "$t1")
cp -a build "$cache/" 2>/dev/null # (for the next run of this job)

# what the compiler warned about (files compiled this run only), then how it went
w=$(grep -c "warning:" make.log)
[ "$w" -gt 0 ] && echo "$w compiler warnings (files compiled this run): $W/make.log"
if [ "$job" = fuzz ]; then
    grep -E "^Done|ERROR|SUMMARY" test.log | head -5
    [ -n "$(ls build/fuzz-found 2>/dev/null)" ] && echo "found: $W/build/fuzz-found"
else
    tail -n 30 test.log | grep -v '^$'
fi
echo "$job: built in $((built / 1000)).$(((built % 1000) / 100)) s, ran in $((ran / 1000)).$(((ran % 1000) / 100)) s - $([ $rc = 0 ] && echo ok || echo FAILED) ($W)"
exit $rc
