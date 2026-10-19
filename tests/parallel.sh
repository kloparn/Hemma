#!/bin/sh
# tests/parallel.sh — `make test`: the whole suite in one process per core (build/test --shard K/N),
# then one summary. What a failing test printed comes out as it did; any failure fails the run.
#   sh tests/parallel.sh build/test        as many processes as cores (HEMMA_TEST_JOBS to choose)
bin=${1:-build/test}
n=${HEMMA_TEST_JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}
[ "$n" -ge 1 ] 2>/dev/null || n=4
out=$(dirname "$bin")/test-out
mkdir -p "$out" || exit 1

k=0
pids=
while [ "$k" -lt "$n" ]; do
    "$bin" --shard "$k/$n" >"$out/$k.log" 2>&1 &
    pids="$pids $!"
    k=$((k + 1))
done
rc=0
for p in $pids; do wait "$p" || rc=1; done

# each process's own lines (a failure's FAIL and where its files were kept), then the sums of the
# "shard K/N: R run, F failed, C checks, M ms" lines - the time is the slowest process's
k=0
while [ "$k" -lt "$n" ]; do
    grep -v '^shard [0-9]*/[0-9]*: ' "$out/$k.log"
    k=$((k + 1))
done
k=0
while [ "$k" -lt "$n" ]; do
    grep '^shard [0-9]*/[0-9]*: ' "$out/$k.log" || echo "shard $k/$n: 0 run, 1 failed, 0 checks, 0 ms"
    k=$((k + 1))
done | awk -v n="$n" -v rc="$rc" '
    { run += $3; failed += $5; checks += $7; if ($9 > ms) ms = $9 }
    END {
        if (failed || rc)
            printf "\n\033[31m%d of %d tests failed\033[0m (%d checks, %d ms in %d processes)\n", failed, run, checks, ms, n
        else
            printf "\n\033[32mall %d tests passed\033[0m (%d checks, %d ms in %d processes)\n", run, checks, ms, n
    }'
exit $rc
