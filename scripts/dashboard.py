#!/usr/bin/env python3
"""Live terminal dashboard for cppbench --stream output.

Reads NDJSON from stdin (one JSON object per line) and re-renders a fixed
terminal layout on every frame using ANSI escape codes. Works in any modern
terminal (Windows Terminal, iTerm, gnome-terminal, alacritty, etc.).

Usage:
    adb shell /data/local/tmp/cppbench --stream | python scripts/dashboard.py

Or with a serial:
    adb -s <SERIAL> shell /data/local/tmp/cppbench --stream | python scripts/dashboard.py

Layout:
    +----------------------------------------+
    | SoC: <name>  Model: <model>  Android N |
    | ----------- CPU freqs (GHz) ---------- |
    | cpu0: 2.30  cpu1: 2.30  cpu2: 0.00 ... |
    | --------- Thermal zones (°C) --------- |
    | cpu-0-0: 42.1  battery: 31.4  ...      |
    | -------------- Sensors --------------- |
    | accelerometer:  x= 0.012  y= 9.78 ...  |
    | gyroscope:      x= 0.001  y= 0.000 ... |
    | magnetic_field: x=18.7    ...           |
    | light:          v=124.0 lux            |
    | proximity:      v=5.0 cm               |
    | ...                                    |
    +----------------------------------------+
"""
from __future__ import annotations

import json
import os
import shutil
import sys
from typing import Any


# --- ANSI helpers ----------------------------------------------------------

CLEAR_SCREEN = "\033[2J"
HOME = "\033[H"
HIDE_CURSOR = "\033[?25l"
SHOW_CURSOR = "\033[?25h"
CLEAR_LINE = "\033[K"

DIM = "\033[2m"
BOLD = "\033[1m"
RED = "\033[31m"
GREEN = "\033[32m"
YELLOW = "\033[33m"
CYAN = "\033[36m"
RESET = "\033[0m"


# --- Sensor type → friendly name (Android sensor.h SENSOR_TYPE_* constants)

SENSOR_TYPE_NAMES = {
    1: "accelerometer",
    2: "magnetic_field",
    3: "orientation_deprecated",
    4: "gyroscope",
    5: "light",
    6: "pressure",
    7: "temperature_deprecated",
    8: "proximity",
    9: "gravity",
    10: "linear_acceleration",
    11: "rotation_vector",
    12: "relative_humidity",
    13: "ambient_temperature",
    14: "magnetic_field_uncalibrated",
    15: "game_rotation_vector",
    16: "gyroscope_uncalibrated",
    17: "significant_motion",
    18: "step_detector",
    19: "step_counter",
    20: "geomagnetic_rotation_vector",
    21: "heart_rate",
    22: "tilt_detector",
    23: "wake_gesture",
    24: "glance_gesture",
    25: "pick_up_gesture",
    26: "wrist_tilt_gesture",
    27: "device_orientation",
    28: "pose_6dof",
    29: "stationary_detect",
    30: "motion_detect",
    31: "heart_beat",
    32: "dynamic_sensor_meta",
    34: "low_latency_offbody_detect",
    35: "accelerometer_uncalibrated",
    36: "hinge_angle",
    37: "head_tracker",
    38: "accelerometer_limited_axes",
    39: "gyroscope_limited_axes",
    40: "heading",
}


def sensor_name(type_id: int) -> str:
    return SENSOR_TYPE_NAMES.get(type_id, f"type_{type_id}")


# --- Rendering -------------------------------------------------------------


def term_size() -> tuple[int, int]:
    try:
        s = shutil.get_terminal_size((80, 24))
        return s.columns, s.lines
    except Exception:
        return 80, 24


def render_header(header: dict[str, Any]) -> list[str]:
    env = header.get("env", {})
    soc = env.get("soc", "unknown")
    model = env.get("model", "")
    rel = env.get("android_release", "")
    sdk = env.get("android_sdk", "")
    cpus = env.get("num_cpus", 0)
    return [
        f"{BOLD}{CYAN}cppbench live{RESET}  "
        f"SoC={BOLD}{soc}{RESET}  Model={model}  "
        f"Android {rel} (API {sdk})  CPUs={cpus}",
    ]


def render_freqs(frame: dict[str, Any], header: dict[str, Any]) -> list[str]:
    freqs = frame.get("freqs_khz") or []
    if not freqs:
        return []
    out = [f"{DIM}— CPU freqs (GHz) —{RESET}"]
    line = "  "
    cols = 6
    for i, khz in enumerate(freqs):
        ghz = (khz or 0) / 1.0e6
        color = GREEN if ghz > 0 else DIM
        line += f"cpu{i}: {color}{ghz:5.2f}{RESET}   "
        if (i + 1) % cols == 0:
            out.append(line.rstrip())
            line = "  "
    if line.strip():
        out.append(line.rstrip())
    return out


def temp_color(c: float) -> str:
    if c >= 70: return RED
    if c >= 55: return YELLOW
    if c >= 35: return GREEN
    return CYAN


def render_thermals(frame: dict[str, Any], header: dict[str, Any]) -> list[str]:
    temps_mc = frame.get("temps_mc") or []
    zones = header.get("thermal_zones", [])
    if not temps_mc or not zones:
        return []
    out = [f"{DIM}— Thermal zones (°C) —{RESET}"]
    line = "  "
    cols = 3
    rendered = 0
    for z, mc in zip(zones, temps_mc):
        c = (mc or 0) / 1000.0
        if mc is None or mc == -2147483648:  # INT32_MIN sentinel for read failure
            continue
        label = z.get("type", f"zone{z.get('idx')}")
        line += f"{label[:18]:<18}: {temp_color(c)}{c:5.1f}°C{RESET}   "
        rendered += 1
        if rendered % cols == 0:
            out.append(line.rstrip())
            line = "  "
    if line.strip():
        out.append(line.rstrip())
    return out


def render_sensor_line(s: dict[str, Any]) -> str:
    typ = s.get("type", 0)
    name = sensor_name(typ)
    if "x" in s:  # vector sensors
        x = s["x"]; y = s["y"]; z = s["z"]
        return f"  {name:<28}: x={x:+8.3f}  y={y:+8.3f}  z={z:+8.3f}"
    if "v" in s:  # scalar sensors
        return f"  {name:<28}: v={s['v']:+9.3f}"
    if "count" in s:
        return f"  {name:<28}: count={int(s['count'])}"
    if "cos" in s:  # rotation vectors
        return f"  {name:<28}: x={s['x']:+7.3f}  y={s['y']:+7.3f}  z={s['z']:+7.3f}  w={s['cos']:+7.3f}"
    if "raw" in s:
        raw = "  ".join(f"{v:+8.3f}" for v in s["raw"][:4])
        return f"  {name:<28}: {raw}"
    return f"  {name:<28}: (no fields)"


def render_sensors(frame: dict[str, Any]) -> list[str]:
    sensors = frame.get("sensors") or []
    if not sensors:
        return [f"{DIM}— Sensors (no data yet — waiting for events) —{RESET}"]
    sensors_sorted = sorted(sensors, key=lambda s: s.get("type", 99999))
    out = [f"{DIM}— Sensors ({len(sensors_sorted)} active) —{RESET}"]
    for s in sensors_sorted:
        out.append(render_sensor_line(s))
    return out


def render_dt(frame: dict[str, Any]) -> str:
    dt = frame.get("dt_ms", 0)
    color = GREEN if dt < 300 else (YELLOW if dt < 1000 else RED)
    return f"{DIM}frame dt={color}{dt}ms{RESET}{DIM}{RESET}"


def render_frame(header: dict[str, Any], frame: dict[str, Any], rows: int) -> str:
    lines: list[str] = []
    lines += render_header(header)
    lines.append(render_dt(frame))
    lines += [""]
    lines += render_freqs(frame, header)
    lines += [""]
    lines += render_thermals(frame, header)
    lines += [""]
    lines += render_sensors(frame)
    # Truncate to fit terminal height; clear each line as we draw.
    out = [HOME]
    for i, ln in enumerate(lines[: rows - 1]):
        out.append(ln + CLEAR_LINE + "\n")
    # Pad to bottom of screen
    for _ in range(max(0, rows - 1 - len(lines))):
        out.append(CLEAR_LINE + "\n")
    return "".join(out)


# --- Main ------------------------------------------------------------------


def main() -> int:
    header: dict[str, Any] | None = None

    # Enable ANSI on Windows if possible (Python 3.13+ does this by default,
    # but be defensive).
    if os.name == "nt":
        try:
            import ctypes  # type: ignore
            kernel32 = ctypes.windll.kernel32
            kernel32.SetConsoleMode(kernel32.GetStdHandle(-11), 7)
        except Exception:
            pass

    sys.stdout.write(CLEAR_SCREEN + HOME + HIDE_CURSOR)
    sys.stdout.flush()

    try:
        for raw in sys.stdin:
            raw = raw.strip()
            if not raw:
                continue
            try:
                obj = json.loads(raw)
            except json.JSONDecodeError:
                # Pass through non-JSON lines (e.g. early adb shell noise) to stderr
                # so they don't break the layout.
                sys.stderr.write(f"[non-JSON] {raw}\n")
                continue

            kind = obj.get("kind")
            if kind == "header":
                header = obj
                continue
            if kind == "footer":
                break
            if kind == "frame" and header is not None:
                _, rows = term_size()
                sys.stdout.write(render_frame(header, obj, rows))
                sys.stdout.flush()
    except KeyboardInterrupt:
        pass
    finally:
        sys.stdout.write(SHOW_CURSOR + "\n")
        sys.stdout.flush()
    return 0


if __name__ == "__main__":
    sys.exit(main())
