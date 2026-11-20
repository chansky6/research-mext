#!/bin/sh
set -eu
p=/opt/predswap
cg=/sys/fs/cgroup/memory/g2-$$
log=$(mktemp -d /tmp/g2-uniform.XXXXXX)
d=''
stop_d() {
    if [ -n "$d" ]; then kill -TERM "$d"; wait "$d"; d=''; fi
}
trap 'stop_d; rmdir "$cg" 2>/dev/null || :' EXIT
printf 'artifacts=%s\n' "$log"
for r in 1 2 3; do
    case $r in 2) order='P B1';; *) order='B1 P';; esac
    for v in $order; do
        mkdir "$cg"
        echo 320M > "$cg/memory.limit_in_bytes"
        echo 100 > "$cg/memory.swappiness"
        if [ "$v" = P ]; then
            "$p/predswapd" run --cgroup "$cg" --budget-mb 320 --headroom-mb 64 --scan-period 5 --cooldown 20 --major-guard-ps 3000 > "$log/$r.daemon" 2>&1 &
            d=$!
            sleep 1
            kill -0 "$d"
        fi
        (echo 0 > "$cg/tasks"; LD_PRELOAD="$p/predswap_agent.so" exec "$p/synth" uniform 512 60 "$r") > "$log/$r.$v"
        awk -v r="$r" -v v="$v" '/^uniform/{split($2,a,"=");ops=a[2]} /^steady/{split($4,t,"="); printf "%s %s %.2f ops/s\n",r,v,ops/(t[2]+0)}' "$log/$r.$v"
        stop_d
        rmdir "$cg"
    done
done
