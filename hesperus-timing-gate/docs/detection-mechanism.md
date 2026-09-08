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

## 3. Detection algorithm — dual-EMA fast/slow ratio

Implemented in `beam-sensor.h`, ported from the pre-ESP32 Arduino prototype's
`ExpFilter`/`GateSensor` classes (`legacy/gate-detector/`).

Each channel keeps two exponential moving averages of the same raw ADC
signal:

| EMA | Time constant | Tracks |
|-----|---------------|--------|
| `fast` | ~2 ms (`BEAM_TAU_FAST_S`) | the instantaneous reading |
| `slow` | ~1 s (`BEAM_TAU_SLOW_S`) | the ambient/illuminated baseline |

```
value += alpha * (new_value - value)     // alpha = 1 / (sample_rate * tau)
```

A beam-break pulls `fast` down toward zero much faster than `slow` can
follow, so the ratio between them is a self-normalizing, illumination-
independent trigger:

- **Trigger (falling):** `fast < 0.25 * slow`
- **Re-arm (rising):** `fast > 0.75 * slow`

The 0.25/0.75 gap is built-in hysteresis — it stops noise sitting near a
single threshold from chattering. Confirmation requires `BEAM_CONFIRM_SAMPLES`
(3) consecutive samples past the trigger ratio before latching an interrupt
(~3 ms added latency at 1 kHz), rejecting single-sample glitches that the
literal legacy algorithm (single-sample latch) would have accepted.

Below `BEAM_DARK_FLOOR_COUNTS` (40 counts, ~1% of 12-bit full scale) on
`slow`, the sensor isn't seeing usable light at all — the ratio logic is
skipped rather than dividing by near-zero noise, and the confirm counter is
reset so a stale near-miss from before the dark period can't complete once
light returns.

### Fixes applied over the literal legacy algorithm

Per `legacy/gate-detector/legacy-evaluation.md`'s suggested-improvements
list, three changes sit on top of the ported algorithm:

1. **Startup seeding** (`beam_sensor_seed_from_adc()`) — both EMAs are
   seeded from the mean of `BEAM_SEED_SAMPLE_COUNT` (64) real ADC samples at
   boot, instead of legacy's implicit start at `value = 0`, which caused a
   startup delay before readings were valid. A seed mean below the dark
   floor is a real, technician-actionable fault (sensor unlit/miswired/
   obstructed) and is surfaced via `debug_printf`, not silently accepted.
2. **Unconditional recovery clamp** — `slow = max(slow, fast)` runs on
   *every* update, not gated behind the dark-floor check. Legacy only ran it
   once already above the floor, so it never applied during a dark/occluded
   period and the detector could get stuck non-responsive after a long
   occlusion.
3. **Confirm counter** (`BEAM_CONFIRM_SAMPLES`) — described above; legacy
   latched on a single sample.

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
  beam alignment), not something that varies with ambient light. Not
  necessarily a fault since the ratio detector is self-normalizing per
  channel, but worth keeping in mind if the two channels are ever compared
  directly.
- **Incandescent light does add a real, non-negligible offset on top of the
  emitter baseline** (case C → D, armSlow 870 → 1160) — incandescent sources
  have much more IR content than LED/daylight. Not a problem for the ratio
  detector itself, but something to keep in mind if `BEAM_DARK_FLOOR_COUNTS`
  or absolute-count assumptions are ever revisited.

**Open question — anomalous transient rise.** All four cases note the same
behaviour: `slow` periodically rises to ~12 counts even during stretches
where the raw ADC reading itself stays at 0, uncorrelated between the two
channels. Not yet explained or root-caused. One plausible mechanism worth
checking against a live trace: the unconditional recovery clamp
(`slow = max(slow, fast)`, fix #2 above) means a single-sample noise spike on
`fast` (tau ~2 ms, so it reacts almost immediately) gets latched into `slow`
on that same sample, then only decays back over `slow`'s ~1 s time constant
— so a single transient glitch in the raw signal could show up as a
multi-hundred-millisecond bump in `slow` even though `raw` itself looks
clean on a coarser view. Unverified — use `BEAM_SENSOR_STREAM_DEBUG` (below)
to catch `raw`/`fast`/`slow` together across an occurrence before concluding
anything.

## 5. Bench tuning / debugging

Build with `-D BEAM_SENSOR_STREAM_DEBUG=1` (already on for
`hesperus-gate-s3-super-mini`) to stream `armRaw`/`armFast`/`armSlow`/
`startRaw`/`startFast`/`startSlow` over serial as `label:value` pairs, rate-
limited to 20 Hz. Feed it to a live plotter — see `tools/serial-plotter.md`
at the workspace root for the recommended tool and why the Arduino IDE's
built-in plotter isn't used instead.

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

- `synchronous-reflective-detection.md` — a closed-loop MCPWM/FIR/IIR
  reflective-sensing spec. Never implemented; unrelated to the algorithm
  actually running (see `beam-sensor.h`'s own header comment). Kept for
  reference only.
- `reflective-detection-feasibility.md` — a feasibility read of that spec
  against the firmware *as it stood before this detector existed* (a purely
  digital `attachInterrupt(CHANGE)` break-beam). Superseded by this
  document; kept for its architectural analysis (FreeRTOS core/task
  placement, latency modeling) which remains relevant if reflective sensing
  is ever revisited.
