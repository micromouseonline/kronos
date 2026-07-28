# Converting a PlatformIO Environment to Arduino IDE

See [`PLATFORMIO.md`](PLATFORMIO.md) for how this workspace's
`platformio.ini`/`boards.ini` composition actually works — this page is
the fallback for the rare case where you need to build a target through
Arduino IDE instead.

An earlier build-time script (`tools/auto_compose.py`) used to
auto-generate an Arduino IDE setup reference from `boards.ini`; it's been
removed (see the "remove arduino support" commit), so there is no tool for
this anymore — everything below is a manual translation, done by hand.

Not recommended for day-to-day work in any of this workspace's PlatformIO
projects (you'd be reassembling by hand what `extends` does automatically),
but if needed for one-off debugging:

- **Resolve the full chain yourself first.** Pick the target `[env:*]`, then
  manually walk its `extends` list and every parent's `extends` list
  (`env:* → base_* → env_common, proc_*, feature_*`) to get the flattened
  set of `board`, `build_flags`, and `lib_deps`. There's no tool for this -
  read `boards.ini` and `platformio.ini` directly.
- **Board selection**: match `board` + the `proc_*` block's MCU to the
  closest entry under Arduino IDE's Tools > Board > esp32 menu (e.g.
  `esp32-s3-devkitc-1` -> "ESP32S3 Dev Module"). Flash size, partition
  scheme, PSRAM, and USB CDC/DFU mode become individual Tools submenu
  selections instead of `board_build.*` keys and `-D ARDUINO_USB_*` flags -
  set each one to match.
- **Partition table**: `board_build.partitions = no_ota.csv` /
  `default_16MB.csv` has no direct Arduino IDE picker for custom CSVs;
  choose the closest built-in scheme from Tools > Partition Scheme, or
  install the `.csv` into the ESP32 core's `tools/partitions/` folder to
  make it selectable.
- **`-D` build flags**: Arduino IDE has no `build_flags` field. Either
  `#define` each one (e.g. `HAS_HTTP`, `HAS_LOVYANGFX`, `BOARD_JC2432W328C`,
  `STATUS_LED`) at the top of the `.ino`/main source before any header that
  reads it, or add them to `boards.txt`/a `platform.local.txt` build
  extra-flags override. Missing one silently disables a feature rather than
  failing to compile, so cross-check against the flattened list.
- **Libraries**: install each `lib_deps` entry's library manually via
  Library Manager (name only - the `@ ^x.y.z` version pin is not enforced
  by Arduino IDE, so pin the version yourself if it matters).
- **`-Iinclude` / `-Isrc`**: Arduino IDE auto-includes the sketch folder but
  not `include/`. `lv_conf.h` and similar headers need copying alongside
  the `.ino`, or LVGL's `LV_CONF_INCLUDE_SIMPLE` lookup will fail.
- **What you lose**: the `generate_merged_bin.py` post-build step (no
  auto-generated `dist/*-all-in-one.bin`), and `check_ini_composition.py`'s
  safety net - nothing will warn you if the manual translation drops a flag
  or library.

One case where an Arduino IDE build is the intended fallback rather than
just a debugging escape hatch: `env_common`'s ESP32-C6 note in `boards.ini`
points here directly if C6 support is ever needed again, since the
PlatformIO-side fork required for it conflicts with the shared package
cache (see that comment for details).
