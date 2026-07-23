# PlatformIO Environment Configuration

How PlatformIO environments are structured across this workspace, using
`cerberus-gate-controller/` as the reference implementation. The other
firmware projects (`ESP32-timing-gates-hesperus/`,
`event-pulse-generator-arduino-nano/`) are simple, single-environment
`platformio.ini` files and have not been migrated to this pattern.

## Why this exists

Cerberus ships the same firmware to five different display boards (different
SoCs, flash sizes, panels). Rather than five near-duplicate `platformio.ini`
blocks, the config is factored into small reusable pieces and composed with
PlatformIO's `extends` mechanism.

## File layout

- `cerberus-gate-controller/platformio.ini` - the actual build environments
  (`[env:*]`). Pulls in `boards.ini` via `extra_configs`.
- `cerberus-gate-controller/boards.ini` - reusable building blocks: global
  settings, per-SoC blocks, per-feature blocks, and per-board "base" blocks.
  `platformio.ini` is the only file PlatformIO reads directly; `boards.ini`
  only exists to be `extend`-ed from it.

## Composition layers (in `boards.ini`)

1. **`env_common`** - settings shared by every board: platform/framework
   pin, C++17 flags, global include paths, `extra_scripts` (see below).
2. **`proc_*`** (`proc_esp32s3`, `proc_esp32c3`, `proc_esp32`) - per-SoC
   settings: MCU, clock speed, USB mode flags. One per chip family.
3. **`feature_*`** (`feature_lovyangfx`, `feature_neokey`, `feature_lvgl`,
   `feature_http`, `feature_oled`, `feature_ble`, `feature_neopixel`) -
   one optional capability each: its library dependency and a `-D HAS_X=1`
   build flag. A board only pulls in the features it actually uses.
4. **`base_*`** (`base_m5_core`, `base_cyd2usb_diymalls_ili9341`, ...) -
   one per physical board. Combines `env_common` + the matching `proc_*` +
   whichever `feature_*` blocks that board's hardware needs (e.g. every CYD
   variant needs `feature_oled`; only touchscreen boards need `feature_lvgl`).
   Also sets board-specific values: `board`, flash size, partition table,
   `STATUS_LED` pin, etc.
5. **`[env:*]`** (in `platformio.ini`) - the buildable target. Extends a
   `base_*` plus any extra app-level features (currently every shipping
   board extends the same four: `feature_lovyangfx, feature_neokey,
   feature_lvgl, feature_http`). This is the environment name you pass to
   `pio run -e <name>`.

## The `extends` gotcha - and the fixed convention

PlatformIO's `extends` does **not** merge `build_flags` / `lib_deps` from
parents once a child section redefines that key - it silently drops
whatever the child doesn't re-list. So every level that redefines
`build_flags`/`lib_deps` must explicitly pull in each parent's value with
`${parent_section.key}`, e.g.:

```ini
[env:cerberus-m5-core]
extends = base_m5_core, feature_lovyangfx, feature_neokey, feature_lvgl, feature_http
lib_deps =
    ${base_m5_core.lib_deps}
    ${feature_lovyangfx.lib_deps}
    ${feature_neokey.lib_deps}
    ${feature_lvgl.lib_deps}
    ${feature_http.lib_deps}
build_flags =
    ${base_m5_core.build_flags}
    ${feature_lovyangfx.build_flags}
    ${feature_neokey.build_flags}
    ${feature_lvgl.build_flags}
    ${feature_http.build_flags}
```

Convention (documented in `boards.ini` itself): list `extends` parents in
the fixed order `env_common, proc_*, feature_*`, and mirror that same order
in the `build_flags`/`lib_deps` reference list. Non-compose keys (`board`,
`board_build.flash_mode`, etc.) behave as normal scalar overrides and don't
need this treatment.

After editing `boards.ini`, run:

```
python3 tools/check_ini_composition.py
```

It statically flags sections that redefine `build_flags`/`lib_deps` but
forgot a `${parent.key}` reference, or that inherit the same compose key
from more than one parent ambiguously.

## Adding a new board

1. Add a `base_<board>` block to `boards.ini`: `extends = env_common,
   proc_<soc>` (+ any `feature_*` the hardware needs), then set `board`,
   flash/partition settings, and any `-D BOARD_*` / `-D STATUS_LED=..`
   identifiers.
2. Add an `[env:cerberus-<board>]` block to `platformio.ini`: `extends =
   base_<board>` plus whichever app-level `feature_*` blocks it should ship
   with, and the matching `${...}` references for `lib_deps`/`build_flags`.
3. Run `check_ini_composition.py`, then build: `pio run -e cerberus-<board>`.

## Adding a new feature

Add one `feature_<name>` block to `boards.ini` with just that feature's
`lib_deps` and a `-D HAS_<NAME>=1` build flag, then reference it (in order)
from every `base_*` or `[env:*]` block that should carry it.

## Other build-time behaviour

- `env_common`'s `extra_scripts` runs `tools/generate_merged_bin.py` after
  every build, producing a single flashable `<env>-v<version>-all-in-one.bin`
  in `dist/` (version number comes from the `[env:version_metadata]` block
  in `platformio.ini`).
- `[env:native]` in `platformio.ini` is a separate, unrelated environment for
  running host-side unit tests (`pio test -e native`) - it does not extend
  anything in `boards.ini` and has no board/display/WiFi dependency.
- ESP32-C6 support exists as commented-out blocks in `boards.ini` (disabled
  due to a PlatformIO package-cache conflict with the `pioarduino` fork
  needed for C6 - see the comment above `[proc_esp32c6]` for details and how
  to re-enable).

## Building

From inside `cerberus-gate-controller/`:

```
pio run -e <env-name>       # build one environment, e.g. cerberus-m5-core
pio run -e <env-name> -t upload
pio run                     # builds ALL environments - avoid unless you mean to
```

## Other sub-projects

`ESP32-timing-gates-hesperus/` and `event-pulse-generator-arduino-nano/`
each use a single flat `[env:*]` block with no `extends`/feature
composition - there is nothing to cross-reference against this document
for them.

## Converting an environment to Arduino IDE

Not recommended for Cerberus day-to-day (you'd be reassembling by hand what
`extends` does automatically), but if needed for one-off debugging:

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
  `#define` each one (e.g. `HAS_HTTP`, `HAS_LOVYANGFX`, `BOARD_M5_CORE`,
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
