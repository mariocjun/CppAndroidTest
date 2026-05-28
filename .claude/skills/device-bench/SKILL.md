---
name: device-bench
description: Run the cppbench profiler on a connected physical Android device (rooted or not) via the device harness, with median/CV aggregation and root-only PMU counters + governor pinning. Use when the user wants to benchmark/profile on real hardware, run on the phone/device, get perf numbers from the N975F or any connected device, drive the app UI on-device, or debug a device-test CI failure. Triggers: "run on the device", "bench on the phone", "profile on real hardware", "test on the N975F", "device-test".
---

# Run cppbench on a physical device

`scripts/device-harness.sh` is the rooted-device session manager. It mirrors a
firmware-flash tool's safety (device-identity gate, `asroot`, recovery-slot root
activation) but only ever does OS/app-level ops — `adb reboot recovery`, install,
`su` exec, cpufreq governor. It never touches firmware/bootloader.

## First: orient on the device

```bash
adb devices -l                                   # what's connected
bash scripts/device-harness.sh probe <serial>    # root status, clusters, governors, paranoid
```

`probe` is read-only. It tells you whether root is active (some devices root via
`adb reboot recovery` — the harness handles that in `ensure_root`), the cpufreq
clusters, and whether PMU is reachable.

## Run benchmarks

```bash
# One run (as root if the device supports it), default suite:
bash scripts/device-harness.sh bench <serial> [filter]

# Statistically sound: N runs under the performance governor, median+CV:
bash scripts/device-harness.sh full-stats <serial> "" 7
#   = pin-perf (performance governor) -> 7 runs -> aggregate-runs.py -> unpin
```

- **Always prefer `full-stats`** for any number you'll report — single mobile
  runs are DVFS/scheduler noise. CV>15% in the output = treat as unreliable.
- **Root unlocks** PMU counters (`perf_event_open` via CAP_PERFMON despite
  `perf_event_paranoid=3`) and governor pinning. Unrooted devices: the benches
  still run, PMU reports `pmu_available=false`, governor stays as-is.
- `filter` selects one bench (`stream`, `neon_fma`, `i8mm`, ...); empty = the
  default suite. Opt-in benches (i8mm, sve2, sustained) need the explicit filter.

Results land in `results/<label>-<tag>-<ts>.json` (gitignored). Aggregate any
set with `python scripts/aggregate-runs.py "results/<glob>*.json"`.

## Drive the app UI (not just the ELF)

```bash
bash scripts/device-harness.sh ui-test <serial> <path-to.apk>
```

Installs the APK and taps `btn_hwcaps` / `btn_run` **by content-description**
(`scripts/ui_tap.py` reads the live uiautomator tree — resolution/layout
independent, never pixel coordinates), then verifies each tap wrote its result
file. `ui_tap.py <serial> find` lists every locatable node if a locator breaks.

## CI: physical device-test

`.github/workflows/device-test.yml` runs this on a self-hosted runner. It's
**owner-only** (`workflow_dispatch` + repository-owner guard; never
`pull_request`). `release.yml` dispatches it on every `v*` tag. To run manually:
`gh workflow run device-test.yml -f filter="" -f runs=7`.

## Gotchas

- **Serial is the SECOND arg**: `device-harness.sh <cmd> <serial> [args]`.
  Forgetting it makes the next arg get parsed as the serial.
- **`adb uninstall` before install** — CI debug keystores are ephemeral, so a
  new APK won't install over an old one (the harness `ui-test` already does this).
- **`MSYS_NO_PATHCONV=1`** for adb `/sdcard`/`/data` paths on Windows git-bash;
  the harness sets it. Local `python.exe` calls need it OFF.
- The device registry (serial → codename/root_method) lives at the top of
  `device-harness.sh`. Add your device there; a codename mismatch aborts (safety).
