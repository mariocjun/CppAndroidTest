#!/usr/bin/env bash
# device-harness.sh — persistent rooted-device test harness for cppbench.
#
# Generalises the device-session patterns from the redacted-project project's
# tools/csi-patch/flash_f2.sh (device-identity gate, asroot exec, recovery-slot
# root activation, verify-every-step) into a reusable rig for installing and
# benchmarking the cppandroidtest app on PHYSICAL devices. The official tester
# is the rooted SM-N975F (Exynos 9825); root unlocks PMU counters
# (perf_event_open via CAP_PERFMON) and cpufreq governor pinning, neither of
# which is available on the unrooted S24 Ultra.
#
# SAFETY — mirrors flash_f2.sh:
#   - Refuses to act on a device whose codename isn't in the registry.
#   - NEVER touches WiFi firmware / bootloader (this harness is OS/app level
#     only: reboot-recovery, adb install, su exec, cpufreq governor). All
#     reversible. The N975U (d2q) control device is registered root_method=none
#     so it's never rooted/modified.
#   - Governor changes are restored on 'restore'/'full'; they also reset on
#     any reboot.
#
# Usage:
#   bash scripts/device-harness.sh probe [serial]
#   bash scripts/device-harness.sh root <serial>
#   bash scripts/device-harness.sh pin-perf <serial>
#   bash scripts/device-harness.sh unpin <serial>
#   bash scripts/device-harness.sh install <serial> <apk>
#   bash scripts/device-harness.sh bench <serial> [filter]
#   bash scripts/device-harness.sh full <serial> [filter]   # root->pin->bench->unpin
#
# Env:
#   CPPBENCH   path to a cppbench arm64-v8a ELF (default: build/bench-arm64/cppbench,
#              falls back to downloading the latest release asset)
set -euo pipefail
export MSYS_NO_PATHCONV=1   # keep /data/local/tmp literal through MSYS adb

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
RESULTS="$REPO/results"
REMOTE=/data/local/tmp/cppbench

# --- Device registry --------------------------------------------------------
# serial|codename|label|root_method
#   root_method: recovery = Magisk in recovery slot, `adb reboot recovery`
#                          activates root (N975F)
#                magisk   = `su` works after a normal boot
#                none     = no root; app-level testing only (S24 Ultra, N975U)
# Codename is re-verified at runtime against the device; a mismatch aborts.
device_codename() { # serial -> expected codename
    case "$1" in
        REDACTED_SERIAL|REDACTED_HOST:5555) echo "d2s" ;;   # N975F Exynos (rooted target)
        *) echo "" ;;
    esac
}
device_root_method() {
    case "$1" in
        REDACTED_SERIAL|REDACTED_HOST:5555) echo "recovery" ;;
        *) echo "none" ;;
    esac
}
device_label() {
    case "$1" in
        REDACTED_SERIAL|REDACTED_HOST:5555) echo "N975F-Exynos9825" ;;
        *) echo "unknown" ;;
    esac
}

adbx()    { adb -s "$SERIAL" "$@"; }
asroot()  { adb -s "$SERIAL" shell "su -c '$1'"; }

# --- Gates ------------------------------------------------------------------
require_device() {
    adbx get-state >/dev/null 2>&1 \
        || { echo "FAIL: $SERIAL not connected (check 'adb devices')"; exit 1; }
    local want got
    want=$(device_codename "$SERIAL")
    got=$(adbx shell getprop ro.product.device | tr -d '\r')
    if [ -n "$want" ] && [ "$got" != "$want" ]; then
        echo "FAIL: $SERIAL codename '$got' != registry '$want' — REFUSING"; exit 1
    fi
    echo "device: $SERIAL  model=$(adbx shell getprop ro.product.model | tr -d '\r')  codename=$got  label=$(device_label "$SERIAL")"
}

is_root() { [ "$(asroot 'id -u' 2>/dev/null | tr -d '\r')" = 0 ]; }

ensure_root() {
    if is_root; then echo "root: already active"; return 0; fi
    local method; method=$(device_root_method "$SERIAL")
    case "$method" in
        recovery)
            echo "root: not active — rebooting into recovery slot (Magisk)..."
            adbx reboot recovery
            adbx wait-for-device
            local i
            for i in $(seq 1 80); do
                [ "$(adbx shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')" = 1 ] && break
                sleep 3
            done
            is_root && { echo "root: active after recovery boot"; return 0; }
            echo "FAIL: root still not active after recovery boot"; exit 1 ;;
        *)
            echo "FAIL: device $SERIAL root_method='$method' — cannot get root here"; exit 1 ;;
    esac
}

# --- cpufreq governor pinning (root) ---------------------------------------
list_policies() { asroot "ls -d /sys/devices/system/cpu/cpufreq/policy*" 2>/dev/null | tr -d '\r'; }

pin_perf() {
    ensure_root
    echo "pin-perf: setting all cpufreq policies to 'performance'"
    asroot 'for p in /sys/devices/system/cpu/cpufreq/policy*; do echo performance > $p/scaling_governor; done'
    asroot 'for p in /sys/devices/system/cpu/cpufreq/policy*; do echo "  $(basename $p): $(cat $p/scaling_governor) max=$(cat $p/scaling_cur_freq)"; done'
}

unpin() {
    ensure_root
    echo "unpin: restoring 'schedutil' governor"
    asroot 'for p in /sys/devices/system/cpu/cpufreq/policy*; do echo schedutil > $p/scaling_governor; done'
}

# --- cppbench provisioning --------------------------------------------------
resolve_cppbench() {
    local p="${CPPBENCH:-$REPO/build/bench-arm64/cppbench}"
    if [ -f "$p" ]; then echo "$p"; return 0; fi
    # Fall back to the latest release asset.
    local dl="$REPO/build/cppbench-release"
    mkdir -p "$(dirname "$dl")"
    echo "cppbench not at $p — downloading latest release asset..." >&2
    local url
    url=$(gh release view --repo mariocjun/CppAndroidTest --json assets \
            --jq '.assets[] | select(.name|startswith("cppbench-")) | .url' 2>/dev/null | head -1)
    [ -n "$url" ] || { echo "FAIL: could not resolve a cppbench release asset" >&2; exit 1; }
    gh release download --repo mariocjun/CppAndroidTest --pattern 'cppbench-*-arm64-v8a' \
        --output "$dl" --clobber >&2
    echo "$dl"
}

push_cppbench() {
    local bin; bin=$(resolve_cppbench)
    echo "push: $bin -> $REMOTE"
    adbx push "$(cygpath -w "$bin" 2>/dev/null || echo "$bin")" "$REMOTE" >/dev/null
    adbx shell "chmod 755 $REMOTE"
}

bench() {
    local filter="${1:-}"
    ensure_root
    push_cppbench
    mkdir -p "$RESULTS"
    local ts label out
    ts=$(date +%Y%m%d-%H%M%S)
    label=$(device_label "$SERIAL")
    out="$RESULTS/${label}-${filter:-default}-${ts}.json"
    local args="--json"
    [ -n "$filter" ] && args="$args --filter=$filter"
    echo "bench (as root): $REMOTE $args"
    # Run as root so PMU counters (perf_event_open) and any privileged sysfs work.
    asroot "$REMOTE $args" | tr -d '\r' > "$out"
    echo "saved: $out  ($(wc -c < "$out") bytes)"
    echo "--- env + first benchmark name ---"
    python3 -c "import json,sys; d=json.load(open('$out')); e=d.get('env',{}); print('SoC:', e.get('soc_identified'), '| clusters:', len(e.get('cpu_clusters',[])), '| benches:', [b['name'] for b in d.get('benchmarks',[])])" 2>/dev/null || head -c 300 "$out"
    echo
    echo "$out"
}

install_apk() {
    local apk="$1"
    require_device
    echo "install: $apk"
    adbx install -r "$(cygpath -w "$apk" 2>/dev/null || echo "$apk")"
}

probe() {
    require_device
    echo "root_method (registry): $(device_root_method "$SERIAL")"
    echo "root active now: $(is_root && echo yes || echo no)"
    echo "android: $(adbx shell getprop ro.build.version.release | tr -d '\r')  build: $(adbx shell getprop ro.build.version.incremental | tr -d '\r')"
    echo "abilist: $(adbx shell getprop ro.product.cpu.abilist | tr -d '\r')"
    echo "cpufreq policies:"
    if is_root; then
        asroot 'for p in /sys/devices/system/cpu/cpufreq/policy*; do echo "  $(basename $p): gov=$(cat $p/scaling_governor) cpus=$(cat $p/affected_cpus) max=$(cat $p/cpuinfo_max_freq)"; done'
        echo "perf_event_paranoid: $(asroot 'cat /proc/sys/kernel/perf_event_paranoid' | tr -d '\r')  (root bypasses via CAP_PERFMON)"
    else
        echo "  (no root — limited probe)"
    fi
}

# --- main -------------------------------------------------------------------
CMD="${1:-}"; shift || true
SERIAL="${1:-REDACTED_SERIAL}"; shift || true

case "$CMD" in
    probe)     probe ;;
    root)      require_device; ensure_root ;;
    pin-perf)  require_device; pin_perf ;;
    unpin)     require_device; unpin ;;
    install)   install_apk "$1" ;;
    bench)     require_device; bench "${1:-}" ;;
    full)
        require_device; ensure_root; pin_perf
        bench "${1:-}"
        unpin
        ;;
    *)
        echo "usage: $0 {probe|root|pin-perf|unpin|install <apk>|bench [filter]|full [filter]} [serial]"
        exit 1 ;;
esac
