# Feasibility Assessment: Synchronous Reflective Detection for Hesperus

Assessment of `synchronous-reflective-detection.md` against the current firmware. This is a feasibility read, not an implementation plan.

## What hesperus currently does

- **Transmissive break-beam, not reflective.** IR LED and phototransistor sit on
  opposite sides of the gap; the object interrupts the beam. Confirmed in
  `README.md:35`, `docs/TEST-TOOLING.md:24`, and `cerberus-gate-controller/docs/RACE-STATE-MACHINE.md:4`.
- **Purely digital.** `main.cpp:834-837` — `attachInterrupt(..., CHANGE)` on
  `GATE_PIN_A`/`GATE_PIN_B`. No ADC, no PWM, no MCPWM, no DMA anywhere in this
  firmware today (grepped `src/`, `platformio.ini`, `boards.ini` — zero hits).
  This part of the proposal is a genuinely blank slate, not a rework.
- **Detection latency today is effectively hardware-interrupt speed** (sub-µs to
  low-µs, dominated by the sensor module's own comparator response), gated by a
  50 ms software debounce (`DEBOUNCE_US`, `main.cpp:139`) that exists purely to
  reject electrical/mechanical bounce, not to filter a noisy analog signal. There
  is no ambient-light calibration problem today because the digital comparator
  already handles that in hardware.
- **All FreeRTOS tasks are pinned to Core 1**, including the app's own
  highest-priority task, `TsfCaptureTask` (prio 3, `main.cpp:883-890`), which
  does the ISR-unsafe `esp_wifi_get_tsf_time()` read that the whole cross-gate
  sync scheme depends on. Core 0 is presumably running bare IDF Wi-Fi/lwIP
  internals (Arduino-esp32 default), not something the app currently uses
  directly.
- The dual-timestamp sync (`tsf_observed` + `processor_clock`, EMA-disciplined
  via `clock_alpha`) is already validated at high accuracy (`review.md:65`:
  mean gap 2.36 µs / max 55 µs over 60,000 events) and is orthogonal to how the
  optical trigger itself is generated — that part is unaffected either way.

## Feasibility of the proposal as written

**Mechanically/optically: different sensing modality, not a drop-in swap.**
Reflective single-side sensing requires a reflective baseline on the opposite
side of the gap and a different mechanical mount than the current two-post
transmissive layout. This is a hardware redesign, not a firmware-only change —
worth confirming this is intentional (e.g. a mounting constraint driving the
switch) before going further, since the doc doesn't state the motivation.

**Peripherals (ADC1-DMA + MCPWM): feasible, no conflicts.** Nothing today
claims these peripherals. Continuous ADC/DMA and MCPWM are ESP-IDF APIs; this
codebase already reaches into ESP-IDF directly (`esp_timer_get_time()`,
`esp_wifi_get_tsf_time()`) from Arduino framework code, so the mixed-API
pattern is consistent with existing practice.

**Task/core architecture: real conflict, not addressed by the doc.** The spec
assumes Core 0 is free for comms/state-machine work and wants a new
near-max-priority (`configMAX_PRIORITIES-2`) task pinned to **Core 1** for
"zero interrupt/scheduling jitter." The actual firmware is the inverse: Core 1
is already carrying the app's highest-priority task plus Wi-Fi-heavy
`WsPump`/`UploadWorker`/`loopTask`. Dropping a new near-max-priority DMA-driven
task onto Core 1 would contend directly with `TsfCaptureTask` for CPU and risks
introducing jitter into the exact timestamp-capture path the sync accuracy
above depends on — plausibly reintroducing the class of Interrupt-WDT panic
that `main.cpp:423-429` was already changed once to avoid. The proposal would
need to either move to Core 0 (unverified whether that's actually free enough)
or the existing task placement would need to be revisited. This is the single
biggest architectural risk in adopting the doc as written.

**Latency model: internally consistent but not comparable to today's
behavior.** The doc's own ~2.25-2.5 ms pipeline latency (with a compensating
fixed-offset subtraction) is a new source of *systematic* delay that doesn't
exist today — today's break-beam ISR timestamps at effectively zero added
latency. As a constant, well-characterized offset it shouldn't break the
existing cross-gate sync (which cares about *disciplined* clock alignment, not
absolute latency), but it's a new class of thing to validate, and there's no
existing "beam-break to GPIO edge" latency figure on record to benchmark
against — none found in the docs or logs, only network/TSF-pairing latency,
which measures something else entirely.

**Complexity: substantial increase.** Today's detection is a single
active-low ISR with a debounce timer — no calibration, no filtering, no
control loop. The proposal adds a tuned closed-loop PWM controller (gain
`Kp`), two cascaded filters (16-tap FIR + 2 Hz IIR), fixed-point arithmetic
constraints in the hot path, saturation-guard logic, and derivative-based edge
interpolation. None of this has an analog in the current codebase to build on
— it would all be new, and all of it needs tuning against real hardware
(reflectivity of target, ambient IR, LED brightness) before it's trustworthy
for race timing.

## Verdict

Feasible at the peripheral level (no conflicts), but not a safe drop-in as
written:
1. It's a different physical sensing modality (reflective vs. transmissive) —
   confirm that's an intentional requirement, not just a technique swap.
2. The proposed task/core placement directly contradicts this firmware's
   actual architecture and threatens the already-validated TSF timestamp path
   — this needs to be resolved before any implementation, not discovered
   during it.
3. It trades a very simple, already-reliable detection path for a materially
   more complex one, with real tuning/validation effort before it can be
   trusted for timing accuracy comparable to today's.

## Open questions — answered

1. **Motivation.** The active-low digital trigger was always a simplified
   starting point for optimistic early development; analog/software detection
   was anticipated as a likely eventual need, and has precedent ("benefits
   ... indicated for in other legacy systems"). A plain active-low trigger is
   not ruled out as the long-term interface — it could remain the interface
   hesperus's ISR sees even if the signal processing behind it changes.
2. **Latency.** Not a real constraint. Race timing is gate-to-gate relative,
   so absolute detection latency only matters if it differs between gates;
   the current microsecond-level accuracy is a deliberately stringent
   design target, not a hard requirement — sub-millisecond is acceptable in
   practice. This significantly de-risks the reflective method's ~2.5 ms
   pipeline latency, provided it's consistent gate-to-gate.
3. **Core loading.** Unknown, needs measuring rather than assuming. The
   proposal's Core 1 placement was a mistake by an author who didn't know
   hesperus's actual task architecture, not a considered choice — so this
   isn't a reason to reject the approach, just a parameter to fix once Core 0
   headroom (Wi-Fi/lwIP internals) is actually profiled.

## Revised assessment: a fourth option worth weighing

The reply raised a possibility not covered above: **do the analog signal
processing on a second, dedicated processor**, and have it drive
`GATE_PIN_A`/`GATE_PIN_B` with a clean active-low pulse — i.e. hesperus's
existing ISR → `triggerCaptureQueue` → `TsfCaptureTask` → `networkQueue` path
does not change at all.

This is attractive specifically because of what the exploration above found:
- The Core 0/Core 1 contention risk disappears entirely — no new task
  competes with `TsfCaptureTask` for CPU on the board that owns the Wi-Fi/TSF
  timing, since none of the DSP work runs on that chip.
- The already-validated 2.36 µs / 55 µs max sync accuracy and the
  ISR-context-safety fix (`main.cpp:423-429`, the 2026-08-03 panic) are both
  left completely undisturbed — zero regression risk to the part of the
  system that's hardest to get right.
- The closed-loop DSP (FIR/IIR tuning, `Kp` gain, saturation guard) becomes
  isolated on a chip with no Wi-Fi/RTOS-networking obligations, which is
  arguably a better fit for a tight, latency-sensitive control loop anyway.
- It matches the "active-low trigger, possibly software-derived" interface
  the reply says was anticipated from the start.

Trade-offs: extra BOM cost and board complexity per gate (second MCU),
inter-board wiring/power, and a second firmware codebase to build and
maintain. Given the sub-millisecond latency tolerance confirmed above, even a
modest, inexpensive MCU should be more than adequate for the DSP pipeline in
the original spec.

## Updated verdict

Both the original single-chip approach (with corrected Core 0 placement,
pending profiling) and the secondary-processor split are now plausible paths
— the earlier objections (core contention, latency budget) are substantially
weakened by the answers above. The secondary-processor split is the lower-risk
option against hesperus's existing, already-validated architecture, since it
requires zero changes to the ISR/timestamp/sync path that took real effort to
get right. The single-chip approach is viable too, but first needs Core 0
headroom measured on real hardware before the task/core placement question
can be answered with confidence.

This is still not an implementation plan — just an updated feasibility read.
Next step, if you want it, would be to decide between the two architectures
(or explicitly scope further investigation, e.g. profiling Core 0 load) before
any code gets written.

## Second consideration: two channels per gate

Hesperus boards already run two independent sensor circuits per gate
(`GATE_PIN_A`/`GATE_PIN_B` — mapped per `board-role.h` to ARM/START on a start
board, or both to GOAL with no lane distinction on a goal board). Reflective
detection would need to replicate the whole emitter/detector/control-loop
setup per channel, not once per gate. This compounds the earlier concerns
rather than introducing an unrelated new one:

- **Resource sharing on one chip gets tighter.** Two independent closed-loop
  DSP pipelines (2x FIR/IIR filtering, 2x threshold detection, 2x PWM duty
  control) roughly double the per-sample compute in whatever task runs them.
  If both channels share a single ADC1 unit's continuous-DMA conversion group
  (round-robin across channels), the effective sample rate *per channel* drops
  unless the aggregate rate is raised to compensate — I don't have a reliable
  figure for sustained achievable throughput here and would want to check the
  datasheet/measure on hardware rather than guess. Using the ESP32-S3's second
  ADC unit (ADC2) to give each channel its own unit is the obvious fix, but
  ADC2 has a long-standing conflict with active Wi-Fi on this chip family that
  needs verifying before relying on it — flagging as uncertain, not confirmed
  either way for the S3. This is a second, independent point in favor of
  keeping this work off the Wi-Fi-carrying chip.
- **New failure mode: optical crosstalk.** Two reflective pairs mounted close
  together at one gate (e.g. two lanes) each modulate their own IR LED and
  read a phototransistor — one channel's detector can pick up the other
  channel's emitter, especially if reflectors are shared or close, or if the
  channels aren't distinguished (different PWM carrier frequency, or
  time-interleaved sampling windows so only one LED is ever lit while its
  neighbor samples). This has no analog in the current transmissive design
  and would need explicit handling, not just doubling the single-channel
  pipeline.
- **Strengthens the case for a secondary processor.** A dedicated companion
  MCU (potentially one per gate handling both channels, if it has two
  independent ADC/PWM resources) absorbs both the doubled DSP load and the
  crosstalk-avoidance logic without touching hesperus's Wi-Fi/TSF-carrying
  chip at all. Doubling the channel count makes the single-chip option
  correspondingly more crowded, while the secondary-processor option scales
  by just sizing that second chip appropriately — the risk stays off hesperus
  either way.

*(Update: crosstalk is not a concern per the existing optical design — noted,
not a factor in the assessment below.)*

## Would an ATtiny1604 have enough processing power?

Yes — the algorithm in the spec is well within an 8-bit AVR's capability at
these sample rates, for reasons specific to how it's structured, not just "it's
a small job":

- **The 16-tap FIR is already specified as an O(1) recursive average**
  (`y[n] = y[n-1] + (x[n]-x[n-16])>>4`) — one subtract, one shift, one add per
  sample, plus a 16-entry delay line (32 bytes for 16-bit samples — trivial
  against 2 KB of SRAM).
- **The 2 Hz IIR feedback filter is the one place needing a fractional
  multiply** (`alpha ≈ 0.00313`) — but single-pole IIRs of this kind are
  routinely implemented as `y += (x - y) >> k` with an integer shift instead
  of a true multiply-accumulate, which sidesteps the question of whether this
  part's AVR core includes hardware `MUL` (I'm not confident enough in that
  detail off the top of my head to state it either way — flagging as
  something to confirm in the datasheet — but it doesn't end up mattering
  because the shift-based form avoids needing the answer).
- **Threshold/derivative detection is pure compare-and-branch** — no
  multiply anywhere.
- **Budget check:** at a 20 MHz core clock and a 4 kHz per-channel sample
  rate, that's roughly 5000 cycles available per sample for one channel (or
  ~2500 if one chip time-multiplexes two channels at 4 kHz each). The whole
  per-sample workload above is on the order of tens of cycles. This isn't a
  close call — there's substantial headroom either way.
- **PWM and ADC are both hardware peripherals on this part** (TCA0, usable in
  split mode for multiple independent PWM outputs, plus TCB0; a single SAR
  ADC multiplexed across input channels) — duty-cycle updates and sampling
  don't consume CPU cycles beyond issuing the read/write.

Two things worth confirming against the actual datasheet before committing,
not because I expect them to be a problem but because I don't want to assert
numbers I'm not certain of:
1. **ADC resolution is 10-bit on this part's ADC, not the 12-bit the original
   spec assumes** (its `Vtarget ≈ 1433/4095` counts). This rescales cleanly
   (35% of 1023 instead of 4095) but halves the raw count resolution
   available for threshold margins — likely fine given the optical design
   headroom you mentioned, but worth a sanity check once real signal levels
   are known.
2. **Peak achievable ADC sample rate** — tinyAVR 1-series parts are generally
   capable of well over the ~4-8 kHz aggregate this design needs, but I'd
   want to check the exact figure in the datasheet rather than quote one from
   memory.

Net: processing power is not the constraint here. If you go the
secondary-processor route, an ATtiny1604 (or similar tinyAVR 1-series part)
looks like a very comfortable fit for one gate's worth of channels (one or
two), with margin to spare.

## Bottom line: single-chip (ESP32-S3) path

With crosstalk ruled out and processing power confirmed comfortable on the
secondary-processor alternative, the single-chip path's remaining open
questions are narrower than they first looked, and both are ESP32-S3-specific
unknowns rather than firm objections:

1. **Core 0 headroom is unmeasured.** Unknown how much margin exists once the
   Wi-Fi/lwIP internals ESP-IDF places there are accounted for — needs
   profiling on real hardware before the new task's placement can be decided.
   This is a scheduling-contention question, not a raw-throughput one: an
   ESP32-S3 core has far more headroom than an ATtiny1604, and the DSP
   pipeline was already shown comfortably feasible on that much smaller part
   (see above), so compute capacity was never actually in doubt on this chip
   — the open question is whether a new real-time task disrupts the
   *timing* of Wi-Fi/lwIP work on Core 0 (or, if placed on Core 1 instead,
   the *timing* of `TsfCaptureTask`), not whether the S3 has cycles to spare.
   **How to measure:** enable FreeRTOS runtime stats
   (`configGENERATE_RUN_TIME_STATS` + `configUSE_TRACE_FACILITY`) and read
   `vTaskGetRunTimeStats()`/`uxTaskGetSystemState()` — this reports per-core
   `IDLE0`/`IDLE1` percentages, the standard way to see actual headroom.
   Should be measured under real load (Wi-Fi connected, WS session active,
   events flowing), not at idle — this project's own `wsClient.loop()`
   blocking-under-congestion issue (`NETWORK-TIMING-LOG.md`) already shows
   Wi-Fi activity causes measurable stalls elsewhere in this firmware, so
   idle-bench headroom on Core 0 wouldn't necessarily reflect race
   conditions.
2. **ADC2-vs-Wi-Fi likely moot.** ADC1 has multiple input channels and
   continuous/DMA mode can round-robin sample a pattern of channels within
   one ADC unit — two channels at 4 kHz each is only ~8 kHz aggregate, a
   small fraction of what ADC1's continuous mode should be able to sustain.
   This likely means both channels fit on ADC1 alone, removing the
   ADC2/Wi-Fi conflict question from consideration entirely. Worth
   confirming ADC1's actual max aggregate continuous rate against the TRM
   once real numbers matter, but this is no longer a first-order concern.

Point 1 is resolvable by measurement, not a structural blocker. Neither point
applies to the secondary-processor path, which stays off the Wi-Fi-carrying
chip entirely.

## Current per-core responsibility breakdown

For reference, what's actually running where today (`main.cpp:883-890`):

**Core 1 — everything the app explicitly creates:**
- `TsfCapture` (`tsfCaptureTask`, priority 3 — highest in the app) — consumes
  `triggerCaptureQueue` from the ISRs, does the ISR-unsafe
  `esp_wifi_get_tsf_time()` read, assembles the `GateEvent`, pushes to
  `networkQueue`. This is the task whose scheduling latency the cross-gate
  sync accuracy depends on.
- `WsPump` (`wsPumpTask`, priority 2) — sole owner of `wsClient.loop()`.
- `UploadWorker` (`uploadWorkerTask`, priority 2) — consumes `networkQueue`,
  runs the HTTP/WS ack-wait/retry dispatch logic.
- `LED_Task` (`ledDiagnosticTask`, priority 1) — consumes `ledQueue`, drives
  the NeoPixel status feedback.
- Arduino's own `loopTask` (the `loop()` state machine, Wi-Fi
  connect/reconnect handling) — also Core 1, by the Arduino-ESP32 default;
  **moderate confidence**, since no explicit `ARDUINO_RUNNING_CORE`/unicore
  override was found in `platformio.ini`/`boards.ini` to confirm this rather
  than infer it.

**Core 0 — nothing app-level.** No task in this codebase is created pinned to
Core 0. It presumably carries ESP-IDF's own Wi-Fi/lwIP (and possibly BT)
housekeeping tasks, per Arduino-ESP32/IDF defaults — **inferred from general
platform behavior, not from an explicit project setting**, and exactly the
thing the runtime-stats measurement above (Core 0 headroom under real load)
would confirm or correct.

Net: Core 1 is already the busy, latency-sensitive core; Core 0 is presumed
lighter but its actual load has never been measured in this project.
