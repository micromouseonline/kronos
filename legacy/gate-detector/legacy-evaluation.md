# Legacy Gate Detector: Detection Mechanism Evaluation

Scope: the occlusion-detection algorithm in `gate-detector/gate-detector.ino`
(`ExpFilter`, `GateSensor`). Not covered here: RF protocol, packet format,
transmission timing.

## Technique

Two exponential moving averages run on every ADC sample (1300 Hz, driven by
TIMER2):

- `fast`: tau = 2 ms. Tracks the raw sensor reading almost instantly.
- `slow`: tau = 1 s. Tracks the ambient baseline (unoccluded light level).

Detection is by ratio, not absolute level:

- `fast < 0.25 * slow` -> beam considered broken (`mInterrupted = true`)
- `fast > 0.75 * slow` -> beam considered clear, re-arm (`mInterrupted = false`)

A recovery step, `slow = max(slow, fast)`, runs every sample once `slow`
is above a floor of 10 counts. This lets `slow` snap back up immediately
once occlusion ends, instead of waiting out its own 1 s time constant.

Below the floor of 10 counts, `slow` is treated as "no signal" (sensor
dark/faulty/misaligned): the built-in LED lights continuously and the
ratio logic is skipped for that sample.

## Detection Latency (Full Occlusion)

Assume a robot fully blocks the beam (input drops to ~0) starting from
steady state (`fast = slow = B`). The fast filter's discrete update gives:

```
fast(n) = B * (1 - alpha_fast)^n,   alpha_fast = 1/(1300 * 0.002) = 0.3846
```

Solving `fast(n) < 0.25 * slow` (with `slow ~ B` over this short a
window):

```
n > ln(0.25) / ln(1 - 0.3846) = 2.86  ->  n = 3 samples
fast(3) = 0.233 * B   (confirms threshold crossed at n = 3)
```

At the 1300 Hz sample rate (769 us/sample):

```
detection time = 3 * 769 us = 2.31 ms   (typical)
worst case      = 4 * 769 us = 3.08 ms  (+1 sample for arrival-phase uncertainty)
```

So detection latency from physical beam-break to `mInterrupted = true`
is about 2.3 ms typical, up to ~3.1 ms worst case, set almost entirely by
the fast filter's 2 ms time constant. `loop()` picks the flag up on its
next pass with negligible added delay (pure polling, no blocking work
when idle). This excludes the separate ~500 us transmitter-stabilization
delay in `send_trigger()` before the sync character is sent.

The same math applies in reverse to re-arming: `fast` rising from ~0
back toward `B` crosses the `0.75 * slow` re-arm threshold after the same
`ln(4)` relationship (continuous approximation: `t = tau_fast * ln(4) =
tau_fast * 1.386`), since 0.25 and 0.75 are complementary about the
threshold. For `tau_fast = 2 ms` that's ~2.77 ms, consistent with the
discrete result above.

## Minimum Detectable Event (Emitter Blink Test)

A useful bench test: instead of physically occluding the beam, drive the
emitter off then back on for a duration `D`, and ask how short `D` can be
and still register. This is a clean instantaneous step in both
directions (no finite object width or approach speed to blur the edge),
so it isolates the firmware/filter latency from any optical or mechanical
factor.

`fast` is causal and decays monotonically for the whole time the emitter
is off, only turning around once light returns. So the minimum value of
`fast` over the whole event always occurs at the instant `t = D` (right
before recovery starts) - which means the pulse is detected if and only
if it lasts long enough for `fast` to reach the 25% threshold on its own.
That is the same trajectory already used for the sustained-occlusion
detection-time calculation:

```
best case (favorable sample alignment):  D_min = 3 * 769 us = 2.31 ms
worst case (any phase, +1 sample margin): D_min = 4 * 769 us = 3.08 ms
```

Below this, `fast` simply never reaches `0.25 * slow` before the emitter
comes back on - the pulse is invisible to the detector, not marginally
registered. This test is a good way to directly measure the real
detection latency against hardware, since it removes robot speed and
optical-path variation from the measurement entirely.

## What works well

**Ratio-based detection is self-normalizing.** Because both filters see
the same optical setup, absolute brightness cancels out of the comparison.
The gate doesn't need a fixed light-level threshold tuned to one
installation; it works across a range of ambient conditions as long as
the fast/slow separation is clean.

**The two time constants are well separated for the stated goal.** `tau_slow
= 1 s` is far longer than any expected occlusion (tens to hundreds of ms
for a robot transit), so slow ambient drift (a light dimming over
seconds, sunlight moving) is tracked by both filters in near lockstep and
never produces a large fast/slow ratio. Ambient drift is rejected by
construction, not by a fixed threshold.

**The recovery clamp handles moderate occlusions cleanly.** For any
occlusion short enough that `slow` stays above the 10-count floor, the
clamp snaps `slow` back to the recovered `fast` value on the very sample
light returns, and re-arm hysteresis fires within a sample or two. No
drawn-out reacquisition.

**Hysteresis prevents chatter.** The gap between the 0.25 and 0.75
thresholds means a signal sitting near either boundary doesn't
oscillate the interrupted/armed state.

**Diagnostics are cheap and already present.** `mDiff` and the `slow <
10` LED both give a visible signal that the sensor isn't seeing usable
contrast, which is useful for the "wave your hand" startup check already
in `setup()`.

## Issues and Analysis

### Long occlusion and the stuck-state recovery gap

During occlusion, `slow` still runs its plain EMA update toward the dark
reading every sample. The recovery clamp (`slow = max(slow, fast)`) does
nothing during this phase, because `fast` has already converged to the
same dark value within a few ms, so the clamp is a no-op. `slow` decays
with `tau = 1 s` and crosses the 10-count floor after roughly:

```
t_threshold = ln(B / 10) seconds
```

where `B` is the unoccluded (bright) ADC reading. For plausible `B` in
the 100-1023 range that's about 2.3 s to 4.6 s of continuous occlusion.
Past that point the ratio logic is skipped entirely (`mDiff` freezes,
armed/interrupted state freezes) until `slow` climbs back above 10, which
takes an extra ~10-100 ms once light actually returns (`t_recover =
-ln(1 - 10/B)` seconds). The detector has no defined behavior for "beam
blocked for several seconds" beyond this fixed, hard-coded floor - it
was tuned for one specific optical budget and isn't self-calibrating.

This isn't a rare edge case. The arm/home sensor on the start-cell board
sits occluded for "several seconds at least" every time a robot dwells
in the start cell before a run - reliably past the 2.3-4.6 s onset
threshold. So on essentially every run, that sensor is in the
frozen/stuck state by the time the robot departs, and takes the extra
~10-100 ms to recover once the beam clears. This has no effect on
measured race time - it only delays the arm sensor's own readiness to
detect a *subsequent* occlusion, not the start sensor's trigger, which
fires independently the moment that separate beam breaks. The value in
fixing it is cleanliness and diagnostic accuracy (the LED-on/frozen
state being normal at the start of every run is misleading), not timing
accuracy.

### Startup stabilization: filters seeded at zero, not ambient

`GateSensor`'s constructor seeds both filters at `value = 0` (the
`ExpFilter::begin()` default), not the actual ambient reading:

```cpp
slow.begin(0.000769);                 // value defaults to 0
fast.begin(1.0 / (1300.0 * 0.002));   // value defaults to 0
```

`fast` (tau = 2 ms) climbs from 0 to the true bright level `B` in a
handful of ms - negligible. `slow` (tau = 1 s) is the problem: it has to
climb from 0 past the 10-count "no signal" floor before the ratio logic
and the recovery clamp engage at all, and until it does, `update()`
takes the early-return branch every sample - `mDiff` stays frozen at 0
and `LED_BUILTIN` stays solid on, indistinguishable from a genuinely
faulty sensor. Time for `slow` to climb from 0 past 10 (`t = -tau *
ln(1 - 10/B)`):

| B (bright reading) | startup delay today |
|---|---|
| 1023 | 9.8 ms |
| 500 | 20.2 ms |
| 300 | 33.9 ms |
| 100 | 105 ms |
| 50 | 223 ms |
| 20 | 693 ms |

On a dim or marginal installation this stretches past half a second,
entirely because the filters start by pretending the sensor read zero
when it actually read `B` the whole time. It also blinds the `setup()`
wave-your-hand handshake for the same duration, since `mDiff` isn't
computed until `slow` clears the floor - an operator waving their hand
right after power-on could be waving into a dead zone.

### No debounce on the detection edge

A single sample where `fast < 0.25 * slow` is enough to declare
interrupted. At 1300 Hz this is fine for genuine occlusion (many
consecutive samples), but it also means any single-sample dropout
(electrical noise, brief specular reflection change, ADC glitch) can
register as an event with no minimum-duration requirement. The filter
itself already rejects true single-sample spikes on its own - one bad
sample can't drag `fast` past the 25% threshold (`alpha_fast = 0.385`
only pulls it to `0.615*B` in one step) - so the real exposure is
multi-sample noise bursts that happen to last close to the inherent
~3-sample detection minimum, not isolated glitches.

### Fixed thresholds, not derived from measured signal

0.25, 0.75, and the 10-count dark floor were tuned for the original
occlusion-based optical setup. Nothing in the code ties them to the
sensor's actual measured bright/dark spread, so they carry over unchanged
regardless of the physical sensor in use. Since the reflective design
the team is considering next will have different contrast and noise
characteristics, these numbers should be treated as needing
re-validation, not assumed correct.

### LED illumination margin: contrast and saturation

Earlier reasoning (in this document and in discussion) assumed a robot
fully occluding the beam drives the reading down to near zero. That's
only true if the phototransistor's signal is dominated by the LED. If
ambient light also reaches it by some other path (direct exposure,
reflection, a wide acceptance angle on the package), full occlusion only
removes the LED's contribution, not the ambient one - the reading drops
to `A` (ambient alone), not to zero.

Let `L` be the LED's own contribution to the reading and `A` the ambient
contribution, with `B_total = A + L` the normal unoccluded reading
(additive, linear-region assumption; occluded reading ~= `A`, since
occlusion removes the LED's path but not the ambient one).

For a full occlusion to still cross the 0.25 detection threshold at all,
the occluded reading must be below 25% of the unoccluded one:

```
A          <  0.25 * B_total          (detection condition)
A          <  0.25 * (A + L)          (substitute B_total = A + L)
A          <  0.25A + 0.25L           (expand)
A - 0.25A  <  0.25L                   (move the A term over)
0.75A      <  0.25L
A          <  L / 3                   (divide both sides by 0.75)
```

So ambient has to stay under a third of the LED's own contribution. In
terms of the total reading: since `A < L/3` means `L > 3A`,

```
B_total = A + L  >  A + 3A  =  4A
```

`B_total > 4A` is the boundary case - an occluded reading landing
exactly on the detection line, with zero margin. For a safer target,
`B_total = 6A` gives an occluded/baseline ratio of `A / 6A = 1/6 ~= 0.17`,
comfortably under the 0.25 threshold rather than sitting right on it.

This failure mode is invisible to the existing diagnostics: `slow` stays
well above the 10-count floor (the "no signal" LED never lights), and
`mDiff` shows a real, nonzero value (the signal genuinely moves on
occlusion, just not far enough) - everything looks nominal while a
fully-occluding robot silently fails to trigger. It also limits what the
adaptive-threshold suggestion below can achieve: calibrating against a
measured bright/dark spread only helps if a usable spread exists - no
threshold choice recovers a contrast gap that isn't physically there.

**Saturation compounds the same problem from the other direction.** If
combined illumination (ambient + LED) is high enough to saturate the
phototransistor or pin the ADC near 1023, `B_total` stops being
proportional to actual light. Since occlusion is judged relative to that
ceiling, headroom matters: a design with no margin below saturation
leaves no room for ambient to increase (a brighter venue, a different
time of day) before the sensor reads a clipped, non-representative
value.

**Combined design target.** Balancing both constraints:

```
B_total >= 6 * A              (comfortable occlusion-detection margin)
B_total <= 0.80 * 1023 = ~818 (saturation headroom)
```

Together these imply a ceiling on the ambient level the design can
tolerate at all: `A <= 818 / 6 = ~136 counts` (~13% FSD). Past that, no
amount of extra LED brightness satisfies both constraints simultaneously
- the only lever left is reducing how much ambient reaches the
phototransistor directly (shielding, a narrower acceptance angle, an
optical filter matched to the LED's wavelength).

**Where the current hardware sits.** Per the project documentation, LED
illumination is adjusted to `L ~ 60` counts. Plugging that into the two
targets above:

```
boundary (4x):    A < L / 3  =  60 / 3  =  20 counts
safe margin (6x): A + L >= 6A  ->  L >= 5A  ->  A <= L / 5  =  60 / 5  =  12 counts
```

So even the bare boundary case - zero margin, an occluded reading
sitting exactly on the detection line - only tolerates ambient up to 20
counts, and `B_total` itself is just 60-80 counts (6-8% of FSD) even
before considering ambient. This is a genuine under-illumination
problem: there is essentially no margin today in either direction, and
it doesn't take much ambient light to push a fully-occluded reading back
above the detection threshold with no diagnostic indication that
anything is wrong. This is a strong candidate for real-world
missed-occlusion incidents, particularly because it directly explains
cases where the beam was confirmed fully occluded and the event still
wasn't registered - that observation rules out timing/debounce/geometry
causes, but is exactly consistent with a contrast margin that was too
thin to begin with.

### Delay vs jitter across two gates

Several of the fixes below add processing latency ahead of or within the
detection filter. That only matters if it differs between the two gates
- a delay that's identical at both gates shifts both timestamps by the
same amount and cancels out of a start-to-goal elapsed-time measurement.
It only matters as an uncorrected constant if a gate's timestamp is ever
used on its own (e.g. an absolute WiFi-TSF-referenced event time), where
it would need to be calibrated out once.

Two things determine whether "identical at both gates" actually holds:

1. **Crossing speed.** The edge-rate ("walk") component of any filter's
   delay depends on how fast the occluding object crosses the beam. If
   speed is the same at both gates for a given run, this term cancels
   along with the rest of the fixed delay. If it genuinely differs (e.g.
   decelerating into the goal), the walk term differs too and stops
   being purely common-mode.

2. **Sample-clock quantization - the one piece that never cancels.** Each
   gate's ADC samples on its own free-running clock; the physical
   crossing instant lands at a random, independent phase within each
   gate's 769 us sampling interval. Modeled as uniform over one sample
   period, the standard deviation contributed per gate is
   `T / sqrt(12) = 222 us`. The two gates' quantization errors are
   independent, so they add in quadrature:

```
combined std dev = T * sqrt(1/6) = 769 us * 0.408 = ~314 us
```

So, under the equal-speed assumption, sample-clock quantization is the
dominant, irreducible source of interval jitter at roughly +-300 us
(1 sigma) - unaffected by any of the latency trade-offs discussed below,
since none of them change the sample rate. This ~314 us figure is used
below as the benchmark for whether a given fix's own gate-to-gate
variability actually matters.

### Ambient interference: mains flicker and kHz-range LED PWM

`tau_fast = 2 ms` gives a corner of `fc = 1/(2*pi*0.002) = 79.6 Hz`.
Mains-frequency lighting flicker (100/120 Hz) sits close enough to leak
through substantially:

```
|H(100Hz)| = 1 / sqrt(1 + (100/79.6)^2) = 0.62   (~ -4 dB)
```

About 62% of flicker amplitude still reaches the ratio test - weak
rejection, and it's the same filter that does the detection, so a
digital fix has a direct cost. Two digital options were considered:

- **Widen `tau_fast`.** Getting to -20 dB (10x) at 100 Hz needs
  `fc ~ 10 Hz`, i.e. `tau_fast ~ 15.9 ms` - about 8x the current value.
  Detection and re-arm both scale linearly with `tau_fast`, so this means
  ~18-25 ms detection latency instead of ~2.3-3.1 ms.
- **A matched 13-sample moving-average notch** (100 Hz has an exact
  13-sample period at 1300 Hz, nulling it and its harmonics up to
  Nyquist). Fixed group delay of `(N-1)/2 = 6` samples = 4.6 ms, giving
  combined detection latency of ~6.9 ms - a third of the first option's
  cost.

Per the Delay vs Jitter analysis above, the latency cost of either isn't
actually the deciding factor (it's common-mode). What rules both out is
a different problem: modern LED luminaires typically PWM-dim well above
the old mains-flicker range - 500 Hz to several kHz, with no fixed
relationship between fixtures. The sample rate here is `Fs = 1300 Hz`,
so Nyquist is `650 Hz`. Anything above that doesn't reach the filters as
itself - sampling folds it down first:

```
alias(f) = |f mod Fs|, folded around Fs/2
```

A 1 kHz interferer folds to `1300 - 1000 = 300 Hz`, at full amplitude
(aliasing doesn't attenuate, it just changes the apparent frequency).
Its harmonics scatter unpredictably:

| harmonic | true freq | aliases to |
|---|---|---|
| 1st | 1000 Hz | 300 Hz |
| 2nd | 2000 Hz | 600 Hz |
| 3rd | 3000 Hz | 400 Hz |
| 4th | 4000 Hz | 100 Hz |
| 5th | 5000 Hz | 200 Hz |
| 6th | 6000 Hz | 500 Hz |

The 4th harmonic lands exactly back on 100 Hz, the filter's weakest
point. Multiple unsynchronized luminaires make this worse, not just
additive - each aliases independently to its own unpredictable
frequency, producing an incoherent noise floor across 0-650 Hz rather
than one interferer. This is why neither digital option is a real fix:
the notch is built around one known, sub-Nyquist frequency and operates
on already-sampled data, so it can't undo aliasing or retarget a moving,
multi-source interferer; widening `tau_fast` helps marginally everywhere
but doesn't address the root cause either. Raising the sample rate would
help, but only as far as it goes (harmonics keep climbing), and the ISR
is already close to its timing budget within the 769 us period, so
there's limited headroom on this hardware anyway.

The only thing that actually addresses the root cause is filtering
before the ADC samples - an analog low-pass that stops energy above
Nyquist from folding down in the first place, regardless of the
interferer's real frequency or how many uncorrelated sources are
present.

### Anti-alias capacitor: sizing and expected benefit

The detector circuit is a phototransistor (collector to V+) with a 2200
ohm resistor from emitter to ground, feeding the ADC from that junction.
Treating the phototransistor as a high-impedance current source, the
2200 ohm resistor is the only significant impedance in play, so a
capacitor added across it forms a single-pole RC low-pass ahead of the
ADC:

```
tau_a = R * C = 2200 * C
fc = 1 / (2*pi*R*C)
```

Because it sits before the sampler, it attenuates the true interferer
frequency before it can fold down - unlike anything digital. It does add
its own group delay ahead of the digital `fast` filter (via the exact
two-pole cascade step response, `g(t) = [tau1*e^(-t/tau1) -
tau2*e^(-t/tau2)] / (tau1-tau2)`, solved against the 2.77 ms baseline):

| C | tau_a | fc | detection time | added latency |
|---|---|---|---|---|
| 100 nF | 0.22 ms | 724 Hz | ~3.0 ms | +0.25 ms |
| 220 nF | 0.48 ms | 329 Hz | ~3.4 ms | +0.6 ms |
| 330 nF | 0.73 ms | 219 Hz | ~3.65 ms | +0.9 ms |
| 470 nF | 1.03 ms | 154 Hz | ~4.0 ms | +1.25 ms |
| 680 nF | 1.50 ms | 106 Hz | ~4.7 ms | +1.95 ms |

Per the Delay vs Jitter analysis, none of this added latency matters on
its own - identical hardware at both gates makes it a fixed, common-mode
delay that cancels in the start-to-goal interval, the same as the
digital filter's own latency. That removes the pressure to pick the
smallest workable capacitor.

The actual benefit, using 220 nF / 329 Hz as an example against the two
worst cases identified above:

- **4 kHz PWM harmonic (aliases to 100 Hz):** today ~62% of amplitude
  reaches the ratio test untouched. With the analog pole,
  `|H(4000Hz)| = 1/sqrt(1+(4000/329)^2) = 0.082` attenuates it before
  sampling; combined with the same 62% digital-side leakage at 100 Hz,
  `0.082 * 0.62 = 5.1%` reaches the detector - roughly a 12x reduction.
- **1 kHz fundamental (aliases to 300 Hz):** analog attenuation `0.313`,
  digital-side leakage at 300 Hz is 25.7% -> combined 8.0%, versus 25.7%
  today.

What still matters, since latency itself cancels, is whether `tau_a` is
the *same* at both gates. Capacitor tolerance is the residual risk: a
10% part gives up to ~20% nominal-to-nominal spread in `tau_a` between
two gates, translating (via the sensitivity in the table above) to
roughly 150-250 us of *differential* detection time - comparable in size
to the ~314 us quantization-jitter floor established earlier, i.e.
non-negligible. A 5% (or tighter) part, e.g. C0G/NP0 ceramic or
polyester film, keeps this to roughly 75-125 us, clearly subdominant to
that floor.

### Missed occlusion and false positive survey

A pass over what could cause a genuine transit to go unregistered, or a
non-event to register as one.

**Missed occlusion:**

- **RF transmission blackout** (`sendString` disables interrupts for
  ~40 ms) - checked against both real kinematics and actual usage
  sequence, and downgraded on both counts. It only matters where the two
  co-located sensors (home/start, on the `gateID == 0` board) could
  trigger close enough in time for one's blackout to swallow the other's
  event:
  - *Return leg* (robot re-enters the start cell and brakes to a stop):
    bounding by the stated braking limits (150 mm max travel, 10 m/s^2
    max deceleration) caps entry speed at `v0 = sqrt(2*10*0.15) =
    1.73 m/s`, giving a full stop time of `173 ms`. Even the more
    conservative figure - time for the 100 mm body to clear the beam,
    `t = 73 ms` - is well over 4x the 40 ms blackout. Low risk.
  - *Departure leg* (robot dwells in the start cell for several seconds,
    then sets out and breaks the start sensor as it leaves): no
    kinematic check even needed - only occlusion *onset* transmits, so
    the arm sensor's transmission happened seconds earlier and is long
    over by the time the start sensor breaks. Effectively zero risk.
- **Occlusion shorter than the ~2.3-3.1 ms filter minimum:** not a
  practical concern for a full robot body at any plausible speed (needs
  <15 mm of occluding width even at 5 m/s); only relevant if the beam
  only catches a thin feature.
- **Insufficient LED-to-ambient contrast margin, or saturation** (see
  the LED illumination margin analysis above): the strongest candidate
  identified for real-world missed occlusions where the beam was
  confirmed fully occluded and still went unregistered. Unlike other
  items on this list, this one is now quantified: with the LED
  contribution at its documented ~60-count setting, the design has
  essentially no ambient margin at all.
- **Partial/grazing occlusion** (dust, emitter wear, physical
  misalignment leaving the beam only partly blocked): a separate,
  still-unquantified geometry/install-quality cause.
- **Post-long-occlusion recovery lockout** (~tens-100 ms extra after a
  multi-second block): covered above.

**False positives:**

- **Mains/LED PWM flicker aliasing:** the one item with real, quantified
  exposure and a concrete fix (the anti-alias capacitor above), cutting
  the worst identified case from ~62% to ~5% of interferer amplitude.
- **Self-inflicted noise at RF transmit on/off:** plausible but
  unverified without a bench measurement.
- **Foreign objects, marginal-SNR installs, single-sample ADC glitches:**
  operational or install-quality concerns rather than algorithm defects;
  the last is already shown to be inherently rejected by the filter's
  own time constant.

## Suggested Improvements

Ordered by importance and expected impact on real detection performance.
Each is atomic - implement and verify one before moving to the next.

1. **Increase LED illumination (drive current and/or optical coupling)
   until the unoccluded reading is dominated by the LED by at least 6x
   over ambient, while staying at or below ~80% FSD (~818 counts) for
   saturation headroom.** Hardware-only. Ranked first because it targets
   the mechanism best matching the field evidence to date: missed
   occlusions have been observed while false positives have not, and
   every observed miss involved confirmed full occlusion - which rules
   out timing/geometry causes but is exactly what an under-illuminated
   sensor produces, silently, with no diagnostic indication. The current
   documented setting (`L ~ 60` counts) is far below this target.
   *Verify:* measure ambient-alone and LED+ambient readings directly on
   the bench (and ideally in representative venue lighting) before and
   after, confirming both the >=6x ratio and the <=80% FSD ceiling are
   met.

2. **Add a 470 nF, 5% (or better) tolerance capacitor across the
   existing 2200 ohm emitter resistor.** Hardware-only, no firmware
   change. Addresses the one false-positive mechanism with real
   quantified exposure (mains flicker and kHz-range LED PWM, including
   its aliased harmonics), cutting worst-case interference amplitude by
   roughly 12x. Its added ~1.25 ms latency is irrelevant since it's
   common to both gates. *Verify:* repeat the emitter blink test before
   and after under a known interference source (or just ambient LED
   lighting) and confirm normal transit detection is unaffected while
   susceptibility to flicker is visibly reduced.

3. **Make the recovery clamp unconditional** - let `slow = max(slow,
   fast)` run every sample regardless of the `slow < 10` floor; keep the
   floor purely as a "no usable signal" diagnostic. One-line-scope
   firmware change, fixes what's shown to be a routine (not edge-case)
   stuck state on the arm sensor after every start-cell dwell. *Verify:*
   occlude a sensor for >10 s, confirm the LED/`mDiff` state recovers
   within the normal ~10-100 ms window once the beam clears, instead of
   staying frozen.

4. **Add an N = 6 sample confirmation (~4.6 ms) before latching
   `mInterrupted`** - roughly double the inherent ~3-sample/2.3 ms filter
   minimum. Small firmware change (a counter). Reduces false-positive
   exposure from noise bursts that happen to land near the detection
   threshold, at no cost to genuine transits (tens of ms minimum).
   *Verify* via the blink test: confirm the minimum detectable pulse
   width moves from ~2.3-3.1 ms to ~7-9 ms, and confirm a normal
   transit's reported trigger point shifts by the expected fixed
   ~4.6 ms (which cancels between gates per the Delay vs Jitter
   analysis).

5. **Seed both filters from a short burst of real ADC samples at
   startup, instead of the hardcoded 0 default.** Take ~16-32
   `analogRead()` samples (~1.6-3.2 ms total) at the start of `setup()`,
   average them, and pass that average as the `value` argument to both
   `slow.begin()` and `fast.begin()`. Removes the artificial
   climb-from-zero delay (up to ~700 ms on a dim install, per the table
   above) and the matching dead zone in the wave-your-hand handshake.
   Small, self-contained firmware change, no interaction with steady-
   state detection. *Verify:* compare time from power-on to first valid
   `mDiff` before and after, on the bench.

6. **Capture real fast/slow/mDiff traces from the hardware** (the DEBUG
   serial-plotter path already exists) across: normal transit, ambient
   dimming, a multi-second deliberate block, and, if available, a known
   flicker/PWM source. Provides real numbers for `B` and the actual
   noise floor to check every estimate above against, and directly
   informs whether the LED illumination target in #1 has been met,
   whether the capacitor value in #2 needs adjusting, or whether #7 is
   worth doing at all.

7. **Derive the dark floor and hysteresis ratios from a measured
   calibration phase**, rather than the fixed 0.25 / 0.75 / 10-count
   constants - e.g. extend the existing `setup()` wave-detection
   handshake to record the actual bright/dark spread and set thresholds
   as a fraction of it. Larger, more invasive change; most valuable once
   the reflective-sensor redesign changes the achievable contrast ratio,
   so it's reasonable to sequence last for the current hardware.

**Portability note, not an action item:** `alpha * F = 1/tau` is baked
into the filter constants, not computed at runtime. If a future ISR rate
change is made (e.g. porting to different hardware), both filter alphas
need recalculating together to preserve `tau_slow = 1 s` /
`tau_fast = 2 ms`.
