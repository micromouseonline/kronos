# Tone-Decoder Gate: Design Discussion

Status: exploratory. Not a proposal to replace the interim plan of testing
the existing dual-EMA-filter firmware on existing hardware (see
`legacy-evaluation.md`) - this is a look at what a modulated-emitter /
tone-decoder front end would take, as a candidate for a future board
revision.

## Contents

- [Concept](#concept)
- [Why this sidesteps the amplitude-domain problems](#why-this-sidesteps-the-amplitude-domain-problems)
- [System block diagram](#system-block-diagram)
- [Emitter: oscillator and LED drive](#emitter-oscillator-and-led-drive)
- [Receiver: AC front end and LMC567](#receiver-ac-front-end-and-lmc567)
- [Frequency plan and channel separation](#frequency-plan-and-channel-separation)
- [Component tolerance and calibration](#component-tolerance-and-calibration)
- [Expected response time](#expected-response-time)
- [Power budget (rough, needs datasheet confirmation)](#power-budget-rough-needs-datasheet-confirmation)
- [What stays the same](#what-stays-the-same)
- [Open questions](#open-questions)

## Concept

Replace amplitude-threshold detection (compare a filtered ADC reading
against a fraction of a slowly-tracked baseline) with frequency-selective
detection: modulate each emitter at a fixed audio-range tone, and decode
each receiver with a phase-locked tone decoder (LMC567) tuned to that
same frequency. The decoder's digital output reflects whether its tone is
present (beam intact) or absent (beam broken), independent of the
absolute brightness of anything else hitting the phototransistor.

Two co-located beams (home/start on one physical unit) get two different
tones - proposed as 17 kHz and 23 kHz - so each receiver locks only onto
its own emitter and ignores the other's light entirely, regardless of how
much optical isolation exists between the two beam paths.

## Why this sidesteps the amplitude-domain problems

Every major issue in `legacy-evaluation.md` is a consequence of detecting
occlusion by comparing signal *levels*: the fast/slow filter tuning, the
0.25/0.75 hysteresis thresholds, the mains/PWM aliasing analysis, and the
LED-vs-ambient contrast margin problem all exist because the circuit has
to infer "beam broken" from how much a level changed relative to a
baseline it also has to estimate. Ambient light - DC sunlight, 100 Hz
mains flicker, kHz-range LED PWM - carries essentially no energy at 17 or
23 kHz. A tone decoder doesn't compare levels at all; it reports lock
state. So none of that class of problem applies here: no filter time
constants to trade against noise rejection, no threshold to derive from a
measured spread, no aliasing to reason about (this is a continuous analog
PLL, not a sampled system), and no minimum LED-to-ambient ratio to
maintain, because ambient brightness isn't part of the decision at all.

What *doesn't* go away: the same fundamental speed-vs-selectivity
tradeoff reappears in analog form, in the decoder's loop filter capacitor
(see Response Time below). It's a much better version of that tradeoff,
because a PLL-based decoder is genuinely narrowband rather than a simple
low-pass averaging everything below a cutoff, but it's not free.

## System block diagram

```
Emitter side (per beam):
  oscillator (17 kHz or 23 kHz) --> LED driver transistor --> IR LED

Receiver side (per beam):
  phototransistor --> AC-coupling cap --> [optional gain stage] -->
    LMC567 (tuned to matching tone) --> open-collector output,
    pulled high through R --> clean digital "beam broken" signal -->
    same MCU / RF path as today
```

Two full receiver chains are needed on the shared home/start board (one
per tone); the RF transmission side (`send_trigger`, the 433 MHz link)
is unaffected by any of this - this design only replaces the sensing
front end, not the messaging path already analyzed in
`legacy-evaluation.md`.

## Emitter: oscillator and LED drive

Two candidates, both cheap and low-part-count:

- **Single CMOS Schmitt-trigger inverter (e.g. 74HC14/74AHC1G14) RC
  oscillator.** Lowest quiescent current of the two options (CMOS logic
  quiescent draw is typically in the tens of microamps range, but this
  needs confirming against the specific part and supply voltage chosen -
  not asserting an exact figure here). Two resistors and a capacitor.
  Approximate frequency: `f ~= 1 / (2.2 * R * C)` for a standard
  single-inverter Schmitt oscillator - this is a commonly used
  approximation, not an exact figure, since actual frequency depends on
  the specific part's hysteresis thresholds; expect to trim in practice.
- **Low-power CMOS 555 (TLC555 / ICM7555) astable.** More current draw
  than a bare logic-gate oscillator but still modest, more conventional
  to design and adjust (standard 555 astable formulas, well documented),
  easier to get an independent duty-cycle control if that turns out to
  matter.

Given the explicit low-current goal and that a battery-powered board is
already the existing hardware direction (LiPo charger footprint present
in the KiCad project), the gate-oscillator option is the better starting
point; the 555 is the fallback if debugging/adjustability matters more
than the last bit of quiescent current.

Either oscillator's square-wave output needs a driver transistor (BJT or
small MOSFET) to switch the LED at its intended drive current (tens of
mA) - logic-gate outputs alone aren't sized for that.

One incidental benefit: the original occlusion sensor ran its LED
continuously ("constantly illuminated," per the original design brief).
A tone-modulated drive is on for roughly half of each cycle (assuming a
~50% duty square wave), so average LED current is roughly halved for the
same peak brightness - a small but real power win on top of the circuit
simplification.

## Receiver: AC front end and LMC567

The phototransistor's DC/slowly-varying component (ambient) needs to be
blocked before the signal reaches the decoder - a series coupling
capacitor between the phototransistor's output node and the LMC567
input does this, passing only the AC tone component.

Whether a gain stage (a single transistor or op-amp stage) is needed
between the coupling cap and the LMC567 input depends on how large the
AC signal actually is at the phototransistor for the achievable optical
coupling and modulation depth - the LMC567 needs a minimum input level
to lock reliably (on the order of tens of mV RMS, typical for this class
of part, needs checking against the actual datasheet). This is a bench
question, not something to guess at from first principles - flagged
below as an open item.

**Center frequency.** The classic tone-decoder relationship (from the
NE567 family this part is descended from) is:

```
fo = 1 / (1.1 * R1 * C1)
```

Solving `R1 * C1` for the two target frequencies:

```
17 kHz: R1*C1 = 1 / (1.1 * 17000) = 53.5 us
23 kHz: R1*C1 = 1 / (1.1 * 23000) = 39.5 us
```

Recommended `R1` range for this part family is roughly 2 k-20 kOhm.
Picking `R1 = 10 kOhm` as a round starting point:

```
17 kHz: C1 = 53.5us / 10k = 5.35 nF  ->  5.6 nF (nearest E12 value)
23 kHz: C1 = 39.5us / 10k = 3.95 nF  ->  3.9 nF (nearest E12 value)
```

These are starting values, not final ones - see Component Tolerance
below before committing to them.

**Bandwidth and response time** are set by the loop filter capacitor
(`C2` in the standard application circuit). Wider bandwidth locks/unlocks
faster but rejects less; narrower bandwidth is more selective but slower.
The exact bandwidth-vs-C2 and response-time-vs-bandwidth relationships
are datasheet-specific formulas that should be pulled from the actual
LMC567 datasheet rather than approximated here from memory - flagged as
an open item. The general design goal: pick the narrowest bandwidth that
still responds at least as fast as the existing ~2.3 ms digital detection
latency, so this design is a strict improvement on speed as well as
robustness, not a trade of one for the other.

## Frequency plan and channel separation

17 kHz and 23 kHz gives 6 kHz of separation - about 35% of the lower
frequency. For each decoder to reject the other channel with real
margin, per-channel bandwidth should stay well under half that
separation (a few hundred Hz to roughly 1-2 kHz), which should be easily
achievable while still beating the existing detection latency, since a
resonant/PLL detector gets much better selectivity per unit of response
time than a simple RC low-pass.

Worth checking against one more thing before finalizing: whether the
robot's own motor driver PWM frequency lands anywhere near 17 or 23 kHz.
Motor drivers are commonly run in the tens-of-kHz range specifically to
be inaudible, which is the same range being proposed here - this is the
same kind of coincidental-interference risk already seen with LED PWM
aliasing in the amplitude-domain design, just in a different frequency
band, and it's cheap to check before committing to specific tones.

## Component tolerance and calibration

This is the one place where sloppy component selection could break the
whole scheme, and it's worth being explicit about it. The oscillator's
actual frequency (emitter side) and the decoder's programmed center
frequency (receiver side) are set by independent RC networks on two
different boards - their tolerances stack. With ordinary 5-10% resistors
and capacitors, a decoder's actual center frequency could easily be off
by less than the target bandwidth, or an emitter's actual tone could
drift outside its own decoder's capture range entirely, especially if
bandwidth is deliberately kept narrow for good channel separation.

Two ways to handle this, likely both worth doing:

- Use tighter-tolerance parts where it matters (1% resistors, 5% or
  better C0G/NP0 ceramic or film capacitors) to shrink the stack-up.
- Include a trimmer (a small trim potentiometer for `R1`, or a
  select-at-test capacitor) on the decoder side, so each unit can be
  calibrated to its actual paired emitter's measured frequency after
  assembly, rather than relying on nominal component values alone.

## Expected response time

Not yet quantified against a real datasheet (see open items), but the
target is straightforward to state: match or beat the existing ~2.3 ms
typical / ~3.1 ms worst-case detection latency from the amplitude-domain
design, while getting meaningfully better noise/interference rejection
for that same speed, since a PLL-based decoder's bandwidth-vs-selectivity
tradeoff is fundamentally better than a single-pole low-pass filter's.
This needs the actual LMC567 lock-time formula from the datasheet plus a
chosen bandwidth to turn into a real number.

## Power budget (rough, needs datasheet confirmation)

| Element | Rough order of magnitude | Confidence |
|---|---|---|
| CMOS gate oscillator, quiescent | tens of uA | needs datasheet check |
| LED drive, average (50% duty) | ~half of today's peak drive current | depends on chosen peak current |
| LMC567, supply current | low - this is the CMOS, low-power version of the NE567 family, chosen specifically for that reason | needs datasheet check |
| Optional gain stage | depends on topology chosen | not yet designed |

Two receiver chains (17 kHz + 23 kHz) plus two emitter oscillators are
needed on the shared home/start board; a single-beam board (e.g. a goal
gate) only needs one of each.

## What stays the same

The RF transmission path, packet format, and everything analyzed as part
of `legacy-evaluation.md`'s "Not covered here: RF protocol" scope is
unaffected - this design only replaces the analog/firmware sensing front
end. An MCU is still assumed downstream of the LMC567 outputs, reading
clean digital edges instead of running the fast/slow filter and ratio
logic, and doing the same `send_trigger()` job as today.

## Open questions

1. Confirm exact LMC567 pin assignments, the bandwidth-vs-C2 formula, and
   minimum input sensitivity from the current datasheet - the formulas
   above are from the general NE567-family tone-decoder application
   note tradition, not a fresh read of this specific part's datasheet.
2. Bench-measure whether the phototransistor's AC signal at the tone
   frequency clears the decoder's minimum input level without an added
   gain stage, for the intended beam distance and modulation depth.
3. Check the target robot's motor-driver PWM frequency against 17/23 kHz
   before finalizing the tone plan.
4. Decide component tolerance strategy (tighter parts vs. a trim
   element) once real center-frequency accuracy requirements are known
   from the datasheet's bandwidth formula.
5. Bench-measure actual lock/unlock response time once C2 is chosen, and
   compare directly against the ~2.3-3.1 ms figure from the existing
   design.
