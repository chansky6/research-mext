#!/bin/sh
# mc_spd.sh: memcached under spd, fixed work per run.
# Usage: sh mc_spd.sh VARIANT:BUDGET_MB ...
# VARIANT is B0 (no daemon, everything in DRAM) or a spd predictor
# (none|next|markov|both|stream).  Needs ssdpager.ko loaded, swap off, THP never.
# mcbench is code/predswap/bench/mcbench.c.  MC_OPTS adds memcached options,
# e.g. MC_OPTS="-o no_lru_crawler".  idle_fault_s is the demand-fault rate over
# the last 10 s before the clients start.
# PATTERN=run:L or session:S:L replaces the Zipf clients with bench/mcloc.
# Variant oracle (needs PATTERN, or TRACE with TRACE_MAP): the client tells spd
# which pages each request LOOKAHEAD (default 256) requests ahead will touch.
# CHAIN=1 (TRACE oracle only) also hints the hash-bucket items assoc_find()
# walks through before each requested item.
# SPD_OPTS adds spd options.
# TRACE=FILE (from bench/twprep.py) replays a request trace with bench/mctrace
# instead: records [0, WARM) fill the cache before the daemon starts, records
# [SKIP, SKIP+OPS) are measured (SKIP defaults to WARM), at QPS requests/s when
# QPS is set (else closed loop).  WARM past the end of the trace populates every
# key the trace has, which makes the measured window free of cold starts.
# PREWIN=N (TRACE only) replaces the idle settle: the daemon reaches the budget
# while the GETs of records [SKIP-N, SKIP) run (with hints under oracle), so page
# ages come from the traffic right before the window and no item moves.
# MC_MEM (default 3072) is memcached's -m.
# The daemon gets SIGUSR1 when the measured clients start (a mark in spd -F logs).
# DISK=sda samples that /proc/diskstats row every second while the measured
# clients run (VARIANT-BUDGET.disk) and prints a summary (VARIANT-BUDGET.disksum).
# With TRACE the summary also splits the run by disk state: a second is "slow"
# when its writes waited over 8 ms on average (the SSD's sustained-write cliff,
# report §5.14.3), and gives the request rate in each state.
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
port=12346
keys=1000000
value=2000
ops=${OPS:-2000000}
threads=${THREADS:-8}
zipf=${ZIPF:-1.0}
seed=${SEED:-11}
getpct=${GET_PCT:-95}
settle=${SETTLE:-90}
out=$(mktemp -d /tmp/mc-spd.XXXXXX)
memc=; d=; mon=
cleanup() {
	[ -z "$mon" ] || kill "$mon" 2>/dev/null || :
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
	numactl --cpunodebind=0 --membind=0 memcached -u root -l 127.0.0.1 -p "$port" -m "${MC_MEM:-3072}" -t 4 -c 4096 ${MC_OPTS:-} &
	memc=$!
	sleep 1
	if [ -z "${DAEMON_EARLY:-}" ]; then
	if [ -n "${TRACE:-}" ]; then
		"$here/bench/mctrace" 127.0.0.1 "$port" "$threads" "$TRACE" 0 "$WARM" > "$out/$variant-$budget.preload"
	else
		"$here/mcbench" preload 127.0.0.1 "$port" "$threads" "$keys" "$value" 0 "$zipf" 95 > "$out/$variant-$budget.preload"
	fi
	fi
	if [ -n "${TRACE:-}" ] && [ -n "${TRACE_MAP:-}" ]; then
		# item address of every key and the region layout, before anything is evicted
		"$here/bench/mctrace" map "$memc" "$TRACE_MAP" "$out/$variant-$budget.imap" ${CHAIN:+"$out/$variant-$budget.chain"} > "$out/$variant-$budget.mapout"
		cp "/proc/$memc/maps" "$out/$variant-$budget.maps"
	fi
	if [ "$variant" = oracle ]; then
		# every key's item address, taken before anything is evicted
		[ -n "${TRACE:-}" ] || "$here/bench/mcloc" map "$memc" "$keys" "$value" "$out/map" > "$out/$variant-$budget.map"
		rm -f "$out/hints"
		mkfifo "$out/hints"
		# shellcheck disable=SC2086
		"$here/spd" -p "$memc" -b "$budget" -P hint -H "$out/hints" ${SPD_OPTS:-} > "$out/$variant-$budget.spd" &
		d=$!
	elif [ "$variant" != B0 ]; then
		# shellcheck disable=SC2086
		"$here/spd" -p "$memc" -b "$budget" -P "$variant" ${SPD_OPTS:-} > "$out/$variant-$budget.spd" &
		d=$!
	fi
	if [ -n "${DAEMON_EARLY:-}" ] && [ -n "${TRACE:-}" ]; then
		# the daemon runs while the cache fills, so its page ages come from real traffic
		"$here/bench/mctrace" 127.0.0.1 "$port" "$threads" "$TRACE" 0 "$WARM" > "$out/$variant-$budget.preload"
	fi
	if [ -n "$d" ] && [ -n "${PREWIN:-}" ] && [ -n "${TRACE:-}" ]; then
		pre0=$((${SKIP:-$WARM} - PREWIN))
		if [ "$variant" = oracle ]; then
			MCTRACE_GETS_ONLY=1 "$here/bench/mctrace" 127.0.0.1 "$port" "$threads" "$TRACE" "$pre0" "$PREWIN" 0 \
				"$out/$variant-$budget.imap" "$out/hints" "${LOOKAHEAD:-256}" ${CHAIN:+"$out/$variant-$budget.chain"} > "$out/$variant-$budget.prewin"
		else
			MCTRACE_GETS_ONLY=1 "$here/bench/mctrace" 127.0.0.1 "$port" "$threads" "$TRACE" "$pre0" "$PREWIN" > "$out/$variant-$budget.prewin"
		fi
	elif [ -n "$d" ]; then
		# let the daemon reach the budget while the keyspace is idle
		[ "$((settle - 10))" -le 0 ] || sleep "$((settle - 10))"
		fi=$(stat fault)
		[ "$settle" -le 0 ] || sleep 10
		idle=$((($(stat fault) - fi) / 10))
	fi
	[ -z "$d" ] || kill -USR1 "$d"
	if [ -n "${DISK:-}" ]; then
		while :; do
			echo "$(date +%s.%N) $(awk -v d="$DISK" '$3 == d' /proc/diskstats)"
			sleep 1
		done > "$out/$variant-$budget.disk" &
		mon=$!
	fi
	f0=$(stat fault)
	export MCTRACE_PROGRESS="$out/$variant-$budget.ops"
	if [ -n "${TRACE:-}" ] && [ "$variant" = oracle ]; then
		"$here/bench/mctrace" 127.0.0.1 "$port" "$threads" "$TRACE" "${SKIP:-$WARM}" "$ops" "${QPS:-0}" \
			"$out/$variant-$budget.imap" "$out/hints" "${LOOKAHEAD:-256}" ${CHAIN:+"$out/$variant-$budget.chain"} > "$out/$variant-$budget.steady"
	elif [ -n "${TRACE:-}" ]; then
		"$here/bench/mctrace" 127.0.0.1 "$port" "$threads" "$TRACE" "${SKIP:-$WARM}" "$ops" "${QPS:-0}" > "$out/$variant-$budget.steady"
	elif [ "$variant" = oracle ]; then
		"$here/bench/mcloc" 127.0.0.1 "$port" "$threads" "$keys" "$value" "$ops" "$getpct" "$seed" "$PATTERN" \
			"$out/map" "$out/hints" "${LOOKAHEAD:-256}" > "$out/$variant-$budget.steady"
	elif [ -n "${PATTERN:-}" ]; then
		"$here/bench/mcloc" 127.0.0.1 "$port" "$threads" "$keys" "$value" "$ops" "$getpct" "$seed" "$PATTERN" > "$out/$variant-$budget.steady"
	else
		"$here/mcbench" steady 127.0.0.1 "$port" "$threads" "$keys" "$value" "$ops" "$zipf" "$getpct" "$seed" > "$out/$variant-$budget.steady"
	fi
	unset MCTRACE_PROGRESS
	f1=$(stat fault)
	if [ -n "$mon" ]; then
		kill "$mon" 2>/dev/null || :
		wait "$mon" 2>/dev/null || :
		mon=
		# fields after the time stamp: 5 reads, 7 sectors, 8 ms; 9 writes, 11 sectors, 12 ms; 14 busy ms, 15 queue ms
		awk 'NR == 1 { t0 = $1; for (i = 5; i <= 15; i++) a[i] = $i }
		     NR > 1 && $5 > pr && ($8 - pm) / ($5 - pr) > worst { worst = ($8 - pm) / ($5 - pr) }
		     { pr = $5; pm = $8; t = $1; for (i = 5; i <= 15; i++) b[i] = $i }
		     END {
			dt = t - t0; r = b[5] - a[5]; w = b[9] - a[9]
			if (dt <= 0) exit
			printf "seconds=%.0f read_iops=%.0f read_mb_s=%.1f read_wait_ms=%.2f worst_1s_read_wait_ms=%.2f write_iops=%.0f write_mb_s=%.1f write_wait_ms=%.2f util_pct=%.0f queue=%.1f\n",
				dt, r / dt, (b[7] - a[7]) / 2048 / dt, r ? (b[8] - a[8]) / r : 0, worst,
				w / dt, (b[11] - a[11]) / 2048 / dt, w ? (b[12] - a[12]) / w : 0,
				(b[14] - a[14]) / 10 / dt, (b[15] - a[15]) / 1000 / dt
		     }' "$out/$variant-$budget.disk" > "$out/$variant-$budget.disksum"
		if [ -s "$out/$variant-$budget.ops" ]; then
			awk 'NR == FNR { ot[++n] = $1; od[n] = $2; next }
			     function done_at(t) { while (j < n && ot[j + 1] <= t) j++; return od[j] + 0 }
			     FNR > 1 {
				if ($9 > pc) s = ($12 - pw) / ($9 - pc) > 8 ? "slow" : "fast"
				d = done_at($1)
				sec[s] += $1 - pt; op[s] += d - pd; rd[s] += $5 - pr; rm[s] += $8 - pm
			     }
			     { if (FNR == 1) { s = "fast"; d = done_at($1) }
			       pt = $1; pc = $9; pw = $12; pr = $5; pm = $8; pd = d }
			     END {
				for (s in sec)
					printf "%s seconds=%.0f ops_s=%.0f read_iops=%.0f read_wait_ms=%.2f\n",
						s, sec[s], op[s] / sec[s], rd[s] / sec[s], rd[s] ? rm[s] / rd[s] : 0
			     }' "$out/$variant-$budget.ops" "$out/$variant-$budget.disk" >> "$out/$variant-$budget.disksum"
		fi
	fi
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
