# PlatformIO Environment Configuration

How PlatformIO environments are structured across this workspace, using
`cerberus-gate-controller/` as the reference implementation. The other two
embedded firmware projects (`hesperus-timing-gate/`,
`ares-pulse-generator/`) share the same `platformio.ini`/`boards.ini`
composition pattern described below (their `boards.ini` section layout is
close to identical to cerberus's) — cerberus is used throughout as the
worked example because it has the most environments and features.

## Why this exists

Cerberus ships the same firmware to five different display boards (different
SoCs, flash sizes, panels). Rather than five near-duplicate `platformio.ini`
blocks, the config is factored into small reusable pieces and composed with
PlatformIO's `extends` mechanism.

## File layout

- `cerberus-gate-controller/firmware/platformio.ini` - the actual build environments
  (`[env:*]`). Pulls in `boards.ini` via `extra_configs`.
- `cerberus-gate-controller/firmware/boards.ini` - reusable building blocks: global
  settings, per-SoC blocks, per-feature blocks, and per-board "base" blocks.
  `platformio.ini` is the only file PlatformIO reads directly; `boards.ini`
  only exists to be `extend`-ed from it.

## Composition layers (in `boards.ini`)

1. **`env_common`** - settings shared by every board: platform/framework
   pin, C++17 flags, global include paths, `extra_scripts` (see below).
2. **`proc_*`** (`proc_esp32s3`, `proc_esp32c3`, `proc_esp32`) - per-SoC
   settings: MCU, clock speed, USB mode flags. One per chip family.
3. **`feature_*`** (`feature_lovyangfx`, `feature_neokey`, `feature_lvgl`,
   `feature_http`, `feature_oled`, `feature_ble`, `feature_neopixel`,
   `feature_psram_qspi_8mb`, `feature_psram_opi_8mb`,
   `feature_psram_opi_16mb`) - one optional capability each: its library
   dependency and a `-D HAS_X=1` build flag. A board only pulls in the
   features it actually uses.
4. **`base_*`** (`base_s3_zero`, `base_cyd2usb_diymalls_ili9341`, ...) -
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

After editing `boards.ini`, run (from the workspace root — the script
takes an explicit path and does nothing useful without one, since its
no-argument default looks for a `base-boards.ini` that doesn't exist in
this repo):

```
python3 tools/check_ini_composition.py cerberus-gate-controller/firmware/boards.ini
```

Substitute `hesperus-timing-gate/firmware/boards.ini` or
`ares-pulse-generator/firmware/boards.ini` to check those projects instead (or
list more than one path — the script accepts multiple files at once).

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

- `env_common`'s `extra_scripts` runs `tools/generate_version_header.py`
  before every build (reads the release version from the
  `[env:version_metadata]` block in `platformio.ini` and generates a
  version header for the firmware), and `tools/generate_merged_bin.py`
  after every build, producing a single flashable
  `<env>-v<version>-all-in-one.bin` in `dist/`.
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

## Setting a default environment

No `platformio.ini` in this workspace sets `default_envs`, so a bare `pio
run` builds every `[env:*]` (see the warning above). To make `pio
run`/`pio run -t upload` target just one environment by default, add
`default_envs` to the `[platformio]` section at the top of that project's
`platformio.ini`:

```ini
[platformio]
extra_configs = boards.ini
default_envs = cerberus-esp32-s3-cyd-touch-freenove
```

Accepts a comma-separated list if more than one environment should build by
default. `-e <name>` on the command line always overrides this. Don't point
it at `[env:version_metadata]` (release-metadata only, not buildable) or
`[env:native]` (host-side unit tests, no board/display/WiFi dependency) —
pick one of the actual board environments.

## Selecting a build environment in VSCode

The PlatformIO IDE extension exposes environments two ways:

- **PlatformIO sidebar (alien-head icon) → PROJECT TASKS**: each environment
  is its own expandable entry (e.g. `cerberus-esp32-s3-cyd-touch-freenove`)
  with Build/Upload/Clean/Monitor tasks underneath. Expand the one you want
  and run its task directly — this works regardless of `default_envs`.
- **Status bar environment switcher**: the bottom status bar shows the
  currently active environment (`default_envs` if set, otherwise the first
  `[env:*]` PlatformIO finds). Click it to open a picker listing every
  environment in `platformio.ini`; the one you pick becomes the target for
  the status bar's Build/Upload/Clean icons until changed again.

## Other sub-projects

`hesperus-timing-gate/` and `ares-pulse-generator/` each have their own
`platformio.ini`/`boards.ini` pair using the same composition layers
described above (`env_common`, `proc_*`, `feature_*`, `base_*`), with
their own per-project environment name prefix (`hesperus-gate-*`,
`ares-pulser-*`) instead of `cerberus-*`. Everything on this page —
composition layers, the `extends` gotcha, adding a board/feature, running
`check_ini_composition.py` — applies the same way in either project;
just substitute the project's own `platformio.ini`/`boards.ini` for
cerberus's.

## Converting an environment to Arduino IDE

Direct Arduino IDE support (an auto-generated setup guide) has been
dropped from the build — see [`ARDUINO.md`](ARDUINO.md) for the manual
translation process, needed only for rare one-off debugging.
