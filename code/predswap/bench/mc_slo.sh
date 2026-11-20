#!/bin/sh
# mc_slo.sh: fixed-work memcached comparisons and minimum-DRAM search.
# Usage: sh mc_slo.sh B0:4096 B1:1024 P:1024 ...
# Each ARG is VARIANT:BUDGET_MB. B0 is unconstrained; B1 native swap;
# B2 daemon offload-only; P full daemon.
set -eu
p=/opt/predswap
port=12345
keys=1000000
value=2000
ops=2000000
getpct=95
zipf=1.0
threads=8
seed=${SEED:-11}
out=$(mktemp -d /tmp/mc-slo.XXXXXX)
cg=/sys/fs/cgroup/memory/mc-$$
memc=; d=
cleanup() {
    [ -z "$d" ] || { kill -TERM "$d" 2>/dev/null || :; wait "$d" 2>/dev/null || :; }
    [ -z "$memc" ] || { kill -TERM "$memc" 2>/dev/null || :; wait "$memc" 2>/dev/null || :; }
    rmdir "$cg" 2>/dev/null || :
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM
printf 'artifacts=%s\n' "$out"
printf 'variant,budget_mb,rate,p50_us,p99_us,p999_us,err,pgmajfault,seconds\n' > "$out/results.csv"
for spec do
    variant=${spec%%:*}
    budget=${spec##*:}
    d=; memc=
    mkdir "$cg"
    echo 100 > "$cg/memory.swappiness"
    echo 4096M > "$cg/memory.limit_in_bytes"
    LD_PRELOAD="$p/predswap_agent.so" sh -c "echo \$\$ > $cg/tasks; exec numactl --cpunodebind=0 --membind=0 \
        memcached -p $port -U 0 -u root -t 4 -c 4096 -m 3072" &
    memc=$!
    i=0
    until python3 -c "import socket,sys; s=socket.socket(); s.settimeout(0.2); sys.exit(0 if s.connect_ex(('127.0.0.1',$port))==0 else 1)"; do
        i=$((i+1))
        if [ $i -gt 100 ]; then echo "memcached did not listen" >&2; exit 1; fi
        sleep 0.2
    done
    "$p/mcbench" preload 127.0.0.1 "$port" 1 "$keys" "$value" "$ops" "$zipf" "$getpct" > "$out/$variant-$budget.load"
    grep '^preload' "$out/$variant-$budget.load"
    case $variant in
        B0) ;;
        B2|P)
            only=0 mode=trained
            if [ "$variant" = B2 ]; then only=1; mode=off; fi
            PREDSWAP_OFFLOAD_ONLY=$only "$p/predswapd" run --cgroup "$cg" --budget-mb "$budget" --headroom-mb 64 \
                --scan-period 5 --cooldown 20 --major-guard-ps 3000 --m4-mode "$mode" > "$out/$variant-$budget.daemon" 2>&1 &
            d=$!
            sleep 2
            kill -0 "$d" ;;
    esac
    if [ "$variant" != B0 ] && [ "$budget" != 4096 ]; then
        echo "${budget}M" > "$cg/memory.limit_in_bytes"
        sleep 5
    fi
    timeout 600 numactl --cpunodebind=0 --membind=0 "$p/mcbench" steady 127.0.0.1 "$port" "$threads" \
        "$keys" "$value" "$ops" "$zipf" "$getpct" "$seed" > "$out/$variant-$budget.steady"
    cat "$out/$variant-$budget.steady"
    maj=$(awk '$1=="pgmajfault"{print $2}' "$cg/memory.stat")
    awk -v v="$variant" -v b="$budget" -v m="$maj" '
        /^steady/ {
            for (i=1;i<=NF;i++) { split($i,k,"="); f[k[1]]=k[2] }
            rate=f["rate"]; p50=f["p50"]; p99=f["p99"]; p999=f["p999"]; err=f["err"]
            sub(/\/s$/,"",rate); sub(/us$/,"",p50); sub(/us$/,"",p99); sub(/us$/,"",p999)
            printf "%s,%s,%s,%s,%s,%s,%s,%s,%s\n", v, b, rate, p50, p99, p999, err, m, f["seconds"]
        }' "$out/$variant-$budget.steady" >> "$out/results.csv"
    if [ -n "$d" ]; then
        "$p/predswapd" status > "$out/$variant-$budget.status"
        kill -TERM "$d"
        wait "$d" 2>/dev/null || :
        d=
    fi
    kill -TERM "$memc" 2>/dev/null || :
    wait "$memc" 2>/dev/null || :
    memc=
    rmdir "$cg"
done
cat "$out/results.csv"
