# Network Timing Issue: Observations, Resolutions, and Outstanding Work

Date: 2026-07-30. Restructured 2026-07-31 — see "Restructuring note" at the
bottom for what changed and why.

## Context

`hesperus-timing-gate` boards report race events (`ARM`, `START`, `GOAL`) to
`cerberus-gate-controller`, with disciplined Wi-Fi-TSF-based microsecond
timestamps carried in the payload. Transport was originally per-event HTTP
(`POST /api/event`); a persistent WebSocket connection (`/ws`) was added
during this investigation and is now the default (see the first issue
below). Bench testing the two boards together (a start board and a goal
board, both clients of a single cerberus controller) surfaced a chain of
related issues in network latency, its effect on displayed race times, and
its effect on race-result correctness. This document records what was
found, why it happens, what's been done about it, and what's still open.

Bench-testing tools referenced throughout (`ares-pulse-generator`'s
`trial_*` functions, `ws_send_event.py`, `cerberus_log_stats.py`, etc.) are
catalogued in `docs/TEST-TOOLING.md` — that doc covers what each tool does
and how to invoke it; this doc only references them by name.

## Acceptance criteria for beacon-spam stress testing

Decided 2026-08-03, after three stress-test sessions (see the beacon-spam
issues below): any lost timing event is a broken system by acceptance-testing
standards, but demanding that in *every conceivable* radio environment isn't
realistic — a pass/fail line has to be drawn somewhere, and where matters
because the stress-test protocol (15 runs no spammer, 15 with one spammer,
then a second spammer added) exercises two very different threat levels in
one script:

- **One beacon spammer is a realistic-but-heavy load** — plausibly at the
  high end of what a busy exhibition hall could produce, but not something
  a contest would rarely or never see. **This is the pass/fail bar.**
- **Two simultaneous spammers (~1000 beacon frames/sec combined) is an
  extreme adversarial scenario** — far beyond anything a real venue's own
  traffic would generate; every radio on the channel has to process each
  beacon frame at the hardware/MAC level regardless of payload, which is why
  it hits the ESP32's software WiFi stack disproportionately hard (see the
  `wsClient.loop()`-blocking issue below) even though a laptop on the same
  air noticed nothing. Treated as a **smoke test** (does the system degrade
  gracefully and recover automatically, without crashing or corrupting a
  result) rather than a pass/fail gate — zero-loss under deliberate,
  sustained ~1000fps beacon flooding is not a requirement.

**Single-spammer result so far: passes cleanly.** Isolating each stress
session's no-spammer + single-spammer phase (everything before the first
`Resent` line, which lines up almost exactly with the 15+15-run protocol)
and checking committed vs. armed+started: session 3, 29/30 (the one gap is
the session's very first run, a cold-start artifact, not spammer-induced);
session 4, 31/31; session 5, 29/30 (the one gap is the *last* run before
cutoff, whose `GOAL` landed 131ms after the first `Resent` — caught by the
onset of the second spammer, not a failure during clean single-spammer
running). **Zero genuine single-spammer losses across all three
independent sessions** once the cold-start and cutoff-boundary artifacts are
excluded. This held both before and after the 2026-08-03 retry-schedule
mitigation (session 5's session-3-era predecessor already passed too) — the
retry/deadline logic never even engages during single-spammer running (no
`Resent` lines appear until the second spammer goes on), so that mitigation
is aimed entirely at the smoke-test scenario, not the pass bar.

**Practical implication**: further tuning effort aimed specifically at the
two-spammer case (chasing its ~2-3% residual loss, or side-effects like the
`networkQueue`/`ledQueue` overflow seen in session 5) is lower priority than
confirming single-spammer robustness more rigorously — larger sample sizes,
and the more realistic congested-airtime conditions sketched in outstanding-
work item 5 below, rather than continuing to optimize against a scenario
this system isn't required to survive losslessly.

**Large-N no-spammer baseline, 2026-08-03** (`test-data/spam-tests/
{cerberus-6,hesperus-start-6,hesperus-goal-6}.log`, 5000-run trial, no
spammer — the first of a planned pair, single-spammer counterpart still to
come): **5000/5000 ARM, START, and GOAL, zero retries, zero drops, zero
queue overflow, one single WS disconnect/reconnect across the entire
~6-hour test.** A perfect result, as expected for the no-spammer condition,
and a clean baseline to compare the equivalent single-spammer 5000-run trial
against once it's run. (Session did produce one unrelated curiosity ~64-84
minutes after the last logged race, well outside the trial itself — see the
"AP radio interruption" issue's 2026-08-03 addendum below.)

## Outstanding work, prioritized

1. **[Acks not arriving back at hesperus in time](#issue-acks-not-arriving-back-at-hesperus-in-time-despite-cerberus-receiving-the-event)**
   — **START HERE.** Found 2026-08-03 while reviewing item 3's verification
   data: cerberus received the retried event multiple times, well inside
   hesperus's own wait window, in both residual outlier cases — yet no ack
   ever got back to hesperus in time. Same-day session 3, with new
   `[WS-ACK]` timing instrumentation hardware-verified: **cerberus's own
   ack dispatch is confirmed fast** (max 15ms, `recv`→`client->text()`
   return, even under the worst congestion) — rules out "cerberus is slow"
   as a cause. But a specific case shows cerberus application-acked the
   same event 4 times, all well inside hesperus's window, with no
   `wsPumpTask` stall logged either — yet hesperus still never recognised
   an ack and dropped after max retries. Narrows to two undistinguished
   candidates: `AsyncTCP`'s actual on-air write completion (vs. just
   accepting the call) lagging behind `client->text()` returning, or
   genuine asymmetric packet loss on the return leg specifically. Next
   step: instrument `AsyncTCP`'s write-completion callback, or a return-leg
   packet capture. Full detail in the issue below.
2. **[Wi-Fi power-save vs. battery budget](#issue-wi-fi-power-save-vs-battery-budget)**
   — measure current draw across `WIFI_PS_NONE`/`MIN_MODEM`/`MAX_MODEM` with
   persistent connections in place. Real, measured 110mA cost today; the
   thing this was explicitly deferred pending (persistent connections) has
   now landed, so this is unblocked.
3. **[`wsClient.loop()` blocking under congestion](#issue-wsclientloop-blocking-under-congestion-defeats-the-ackretry-deadline-bound)**
   — confirmed via instrumentation + cross-board log correlation: beacon
   spam causes real TCP-level WS disconnects (up to ~18s outages seen) on
   the hesperus side, with `wsClient.loop()` blocking up to ~9.5s per call,
   silently blowing the 2000ms ack/retry deadline bound. Not classic
   airtime saturation (a laptop held 25Mbps through the same spam) —
   likely ESP32-specific WiFi-stack overhead from beacon-frame volume.
   Undermined the already-shipped retry/dedup mechanism precisely under the
   real-world congestion it exists for. **Resolved and hardware-verified
   2026-08-03** (dedicated `wsPumpTask` + mutexes decouple the socket pump
   from the retry deadline logic) — 5/6 verification deltas landed within
   ~500ms of the 2000ms target, down from the original bug's 7.6s+
   untethered stalls. The two residual outliers are now tracked as item 1
   above, not here. Root RF-level cause (why the outages happen at all)
   remains open too, folded into item 5 below.
4. **[Duplicate triggers from gapped robot structure](#issue-duplicate-triggers-from-gapped-robot-structure)**
   — the proposed ~300ms post-trigger lock-out window is arbitrary and its
   failure mode (silently dropping a genuine second crossing, rather than
   today's harmless ignore-once-out-of-`RUNNING`) hasn't been worked out.
   Cheap once resolved, but resolve the design questions first.
5. **Congested-airtime stress testing** (cross-cutting, not tied to one
   issue) — the four stressor layers sketched in
   `hesperus-timing-gate/review.md` (airtime saturation, bulk throughput,
   channel interference, broadband noise; see `docs/TEST-TOOLING.md`) have
   runs behind items 1 and 3 above are informal data points in that
   direction, and are how both were found. Would validate several of the
   already-shipped fixes (dedup, retry, TSF trust-on-reconnect) under
   realistic contest-venue conditions, not just the quiet-network bench
   tests done so far.
6. **[Hedged burst sends](#issue-hedged-burst-sends-for-tail-latency)** —
   deprioritized; watch real-world retry counts/rate from the now-shipped
   ack/retry mechanism before building this. No action needed unless that
   telemetry shows frequent retries.
7. **[Unexplained minor WS jitter / reconnect blip](#issue-unexplained-minor-ws-jitter--reconnect-blip)**
   — low-priority curiosity, self-healing, doesn't touch committed times.
   Proposed long steady-state (10,000-message) characterization test not
   yet run. Its own leading candidate cause (`wsClient.loop()` occasionally
   blocking) is the same one item 3 now has direct evidence for, just at
   millisecond rather than multi-second scale — worth re-reading together
   now that item 3's instrumentation exists.

Everything else below this list is either resolved or has no further action
planned. (Explicit HTTP connect/read timeouts, formerly tracked here, turned
out to be N/A rather than just deprioritized — see the first issue below.)

## Issues

### Issue: Per-event connection latency & queueing

**[RESOLVED — persistent WebSocket connections shipped; the one remaining recommendation (explicit HTTP timeouts) is now N/A, see below]**
*(was Observations #1, #2, #3, #8; Recommendation 1, Recommendation 3)*

**Observation.** Every trigger initially produced exactly four identical
`POST /api/event` requests at cerberus, matching `MAX_HTTP_RETRIES`
precisely. Root cause: `HTTP_TIMEOUT_MS` was set to 250ms, which the real
round-trip time consistently exceeded — hesperus exhausted all four retry
attempts on every single event, even though cerberus was receiving and
correctly processing every one of them. Raising `HTTP_TIMEOUT_MS` to 2000ms
initially resolved the duplicate-POST pattern, but didn't fully explain
later data: a "confirmed" event was observed taking 2993ms — longer than
the configured 2000ms timeout, yet still succeeding. Inspecting the ESP32
Arduino `HTTPClient` source
(`framework-arduinoespressif32/libraries/HTTPClient/src/HTTPClient.cpp`)
showed why: `HTTPClient::setTimeout()` only sets `_tcpTimeout`, the
**response-read** timeout, applied only *after* a TCP connection is already
established. The TCP **connect** phase uses a separate `_connectTimeout`,
defaulting to a hardcoded `HTTPCLIENT_DEFAULT_TCP_TIMEOUT = 5000`
(`HTTPClient.h:42`) that nothing in the firmware had ever configured — so
tuning `HTTP_TIMEOUT_MS` never bounded the dominant source of latency at
all.

Both boards were given TSF-based (`esp_wifi_get_tsf_time(WIFI_IF_STA)/1000`)
millisecond timestamps on every debug log line — hesperus and cerberus are
both stations on the same AP, so this is a shared, sub-ms-precision clock
across boards, letting logs from each be correlated directly.
`tsf_us` (captured in hesperus's ISR at the instant of the physical
trigger) against cerberus's own logged receipt time gives the true one-way
"trigger → cerberus received" latency, independent of the return trip:

| Event | One-way (trigger → received) | Full round trip (hesperus-measured) | Implied return leg |
|---|---|---|---|
| GOAL | ~132ms | 146ms | ~14ms |
| START | ~270ms | 284ms | ~14ms |
| ARM | ~267ms | 285ms | ~18ms |

Outliers of 856ms (first-ever connection in a session) and ~2.9s (later in
the same session) were also observed on single attempts that still returned
`httpCode == 200` — consistent with TCP connect-phase trouble (e.g. a lost
SYN packet triggering the underlying stack's own retransmission).
**Conclusion: essentially all latency, and effectively all of its
variance, is attributable to TCP connection establishment**, paid fresh on
every single event, since no connection reuse/keep-alive existed
(`http.begin()`/`http.end()` opened and closed a new connection each time).
Cerberus's own processing (JSON parse, dispatch, queue, respond) is fast
and consistent — the ~14-18ms return leg above — and was never a meaningful
contributor.

Separately, at higher trigger rates, a second effect compounds the first:
two bench tests isolated a queueing theory directly, using the same
`receipt_ms*1000 − tsf_us` method against a rapid sequence of real `GOAL`
triggers. **Test A — rapid-fire triggering (~88 events, spacing mostly
60-800ms):** one-way latency ramped smoothly from ~110ms up to 2773ms over
each ~8s burst, resetting back down whenever a multi-second pause let the
backlog drain. Latency growth per event correlated directly with
inter-trigger spacing: gaps under ~130ms added ~140-190ms of extra latency
each; gaps over ~250ms let latency drain instead of growing. This is a
queueing signature, not network jitter or clock drift (hesperus's own
`tsf_observed` and `processor_clock` tracked each other to within tens of
microseconds throughout). The mechanism: `uploadWorkerTask`
(`hesperus-timing-gate/src/main.cpp:238`) drains `networkQueue` one event at
a time, and each full connect+POST+confirm cycle costs ~230-270ms (matching
the baseline above exactly) — triggers arriving faster than that queue up
and each waits longer than the last for its turn. **Test B — control, same
trigger source, ~1-2s spacing (~87 events):** mean 234.6ms, median 259.5ms,
p90 319ms, p95 345ms, stdev 102ms. Only one outlier, 818.8ms — the very
first request of the session (cold connection). This confirmed Test A's
spike was purely a by-product of sustained rapid-fire triggering outrunning
hesperus's one-event-at-a-time send cycle, not a new or separate network
problem.

**Confirmation.** `networkQueue` is only 10 deep
(`hesperus-timing-gate/src/main.cpp:452`). At a ~250ms service time, a
sustained trigger rate faster than that for more than ~2.5s overflows it —
silently dropping events, not just delaying them. `ares-pulse-generator`'s
`trial_burst` fired 40 `GOAL` pulses at a fixed, known 90ms interval (raw
data in `test-data/trial-burst-20260630-1542*.txt`); only 24 reached
cerberus. The other 16 are independently confirmed dropped two ways at
once: hesperus's own serial log showed `[QUEUE OVERFLOW] N networkQueue
event(s) dropped.` lines summing to exactly 16, and cerberus's received
events show gaps of 180ms/270ms (2x/3x the 90ms send interval) exactly
where pulses are missing. The first ~15-16 pulses got through with no drops
at all, at a clean, unbroken 90ms spacing — but their latency climbed the
whole time, from 272ms to 2726ms, the same ramp signature as Test A. Once
saturated, latency flattened at ~2700-2770ms instead of growing further.
This was no longer an inferred risk: at realistic burst rates, the firmware
was silently losing race events.

**Resolution.** **Persistent connection per gate board (highest leverage),
implemented and merged.** Open one WebSocket connection per hesperus board
once it becomes ready (Wi-Fi connected + cerberus discovered), and hold it
open across all subsequent trigger events instead of opening/closing per
event — eliminates the repeated TCP-handshake cost for every event after
the first. Compatible with Wi-Fi modem sleep: TCP session state persists
across the radio's sleep/wake cycles since they operate at different
layers; only the first transmission after a sleep period pays a smaller,
bounded radio wake-up cost instead of a full handshake (relevant to the
power-save issue below). Two design details settled during implementation:

- **A held-open connection alone does not give rapid failure detection** —
  if the peer silently disappears (power loss, Wi-Fi drop with no clean
  FIN/RST) while the socket is idle, plain TCP has no built-in way to
  notice. A WebSocket gives proactive detection for free via its ping/pong
  control frames and library-level disconnect callbacks
  (`AsyncWebSocket::onDisconnect`), rather than needing hand-built
  keepalive/timeout logic.
- **The gate remains the connection initiator**, matching the existing
  client/server roles and mDNS discovery direction (gate resolves
  `cerberus.local`; cerberus has no equivalent way to discover a gate).
  Who initiates doesn't restrict data direction afterward — a WebSocket is
  full-duplex once established regardless of who dialed.

WebSocket was chosen over a bare kept-open TCP/HTTP socket specifically
because the ping/pong-and-disconnect-detection property comes essentially
free with a library on both ends, `ESPAsyncWebServer` (already a cerberus
dependency) includes `AsyncWebSocket` with no new library needed
server-side, message framing is native, and the existing dispatch logic
(`race_command_from_http()` / JSON schema) didn't need to change at all —
only the transport underneath moved. Honest cost: hesperus had no
WebSocket *client* before this (added `Links2004/arduinoWebSockets`), and a
WS connection still needs an initial HTTP-upgrade handshake, so there's
still a first-connection cost, just paid once per board instead of once per
event.

**N/A (was "Still open")**: the recommendation to configure `HTTPClient`
timeouts explicitly (`http.setConnectTimeout()`, alongside `http.setTimeout()`)
was never implemented, and now never will be — hesperus's runtime code no
longer constructs an `HTTPClient` object at all (verified: `main.cpp` only
mentions it in a historical comment). The WS client library
(`WebSocketsClient`) handles the persistent connection instead, with its own
timeout/heartbeat config (`enableHeartbeat()`), unrelated to `HTTPClient`'s
settings. The two proposed characterization experiments (isolating
connect-phase latency with temporary `WiFiClient::connect()` instrumentation;
injecting packet loss via Linux `netem`) are moot for the same reason — both
targeted the old per-event HTTP connect phase, which no longer exists.

**Verification.** Bench-measured WS latency settled at ~5-12ms typical
(a one-time ~1.1s first-connection warm-up on an early run didn't recur
later), against ~115-560ms for HTTP comparison runs — beat the original
~15-30ms prediction. `trial_burst` re-run under WS (raw data in
`test-data/trial-burst-ws-20260630-1036.txt`): all 40/40 pulses reached
cerberus, zero drops, no `[QUEUE OVERFLOW]` lines. Latency: mean 8.7ms,
median 8.2ms, stdev 2.7ms, min 5.2ms, max 18.2ms, p99 18.2ms — flat
throughout, no climbing/ramp signature. Confirms the mechanism directly:
the persistent connection eliminates the ~250-270ms per-event
connect+POST+confirm service time that was the actual bottleneck, so the
depth-10 `networkQueue` never comes close to saturating at this rate.
Reconnect-after-Wi-Fi-drop and reconnect-after-cerberus-reboot are both
bench-confirmed working (self-healing, no manual intervention needed).
Both physical hesperus test boards flashed and running WS successfully;
the other 3 hesperus board envs share the same firmware so didn't need
separate testing. See also the ARM/START and double-trigger issues below,
whose WS re-runs are further confirmation of the same mechanism.

### Issue: Wi-Fi power-save vs. battery budget

**[OPEN — see outstanding-work #1]**
*(was Observations #4, #5; Recommendation 2)*

**Observation.** Hesperus originally used `WIFI_PS_MAX_MODEM` (aggressive
power saving). A ~2.9s latency spike was traced to modem-sleep wake-up: the
radio sleeps between sparse events — exactly the ARM/START/GOAL spacing of
a real race — and must wake before it can transmit. Cerberus's own code
already disables power-save (`WIFI_PS_NONE` in `net/wifi-manager.h`) for
this same reason; hesperus was changed to match. However, a subsequent test
*after* switching to `WIFI_PS_NONE` still showed a ~2993ms outlier —
confirming modem sleep was not the sole cause; the TCP connect-phase issue
above is a separate, additional contributor that disabling power-save alone
doesn't fix.

Gates are battery-powered, and batteries above 500mAh are hard to fit
physically. `WIFI_PS_NONE`'s continuously-active radio draws considerably
more current than a modem-sleep mode — generic ESP32 figures put
power-save-off in the 80-150mA range versus roughly 15-30mA average with
modem sleep enabled (generic chip figures, not measured on this
hardware/firmware at the time). Against a sub-500mAh budget, that's
plausibly the difference between a few hours and most of a day of runtime.

**Confirmation.** Measured 2026-07-31: current `WIFI_PS_NONE` draw is
**~110mA average on a gate** — a real, non-hypothetical cost, now
confirmed for this actual hardware rather than inferred from generic
figures.

**Resolution.** Not yet resolved. Decided direction: reintroduce a lighter
modem-sleep mode now that persistent connections exist —
`WIFI_PS_NONE` was originally adopted to fix a problem a persistent
connection now addresses more directly and without the full power cost.
`WIFI_PS_MIN_MODEM` (wakes every beacon interval, ~100ms typically) is a
reasonable middle ground to test empirically against `MAX_MODEM` and
`NONE` for both latency and current draw — this was explicitly deferred
(2026-07-30) until persistent connections landed, since that's the
configuration it's actually meant to be measured under; they've now
landed, so this is unblocked. Keep the `NONE` baseline figure (110mA)
alongside whatever `MIN_MODEM` measures — "we cut power" needs both
numbers to mean anything, not just the new one.

**Verification.** None yet. Proposed method (never run): for each of
`WIFI_PS_NONE`, `WIFI_PS_MIN_MODEM`, `WIFI_PS_MAX_MODEM`, with a persistent
connection open, measure (a) current draw via a USB power meter or
multimeter, at idle and during a trigger, and (b) wake-to-first-byte
latency for a trigger fired after a range of idle gaps (1s, 5s, 15s, 30s,
60s+, spanning realistic ARM-to-START and START-to-GOAL spacing). The WS
ack/retry mechanism (see the reliable-delivery issue below) now also gives
a concrete pass/fail signal that didn't exist when this was first
proposed: run the existing multi-run trial suite under `MIN_MODEM` and
check retry counts/rate against the `NONE` baseline, not just whether runs
complete — a real regression (missed beacon wake windows delaying sends)
would show up as elevated retries before it shows up as a visible fault.

### Issue: Displayed race time vs. true TSF time

**[RESOLVED]**
*(was Observation #6; Recommendation 4)*

**Observation.** Cerberus's `race-timer.h` starts and stops its `run_sw`
`Stopwatch` using cerberus's own local `millis()`, captured at the moment
it processes each `SystemEvent` — not the `tsf_us` timestamp carried in the
event payload. This means the *displayed* run time differs from the true
TSF-computed elapsed time by exactly `L_goal − L_start` (the **difference**
between the two legs' network latencies, not either one individually, and
not their sum). If latency happened to be identical on both legs, this
error would cancel to zero even though the display itself runs a constant
offset behind the real event. Real latency is not constant (see the
connection-latency issue above), so asymmetry between the two legs injects
directly, and visibly, into the displayed number — a spectator-facing
appearance problem, not a data-accuracy problem: the true elapsed time was
already recoverable precisely from `tsf_us` differences regardless.

**Confirmation.** Validated via `ares-pulse-generator`'s `trial_four_runs`
(four back-to-back runs of known 2000/3000/4000/5000ms duration) rather
than by injecting artificial asymmetric latency as originally proposed —
using real gate hardware/network latency directly caught a real design
flaw (below) that synthetic injection likely wouldn't have surfaced as
clearly.

**Resolution.** First attempt: backdate `run_sw` itself at both `START` and
`GOAL` (via `Stopwatch::restart(timestamp)`/`stop(timestamp)`) so the
committed time came out exact. It did — but the live display, ticking in
real time from a backdated start, then had to keep counting past the true
finish for as long as the `GOAL` message's own network leg took to arrive,
and visibly snapped backward the instant it did. That read worse to a
spectator than the original problem (a display that's merely a bit late is
far less alarming than one that runs, then suddenly rewinds).

**What shipped instead**: `run_sw` stays exactly as it was before this
work — plain receipt-time `restart()`/`stop()`, no backdating — so the live
display is smooth and monotonic, just consistently latency-late (the
original, milder problem). Separately, cerberus records the `START`
event's `tsf_us` (`g_run_start_tsf_us` in `race-timer.h`) and, on `GOAL`,
computes the committed time directly as `round((GOAL.tsf_us −
START.tsf_us) / 1000)` — exact to the millisecond, completely independent
of `run_sw` and unaffected by either leg's latency. The display then shows
that exact committed value from the moment the run ends (`RaceState::GOAL`)
instead of `run_sw`'s own frozen reading, so the only visible "snap" left
is the small residual return-leg jitter (single digits to a few tens of
ms) rather than a whole network round trip. Falls back to `run_sw.time()`
unchanged for a locally-buttoned run, which has no `tsf_us` at all and no
network hop to correct for.

**Verification.** `trial_four_runs` confirmed exact 2000/3000/4000/5000ms
committed results. Committed times stayed exact throughout the later WS
bring-up bench suite too, confirming this fix is unaffected by the
transport change.

### Issue: Stale event misattribution across race attempts

**[RESOLVED — reject-only scope]**
*(was Observation #7, "most serious finding"; Recommendation 5)*

**Observation.** The race state machine assumes events arrive in logical
order (`ARM` → `START` → `GOAL`) and has no way to tell which race
*attempt* an event belongs to. Pathological scenario: a `GOAL` event is
delayed (by retries or connect-phase trouble) long enough that a new
attempt is armed and started before the stale `GOAL` finally arrives. If
it lands while cerberus is back in `ARMED`, it's silently dropped (`ARMED`
only reacts to `START`/`RESTART`) — the true result is lost, but the *new*
attempt isn't corrupted. If it lands while cerberus is `RUNNING` on the
*next* attempt, it was accepted **unconditionally** — `RaceState::RUNNING`'s
handler treated any `GOAL` at face value — silently ending the new run
early with a bogus committed time. Nothing distinguished "this GOAL
belongs to the current attempt" from "this is a stale message from a
previous one." A structural gap in the event contract, not a display
cosmetic — persistent connections and retry reduce the probability of this
scenario substantially but don't eliminate it structurally.

**Confirmation.** `tools/testing/ws_send_event.py` gained an optional
`--tsf-us` override so a `GOAL` with an arbitrarily old `tsf_us` can be
sent on demand — reproduces exactly what a sufficiently-delayed real
message would look like on arrival, without needing to actually hold
anything back in flight. This was run 2026-07-31 against the already-landed
fix (below) rather than pre-fix behaviour, since the fix shipped first —
confirming the *fix*, not just the original bug. Confirming the original
unfixed bug this way separately wasn't judged necessary: the code-reading
finding above plus this same tool's confirmation of the fix's correctness
together cover both directions.

**Resolution.** Considered but not (yet) built: an explicit run/attempt
identifier in the event contract — tag each armed attempt with a
monotonically increasing id, include it in the payload, reject
`START`/`GOAL` events that don't match the currently armed attempt. This
is the only option that makes the scenario provably impossible rather than
merely improbable, but has the highest implementation cost (touches the
event schema on both boards and the state machine's acceptance logic) —
still recommended as a follow-on once the lower-effort mitigation below has
had time to prove (or not) its own sufficiency.

**Implemented instead (reject-only scope), 2026-07-31**
(`cerberus-gate-controller/src/race/race-timer.h`, the `RaceState::RUNNING`
`GOAL` branch): `tsf_us` is already a shared, ordered clock across boards,
so staleness can be detected without any new field — a `GOAL` whose
`tsf_us` precedes the *current* attempt's own recorded `START.tsf_us`
cannot possibly belong to the current attempt, since that attempt's clock
only starts counting forward from its own start. A `GOAL` is now rejected
(logged as `[RACE] rejected stale GOAL: tsf_us=... < start_tsf_us=...`,
state stays `RUNNING`, nothing committed) whenever its `event_tsf_us` is
less than `g_run_start_tsf_us`. On review, a ring-buffer of recent attempts
— originally thought necessary for this comparison — turned out **not**
to be needed for reject-only scope: `g_run_start_tsf_us` is a single scalar
always overwritten by the most recent `START`, and `tsf_us` is monotonic
across every attempt, so comparing only against the *current* attempt's
start already catches a stale event regardless of how many attempts were
abandoned in between. A ring buffer would still be needed for a fancier,
still-unimplemented follow-on — retroactively completing an abandoned run
from a late `GOAL` instead of merely rejecting it — which remains a genuine
complement to an explicit attempt-id (not a substitute, and not a gap in
this fix's current scope: it's an enhancement, not a missing piece of what
was promised). Native unit tests added (`test_race_timer.cpp`) covering
the stale-reject case and the `<` vs `<=` boundary (an exactly-equal `tsf`
still commits). Builds clean (`pio test -e native`, `pio run -e
cerberus-cyd2usb-diymalls-ili9341`).

**Verification.** Bench-confirmed 2026-07-31 on real hardware. Sequence:
`NEW_MOUSE` → `ARM` → `START` (real `tsf_us`, e.g. `1785522630871972`) →
`GOAL --tsf-us 1`. Cerberus's own log:
```
[WS] DATA ... {"event":"START","tsf_us":1785522630871972,...}
[WS] DATA ... {"event":"GOAL","tsf_us":1,...}
[RACE] rejected stale GOAL: tsf_us=1 < start_tsf_us=1785522630871972
EVT: GOAL, NEOKEY_BUTTON, PRESSED, tsf=59521406 local=59521406 us
```
The crafted stale `GOAL` was rejected and logged; `race_state` stayed
`RUNNING`; nothing committed. A genuine `GOAL` (real NeoKey button press)
immediately afterward committed normally (`16201ms`).

### Issue: Duplicate triggers from gapped robot structure

**[OPEN — see outstanding-work #2]**
*(was part of Observation #9; Recommendation 8)*

**Observation.** A robot with a gapped/slotted structure can break one
gate's beam more than once during what should count as a single crossing.
`DEBOUNCE_US` (50ms, ISR-level) suppresses electrical/mechanical bounce,
but a structural gap can plausibly be wider than that, generating
genuinely separate, correctly-timestamped triggers for what should be one
logical event. The race state machine already tolerates this once it's out
of `RUNNING` (a later duplicate is dropped by state, not corrupted), so
this isn't a correctness bug today — but needlessly sending/queueing extra
events adds avoidable network-side pressure for no benefit, since only the
first trigger of a crossing is ever wanted for a race time.

**Confirmation.** `ares-pulse-generator`'s `trial_double_trigger` (two
`GOAL` pulses on the same pin, edges 150ms apart, 100 trials). HTTP
baseline (`test-data/trial-double-trigger-20260630-1604*.txt`): of 98
cleanly-paired trials, the first trigger's latency averaged 267.3ms (stdev
5.9ms, matching the general baseline) and the second averaged 371.2ms
(stdev 6.0ms) — a consistent **~104ms** queueing delay, the same mechanism
as the ARM/START case below, scaled up because 150ms of spacing overlaps
more of the ~267ms send cycle than ARM/START's 200ms does. Open question at
the time: 198 of the expected 200 events arrived — one confirmed genuine
drop via `gate_us` (the very first trial's second edge simply missing, its
first edge having an unusually slow ~816ms cold-start connection that may
have tied up the worker, mechanism not fully confirmed); the second missing
event a lone trailing trigger with no partner, most likely just the log
capture ending before the response came back, not confirmed either.

**Re-run under WebSockets, 2026-07-31**
(`test-data/trial-double-trigger-ws-20260631-1104*.txt`): same 100 trials,
150ms apart. **All 200/200 events arrived** — no drops, resolving the open
question above in WS's favour (the HTTP baseline's drop/ambiguous-trailing
-event was specific to per-event connection cost, not reproduced here).
First-trigger latency — mean 7.7ms, median 6.6ms, stdev 3.9ms.
Second-trigger latency — mean 8.5ms, median 7.5ms, stdev 2.4ms. The ~104ms
queueing offset is gone (down to **~0.7ms**). One first-trigger outlier at
41.6ms (isolated, paired second-trigger unaffected) plus a handful of
smaller 10-20ms spikes scattered through the run — see the WS-jitter issue
below, not investigated further here.

**Resolution.** Not yet implemented. Proposed: a gate-side post-trigger
lock-out (~300ms) — deactivate a sensor for a short window after it fires,
on top of the existing 50ms electrical debounce — would suppress these at
the source. Cheap to add, does no harm once persistent connections make
per-event overhead small (they now do).

**Two open concerns, 2026-07-31, not yet resolved:**
- **The ~300ms figure is arbitrary.** It's only justified as "distinct
  from and longer than the 50ms electrical debounce" — not derived from
  any measured robot gap width or crossing speed. Worth a better estimate
  grounded in real robot geometry/speed before implementing.
- **This changes the failure mode, not just the traffic volume.** Today, a
  genuine duplicate is tolerated by being *ignored* on arrival — the event
  still reaches cerberus, it's just a no-op once the state machine is out
  of `RUNNING`. A source-side lock-out is qualitatively different: the
  second event is never sent at all. If the window is too long, a real
  second crossing (two robots close together, a robot re-entering) is
  silently dropped instead of harmlessly ignored — trading a visible
  non-issue today for a potentially invisible one. Worth explicitly
  working out what that failure looks like (and how it'd be noticed)
  before picking a window width.

**Verification.** N/A — not implemented yet. Once it is: re-run
`trial_double_trigger` at the chosen gap to confirm only one event now
reaches cerberus, and at a gap wider than the lock-out window to confirm a
genuinely-separate second crossing still gets through.

### Issue: ARM/START shared-board queueing

**[RESOLVED — no separate action needed beyond persistent connections]**
*(was part of Observation #9)*

**Observation.** One hesperus board can serve both `ARM` and `START`
gates; a robot can cross the `ARM` gate and then the `START` gate as little
as ~200ms apart. Both sensors share one board's `networkQueue`, so this
routinely (not just occasionally) queued the `START` event behind the
`ARM` event's in-flight send cycle — at ~230-270ms per cycle, a 200ms gap
was squarely inside the range where the second event queues.

**Confirmation.** `ares-pulse-generator`'s `trial_arm_then_start`, 99
trigger pairs, edges 200ms apart. HTTP baseline
(`test-data/arm-then-start-test-20260630-1522*.txt`): `ARM` latency — mean
266.6ms, median 266.2ms, stdev 3.4ms — matches the solo-trigger baseline
exactly. `START` latency — mean 319.7ms, median 318.8ms, stdev 6.3ms —
consistently ~53ms higher than `ARM` on essentially every pair. The
tightness of both stdevs (3-6ms) confirmed this was a near-deterministic
queueing offset from `START` waiting out the remainder of `ARM`'s in-flight
send cycle, not ordinary network jitter.

**Re-run under WebSockets, 2026-07-31**
(`test-data/arm-then-start-test-ws-20260631-1045*.txt`): same 100 pairs,
200ms apart. `ARM` — mean 6.7ms, median 5.8ms, stdev 2.0ms. `START` — mean
8.2ms, median 6.7ms, stdev 3.9ms. The `ARM`/`START` offset drops from
~53ms (HTTP) to **~1.5ms**. Unlike the HTTP baseline, no cold-connection
outlier appears at the first pair — steady-state latency from pair 1
onward. One isolated `START` outlier at 37.7ms (pair 39 of 100) — see the
WS-jitter issue below.

**Resolution.** No separate fix needed beyond the persistent-connections
issue above, already shipped. Order was always preserved regardless (FIFO
drain, `ARM`'s full cycle completes before `START`'s begins), so this never
risked `START` arriving before `ARM` or otherwise confusing the state
machine — it only ever delayed *when* `START`'s request landed, never
*what* timestamp it carried, since `START.tsf_us` is captured at the true
trigger instant regardless of transmission time. This delay never touched
the committed `GOAL.tsf_us − START.tsf_us` result (see the displayed-time
issue above, which computes that independently of `run_sw`). The live
display shows this delay as an ordinary part of its receipt-time lag by
design.

**Verification.** Covered by the data above (same re-run also serves as
this issue's own bench confirmation).

### Issue: AP radio interruption silently gates triggers for up to 5 minutes

**[RESOLVED]**
*(was Observation #10; Recommendation 9)*

**Observation.** Found during persistent-connection bring-up bench
testing, while deliberately testing reconnect-after-Wi-Fi-drop — unrelated
to WebSockets itself, this is a pre-existing property of hesperus's
clock-disciplining code (`main.cpp`'s "TIMELINE SANITY AUDIT ENGINE") that
the test happened to expose clearly for the first time.
`MIN_PLAUSIBLE_TSF = 300000000` (`hesperus-timing-gate/src/main.cpp`) is
the threshold a fresh `tsf_observed` reading must clear before hesperus
will trust it as an initial baseline (`has_initial_baseline`) — until that
baseline exists, every trigger is unconditionally dropped
(`[CRITICAL DROP] Baseline missing or un-synchronized. Packet dropped.`),
before ever reaching the network send code. The assumption behind the
threshold was that an implausibly-small TSF value means the Wi-Fi stack
hasn't synced yet (e.g. very early boot) — but **`esp_wifi_get_tsf_time()`
tracks time since the AP's own TSF epoch, not since hesperus associated**,
so anything that resets the AP's TSF resets this for every station on it,
regardless of how briefly.

**Confirmation.** Toggling only the AP's radio off and back on (not a full
router reboot/power-cycle) was enough to reset its TSF epoch. hesperus's
own log shows the exact boundary:
```
[T=297670ms] [PLAUSIBILITY REJECT] TSF 297670304 too low. Wi-Fi stack un-synchronized.
[T=297670ms] [CRITICAL DROP] Baseline missing or un-synchronized. Packet dropped.
[T=302817ms] [INITIALIZED] Valid Baseline Coordinates Locked: 302817313
```
Rejected at TSF=297,670,304 (<300,000,000), accepted 5,147,009us later
(matching the heartbeat timer's ~5147ms period exactly) — recovery is
bounded and self-healing, but takes the *entire* 300-second threshold from
the moment the AP's radio was disrupted, not from whenever hesperus itself
reconnects (hesperus's own Wi-Fi reconnected in ~3.4s in this same test;
TSF wasn't trusted again for another ~4.9 minutes after that). Separately,
this same outage also triggered hesperus's existing "stuck in SYN mode
>10s" watchdog (`main.cpp`'s "PATCH 2", `ESP.restart()`) — a full board
reboot, not just a Wi-Fi radio reset — since a real outage keeps the board
in SYN/extrapolated-time mode well past the 10s threshold; two independent
mechanisms compounding the total time-to-recovery. **Practical
implication**: any AP-side radio interruption — not just a full power
outage — could leave every gate on the network dropping triggers silently
for up to 5 minutes, with no operator-visible indication beyond the serial
debug log. At a real contest this reads as "the gates just stopped
working" for a very long five minutes.

**Resolution.** Decided 2026-07-31, re-examined from first principles
(the original 300-second value's justification isn't documented anywhere
in the code and isn't recalled either): the scenario the gate is actually
trying to guard against is the AP's TSF epoch resetting *mid-run*. On this
system's single-AP topology, the only plausible way for that to happen is
an AP radio failure/reset — which takes down Wi-Fi for every gate on the
network simultaneously, a total system failure regardless of what
hesperus's TSF logic does. A magnitude gate that stays shut for 5 minutes
doesn't protect against that (nothing left to protect at that point); it
only adds unnecessary downtime on top of it, and a much smaller, more
common trigger (a brief AP radio blip well short of a full outage) already
demonstrates this. The residual risk a gate might still be defending
against — a genuinely tiny/implausible `tsf_observed` value trusted right
after reconnect — isn't plausible for a single, non-mesh AP: the client
radio's own reconnection already takes several seconds, during which the
AP's TSF clock (if running at all) will have advanced well past any
near-zero danger zone.

**Decision: trust `tsf_observed` immediately on a fresh Wi-Fi association
event** rather than gating on the value's absolute magnitude at all.
Implemented 2026-07-31 (`hesperus-timing-gate/src/main.cpp`, the
initial-baseline gate at `!has_initial_baseline`): the `tsf_observed >=
MIN_PLAUSIBLE_TSF` check is replaced with `WiFi.status() == WL_CONNECTED`,
matching the connectivity check already used elsewhere in the file — since
this branch only ever runs while no baseline exists yet, checking current
connectivity right there achieves "trust once actually connected" without
needing a separate `WiFi.onEvent` handler/flag. Scoped narrowly to this
one gate — a second, separate use of `MIN_PLAUSIBLE_TSF` exists in the
"escape hatch" SYN-mode recovery heuristic (PATCH 1, `main.cpp:401-415`)
and was deliberately left untouched, not being what this issue documents
as the bug.

**Verification.** The original plan was a second ESP32 as a soft AP, fully
under test control, to check whether there is *any* way to take an AP's
radio down (even briefly) without resetting its TSF epoch. No spare ESP32
was available, so this was superseded the same day by a direct test: the
real venue AP's radio was toggled off and back on directly while collecting
logs from cerberus and the GOAL-board hesperus unit. **Bench-confirmed,
2026-07-31.** hesperus's GOAL board logged `[INITIALIZED] Valid Baseline
Coordinates Locked: 2408999` — a TSF value far below the old
`MIN_PLAUSIBLE_TSF` (300,000,000) that would previously have been rejected
outright — accepted immediately because the new gate only checks
`WiFi.status() == WL_CONNECTED`, not magnitude. This confirmed the soft-AP
assumption above didn't need to be separately proven: the real AP's
radio-only toggle did reset its TSF epoch (matching the original finding)
and the new code recovered from it immediately.

Full recovery sequence observed: losing AP sync tripped the existing
"PATCH 2" `[ROLLOVER FAULT]` watchdog after 10s stuck in `DISCIPLINED SYN`
mode, forcing a full `ESP.restart()` (expected, independent of this fix).
Post-reboot, the separate 15s "Wi-Fi link dead" watchdog cycled at least 3
times (~45s+) while the AP radio was still coming back up, consistent with
its normal ~30s recovery. Once reconnected: `[NETWORK] Link Active!` at
T=1136ms, mDNS resolved at T=2051ms, baseline locked at T=2775ms — **under
2 seconds after Wi-Fi actually came back**, not the old 5-minute wait.
Cerberus itself (never rebooted, just lost/regained Wi-Fi) reconnected in
~916ms and had both gates' WS clients back within a few seconds. One new,
unexplained data point from this same test: cerberus logged
`[E][ESPmDNS.cpp:148] addService(): Failed adding service http.tcp.`
immediately after reconnecting (non-fatal, mDNS started cleanly the very
next line) — see the WS-jitter issue below, flagged not investigated
further.

**Unexplained recurrence, 2026-08-03** (`test-data/spam-tests/{cerberus-6,
hesperus-start-6,hesperus-goal-6}.log`, the 5000-run no-spammer baseline
trial — see the acceptance-criteria section's results). The 5000 runs
themselves were flawless (5000/5000 ARM/START/GOAL, zero retries, zero
drops, one single WS disconnect/reconnect in the entire ~6-hour test). But
~64-84 minutes *after* the last logged race — no triggers in flight, both
boards just sitting connected and idle — both hesperus boards independently
logged `[AUDIT ALERT] Temporal Disruption!` (drift 515-814us) and dropped
into `[DISCIPLINED SYN]` extrapolated-TSF mode; the goal board's persisted
past 10s and tripped the same `[ROLLOVER FAULT]` reboot watchdog described
above, recovering cleanly within seconds (fresh boot, reconnect, valid
baseline re-locked). Same mechanism as the resolved bug above, but the
*trigger* this time is unidentified: confirmed with the user that the AP
showed nothing amiss and the beacon spammer was not running at the time.
Whatever caused the AP's TSF to hiccup (or caused hesperus's own reading of
it to) is unknown — logged here as an open, low-priority, self-healing
curiosity rather than investigated further, consistent with how the
WS-jitter issue below treats similarly rare, non-committed-time-affecting
blips. Worth re-checking if it recurs.

### Issue: Reliable delivery over persistent WS connection

**[RESOLVED]**
*(was Recommendation 6; Status section item 1)*

**Observation.** Persistent WS connections remove per-event connect cost
but introduce a class of risk per-event HTTP never had: the same logical
event can now legitimately be retried over one held-open connection, so
cerberus needs a way to recognise "this is the same event arriving twice"
(most likely because the original ack was lost, not because the sender is
confused) rather than reprocessing it into the race state machine a second
time.

**Resolution.** Implemented together, 2026-07-31, since retry is what
introduces the duplicate-delivery risk in the first place:

- **Event de-duplication** (`cerberus-gate-controller/src/net/gate-event-dedup.h`):
  a small fixed-size (16-slot, round-robin overwrite) table keyed on
  `(gate_id, event)` → last-seen `tsf_us`, checked in
  `handle_gate_event_json()` before `race_command_from_http()` dispatch. A
  recognised duplicate skips re-dispatch into the race state machine but
  still reports `handled = true`, so the caller still acks it — the most
  likely reason the same event arrives twice is that the *original* was
  already processed and only its *ack* was lost, so re-acking without
  reprocessing is correct, not an error. Keyed on `(gate_id, event)`, not
  `tsf_us` alone: `tsf_us` is a clock shared across every station on the
  AP, so two different boards' values could plausibly collide.
- **Ack/retry mechanism**: cerberus acks each handled WS event back to the
  sending gate over the same connection (`ws_event_handler()`, a
  hand-built `{"ack_tsf_us":...}` literal via `client->text()`). Hesperus
  (`uploadWorkerTask`) waits for that ack after sending, resending the
  exact same frozen payload (never re-derived — the TSF drift-audit engine
  that computes `tsf_to_transmit` mutates shared state as a side effect of
  running, so it must run exactly once per logical event) up to 5 attempts
  at a 300ms per-attempt timeout, bounded by a hard 2000ms overall
  deadline that applies even while disconnected (so a real Wi-Fi outage
  can't park the worker on one stale event while fresh ones overflow-drop
  from `networkQueue` behind it). Blocking, single-in-flight design,
  deliberately chosen over an async multi-pending-events table: matches
  the existing one-worker/one-event-at-a-time architecture exactly, at the
  cost of reintroducing burst-queueing behaviour during a lost-ack failure
  window specifically, not normally. Uses a real `vTaskDelay` between poll
  iterations rather than a bare `wsClient.loop()` spin, since
  `uploadWorkerTask` runs at a higher FreeRTOS priority than
  `ledDiagnosticTask`/`loop()` on the same core.

Native unit tests added (`test_gate_event_dedup.cpp`): same-key-same-tsf is
a duplicate, same-key-different-tsf isn't, different gate_id/event isn't,
and table wraparound behaves correctly past 16 distinct keys. Builds clean
(`pio test -e native`, `pio run -e cerberus-cyd2usb-diymalls-ili9341`).

**Verification.** Bench-confirmed 2026-07-31, cerberus + both hesperus
boards, in three parts:

1. **Normal-operation regression check**: one full ARM/START/GOAL cycle
   over real gate WS traffic (committed `3533ms`) and one full cycle via
   local NeoKey buttons (committed `1991ms`), both clean, no unexpected
   `[RACE]`/drop lines.
2. **Dedup+ack**, via `ws_send_event.py --tsf-us`: `NEW_MOUSE` →
   `ARM(10000)` → `<4,2>` (ARMED) → `START(20000)` → `<4,4>` (RUNNING) →
   resending the identical `ARM(10000)` produced **no state-change
   telemetry at all**, and the display stayed on `RUNNING` — confirming
   cerberus recognised the repeat as a duplicate and skipped reprocessing
   it. Without this fix, that resend would have hit
   `RaceState::RUNNING`'s "manual recovery, abandon run" branch and
   wrongly aborted the run in progress. (One methodological snag hit and
   resolved along the way: re-running the same `--tsf-us` values across
   separate attempts *within the same cerberus boot* makes the second
   attempt's own first `ARM` look like a duplicate of the first attempt's
   — the dedup table's memory persists for the whole boot, not just one
   test run — so repeat tests need fresh `tsf_us` values or a reboot in
   between.)
3. **Retry-on-genuinely-lost-ack**, bench-confirmed 2026-07-31: a
   temporary `TEST_DROP_FIRST_ACK` switch was added to
   `ws_event_handler()` (deliberately drop the ack on an event's first
   sighting only, using the existing dedup check to detect "first
   sighting" — no netem/proxy needed), and a full ARM/START/GOAL session
   was run with it enabled. Logs from both sides confirmed the complete
   loop: cerberus received each event, withheld the ack, and logged the
   drop; hesperus's `uploadWorkerTask` got no ack, waited out its
   per-attempt timeout, and resent the identical frozen payload (its own
   "Resent ... (TSF), attempt 2" log lines land ~300ms after the first
   send, matching the per-attempt timeout); cerberus's dedup recognised
   the resend as the same `(gate_id, event, tsf_us)` and acked it normally
   on that second delivery. Confirmed across `ARM`, `START`, and `GOAL`
   events in the same session, including back-to-back `GOAL`
   double-triggers, with no misbehaviour. The test switch was removed
   after confirming (the whole diff was scaffolding; reverted cleanly to
   HEAD).

### Issue: Hedged burst sends for tail latency

**[SPECULATIVE, deprioritized — see outstanding-work #4]**
*(was Recommendation 7; Experiment 7)*

**Observation / idea.** Gate triggers are sparse (one every 20+ seconds
per gate, sometimes minutes apart), so the traffic-volume cost of sending
redundant attempts is much smaller than it would be for a chatty protocol.
Idea: send the first few attempts (e.g. 5) as a rapid burst rather than
gating each on the previous one failing, then fall back to the existing
sequential retry loop for the remaining budget. Whichever attempt returns
first is used; the rest are redundant — the same principle as "hedged
requests" in distributed systems, trading some redundant work for a large
cut in tail latency. Two hard dependencies if built: cerberus-side dedup
(shipped, see the reliable-delivery issue above) becomes mandatory, not
optional, since multiple attempts from the same burst would routinely both
succeed; and each attempt needs to be numbered and logged on both sides
(which attempt actually "hit" first), turning the technique from a
plausible idea into a measured one — this is what the never-run validation
experiment below would use to check whether burst failures are independent
(supporting the technique) or correlated under congestion (undermining it).

**Resolution.** Not implemented. Re-assessed 2026-07-31 as weaker than
when proposed: no observed fault motivates it, and it predates the WS
ack/retry mechanism (see the reliable-delivery issue above), which already
recovers a lost send via bounded sequential retry. That mechanism now also
gives the right signal to decide *whether* hedging is worth building:
watch real-world retry counts/rate over normal operation first. If attempt
1 essentially always succeeds, hedging buys nothing measurable; if retries
are frequent, that's the concrete case for it — build from observed need,
not in advance of it.

**Verification.** N/A — not built, no fault observed yet to validate
against. If revisited: send a burst of ~5 attempts per event over a large
number of trigger cycles and record which attempt wins each time, both
under quiet conditions and under each of the congested-airtime stressors
(see outstanding-work #3 / `docs/TEST-TOOLING.md`) — a meaningfully
non-zero all-attempts-fail-together rate under congestion would indicate
correlated rather than independent failures, directly testing the
assumption the technique's benefit depends on.

### Issue: Unexplained minor WS jitter / reconnect blip

**[OPEN, low priority]**
*(was WS-rerun notes under Observation #8/#9; Experiment 8)*

**Observation.** The `trial_arm_then_start` and `trial_double_trigger` WS
re-runs (see their respective issues above) each showed one or a few
isolated single-event latency spikes (10-40ms, vs. ~6-9ms baseline) with
no shared cause with their paired event and no correlation to connection
start — cause not identified. Candidates: `uploadWorkerTask`'s
`wsClient.loop()` occasionally blocking on a keepalive/partial read, a
single 802.11 MAC-layer retry, or cerberus-side contention from its own
busier workload. n=100-200 per run so far is too small to see a pattern,
if one exists. Confirmed (by checking neighboring events/pairs) that this
is **not** the same phenomenon as a separate, larger blip: one instance of
a WS client connecting then disconnecting ~468ms later, followed by a
clean reconnect ~531ms after that, observed during a
cerberus-reboot-while-hesperus-stays-up test — self-healed, zero effect on
the subsequent test, two orders of magnitude larger than the jitter spikes
above. Investigated `net/wifi-manager.h`'s existing documented
`AsyncServer::begin()` PCB-orphaning gotcha as a plausible (not confirmed)
explanation for that larger blip — the `AsyncWebSocket` handler likely
inherits an analogous "first connection right after
`http_server_restart()`'s `.end()/.begin()` can be flaky" edge case. A
third, separately-noted data point plausibly related: cerberus logged
`[E][ESPmDNS.cpp:148] addService(): Failed adding service http.tcp.`
immediately after reconnecting during the AP-radio-toggle test (see the
AP-interruption issue above), non-fatal — plausibly the same underlying
mechanism as the connect/disconnect/reconnect blip, not confirmed.

**Resolution.** Not investigated further for any of these three data
points — too small/rare/self-healing to matter yet, none touch committed
times. Watch for recurrence rather than fix speculatively.

**Verification / proposed test, not yet run.** A single-gate `GOAL`-only
sequence, much longer and steadier than the existing trials — e.g. 10,000
messages at a fixed 250ms interval (well clear of any queueing effect) —
logged and run through `cerberus_log_stats.py --gaps`. At that volume,
look for: periodicity (a spike every N messages, pointing at a fixed-period
task like the heartbeat timer or a WS keepalive interval), clustering in
time, or drift/rate correlation. Would need `ares-pulse-generator`'s
`MAX_COUNT`/interval reconfigured (currently 100 at 1s spacing) — a
10,000-message run at 250ms is ~42 minutes per pass, worth planning for.
Would not, on its own, isolate *which* stage the delay happens in (hesperus
pickup vs. network transit vs. cerberus processing) — the current log only
has `tsf_us` (hesperus ISR time) and `recv_ms` (cerberus receipt time),
which bundles all three. If the pattern-hunt doesn't point at an obvious
cause, the next step would be adding a hesperus-side send timestamp to
split the latency into legs.

### Issue: `wsClient.loop()` blocking under congestion defeats the ack/retry deadline bound

**[RESOLVED, with a smaller residual issue tracked separately below]**
*(new, 2026-08-02; fix implemented and hardware-verified 2026-08-03)*

**Observation.** Manual stress test 2026-08-02: ARES set to generate
continuous full runs against a live cerberus + two hesperus boards, then
one and then a second Wi-Fi beacon spammer switched on partway through.
Only the GOAL gate's serial output was kept (not the full log, so
`networkq_overflow_count` reports are not available for this run). Early in
the log, sends succeed on attempt 1; as congestion increases, retries
appear and mostly follow the expected shape — attempts ~300ms apart
(`WS_ACK_TIMEOUT_MS`), "dropped after max retries" at ~1.5s, safely under
the 2000ms `WS_ACK_OVERALL_DEADLINE_MS`. But several drops instead show
almost no gap between the last logged resend and "dropped after ack
deadline" — e.g. `Resent ... attempt 3` then `dropped after ack deadline`
8ms later — while the *previous* resend was 7.6 real seconds earlier. That
7.6s is genuine elapsed time: the `[T=...]` log prefix is
`debug_timestamp_ms()` (`hesperus-timing-gate/src/debug-log.h`), which
reads the WiFi TSF hardware counter, not `millis()`.

That's inconsistent with the retry loop itself
(`hesperus-timing-gate/src/main.cpp`, `uploadWorkerTask`'s ack-wait loop,
~line 488 onward): it re-checks the 2000ms deadline every
`vTaskDelay(WS_ACK_WAIT_TICK_MS)` (5ms) iteration, so it shouldn't be able
to let 7+ seconds pass silently between checks. The only place that much
wall time can disappear without a log line is inside a single call to
`wsClient.loop()`. Leading hypothesis: the `WebSocketsClient` library
performing a blocking internal TCP reconnect (`WiFiClient::connect()`)
after the socket drops, which can stall for several seconds on a congested
channel — a known characteristic of that library, not investigated further
here since it's vendored (out of scope to read/modify per this repo's
CLAUDE.md). If correct, the consequence is real: the whole point of the
300ms/2000ms bounds is to cap how long `uploadWorkerTask` can stall on one
event so `networkQueue` (depth 10) doesn't back up; a multi-second block
inside `wsClient.loop()` defeats that, and ARES's continuous-run traffic
arriving during the stall would silently overflow-drop from the queue
rather than go through the visible retry/drop path — not confirmed in this
run since the overflow counter wasn't captured.

Directly relevant to the "Unexplained minor WS jitter / reconnect blip"
issue below, whose leading candidate cause is the same `wsClient.loop()`
call, just observed there at 10-40ms scale under quiet conditions rather
than seconds under real congestion.

**Confirmed, 2026-08-02** (`test-data/spam-tests/{hesperus-gate,hesperus-start,cerberus}.log`).
Re-ran with the proposed instrumentation in place (per-call `wsClient.loop()`
timing, logged above 50ms) and full unfiltered serial from both hesperus
boards and cerberus this time, not just GOAL. Scenario: continuous ARES
full runs; one beacon spammer switched on after ~15 runs, a second after
~30, first one switched off after ~40, second shortly after.

- Both boards hit the block directly: hesperus-gate up to 9450ms, hesperus-start
  up to 6233ms, several others in the 100ms-5s range, all logged with
  `wifi_status=3` (`WL_CONNECTED` — the underlying 802.11 association to
  the AP never drops) and `ws_connected` varying 0/1. So this is squarely a
  TCP/WebSocket-layer stall, not a loss of Wi-Fi association.
- Cross-referenced against cerberus's own WS accept/disconnect log — a
  source independent of hesperus's self-reported state. During the
  spammer-active window (T~206082107-145951, ~64s) cerberus recorded real
  connect/disconnect churn for both boards: hesperus-start (192.168.0.6)
  had a 16.1s outage (client #5 disconnect @082107 → #7 reconnect @098160)
  plus a connection that lasted only 1.1s shortly after; hesperus-gate
  (192.168.0.189) had an 18.1s outage (#6 @084550 → #8 @102681). These
  line up with the largest `wsClient.loop()` blocks — e.g. hesperus-gate's
  9450ms block at T=206097314 falls inside its own 18.1s outage window.
  Confirms the stall is a genuine two-sided TCP connection loss, not a
  one-sided reporting quirk in the client library.
- Secondary symptom: `[QUEUE OVERFLOW] ledQueue full; LED feedback dropped`
  fires alongside the worst blocks (T=206097316, T=206143632) — the stall
  starves more than just the network path, consistent with
  `uploadWorkerTask` running at higher FreeRTOS priority than
  `ledDiagnosticTask` on the same core.
- cerberus.log has zero `[E]`/error lines through the whole run — cerberus
  itself stayed healthy throughout; the instability is specific to the two
  hesperus (client) boards' connections.
- New context reframes the mechanism: a laptop on the same network
  sustained 25Mbps on a browser speed test with both spammers running,
  ruling out classic airtime/throughput saturation — the channel isn't
  actually full. Beacon spammers flood small, low-duty-cycle management
  frames rather than payload traffic, which a laptop's WiFi chipset filters
  largely in hardware. The ESP32's software-heavier WiFi stack processes
  more of that on the same CPU that also runs `uploadWorkerTask`/
  `wsClient.loop()`, which better explains why the ESP32-side connection
  destabilizes while a laptop on the same air doesn't notice anything. Not
  "the channel is congested" in the classic sense — beacon-frame volume
  overloading the ESP32's own processing path is the better-supported
  reading of the evidence so far.
- Race-outcome-level impact, walked through cerberus's own state telemetry
  in the same log: 64 distinct ARM triggers and 65 distinct START triggers
  arrived, and the race state machine entered `RUNNING` (`<4,4>`) 63 times
  — ARM/START came through almost intact. But only 54 of those runs ever
  reached `GOAL` (`<4,5>`) and committed a result — **9 runs (~14% of
  those armed and started) vanished with no GOAL ever arriving**, against
  60 distinct GOAL triggers cerberus received in total (a handful landed
  outside a `RUNNING` context and were correctly ignored). Not evidence
  that GOAL is transported worse than ARM/START — same `uploadWorkerTask`/
  `wsClient.loop()` path handles all three — but GOAL is the last event in
  a run, so it's the only one with no downstream step to mask a stall: an
  ARM/START hiccup that eventually lands via retry leaves no visible
  trace, while any GOAL lost during one of the multi-second
  `wsClient.loop()` stalls above shows up directly as a run with no
  recorded time. (Separately, and not a bug: every committed run in this
  log reads exactly `<13,3000>` — `ares-pulse-generator`'s `trial_full_run()`
  waits exactly `RUN_DURATION_MS` = 3000ms between firing START and GOAL
  by construction, so a fixed 3000ms result is the synthetic trial working
  correctly, not a lost-signal fallback. The message appears 3x per run,
  20ms apart, `162` lines / `3` = the same `54` completed runs above — RATS
  V2's deliberate line-noise mitigation, already documented in
  `messages-reference.h`/TODO.md, unrelated to this issue.)

**Resolution, 2026-08-03.** Implemented in `hesperus-timing-gate/src/main.cpp`:
decoupled the WS socket pump from `uploadWorkerTask`'s ack-wait deadline
logic, without switching networking libraries (no async WebSocket *client*
exists in this ecosystem — `ESPAsyncWebServer`'s `AsyncWebSocket` is
server-only, so a client-side rewrite would mean hand-rolling WS framing on
raw `AsyncTCP`, out of proportion for this fix). A new task, `wsPumpTask`,
becomes the sole caller of `wsClient.loop()`. Two new mutexes:
`ws_client_mutex` guards every call into `wsClient` (`.loop()`, `.sendTXT()`,
`.isConnected()`, `.begin()`, `.enableHeartbeat()`, `.onEvent()`) —
`wsPumpTask` holds it for the full duration of each `.loop()` call (the one
place still allowed to block for however long a stall runs), while every
other caller (`uploadWorkerTask`'s send/resend, `loop()`'s reconnect edge)
takes it with a short bounded timeout (`WS_MUTEX_SEND_TIMEOUT_MS`=20ms,
`WS_MUTEX_SETUP_TIMEOUT_MS`=50ms) via new `wsIsConnectedBounded()`/
`wsSendTxtBounded()` wrappers, treating a failed acquisition as "skip this
attempt / not connected" rather than blocking. `ws_ack_state_mutex` guards
`(g_ws_ack_received, g_ws_ack_tsf_us)` as one unit — needed because
`g_ws_ack_tsf_us` is a non-atomic `uint64_t` write on this 32-bit hardware,
and the ack callback now genuinely fires from a different task
(`wsPumpTask`) than the one reading it (`uploadWorkerTask`), unlike before.
`uploadWorkerTask`'s ack-wait loop no longer calls `wsClient.loop()` at all
— it only polls the ack flag (`wsTakeAckIfReceived()`) and its own
`millis()` timers, so its 300ms/2000ms deadline checks run on schedule
regardless of how long `wsPumpTask` is stalled. The `wsClient.loop()`-timing
diagnostic (added 2026-08-02) was relocated into `wsPumpTask` and kept
permanently as a low-cost canary (only logs above 50ms) rather than
stripped, resolving that open question in its favor.

Also added, same session, as a separate additive changeset (Track 2, not
touching the above): an on-demand diagnostics HTTP server
(`hesperus-timing-gate/src/net/debug-http-server.h`, `feature_http` now
extended by all 4 hesperus envs) exposing `GET /logs` (an in-RAM 150-line
ring buffer of recent debug output, captured via a new `debug-log.h` line
hook) and `GET /status` (uptime, RSSI, queue depth, overflow count) — motivated
by wanting to remotely interrogate a hesperus board's own logs without a
serial connection. Confirmed comfortable RAM headroom on both non-PSRAM C3
envs (19% used / 81% free after linking `AsyncTCP`+`ESPAsyncWebServer`).

**What this does and doesn't fix**: bounds worst-case per-event stall to
~2000ms regardless of how long the underlying `wsClient.loop()` stall runs,
and stops `uploadWorkerTask` itself from starving `ledDiagnosticTask`/
`cli.poll()`. Does **not** fix the root RF-level cause (genuine ~16-18s
two-sided TCP outages under beacon spam, still open/unscoped) — events lost
during a real outage are still lost, just via a clean bounded drop instead of
an unbounded stall. Also does not resolve whether `wsPumpTask` stalling (same
priority/core as `uploadWorkerTask` was) still starves `ledDiagnosticTask` via
a different task — an empirical question left to the verification pass below,
not assumed either way.

All 4 hesperus envs (`pio run`) build clean with both changesets.

**Hardware-verified, 2026-08-03** (`test-data/spam-tests/{cerberus-2,hesperus-2-goal,hesperus-start-2}.log`).
Re-ran the same beacon-spam scenario: continuous ARES full runs, first
spammer on after ~15 runs, second spammer on after ~15 more — per the user,
the second spammer "effectively killed the system," recovering once it was
switched off.

- Computed every `attempt 1` → `dropped` delta across both hesperus logs:
  `2064ms, 2536ms, 2005ms, 2003ms, 2001ms` (5 of 6 within ~500ms of the
  intended `WS_ACK_OVERALL_DEADLINE_MS`=2000ms — a dramatic improvement over
  the original bug's signature of 7.6s+ gaps completely untethered from the
  deadline) and one outlier at **6611ms** (3.3x over target).
- The 6611ms outlier (and, on closer look, the 2536ms one too) is a
  different, smaller-magnitude phenomenon, not the original bug recurring —
  but the first-pass explanation for it (FreeRTOS scheduler contention
  delaying `uploadWorkerTask`'s own deadline loop) turned out to be too
  hasty and was superseded same-day by a better-supported one, below. Kept
  here crossed out rather than silently deleted, since it shaped the first
  write-up: ~~`wsPumpTask` was stuck across several consecutive `.loop()`
  calls back-to-back, and during that stretch `uploadWorkerTask`'s own
  `vTaskDelay(5ms)`-paced deadline-check loop itself ran late, pointing to
  genuine FreeRTOS scheduler contention under the worst moment of beacon
  flooding.~~
  **Superseded finding, same day**: cross-referencing `cerberus-2.log`'s raw
  `[WS] DATA` receipt lines against the exact frozen `tsf_us` hesperus was
  retrying shows cerberus actually **received the event multiple times
  during hesperus's own wait window, for both outlier cases**, and never got
  an ack back in time regardless:
  - GOAL (2536ms case): hesperus retried `tsf_us=209988167825` at
    T=209988170/477/779/990700(hesperus-side timestamps); cerberus logged
    receiving that exact payload **four times**, at T=209988746, 989326,
    990243, 991100 — the first three all inside hesperus's own wait window
    (deadline hit at 990706).
  - ARM (6611ms case): `tsf_us=209976367966` received by cerberus at
    T=976573, 977579, 977583, 983237, while hesperus waited from 976370 to
    982981 — again, multiple deliveries landed well inside the window.

  So in both outlier cases the forward path (hesperus → cerberus) was
  intermittently working — repeatedly — while the ack never made it back in
  time either way. That points at the **ack return path** (cerberus slow to
  send it, or the ack itself being lost/delayed asymmetrically under the
  same RF conditions) rather than at `uploadWorkerTask`'s own scheduling.
  Not yet isolated further — see the new issue immediately below, which is
  the natural next step and was still unstarted when this session ended.
- Race-outcome accounting on `cerberus-2.log` (same method as the original
  investigation): 41 distinct ARM and 41 distinct START triggers, 41
  `RUNNING` entries, 38 `GOAL` entries/committed results — a ~7% loss rate
  among armed+started runs, better than the original test's ~14%, despite
  this run reportedly being more severe. Not a strictly controlled A/B
  comparison (different session, different exact run count/duration), but
  consistent with the fix helping rather than a regression.
- `[QUEUE OVERFLOW] ledQueue full` still occurred once, during the worst
  window. Answers the open question from the design, but not in the
  direction of "solved": since `uploadWorkerTask` can no longer itself block
  on `wsClient` by construction, something else is still starving
  `ledDiagnosticTask` under the worst congestion. Cause not identified —
  don't assume it's the same mechanism as the ack-path finding above without
  checking; could be genuine CPU/scheduler contention from beacon-frame
  processing, or something else entirely. Open.
- Stack headroom (`uxTaskGetStackHighWaterMark()`) not yet checked after a
  soak run — still open.

Note: `cerberus-2.log` also shows cerberus's own boot banner partway through
the capture (WS clients disconnecting at `[T=0]`, then a fresh
`CERBERUS: gate controller...` banner) — this is a manual restart performed
at the start of the trial, not a runtime crash; confirmed by the user. Not a
finding, just recorded here in case anyone rereads that raw log later and
wonders about the `[T=0]` lines.

**Not yet done**: `uxTaskGetStackHighWaterMark()` soak check; the ack-path
investigation in the new issue immediately below (this is the priority next
step, unstarted); deciding whether the `ledQueue` overflow shares a cause
with the ack-path finding or is separate.

### Issue: acks not arriving back at hesperus in time despite cerberus receiving the event

**[OPEN — priority next step, unstarted]**
*(new, 2026-08-03, found while reviewing the `wsClient.loop()` fix's
verification data above)*

**Observation.** In both residual outlier cases from the 2026-08-03
verification (GOAL 2536ms, ARM 6611ms — see above), cross-referencing
`cerberus-2.log`'s raw `[WS] DATA` lines by the exact frozen `tsf_us` hesperus
was retrying shows cerberus received the event **multiple times**, well
inside hesperus's own wait window, and no ack ever reached hesperus in time
regardless:

- GOAL: `tsf_us=209988167825` — cerberus received it at T=209988746, 989326,
  990243, 991100 (cerberus-side TSF timestamps); hesperus's wait window ran
  T=209988170 to 990706 (dropped). First three cerberus receipts fall inside
  that window.
- ARM: `tsf_us=209976367966` — cerberus received it at T=976573, 977579,
  977583, 983237; hesperus's window ran 976370 to 982981. Again, multiple
  receipts land inside the window.

Both boards' `wsClient.loop()`s were mid-stall around this same time (per
the relocated diagnostic — see the resolved issue above), so this isn't
necessarily "cerberus is slow" in isolation; hesperus's own client may not
have been cycling fast enough to *process* an ack that cerberus sent
promptly, since incoming frames are also handled inside the same
`wsClient.loop()` call that the mutex redesign still lets block for however
long a stall takes. That's a live, undecided alternative explanation, not
ruled out — the instrumentation below is what would distinguish it from a
genuine cerberus-side or return-path problem.

**Instrumentation added, 2026-08-03** (`cerberus-gate-controller/src/net/http-server.h`,
`ws_event_handler()`'s `WS_EVT_DATA` branch): a new unconditional (not gated
behind `g_debug_verbose_enabled`) log line, `[WS-ACK] tsf_us=... recv=...
dispatch=... sent=... text_ms=...`, captured around the existing
`client->text(ack)` call — `recv` is DATA-receipt time, `dispatch` is just
before `client->text()`, `sent` is just after, `text_ms` is the difference
(isolates time spent inside `client->text()` itself from cerberus's own
JSON-parse/dedup/dispatch processing, which is `dispatch - recv`). Builds
clean (`pio run -e cerberus-cyd2usb-diymalls-ili9341`). Not yet
hardware-verified against a real beacon-spam run — the 2026-08-03 session-2
stress test (analysed immediately below) was captured *before* this
instrumentation was flashed, so it has no `[WS-ACK]` lines; a re-run with it
in place is still the next step.

**Analysed without the above instrumentation, 2026-08-03 (session 2)**
(`test-data/spam-tests/{cerberus-3,hesperus-start-3,hesperus-3-goal}.log`) —
directly answers this session's "is cerberus failing?" question, using only
data already available (raw `[WS] DATA` receipt lines against hesperus's own
retry/drop log), same cross-referencing method as the outlier analysis
above. Scenario: 15 runs with no spammer, 15 with one spammer, then a second
spammer switched on (link drops), then the second spammer switched off (link
recovers, a few more runs recorded), then the first switched off.

- **Cerberus itself stayed healthy throughout**: zero `[E]` error lines,
  zero restarts/reboots (`ESP-ROM` banner appears exactly once per file, at
  each hesperus board's own boot at the start of the capture, not mid-session
  — ruling out a repeat of the earlier false-alarm "restart looks like a
  crash" reading). Both boards' WS connections dropped exactly once each
  during the two-spammer window (client #3/hesperus-start disconnect at
  T=239499727, reconnect T=239520226; client #4/hesperus-goal disconnect
  T=239505029, reconnect T=239520246 — ~20.5s and ~15.2s outages
  respectively) and recovered cleanly with no further churn.
- **Race-outcome accounting** (distinct `tsf_us` per event, not raw
  duplicate-inclusive line counts): 41 distinct `ARM`, 42 `START`, 43 `GOAL`
  triggers; 41 `RUNNING` entries (`<4,4>`), 39 committed results (`<4,5>`) —
  2 of 41 armed+started runs (~5%) never got a committed `GOAL`, similar to
  (slightly better than) the original stress test's ~7%.
- **New finding: at least one "lost" event by hesperus's own bookkeeping was
  not actually lost at cerberus.** GOAL `tsf_us=239494634838`: hesperus sent
  attempt 1 at T=239494637, resent through attempt 5 (T=239495850), and
  logged `Event dropped after max retries` at T=239496151, having given up
  waiting for an ack. But cerberus's own `[WS] DATA` log shows it received
  that exact payload at T=239496925 — **774ms after hesperus had already
  moved on** — and correctly committed it (`<4,5>` immediately follows in
  the log). The same payload arrived at cerberus a second time, T=239502033,
  ~5.1s later still — a genuine duplicate at the transport level, correctly
  recognised and no-op'd by the existing dedup rather than double-committed.
  This reframes the open question: it isn't only "ack lost on the return
  leg" (the outlier analysis above) — under the worst congestion, the
  **forward** leg itself can take over a second longer than hesperus's own
  2000ms deadline budget allows for, with cerberus's receive/commit path
  itself not implicated as slow. (Can't yet separate "cerberus slow to ack"
  from "forward transit slow" with certainty for the *ack* itself — that's
  what the instrumentation above is for — but this specific case shows
  cerberus was neither slow nor wrong once the data did arrive.)
- **New failure mode observed, not present in the 2026-08-02/2026-08-03
  session-1 logs**: `[WS Worker] Link down. Event dropped.` — fires when
  `wsIsConnectedBounded()` returns false (real disconnect, or
  `ws_client_mutex` held because `wsPumpTask` is itself mid-stall), and
  drops the event **immediately with zero retry attempts** (skips straight
  past the whole ack-wait loop). Occurred 9 times across both boards
  (T=239495755–239517456), clustered right around and during the two
  `wsClient.loop() blocked ~5002ms` stalls that mark the worst of the
  two-spammer window — i.e. this is the mechanism by which events are lost
  outright during a genuine multi-second outage, distinct from (and more
  severe than) the ack-timeout/max-retries paths, which at least attempt a
  send. Expected given the design (an unreachable link has nothing to send
  to), not a new bug — but worth naming explicitly since it hadn't shown up
  in a captured log before this run.
- **Conclusion so far: no evidence cerberus itself is the failing
  component.** No crashes, no errors, correct dedup under duplicate
  delivery, correct commit once data arrives. The ~5% run-loss rate is
  consistent with the already-documented forward-path/outage mechanism
  (the `wsClient.loop()`-blocking issue above, root RF cause still open per
  outstanding-work item 5), not a new cerberus-side defect. Still open:
  direct confirmation that cerberus's *ack dispatch* is uniformly fast even
  under this level of congestion — the instrumentation above hasn't been
  run against real beacon-spam conditions yet.

**Resolution.** Root cause itself (AsyncTCP write-completion timing vs.
genuine return-leg RF loss — the two candidates the session-3 hardware
verification above narrowed it to) still not identified. **Mitigation
implemented instead, 2026-08-03** (`hesperus-timing-gate/src/main.cpp`,
same constants block as the WS ack/retry design): rather than chase the
root cause further immediately, tuned hesperus's own (fully in-scope,
non-vendored) retry schedule to absorb more of the residual loss regardless
of which of the two candidates turns out to be right:

- `WS_MAX_SEND_ATTEMPTS`: 5 → 10. Extra attempts only ever fire on a
  timeout, so a healthy send (the normal case) pays none of this cost —
  only the congested tail gets more chances.
- `WS_ACK_TIMEOUT_MS` kept at 300 (not shortened) — deliberately, since
  shortening it would mean retrying *more* aggressively into an
  already-congested channel, working against standard backoff practice.
  New `WS_ACK_TIMEOUT_JITTER_MS` = 60 randomises each attempt's actual
  timeout to 240-360ms, re-rolled per attempt — breaks the fully
  deterministic retry schedule that let both gate boards' retries (or
  retries vs. ARES's own periodic traffic) lock-step and collide
  repeatedly under congestion.
- `WS_ACK_OVERALL_DEADLINE_MS`: 2000 → 3200. Necessary side-effect of
  raising max attempts — at ~300ms/attempt, 10 attempts need ~2700-3000ms
  of room to actually occur; left at 2000 they'd never be reached, since
  the overall deadline is a hard cap that fires independently of the
  attempt counter. Raises worst-case per-event stall on
  `uploadWorkerTask` from 2000ms to 3200ms — acceptable given events are
  sparse (one every 20+ seconds in a real race) and `networkQueue` is
  depth 10, but worth naming explicitly as the tradeoff for the extra
  attempts.

All 4 hesperus envs (`pio run`) build clean. Not yet hardware-verified —
next step is re-running the same beacon-spam stress scenario to see whether
loss rate/outlier frequency improves.

**Verification / next steps.**

1. ~~Instrument `cerberus-gate-controller/src/net/http-server.h`'s
   `ws_event_handler()`~~ — **done, 2026-08-03**, see above. Not yet
   hardware-verified.
2. Re-run the same beacon-spam stress scenario with the new instrumentation
   in place, capturing cerberus's serial output too (session 2, analysed
   above, predates this instrumentation).
3. If cerberus's own ack-dispatch is fast every time, that rules cerberus
   out and points at either the return path (asymmetric RF loss/delay) or
   hesperus's own `wsClient.loop()` not being free to process the incoming
   ack promptly during its own stalls — cross-reference against hesperus's
   `[WS Pump] wsClient.loop() blocked` lines for the same window to
   distinguish these. Session 2's finding above (forward-path delay, not
   just return-path) is also now a live candidate to weigh against these.
4. If cerberus's ack-dispatch itself is slow, look at whether it's
   `AsyncTCP`'s own send path getting backed up under the same connection
   churn cerberus was juggling from both boards during this window.

**Hardware-verified, 2026-08-03 (session 3)**
(`test-data/spam-tests/{cerberus-4,hesperus-start-4,hesperus-4-goal}.log`) —
first run captured with the `[WS-ACK]` instrumentation in place, against a
repeat of the same scenario (15 runs no spammer, 15 with one spammer, then
a period with two spammers where the link drops, second spammer off and
link recovers, first spammer off; this run also had an accidental brief
reactivation of the second spammer near the end, ~T=240826777, which only
produced two harmless retries, no drop). Race outcome this run: 45
`RUNNING` entries, 44 committed — only 1 loss (~2%), the best of the three
stress runs so far (vs. ~7% and ~5% previously); not a controlled
comparison (different session/duration), but not evidence of a regression
either.

- **Cerberus's own ack dispatch is fast, unconditionally.** All 159
  `[WS-ACK]` lines this run: `recv`→`dispatch` (JSON parse + dedup check)
  mean 2.7ms, max 5ms; `client->text()` call itself mean 4.9ms, max 11ms;
  total `recv`→`sent` mean 7.5ms, max 15ms — including samples taken during
  the worst of the two-spammer outage. **This rules out "cerberus is slow
  to compute/dispatch the ack" as a cause of the residual outliers**: even
  under the heaviest congestion in this run, cerberus's application-level
  path from receiving a DATA frame to calling `client->text()` never
  exceeded 15ms.
- **But that doesn't mean the ack reliably gets back.** Cross-referencing a
  specific case: GOAL `tsf_us=240753368700` — hesperus sent attempt 1 at
  T=240753371, resent through attempt 5 (T=240754584), and logged `Event
  dropped after max retries` at T=240754884. Cerberus's `[WS-ACK]` log shows
  it received and **application-acked this exact event four separate
  times** in that same window — T=240753839, 240754621, 240754627,
  240754632 (all `text_ms` 3-11) — every one of them *before* hesperus's own
  754884 drop. hesperus-4-goal.log shows **no `[WS Pump] wsClient.loop()
  blocked` line anywhere in this window** (the nearest is at T=240770419,
  15+ seconds later) — if `wsPumpTask` had been mid-stall and simply not
  polling for the incoming ack, that would show up as a logged block past
  the 50ms threshold, and it doesn't. So the "hesperus's own client isn't
  free to process an ack that arrived fine" theory doesn't fit this case
  either: the pump task appears to have been running normally, yet none of
  four independent, promptly-dispatched acks were recognised.
- **Reading**: this narrows the field to two remaining explanations, neither
  yet distinguished: (a) `client->text()` returning quickly only means
  `AsyncTCP` accepted the write into its own outbound queue, not that the
  frame left the radio promptly — the `[WS-ACK]` timestamps measure the
  call, not the actual on-air transmission, so a backlog inside `AsyncTCP`'s
  own send path (per next-step 4 above) is still untested and not ruled
  out; or (b) genuine asymmetric loss on the return leg specifically — the
  forward direction (hesperus→cerberus) has 5 retry attempts stacked in its
  favour and got through repeatedly, while cerberus's dedup-driven repeat
  acks, though numerous here, still each ride a single transmission with no
  ack-specific retry of their own. Both remain open; distinguishing them
  would need either instrumenting `AsyncTCP`'s write-completion callback
  (not just the call to `client->text()`) on cerberus, or a packet capture
  on the return leg specifically.

## Decision record: ESP-NOW alternative

Worth recording explicitly, since it was raised and weighed rather than
overlooked: an earlier, separate experiment used ESP-NOW as the message
transport and found it unimpressive at getting messages through in a
genuinely congested radio environment.

The reasoning for sticking with infrastructure Wi-Fi/TCP (the persistent
connection issue above) despite that isn't "TCP is more reliable over the
air" — it likely isn't. ESP-NOW operates near the raw 802.11 MAC layer with
no AP association, no DHCP, no TCP handshake — it skips essentially all the
overhead infrastructure Wi-Fi carries. If it still performed poorly under
congestion, that points to the underlying RF channel itself (collisions,
interference, noise floor) as the bottleneck, not protocol overhead — and
that's shared physics: infra Wi-Fi transmits on the same spectrum, subject
to the same contention, plus everything ESP-NOW sidesteps. So infra Wi-Fi
is not obviously going to get an individual frame through a congested
channel any better than ESP-NOW did — it could plausibly be worse per
attempt, given it has strictly more that can go wrong.

What decides it instead is a build-vs-buy tradeoff. TCP already contains a
mature, extensively tested answer to "detect and recover from a lost
packet" — retransmission, ordering, connection-liveness — none of which has
to be designed, implemented, or debugged from scratch. ESP-NOW would be
leaner and lower-latency when a message gets through, but when it
inevitably loses one, a comparable raft of detection-and-recovery measures
would need to be built by hand on top of it. The extra airtime infra
Wi-Fi/TCP costs is the price of not having to invent and maintain that
machinery — several existing layers of resilience today (see the
reliable-delivery and misattribution issues above), rather than a bespoke
protocol built up one edge case at a time.

## Restructuring note, 2026-07-31

This document originally grew as four parallel, chronological sections
(Observations, Recommendations, Proposed experiments, Status-as-of-date)
with each issue's story scattered across all four and dated notes bolted
onto the end of each as work progressed — hard for a newcomer to see "what
is issue X, is it fixed, what's still open" without cross-referencing the
whole document. Restructured into the issue-based
Observation/Confirmation/Resolution/Verification format above, with a
prioritized outstanding-work list moved to the front. No content was
removed — every measurement, log excerpt, and file reference from the
original has a home in one of the issues above. Tool-usage details
(command syntax, flags) were moved out to the new `docs/TEST-TOOLING.md` so
they're documented once instead of repeated across issues; issues here
reference tools by name only.

Old section numbers (`Observation #N`, `Recommendation N`, `Status section
item N`) are preserved as parenthetical tags under each issue's heading,
since ~15 comments across 9 source files
(`race-timer.h`, `http-server.h`, `gate-event-dedup.h`, hesperus/cerberus/
ares `main.cpp`, `ws_send_event.py`, `cerberus_log_stats.py`,
`tools/testing/README.md`) reference this document by those old numbers —
those references still resolve correctly against the tags below each new
heading.
