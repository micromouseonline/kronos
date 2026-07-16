#!/usr/bin/env python3
"""Flash a pre-built PlatformIO firmware directly via esptool.

Skips PlatformIO's build-check overhead in the IDE, for quickly
programming many boards from binaries already built with
`pio run -e <env>`.

Usage (run from inside the target PlatformIO project directory):
    python ../tools/flash.py -e <env> [-p <port>] [--erase]

If -p/--port is omitted, esptool auto-detects the connected board by
scanning available serial ports.

Reads .pio/build/<env>/idedata.json for the bootloader/partition-table/
boot_app0 offsets (these vary by chip family and are resolved there by
PlatformIO itself), then flashes firmware.bin at the app offset on top.
"""
import argparse
import json
import shutil
import subprocess
import sys
from pathlib import Path

PROJECT_DIR = Path.cwd()
DEFAULT_BAUD = "921600"
DEFAULT_APP_OFFSET = "0x10000"


def find_esptool():
    for candidate in ("esptool", "esptool.py"):
        path = shutil.which(candidate)
        if path:
            return [path]
    return [sys.executable, "-m", "esptool"]


def load_idedata(env, build_dir):
    idedata_path = build_dir / "idedata.json"
    if not idedata_path.exists():
        print(f"idedata.json missing, generating it via 'pio run -t idedata -e {env}'...")
        subprocess.run(["pio", "run", "-t", "idedata", "-e", env], check=True, cwd=PROJECT_DIR)
    return json.loads(idedata_path.read_text())


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("-e", "--env", required=True, help="PlatformIO environment name")
    parser.add_argument("-p", "--port", help="Serial port, e.g. COM5 or /dev/ttyUSB0 (auto-detected if omitted)")
    parser.add_argument("-b", "--baud", default=DEFAULT_BAUD)
    parser.add_argument("--app-offset", default=DEFAULT_APP_OFFSET, help=f"Firmware app offset (default {DEFAULT_APP_OFFSET})")
    parser.add_argument("--erase", action="store_true", help="Erase flash before writing")
    args = parser.parse_args()

    build_dir = PROJECT_DIR / ".pio" / "build" / args.env
    firmware = build_dir / "firmware.bin"
    if not firmware.exists():
        sys.exit(f"{firmware} not found, build the project first: pio run -e {args.env}")

    idedata = load_idedata(args.env, build_dir)
    images = [(img["offset"], img["path"]) for img in idedata["extra"]["flash_images"]]
    images.append((args.app_offset, str(firmware)))
    images.sort(key=lambda x: int(x[0], 16))

    esptool = find_esptool()
    base_cmd = esptool + ["--chip", "auto", "--baud", args.baud]
    if args.port:
        base_cmd += ["--port", args.port]

    if args.erase:
        subprocess.run(base_cmd + ["erase_flash"], check=True)

    cmd = base_cmd + ["write_flash"]
    for offset, path in images:
        cmd += [offset, path]

    print("Running:", " ".join(cmd))
    subprocess.run(cmd, check=True)


if __name__ == "__main__":
    main()
