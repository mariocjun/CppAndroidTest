#!/usr/bin/env python3
"""Resolution- and layout-independent UI tap by stable locator.

Instead of hardcoding pixel coordinates (which break the moment a button
moves, the layout reflows, or the screen resolution differs), this dumps the
LIVE uiautomator view tree, finds the node by a stable attribute, reads its
actual rendered bounds, and taps the center. The only thing it depends on is
the locator value — which we control (content-description test IDs set in
MainActivity.kt: btn_run, btn_hwcaps, btn_sensors, btn_cameras, btn_upload,
field_filter).

Usage:
    python scripts/ui_tap.py <serial> tap   <by> <value>
    python scripts/ui_tap.py <serial> find                # list all locatable nodes
    python scripts/ui_tap.py <serial> exists <by> <value> # exit 0 if present

  <by> = desc (content-description) | text | id (resource-id)

Calls adb directly (not via a shell), so /sdcard paths aren't mangled by MSYS.
"""
from __future__ import annotations

import re
import subprocess
import sys
import time

REMOTE_DUMP = "/sdcard/_uidump.xml"
ATTR = {"desc": "content-desc", "text": "text", "id": "resource-id"}


def adb(serial: str, *args: str) -> subprocess.CompletedProcess:
    return subprocess.run(["adb", "-s", serial, *args],
                          capture_output=True, text=True, encoding="utf-8", errors="replace")


def dump_ui(serial: str, retries: int = 4) -> str:
    # Make sure the screen is on/awake — uiautomator dump fails on a black screen.
    adb(serial, "shell", "input", "keyevent", "KEYCODE_WAKEUP")
    for _ in range(retries):
        d = adb(serial, "shell", "uiautomator", "dump", REMOTE_DUMP)
        if "dumped" in (d.stdout + d.stderr).lower() or d.returncode == 0:
            cat = adb(serial, "shell", "cat", REMOTE_DUMP)
            if cat.stdout.strip().startswith("<?xml") or "<hierarchy" in cat.stdout:
                return cat.stdout
        time.sleep(1.0)
    raise RuntimeError("uiautomator dump failed after retries "
                       f"(last stderr: {d.stderr.strip()[:200]})")


def iter_nodes(xml: str):
    for m in re.finditer(r"<node\b[^>]*?/?>", xml):
        yield m.group(0)


def node_center(node_xml: str):
    bm = re.search(r'bounds="\[(\d+),(\d+)\]\[(\d+),(\d+)\]"', node_xml)
    if not bm:
        return None
    x1, y1, x2, y2 = map(int, bm.groups())
    return (x1 + x2) // 2, (y1 + y2) // 2, (x1, y1, x2, y2)


def find(xml: str, by: str, value: str):
    attr = ATTR[by]
    for s in iter_nodes(xml):
        am = re.search(rf'\b{re.escape(attr)}="([^"]*)"', s)
        if am and am.group(1) == value:
            c = node_center(s)
            if c:
                return c
    return None


def cmd_find(serial: str) -> int:
    xml = dump_ui(serial)
    print(f"{'class':<28}{'content-desc':<18}{'text':<22}{'bounds'}")
    print("-" * 90)
    for s in iter_nodes(xml):
        cls = (re.search(r'\bclass="([^"]*)"', s) or [None, ""])[1].split(".")[-1]
        desc = (re.search(r'\bcontent-desc="([^"]*)"', s) or [None, ""])[1]
        txt = (re.search(r'\btext="([^"]*)"', s) or [None, ""])[1][:20]
        bnd = (re.search(r'\bbounds="([^"]*)"', s) or [None, ""])[1]
        if desc or txt or cls in ("Button", "EditText"):
            print(f"{cls:<28}{desc:<18}{txt:<22}{bnd}")
    return 0


def cmd_tap(serial: str, by: str, value: str) -> int:
    xml = dump_ui(serial)
    hit = find(xml, by, value)
    if not hit:
        print(f"NOT FOUND: {by}={value!r} — current locatable elements:", file=sys.stderr)
        cmd_find(serial)
        return 2
    cx, cy, bounds = hit
    adb(serial, "shell", "input", "tap", str(cx), str(cy))
    print(f"tapped {by}={value!r} at ({cx},{cy})  bounds={bounds}")
    return 0


def cmd_exists(serial: str, by: str, value: str) -> int:
    xml = dump_ui(serial)
    return 0 if find(xml, by, value) else 1


def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__, file=sys.stderr)
        return 2
    serial, action = sys.argv[1], sys.argv[2]
    if action == "find":
        return cmd_find(serial)
    if action == "tap":
        return cmd_tap(serial, sys.argv[3], sys.argv[4])
    if action == "exists":
        return cmd_exists(serial, sys.argv[3], sys.argv[4])
    print(f"unknown action: {action}", file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main())
