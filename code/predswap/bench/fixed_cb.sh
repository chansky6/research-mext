#!/bin/sh
# Example: sh fixed_cb.sh B1 B2 P ; all variants perform identical steady work.
set -eu
p=/opt/predswap
out=$(mktemp -d /tmp/fixed-cb.XXXXXX)
cg=/sys/fs/cgroup/memory/fixed-cb-$$
d=''
cleanup() {
    if [ -n "$d" ]; then kill -TERM "$d" 2>/dev/null || :; wait "$d" || :; fi
    rmdir "$cg" 2>/dev/null || :
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM
printf 'artifacts=%s\n' "$out"
cat /proc/swaps > "$out/swaps.before"
for variant do
    mkdir "$cg"
    if [ "$variant" = B0 ]; then echo 1G > "$cg/memory.limit_in_bytes"; else echo 256M > "$cg/memory.limit_in_bytes"; fi
    echo 100 > "$cg/memory.swappiness"
    case $variant in
        B0|B1) ;;
        B2|P)
            only=0; mode=trained
            if [ "$variant" = B2 ]; then only=1; mode=off; fi
            PREDSWAP_OFFLOAD_ONLY=$only "$p/predswapd" run --cgroup "$cg" --budget-mb 256 --headroom-mb 64 --major-guard-ps 3000 --scan-period 5 --cooldown 20 --m4-mode "$mode" > "$out/$variant.daemon" 2>&1 &
            d=$!; sleep 1; kill -0 "$d";;
        *) exit 2;;
    esac
    (echo 0 > "$cg/tasks"; exec numactl --cpunodebind=0 --membind=0 env LD_PRELOAD="$p/predswap_agent.so" "$p/synth" cb 512 0 "${SEED:-7}" 20000 2560 60000 8388608) > "$out/$variant.workload"
    grep -Fx 'cb verify bad=0' "$out/$variant.workload" >/dev/null
    printf '%s: ' "$variant"; grep '^cb ops=' "$out/$variant.workload"
    awk '/^steady/{print}' "$out/$variant.workload"
    awk '$1=="pgmajfault"' "$cg/memory.stat"
    if [ -n "$d" ]; then
        "$p/predswapd" status > "$out/$variant.status"
        grep -E '^  M4|^agent:' "$out/$variant.status"
        kill -TERM "$d"; wait "$d"; d=''
    fi
    rmdir "$cg"
done
cat /proc/swaps > "$out/swaps.after"
