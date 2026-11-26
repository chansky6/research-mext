#!/bin/sh
# mc_spd.sh: memcached under spd, fixed work per run.
# Usage: sh mc_spd.sh VARIANT:BUDGET_MB ...
# VARIANT is B0 (no daemon, everything in DRAM) or a spd predictor
# (none|next|markov|both|stream).  Needs ssdpager.ko loaded, swap off, THP never.
# mcbench is code/predswap/bench/mcbench.c.  MC_OPTS adds memcached options,
# e.g. MC_OPTS="-o no_lru_crawler".  idle_fault_s is the demand-fault rate over
# the last 10 s before the clients start.
# PATTERN=run:L or session:S:L replaces the Zipf clients with bench/mcloc.
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
port=12346
keys=1000000
value=2000
ops=${OPS:-2000000}
threads=${THREADS:-8}
zipf=${ZIPF:-1.0}
seed=${SEED:-11}
settle=${SETTLE:-90}
out=$(mktemp -d /tmp/mc-spd.XXXXXX)
memc=; d=
cleanup() {
	[ -z "$d" ] || { kill -TERM "$d" 2>/dev/null || :; wait "$d" 2>/dev/null || :; }
	[ -z "$memc" ] || { kill -TERM "$memc" 2>/dev/null || :; wait "$memc" 2>/dev/null || :; }
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM
stat() { awk -v k="$1" '{for (i = 1; i < NF; i++) if ($i == k) print $(i + 1)}' /dev/ssdpager | head -1; }
[ "$(awk '/SwapTotal/ {print $2}' /proc/meminfo)" = 0 ] || { echo "swap is on" >&2; exit 1; }
grep -q '\[never\]' /sys/kernel/mm/transparent_hugepage/enabled || { echo "THP not never" >&2; exit 1; }
printf 'artifacts=%s\n' "$out"
printf 'variant,budget_mb,rate,p50_us,p99_us,p999_us,err,fault,late,pf_done,hit,waste,evict,ssd_mb,seconds,idle_fault_s\n' > "$out/results.csv"
for spec do
	variant=${spec%%:*}
	budget=${spec##*:}
	d=; memc=; idle=0
	# shellcheck disable=SC2086
	numactl --cpunodebind=0 --membind=0 memcached -u root -l 127.0.0.1 -p "$port" -m 3072 -t 4 -c 4096 ${MC_OPTS:-} &
	memc=$!
	sleep 1
	"$here/mcbench" preload 127.0.0.1 "$port" "$threads" "$keys" "$value" 0 "$zipf" 95 > "$out/$variant-$budget.preload"
	if [ "$variant" != B0 ]; then
		"$here/spd" -p "$memc" -b "$budget" -P "$variant" > "$out/$variant-$budget.spd" &
		d=$!
		# let the daemon reach the budget while the keyspace is idle
		sleep "$((settle - 10))"
		fi=$(stat fault)
		sleep 10
		idle=$((($(stat fault) - fi) / 10))
	fi
	f0=$(stat fault)
	if [ -n "${PATTERN:-}" ]; then
		"$here/bench/mcloc" 127.0.0.1 "$port" "$threads" "$keys" "$value" "$ops" 95 "$seed" "$PATTERN" > "$out/$variant-$budget.steady"
	else
		"$here/mcbench" steady 127.0.0.1 "$port" "$threads" "$keys" "$value" "$ops" "$zipf" 95 "$seed" > "$out/$variant-$budget.steady"
	fi
	f1=$(stat fault)
	head -3 /dev/ssdpager > "$out/$variant-$budget.module"
	if [ -n "$d" ]; then
		kill -TERM "$d"
		wait "$d" 2>/dev/null || :
		d=
		tot=$(tail -1 "$out/$variant-$budget.spd")
		ssd=$(grep -o 'ssd_mb=[0-9]*' "$out/$variant-$budget.spd" | tail -1 | cut -d= -f2)
	else
		tot=; ssd=0
	fi
	awk -v v="$variant" -v b="$budget" -v fl="$((f1 - f0))" -v tot="$tot" -v ssd="$ssd" -v idle="$idle" '
	/^steady/ {
		for (i = 1; i <= NF; i++) { split($i, k, "="); f[k[1]] = k[2] }
		n = split(tot, w, " ")
		for (i = 1; i <= n; i++) { split(w[i], k, "="); t[k[1]] = k[2] }
		rate = f["rate"]; p50 = f["p50"]; p99 = f["p99"]; p999 = f["p999"]
		sub(/\/s$/, "", rate); sub(/us$/, "", p50); sub(/us$/, "", p99); sub(/us$/, "", p999)
		printf "%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n", v, b, rate, p50, p99, p999, f["err"],
			fl, t["late"] + 0, t["pf_done"] + 0, t["hit"] + 0, t["waste"] + 0, t["evict"] + 0, ssd, f["seconds"], idle
	}' "$out/$variant-$budget.steady" >> "$out/results.csv"
	kill -TERM "$memc" 2>/dev/null || :
	wait "$memc" 2>/dev/null || :
	memc=
	sleep 2
done
cat "$out/results.csv"
