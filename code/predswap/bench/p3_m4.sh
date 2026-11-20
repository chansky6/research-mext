#!/bin/sh
set -eu
p=/opt/predswap
cg=/sys/fs/cgroup/memory/p3-m4-$$
out=$(mktemp -d /tmp/p3-m4.XXXXXX)
d=''
cleanup() {
    if [ -n "$d" ]; then kill -TERM "$d" 2>/dev/null || :; wait "$d" || :; fi
    rmdir "$cg" 2>/dev/null || :
}
trap cleanup EXIT
trap 'exit 1' INT TERM HUP
printf 'artifacts=%s\n' "$out"
for mode in trained m2-only off; do
    mkdir "$cg"
    echo 320M > "$cg/memory.limit_in_bytes"
    echo 100 > "$cg/memory.swappiness"
    "$p/predswapd" run --cgroup "$cg" --budget-mb 320 --headroom-mb 64 --scan-period 5 --cooldown 20 --major-guard-ps 3000 --m4-mode "$mode" > "$out/$mode.daemon" 2>&1 &
    d=$!
    sleep 1
    kill -0 "$d"
    (echo 0 > "$cg/tasks"; LD_PRELOAD="$p/predswap_agent.so" exec "$p/synth" cb 512 100 7 20000 2560 60000) > "$out/$mode.workload"
    "$p/predswapd" status > "$out/$mode.status"
    printf 'mode=%s\n' "$mode"
    cat "$out/$mode.workload" "$out/$mode.status"
    awk '$1=="pgmajfault"' "$cg/memory.stat"
    kill -TERM "$d"; wait "$d"; d=''
    rmdir "$cg"
done
