#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
# xtier-arm machine preparation for 192.168.90.7 (openEuler 22.03 SP4, 5.10 aarch64).
# Source this, then run xtier_arm_sysprep as root.
export XTIER_ARM_ROOT="${XTIER_ARM_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)}"

xtier_arm_sysprep() {
    [ "$(id -u)" -eq 0 ] || { echo "run as root"; return 1; }

    # SPE/aux requires paranoid <= 0 (kernel-mode excluded but PMU access for root)
    echo 0 > /proc/sys/kernel/perf_event_paranoid 2>/dev/null || true
    # aux events are not sample-rate throttled, but keep the headroom anyway
    echo 25     > /proc/sys/kernel/perf_cpu_time_max_percent 2>/dev/null || true
    echo 100000 > /proc/sys/kernel/perf_event_max_sample_rate 2>/dev/null || true
    echo 0      > /proc/sys/kernel/perf_cpu_time_max_percent 2>/dev/null || true
    echo 0      > /proc/sys/kernel/nmi_watchdog 2>/dev/null || true

    # page-state is keyed by virtual address: same reason as xTier
    echo 0 > /proc/sys/kernel/randomize_va_space 2>/dev/null || true
    # THP never: demotion at 4K granularity would split huge pages (SETUP.md §5)
    echo never > /sys/kernel/mm/transparent_hugepage/enabled 2>/dev/null || true

    # swap: keep ONLY the swapfile on /home; raise swappiness so the kernel
    # prefers swap-out over OOM (predswap notes, 2026-11)
    swapoff /dev/dm-1 2>/dev/null || true
    swapon /home/swapfile 2>/dev/null || true
    echo 100 > /proc/sys/vm/swappiness 2>/dev/null || true

    # clock stability
    for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
        echo performance > "$g" 2>/dev/null || true
    done

    echo 3 > /proc/sys/vm/drop_caches
    swapon -s
    echo "sysprep done (perf paranoid=$(cat /proc/sys/kernel/perf_event_paranoid), swappiness=$(cat /proc/sys/vm/swappiness))"
}
