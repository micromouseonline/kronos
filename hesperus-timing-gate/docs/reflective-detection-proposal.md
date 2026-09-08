# Reflective Detection: Proposal and Assessment

> **Never implemented.** The detector actually running today is a dual-EMA
> fast/slow ratio detector over a transmissive break-beam — see
> `detection-mechanism.md`. This document is kept because the architectural
> analysis (Core 0/Core 1 placement, a secondary-processor alternative, real
> profiling data) remains relevant if reflective sensing is ever revisited.

## 1. The proposal

A self-calibrating reflective light gate: an IR LED and IR phototransistor
pair face a reflective baseline (not each other, as the current transmissive
design does) and detect an object passing through the gap with a minimum
dwell time of 7 ms.

To eliminate ambient-light calibration and saturation issues, the LED is
driven via hardware PWM whose duty cycle is dynamically controlled by a
low-pass-filtered feedback loop to hold the phototransistor output at a
target voltage (~35% full-scale deflection). Fast optical transitions
(objects passing) bypass the slow feedback loop and show up as transient
spikes in the error signal.

```
                    +-------------------------------------------------+
                    |                   ESP32-S3                      |
                    |                                                 |
  +--------------+  |  +-----------+    +-------------+               |
  |  IR LED      |<-+--| MCPWM     |    | Dual-Core   |               |
  +--------------+  |  | Timer/Op  |    | FreeRTOS    |               |
         |          |  +-----+-----+    +------+------+               |
   Optical Path     |        | Sync            |                      |
         v          |        v                 | Core 1 High-Pri Task |
  +--------------+  |  +-----------+           v                      |
  | Photo-Trans. |  |  | DIG ADC1  |----> [ Ring Buffer ]             |
  +------+-------+  |  | DMA       |           |                      |
         |          |  +-----------+           v                      |
  +------v-------+  |                 +------------------+            |
  | Hardware LPF |--+---------------->| 4-Tap FIR Notch  |            |
  +--------------+  |                 +--------+---------+            |
                    |                          |                      |
                    |            +-------------+-------------+        |
                    |            |                           |        |
                    |            v                           v        |
                    |  +-------------------+   +-------------------+  |
                    |  | High-Pass / Diff  |   | 2Hz LPF (Feedback)|  |
                    |  +---------+---------+   +---------+---------+  |
                    |            |                       |            |
                    |            v                       v            |
                    |    Threshold Detect         Duty Cycle Adjust   |
                    |   (Object Event >7ms)     (MCPWM Duty Reload)   |
                    +-------------------------------------------------+
```

### 1.1 Hardware interface & pin configuration

- **LED driver pin:** output driving the IR LED via a low-side MOSFET or BJT.
- **Phototransistor input pin:** ADC1 (e.g. `ADC1_CHANNEL_2` / GPIO3).
- **Front-end hardware filter:** passive RC low-pass filter (R = 1.5 kΩ,
  C = 100 nF, fc ≈ 1.06 kHz) between the phototransistor tap and the ADC pin,
  to attenuate high-frequency edges and RF spikes.

### 1.2 Peripheral configuration

**MCPWM (Timer0, Operator0, Generator0):** 20 kHz (50 µs period), initial
duty cycle 25% (adjustable 0.5%–85%). Configure a hardware trigger event at
the center of the active PWM pulse to phase-lock the ADC conversion to the
illumination.

**Continuous ADC with DMA:** ADC1, driven via GDMA/I2S DIG ADC controller
linked to the hardware timer at an effective 4 kHz sample rate, into a
circular DMA buffer (64-sample frames).

### 1.3 Signal processing pipeline

Runs on every incoming DMA buffer frame inside a dedicated, high-priority
FreeRTOS task.

1. **250 Hz/harmonic noise rejection** — 16-tap FIR moving average,
   implemented as an O(1) recursive average (`y[n] = y[n-1] + (x[n]-x[n-16])>>4`)
   so filter length doesn't cost extra per-sample time. Group delay 1.875 ms
   at 4 kHz — subtract this fixed offset from logged event times.
2. **Error signal** — `E[n] = y[n] - V_target` (target ≈ 35% of full scale,
   ~1433 counts at 12-bit).
3. **Fast path (object detection):** trigger when `|E[n]| > V_thresh` for
   ≥8 consecutive samples (~2 ms, to validate a 7 ms event), plus a
   derivative condition (`D[n] = y[n] - y[n-2]`) for leading-edge detection.
   Qualify events sustained for 2–100 ms.
4. **Slow path (closed-loop feedback):** first-order IIR low-pass on `E[n]`
   (α ≈ 0.00313 for fc ≈ 2 Hz at 4 kHz), driving a duty-cycle integrator
   (`Duty[k] = Duty[k-1] - Kp * FilteredError[n]`), soft-clamped to 1.0%–80.0%.

### 1.4 FreeRTOS task architecture (as originally proposed)

- **Optical engine task** — pinned to **Core 1**, `configMAX_PRIORITIES - 2`.
  Blocks on the ADC DMA event queue; on wake, runs the FIR filter, computes
  `E[n]`, runs the 2 Hz IIR control loop, evaluates trigger conditions, and
  dispatches an event on detection.
- **Application/communication tasks** — pinned to **Core 0** (Wi-Fi/BT,
  logging, UI, higher-level state machines), to keep the optical task free
  of interrupt/scheduling jitter.

(§2.2 below covers why this Core 1 placement doesn't fit hesperus's actual
architecture.)

### 1.5 Verification & edge-case requirements

1. The 2 Hz IIR cutoff means any transient under 50 ms (including the 7 ms
   minimum object passage) causes negligible duty-cycle shift, leaving the
   spike fully visible in `E[n]`.
2. ADC DMA queue needs ≥4 frame descriptors to avoid overflow during
   transient system interrupts.
3. Fixed-point/raw integer arithmetic for the FIR/IIR filters in the
   high-frequency ISR/DMA callback path.
4. Saturation guard: if `y[n]` pins at max ADC count even at minimum LED
   duty cycle, flag `AMBIENT_SATURATION_ERROR` and disable auto-tuning
   until light levels drop.

### 1.6 Latency and accuracy (as specified)

- **Detection latency: 2.25–2.5 ms** — RC filter (~0.15 ms) + FIR group
  delay (0.375 ms) + 8-sample threshold verification window (2.0 ms).
- **Timing jitter: ±0.125–0.25 ms** — ±0.25 ms from the 4 kHz sampling
  resolution, halved to ±0.125 ms by derivative-based edge interpolation
  between the two samples straddling the threshold crossing. Software
  jitter is negligible (<1 µs) since ADC conversions are hardware-DMA
  triggered.
- Against a 7 ms dwell time, a 2.5 ms latency flags the event roughly 35%
  of the way through the beam — comfortable margin before the object clears.

## 2. Feasibility assessment

### 2.1 Optical/mechanical

Reflective single-side sensing is a different physical sensing modality
from the current transmissive break-beam design (emitter and receiver on
opposite sides of the gap) — it needs a reflective baseline on the far side
and a different mechanical mount, not just a firmware change. Confirm this
is an intentional requirement (e.g. a mounting constraint) before pursuing
it further, since the proposal itself doesn't state a motivation.

### 2.2 Architecture: where does the DSP task run?

The spec's Core 1 placement for the optical engine task doesn't fit
hesperus's actual firmware architecture: Core 1 already carries the app's
highest-priority task (`TsfCaptureTask`, which does the ISR-unsafe
`esp_wifi_get_tsf_time()` read the cross-gate sync depends on) plus
`WsPump`/`UploadWorker`/`loopTask`. Dropping a new near-max-priority
DMA-driven task there risks contending with `TsfCaptureTask` and
reintroducing the class of Interrupt-WDT panic `main.cpp` was once changed
to avoid. Two architectures resolve this:

**Option A — single-chip (ESP32-S3), DSP task on Core 0 instead of Core 1.**
De-risked by measurement (§2.5): a 1000-simulated-run load trial
(`gather-stats-core0` branch, `test-data/load-tests/`, both gate boards,
~5044 s, Wi-Fi connected and WS session active throughout), profiled with
an `esp_register_freertos_idle_hook_for_cpu` idle-hit counter, found Core 0
idle-hit counts *higher* than Core 1's in both trials (goal board: mean
5312 vs 5165; start board: mean 5311 vs 5170 — ratio ~1.03), with tight
variance and no starvation dips. The trial itself was clean — no
panics/watchdog resets, every WS send acked on the first attempt. This
substantially de-risks placing the DSP task on Core 0, though see the
caveat in §2.5 before relying on these numbers today.

The remaining open question for Option A is whether both channels' ADC
sampling can share ADC1 alone: two channels at 4 kHz each is only ~8 kHz
aggregate, a small fraction of what ADC1's continuous DMA mode should
sustain, which likely removes any need for ADC2 (and ADC2's known conflict
with active Wi-Fi on this chip family). Worth confirming ADC1's actual max
aggregate continuous rate against the TRM before relying on it.

**Option B — secondary dedicated processor**, driving hesperus's existing
active-low sensor-pin interface with a clean pulse, so the ISR →
`triggerCaptureQueue` → `TsfCaptureTask` → `networkQueue` path doesn't
change at all. This removes the Core 0/1 contention question entirely (no
DSP work runs on the Wi-Fi/TSF chip) and leaves the already-validated sync
accuracy (2.36 µs mean / 55 µs max gap) and the ISR-safety fix completely
undisturbed — zero regression risk to the hardest part of the system to get
right. Trade-offs: extra BOM cost/board complexity per gate, inter-board
wiring/power, a second firmware codebase.

An ATtiny1604 (or similar tinyAVR 1-series part) is a comfortable fit for
this role, for reasons specific to how the algorithm is structured:

- The 16-tap FIR is already an O(1) recursive average — one subtract, one
  shift, one add per sample, plus a 32-byte delay line (trivial against
  2 KB SRAM).
- The 2 Hz IIR's fractional multiply (α ≈ 0.00313) can be implemented as
  `y += (x - y) >> k` — an integer shift, sidestepping the question of
  whether the core has hardware `MUL`.
- Threshold/derivative detection is pure compare-and-branch, no multiply.
- At 20 MHz and a 4 kHz per-channel sample rate, roughly 5000 cycles are
  available per sample (≈2500 if one chip time-multiplexes two channels at
  4 kHz each); the actual per-sample workload is on the order of tens of
  cycles — not a close call.
- PWM (TCA0/TCB0) and ADC are both hardware peripherals, so duty-cycle
  updates and sampling don't consume CPU cycles beyond issuing the
  read/write.

Two details worth confirming against the datasheet before committing, not
because either looks like a problem: the ATtiny's ADC is 10-bit rather than
the spec's assumed 12-bit (rescales cleanly — 35% of 1023 instead of 4095 —
but halves raw threshold-margin resolution), and the part's peak achievable
ADC sample rate (tinyAVR 1-series parts are generally well over the
~4–8 kHz aggregate this needs, but not confirmed against the exact figure).

### 2.3 Two channels per gate

Hesperus boards run two independent sensor channels per gate (ARM/START on
a start board, or both to GOAL on a goal board). Reflective detection would
need to replicate the whole emitter/detector/control-loop setup per
channel, which compounds §2.2's concerns rather than adding an unrelated
one: two independent closed-loop DSP pipelines roughly double the
per-sample compute wherever they run, strengthening the case for Option B
(a secondary processor scales by sizing that chip appropriately, without
touching hesperus's Wi-Fi/TSF chip at all).

Optical crosstalk between two reflective pairs mounted close together at
one gate was raised as a candidate new failure mode (no analog in the
current transmissive design) — **ruled out per the existing optical
design**, not a factor in the assessment above.

### 2.4 Latency requirement

Not actually a hard constraint: race timing is gate-to-gate relative, so
absolute detection latency only matters if it differs between gates. The
current microsecond-level sync accuracy is a deliberately stringent design
target, not a hard requirement — sub-millisecond is acceptable in practice.
This significantly de-risks the proposal's ~2.5 ms pipeline latency,
provided it's consistent gate-to-gate.

### 2.5 Caveat: the Core 0 measurement predates the current detector

The Core 0 headroom measurement in §2.2 was taken against the firmware *as
it stood before* the dual-EMA detector existed — Core 0 carried no
app-level task at all at that time. That's no longer true:
`beam-sensor.h`'s `beamSampleTask` now runs pinned to Core 0
(`xTaskCreatePinnedToCore(beamSampleTask, ..., 0)` in `main.cpp`), woken by
a 1 kHz hardware timer. If Option A is ever revisited, **re-profile Core 0
headroom rather than relying on the numbers above** — they no longer
reflect an empty Core 0.

## 3. Recommendation

Both architectures are plausible; Option B (secondary processor) is the
lower-risk one against hesperus's existing, already-validated architecture,
since it requires zero changes to the ISR/timestamp/sync path that took
real effort to get right. Option A (single-chip) is viable too, but needs
Core 0 headroom re-measured on current firmware (§2.5) and ADC1's max
aggregate continuous rate confirmed against the TRM before the task/core
placement question can be answered with confidence.

This is a feasibility read, not an implementation plan. The next step, if
pursued, would be deciding between the two architectures (or scoping
further investigation, e.g. re-profiling Core 0) before any code is written.

## Appendix: per-core responsibility breakdown (at time of this assessment)

For reference, what the assessment above was reasoning about — this
describes the firmware *before* `beam-sensor.h` existed and is not a
current-state description; see `detection-mechanism.md` for that.

**Core 1 — everything the app explicitly created:**
- `TsfCapture` (priority 3, highest in the app) — consumed
  `triggerCaptureQueue` from the ISRs, did the ISR-unsafe
  `esp_wifi_get_tsf_time()` read, assembled the `GateEvent`, pushed to
  `networkQueue`. The task whose scheduling latency the cross-gate sync
  accuracy depends on.
- `WsPump` (priority 2) — sole owner of `wsClient.loop()`.
- `UploadWorker` (priority 2) — consumed `networkQueue`, ran the HTTP/WS
  ack-wait/retry dispatch logic.
- `LED_Task` (priority 1) — consumed `ledQueue`, drove the NeoPixel status
  feedback.
- Arduino's own `loopTask` — also Core 1 by the Arduino-ESP32 default
  (moderate confidence; no explicit `ARDUINO_RUNNING_CORE`/unicore override
  found to confirm this rather than infer it).

**Core 0 — nothing app-level** at the time of this assessment. Presumed to
carry ESP-IDF's own Wi-Fi/lwIP (and possibly BT) housekeeping tasks per
Arduino-ESP32/IDF defaults (inferred from general platform behavior, not an
explicit project setting) — this is exactly what the idle-hit profiling in
§2.2 measured. It now also carries `beamSampleTask` (§2.5).
