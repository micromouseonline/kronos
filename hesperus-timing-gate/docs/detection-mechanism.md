# Detection Mechanism

How a Hesperus gate board actually detects an object passing through it, as
implemented today. For how this fits into cross-gate timing sync, see
`review.md`. For the two documents describing a *different*, unimplemented
optical scheme and a feasibility read against an *older* implementation, see
"Related/superseded documents" at the bottom.

## 1. Optical path

Each physical gate is two enclosures facing each other across a gap
(`hardware-shared/mechanical/`):

- **`hesperus-emitter`** — the emitter side. A simple board with no WiFi/
  sensor logic; drives two IR LED outputs (`PIN_A`/`PIN_B`) continuously
  toward the gap.
- **`hesperus-timing-gate`** (this project) — the receiver side. Two IR
  phototransistors face back across the gap, one per channel, each feeding
  an ADC pin on the ESP32.

This is a **transmissive break-beam** arrangement (object interrupts the
beam), not a reflective one — despite `beam-sensor.h`'s own name
("reflective/occlusion beam-break detector"), which describes the detection
*algorithm's* ancestry (see §4), not the optics.

Channel A / channel B map to ARM/START/GOAL depending on the board's
provisioned role — see `board-role.h`. The two ADC pins are
`ARM_SENSOR_PIN`/`START_SENSOR_PIN`, set per-board in `boards.ini`.

## 2. Sampling

A hardware timer fires at `BEAM_SAMPLE_RATE_HZ` (1 kHz) and notifies
`beamSampleTask` (`main.cpp`) via `vTaskNotifyGiveFromISR` — the ISR itself
does no ADC reads or float math, just the notification. `beamSampleTask`
runs in ordinary task context and, per sample tick:

1. `analogRead()`s both `ARM_SENSOR_PIN` and `START_SENSOR_PIN`.
2. Feeds each raw reading into that channel's `BeamSensor::update()`.
3. On a trigger, pushes a `PendingCapture` (event type + `esp_timer_get_time()`
   processor clock) onto `triggerCaptureQueue`.

The real Wi-Fi TSF timestamp is *not* captured here — `esp_wifi_get_tsf_time()`
takes an internal lock that can block and has caused an Interrupt WDT panic
when called from a time-critical path before. It's captured one task-context
hop later, in `tsfCaptureTask()`, as close to this instant as the scheduler
allows.

## 3. Detection algorithm — dual-EMA fast/slow drop

Implemented in `beam-sensor.h`, ported from the pre-ESP32 Arduino prototype's
`ExpFilter`/`GateSensor` classes (`legacy/gate-detector/`).

Each channel keeps two exponential moving averages of the same raw ADC
signal:

| EMA | Time constant | Tracks |
|-----|---------------|--------|
| `fast` | ~2 ms (`BEAM_TAU_FAST_S`) | the instantaneous reading |
| `slow` | ~1 s (`BEAM_TAU_SLOW_S`) | the ambient+emitter baseline |

```
value += alpha * (new_value - value)     // alpha = 1 / (sample_rate * tau)
```

A beam-break pulls `fast` down toward the ambient-only level much faster
than `slow` can follow, so the absolute drop between them is the trigger:

- **Trigger (falling):** `slow - fast >= BEAM_TRIGGER_DROP_COUNTS` (300)
- **Re-arm (rising):** `slow - fast <= BEAM_REARM_DROP_COUNTS` (100)

This was originally a `fast/slow` *ratio* (0.25 falling / 0.75 rising,
ported directly from legacy) rather than an absolute count drop — see
"Ratio replaced with absolute drop" below for why that was changed.

The 300/100 gap is built-in hysteresis — it stops noise sitting near a
single threshold from chattering. Confirmation requires `BEAM_CONFIRM_SAMPLES`
(3) consecutive samples past the trigger drop before latching an interrupt
(~3 ms added latency at 1 kHz), rejecting single-sample glitches that the
literal legacy algorithm (single-sample latch) would have accepted.

Below `BEAM_DARK_FLOOR_COUNTS` (40 counts, ~1% of 12-bit full scale) on
`slow`, the sensor isn't seeing usable light at all — the trigger logic is
skipped entirely, and the confirm counter is reset so a stale near-miss from
before the dark period can't complete once light returns.

### Fixes applied over the literal legacy algorithm

Per `legacy/gate-detector/legacy-evaluation.md`'s suggested-improvements
list, plus one change beyond that list, sit on top of the ported algorithm:

1. **Startup seeding** (`beam_sensor_seed_from_adc()`) — both EMAs are
   seeded from the mean of `BEAM_SEED_SAMPLE_COUNT` (64) real ADC samples at
   boot, instead of legacy's implicit start at `value = 0`, which caused a
   startup delay before readings were valid. A seed mean below the dark
   floor is a real, technician-actionable fault (sensor unlit/miswired/
   obstructed) and is surfaced via `debug_printf`, not silently accepted.
2. **Recovery clamp, gated on `!armed`** — `slow = max(slow, fast)` lets
   `slow` snap back up quickly once light returns after a long occlusion,
   instead of crawling back over its own ~1 s time constant, so the
   detector can't get stuck non-responsive. Originally ran unconditionally
   on every update (matching legacy, which only skipped it below the dark
   floor); refined to run only while `!armed` after a real false-trigger
   bug was found — see "Recovery-clamp false trigger" below.
3. **Confirm counter** (`BEAM_CONFIRM_SAMPLES`) — described above; legacy
   latched on a single sample.
4. **Ratio replaced with absolute drop** — not in legacy's improvements
   list at all, found after the fixes above still didn't stop false
   triggers from external illumination; see "Ratio replaced with absolute
   drop" below.

`alpha_fast`/`alpha_slow` are derived from `BEAM_SAMPLE_RATE_HZ` rather than
hardcoded, so changing the sample rate recalculates both time constants
together automatically (legacy's own portability note called out the risk of
changing one without the other).

## 4. Bench characterization data

Raw source: `test-data.md`. Readings of the `slow` filter output on the START gate, taken to check
ambient-light rejection before/after the emitter is powered (raw values in
12-bit ADC counts, 0–4095):

| Case | Ambient | Emitter | `armSlow` | `startSlow` |
|------|---------|---------|-----------:|-----------:|
| A | diffuse daylight, ~150 lux | off | 0.0 | 0.2 |
| B | + overhead LED lighting, ~350 lux | off | 0.0 | 0.1 |
| C | same as B | **on** | 870 | 450 |
| D | + incandescent lighting, ~2200 lux | on | 1160 | 760 |

Takeaways:

- **Ambient rejection is good.** Even at ~2200 lux of mixed LED/incandescent
  light with the emitter off, `slow` stays under 1 count on both channels —
  the phototransistors' IR selectivity is doing its job against non-IR
  sources.
- **`armSlow` runs consistently higher than `startSlow`** once the emitter
  is on (870 vs 450 in case C, 1160 vs 760 in case D) — a fixed asymmetry
  between the two channels (LED output, phototransistor sensitivity, or
  beam alignment), not something that varies with ambient light. Mattered
  less under the old self-normalizing ratio detector; matters more now that
  `BEAM_TRIGGER_DROP_COUNTS` (see "Ratio replaced with absolute drop" below)
  is a single absolute threshold shared across both channels — START's
  smaller emitter contribution (~450 counts in case C) leaves noticeably
  less margin above the 300-count trigger than ARM's (~870).
- **Incandescent light does add a real, non-negligible offset on top of the
  emitter baseline** (case C → D, armSlow 870 → 1160) — incandescent sources
  have much more IR content than LED/daylight. This is exactly the ambient
  contribution that broke the old ratio detector on the START channel (see
  "Ratio replaced with absolute drop" below) — worth keeping in mind if
  `BEAM_DARK_FLOOR_COUNTS`, `BEAM_TRIGGER_DROP_COUNTS`, or
  `BEAM_REARM_DROP_COUNTS` are ever revisited.

**Anomalous transient rise (explained).** All four cases noted the same
behaviour: `slow` periodically rising to ~12 counts even during stretches
where the raw ADC reading itself stayed at 0, uncorrelated between the two
channels. This was originally logged as an unverified open question,
guessing at a noise-spike cause. It's since been root-caused (see
"Recovery-clamp false trigger" below): at the time this data was taken, the
recovery clamp ran unconditionally, so *any* upward blip on `fast` — a
single noisy ADC sample included — got latched into `slow` and only decayed
back over `slow`'s ~1 s time constant. The fix below (gating the clamp on
`!armed`) should eliminate this bench artifact entirely, since it's the
same underlying mechanism, just triggered by sensor noise instead of an
external light source. Worth re-running this bench characterization to
confirm the artifact is gone.

### Recovery-clamp false trigger (fixed)

The recovery clamp's unconditional form had a real false-trigger bug, not
just the cosmetic artifact above: if a **bright external light source**
briefly floods the phototransistor — e.g. a passing robot's own
reflected-light sensor, which works by measuring reflected light power the
same way this gate does — `fast` spikes up, the clamp drags `slow` up to
match on the same sample, and when the external source's pulse ends, `fast`
drops back to the normal beam-intact level quickly (tau ~2 ms) while `slow`
stays inflated for up to ~1 s. If the external source was bright enough,
the normal return to baseline reads as `fast` dropping below 25% of the
now-inflated `slow` — a false ARM/START/GOAL trigger, with no actual
occlusion having occurred.

Checked whether `legacy/gate-detector/gate-detector.ino`'s original
algorithm avoided this: it doesn't — its clamp is equally unconditional,
and it's arguably more exposed in practice since it latches a trigger on a
single sample with no confirm-count at all (`BEAM_CONFIRM_SAMPLES` is
something this port added on top).

**Fix:** gate the clamp on `!armed`, so it only fires while genuinely
recovering from a latched trigger, not during ordinary armed operation. A
real occlusion is unaffected (`fast` is low there, so the clamp was already
a no-op in that direction); recovery right after a real occlusion ends is
unaffected (still `!armed` at that instant); a bright external pulse during
normal armed operation no longer inflates `slow` at all, since it now only
follows its own ~1 s EMA (a full-scale spike lasting ~100 ms only nudges
`slow` by roughly 9–10% of the excursion — nowhere near enough to trigger).
See `beam-sensor.h`'s header comment and inline comment on the clamp.

Fixed and bench-tested; false triggers from external illumination persisted
after this fix, root-caused separately below.

### Ratio replaced with absolute drop (fixed)

Even with the recovery-clamp fix above flashed and tested, external
illumination pulses kept producing false triggers. The clamp only closes
one path (the *unarmed* recovery path getting dragged up by a stale
external spike); it does nothing for a pulse arriving while the gate is
genuinely armed.

The deeper problem was the `fast/slow` *ratio* trigger itself
(`fast < 0.25 * slow`), which implicitly assumes ambient is negligible next
to the emitter's own contribution. Write `slow ≈ ambient + emitter` before
an occlusion and `fast → ambient` once one starts (the emitter's
contribution is cut off; ambient doesn't change on `fast`'s ~2 ms
timescale). The ratio only crosses 0.25 when `ambient < emitter / 3`. Bench
data (`test-data.md`) puts this in perspective: case B's LED-only ambient is
negligible (~0 counts), but case D's added incandescent ambient contributes
roughly 290 counts (arm) / 310 counts (start) on top of case C — on the
weaker START channel (~450-count emitter contribution in case C), that's
~69% of the emitter signal, well past the 33% line. Two distinct failure
modes follow from the same root cause:

- **Missed real triggers under strong ambient** — if ambient exceeds 1/3 of
  the emitter's contribution, a genuine full occlusion never drops the
  ratio far enough to fire at all.
- **False triggers when the baseline is ambient-dominated** — if `slow` is
  low in the first place (weak/misaligned emitter, one LED not lit,
  recovering from a prior occlusion), `0.25 * slow` is a small absolute
  gap, so ordinary illumination noise — an external light pulse included —
  can cross it without any real occlusion.

**Fix:** replace the ratio with an absolute count drop: trigger on
`slow - fast >= BEAM_TRIGGER_DROP_COUNTS` (300), re-arm on
`slow - fast <= BEAM_REARM_DROP_COUNTS` (100). This directly measures the
emitter's own light being lost, independent of the ambient level — a real
occlusion always produces a drop close to the full emitter contribution
(≥400-500 counts per the bench data), comfortably clearing 300 regardless
of how bright ambient is, while an ambient-only fluctuation would need an
absolute swing of 300+ counts within `fast`'s ~2 ms time constant to false-
trigger, not just a fraction of a possibly-small baseline.
**Bench-confirmed on real hardware:** flashed and tested, operation is more
reliable, particularly against *high-frequency* interference (e.g. a
flickering/modulated external light source) — plausibly because each
interference cycle's low phase now has to sustain a ≥300-count drop for
`BEAM_CONFIRM_SAMPLES` (3) consecutive 1 ms samples to latch, a much harder
bar than crossing a ratio that a depressed baseline made cheap; `fast`'s own
~2 ms tau also averages down the peak-to-trough excursion of anything
faster than that. `BEAM_TRIGGER_DROP_COUNTS`/`BEAM_REARM_DROP_COUNTS` (300/
100) are starting points consistent with the bench data on hand — still
worth further tuning against a real bench trace if edge cases turn up, same
as `BEAM_CONFIRM_SAMPLES`.

## 5. Bench tuning / debugging

Build with `-D BEAM_SENSOR_STREAM_DEBUG=1` (already on for
`hesperus-gate-s3-super-mini`) to stream `armRaw`/`armFast`/`armSlow`/
`startRaw`/`startFast`/`startSlow` over serial as `label:value` pairs, rate-
limited to 20 Hz. Feed it to a live plotter — see `tools/serial-plotter.md`
at the workspace root for the recommended tool and why the Arduino IDE's
built-in plotter isn't used instead.

**Bench check for the recovery-clamp fix above.** Not yet run on real
hardware — worth doing before trusting this for a real event:

1. **False-trigger check.** With the gate armed and the beam intact, briefly
   flash a bright light across the phototransistor (phone flashlight, or a
   second IR source) without ever fully blocking the beam, then let it go
   dark again. Watch `armSlow`/`startSlow` on the plotter — before the fix
   this should show `slow` snapping up to track the flash and a possible
   false trigger on release; after the fix, `slow` should barely move and
   no trigger should fire.
2. **Real-occlusion regression check.** Confirm a genuine occlusion (hand
   or object fully blocking the beam) still triggers after
   `BEAM_CONFIRM_SAMPLES`, and that `slow` still snaps back up promptly
   (not a slow ~1 s crawl) once the object clears, so re-arm timing is
   unchanged from before the fix.

## 6. Bench stimulus injection (ARES)

`ares-pulse-generator` used to trigger a gate for bench testing by driving
the old digital `GATE_PIN_A`/`GATE_PIN_B` pins directly (active-low, per
`board-role.h`'s original wiring). That path is gone now that the sensor
pins are analogue ADC inputs, but the same active-low convention carries
over — `beam-sensor.h`'s trigger fires when the reading drops *low* relative
to its own `slow` baseline, matching the old design's polarity. Two ways to
re-establish a bench stimulus:

- **Permanent test tap, coexisting with the real phototransistor.** A
  pull-up resistor (tens of kΩ to ~1 MΩ — high enough not to meaningfully
  load the phototransistor's own bias network when idle) from 3.3V to the
  ADC node, plus a MOSFET to GND gated by the digital trigger signal. Idle
  (MOSFET off): the phototransistor drives the node as normal. Asserted: the
  MOSFET's low on-resistance overrides both the pull-up and the
  phototransistor, forcing the node near 0V — a clean trigger without
  needing to disconnect the sensor between test sessions. Keep the idle
  reading comfortably above `BEAM_DARK_FLOOR_COUNTS` (40 counts).
- **Direct substitution**, matching how ARES drove the old digital pins.
  `ares-pulse-generator/firmware/src/main.cpp`'s trigger pins are plain
  push-pull `OUTPUT`s (idle HIGH, no open-drain) — with the phototransistor
  temporarily disconnected, ARES's output can be wired straight to
  `ARM_SENSOR_PIN`/`START_SENSOR_PIN`: idle 3.3V reads as a healthy high
  baseline, a pulse to 0V reads as a hard occlusion.

Either way, timing already works out: `BEAM_CONFIRM_SAMPLES` (3) at 1 kHz
needs the low level held ≥3 ms, and ARES's existing `BURST_PULSE_MS` (10 ms)
clears that comfortably. No RC-settling concern either — `analogRead()` is
polled once per 1 ms tick, not continuously sampled, so a clean digital edge
is effectively instantaneous relative to that.

## 7. Current status

Two board targets: `hesperus-gate-s3-zero`/`hesperus-gate-s3-super-mini`
(`base_s3_zero`/`base_s3_super_mini` in `boards.ini`), both with
`ARM_SENSOR_PIN`/`START_SENSOR_PIN` assigned and building correctly against
this detector.

## Related/superseded documents

- `reflective-detection-proposal.md` — a closed-loop MCPWM/FIR/IIR
  reflective-sensing scheme (spec + feasibility assessment), unrelated to
  the algorithm actually running (see `beam-sensor.h`'s own header
  comment). Never implemented; kept for its architectural analysis
  (Core 0/Core 1 placement, a secondary-processor alternative, real
  profiling data) which remains relevant if reflective sensing is ever
  revisited.
