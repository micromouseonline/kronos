# Network Timing Issue: Observations, Conclusions, and Recommendations

Date: 2026-07-30

## Context

`hesperus-timing-gate` boards report race events (`ARM`, `START`, `GOAL`) to
`cerberus-gate-controller` over HTTP (`POST /api/event`), with disciplined
Wi-Fi-TSF-based microsecond timestamps carried in the payload. Bench testing
the two boards together (a start board and a goal board, both HTTP clients of
a single cerberus controller) surfaced a chain of related issues in network
latency, its effect on displayed race times, and its effect on race-result
correctness. This document records what was found, why it happens, and what's
recommended to address it.

## Observations

### 1. HTTP retry storm (resolved)

Every trigger initially produced exactly four identical `POST /api/event`
requests at cerberus, matching `MAX_HTTP_RETRIES` precisely. Root cause:
`HTTP_TIMEOUT_MS` was set to 250ms, which the real round-trip time
consistently exceeded — hesperus exhausted all four retry attempts on every
single event, even though cerberus was receiving and correctly processing
every one of them. Raising `HTTP_TIMEOUT_MS` to 2000ms initially resolved the
duplicate-POST pattern.

### 2. The hidden second timeout

Raising `HTTP_TIMEOUT_MS` didn't fully explain later data: a "confirmed"
event was observed taking 2993ms — longer than the configured 2000ms timeout,
yet still succeeding. Inspecting the ESP32 Arduino `HTTPClient` source
(`framework-arduinoespressif32/libraries/HTTPClient/src/HTTPClient.cpp`)
showed why: `HTTPClient::setTimeout()` only sets `_tcpTimeout`, the
**response-read** timeout, applied only *after* a TCP connection is already
established. The TCP **connect** phase uses a separate `_connectTimeout`,
defaulting to a hardcoded `HTTPCLIENT_DEFAULT_TCP_TIMEOUT = 5000` (`HTTPClient.h:42`)
that nothing in the firmware had ever configured. In other words, tuning
`HTTP_TIMEOUT_MS` never bounded the dominant source of latency at all.

### 3. Measured latency breakdown (via a shared TSF timeline)

Both boards were given TSF-based (`esp_wifi_get_tsf_time(WIFI_IF_STA)/1000`)
millisecond timestamps on every debug log line — hesperus and cerberus are
both stations on the same AP, so this is a shared, sub-ms-precision clock
across boards, letting logs from each be correlated directly. Using
`tsf_us` (captured in hesperus's ISR at the instant of the physical trigger)
against cerberus's own logged receipt time gives the true one-way
"trigger → cerberus received" latency, independent of the return trip:

| Event | One-way (trigger → received) | Full round trip (hesperus-measured) | Implied return leg |
|---|---|---|---|
| GOAL | ~132ms | 146ms | ~14ms |
| START | ~270ms | 284ms | ~14ms |
| ARM | ~267ms | 285ms | ~18ms |

Outliers of 856ms (first-ever connection in a session) and ~2.9s (later in
the same session) were also observed on single attempts that still returned
`httpCode == 200`. Given finding #2, both are consistent with TCP connect-phase
trouble (e.g. a lost SYN packet triggering the underlying stack's own
retransmission), not anything bounded by `HTTP_TIMEOUT_MS`.

**Conclusion: essentially all latency, and effectively all of its variance,
is attributable to TCP connection establishment** — paid fresh on every
single event, since no connection reuse/keep-alive exists (`http.begin()`/
`http.end()` open and close a new connection each time). Cerberus's own
processing (JSON parse, dispatch, queue, respond) is fast and consistent —
the ~14-18ms return leg above — and is not a meaningful contributor.

### 4. Wi-Fi power-save latency

Hesperus originally used `WIFI_PS_MAX_MODEM` (aggressive power saving). A
~2.9s latency spike was traced to modem-sleep wake-up: the radio sleeps
between sparse events — exactly the ARM/START/GOAL spacing of a real race —
and must wake before it can transmit. Cerberus's own code already disables
power-save (`WIFI_PS_NONE` in `net/wifi-manager.h`) for this same reason;
hesperus was changed to match. However, a subsequent test *after* switching
to `WIFI_PS_NONE` still showed a ~2993ms outlier — confirming modem sleep was
not the sole cause. The TCP connect-phase issue (#2/#3) is a separate,
additional contributor that disabling power-save alone does not fix.

### 5. Battery/power budget constraint

Gates are battery-powered, and batteries above 500mAh are hard to fit
physically. `WIFI_PS_NONE`'s continuously-active radio draws considerably
more current than a modem-sleep mode — general ESP32 figures put power-save-off
in the 80-150mA range versus roughly 15-30mA average with modem sleep enabled
(these are generic chip figures, not measured on this hardware/firmware, and
should be verified with real measurement before relying on them). Against a
sub-500mAh budget, that's plausibly the difference between a few hours and
most of a day of runtime.

### 6. Displayed run time vs. true TSF-based time (resolved)

Cerberus's `race-timer.h` starts and stops its `run_sw` `Stopwatch` using
cerberus's own local `millis()`, captured at the moment it processes each
`SystemEvent` — not the `tsf_us` timestamp carried in the HTTP payload. This
means the *displayed* run time differs from the true TSF-computed elapsed
time by exactly `L_goal − L_start` (the **difference** between the two legs'
network latencies, not either one individually, and not their sum). If
latency happened to be identical on both legs, this error would cancel to
zero even though the display itself runs a constant offset behind the real
event. Real latency is not constant (per #3), so asymmetry between the two
legs injects directly, and visibly, into the displayed number — this is a
spectator-facing appearance problem, not a data-accuracy problem: the true
elapsed time is already recoverable precisely from `tsf_us` differences today,
independent of any of this.

**Resolved 2026-07-30** — see recommendation 4 for what was actually shipped.
The first attempt (backdating `run_sw` itself at both `START` and `GOAL` using
`tsf_us`, as originally proposed below) made the *committed* time exact but
introduced a worse spectator-facing problem than the one it fixed: the live
display, ticking in real time off a backdated start, correctly kept counting
past the true finish for however long the `GOAL` message's own network leg
took to arrive, then visibly snapped backward once it did. Bench-confirmed
(`ares-pulse-generator`'s `trial_four_runs`, four back-to-back runs of known
2000/3000/4000/5000ms duration) before being replaced by the adopted design.

### 7. Race state-machine event-ordering risk (most serious finding)

The race state machine assumes events arrive in logical order (`ARM` →
`START` → `GOAL`) and has no way to tell which race *attempt* an event
belongs to. Pathological scenario: a `GOAL` event is delayed (by retries or
connect-phase trouble) long enough that a new attempt is armed and started
before the stale `GOAL` finally arrives.

- If it lands while cerberus is back in `ARMED`, it's silently dropped
  (`ARMED` only reacts to `START`/`RESTART`) — the true result is lost, but
  the *new* attempt isn't corrupted.
- If it lands while cerberus is `RUNNING` on the *next* attempt, it is
  accepted **unconditionally** — `RaceState::RUNNING`'s handler treats any
  `GOAL` at face value — silently ending the new run early with a bogus
  committed time. Nothing distinguishes "this GOAL belongs to the current
  attempt" from "this is a stale message from a previous one."

This is a structural gap in the event contract, not a display cosmetic.
Persistent connections and more aggressive retry (see recommendations) reduce
the probability of this scenario substantially but do not eliminate it
structurally.

### 8. Confirmed by direct bench test: burst latency is queueing on hesperus, not the network

Two follow-up bench tests isolated the queueing theory from #3 directly, using
the same `receipt_ms*1000 − tsf_us` method against a rapid sequence of real
`GOAL` triggers.

**Test A — rapid-fire triggering (~88 events, spacing mostly 60-800ms):**
one-way latency ramped smoothly from ~110ms up to 2773ms over each ~8s burst,
resetting back down whenever a multi-second pause let the backlog drain.
Latency growth per event correlated directly with inter-trigger spacing: gaps
under ~130ms added ~140-190ms of extra latency each; gaps over ~250ms let
latency drain instead of growing. This is a queueing signature, not network
jitter or clock drift (hesperus's own `tsf_observed` and `processor_clock`
tracked each other to within tens of microseconds throughout, ruling out any
clock-discipline artifact on the gate). The mechanism: `uploadWorkerTask`
(`hesperus-timing-gate/src/main.cpp:238`) drains `networkQueue` one event at a
time, and each full connect+POST+confirm cycle costs ~230-270ms (matching the
#3 baseline exactly) — triggers arriving faster than that queue up and each
waits longer than the last for its turn, on top of whatever the network
itself costs.

**Test B — control, same trigger source, ~1-2s spacing (~87 events):** mean
234.6ms, median 259.5ms, p90 319ms, p95 345ms, stdev 102ms. Only one outlier,
818.8ms — the very first request of the session (cold connection), closely
matching the ~856ms first-connection figure already noted in #3. A few
switch-bounce pairs (~140-235ms apart) showed a mild one-off bump
(~300-385ms) but never compounded, since the next event always had enough of
a gap to drain the queue first. This confirms Test A's spike was purely a
by-product of sustained rapid-fire triggering outrunning hesperus's
one-event-at-a-time send cycle, not a new or separate network problem — at
realistic trigger spacing, latency stays in the range #3 already documented.

**Practical implication, confirmed by a third, controlled bench test:**
`networkQueue` is only 10 deep (`hesperus-timing-gate/src/main.cpp:452`). At
a ~250ms service time, a sustained trigger rate faster than that for more
than ~2.5s overflows it — silently dropping events, not just delaying them.
`ares-pulse-generator`'s `trial_burst` fired 40 `GOAL` pulses at a fixed,
known 90ms interval (raw data/analysis in
`test-data/trial-burst-20260630-1542*.txt`); only 24 reached cerberus. The
other 16 are independently confirmed dropped two ways at once: hesperus's own
serial log showed `[QUEUE OVERFLOW] N networkQueue event(s) dropped.` lines
summing to exactly 16, and cerberus's received events show gaps of 180ms/270ms
(2x/3x the 90ms send interval) exactly where pulses are missing, precisely
matching hesperus's per-line drop counts. The first ~15-16 pulses got through
with no drops at all, at a clean, unbroken 90ms spacing — but their latency
climbed the whole time, from 272ms to 2726ms, the same ramp signature as
Test A. Drops begin almost exactly where the depth-10 queue at a ~250-270ms
service time against a 90ms arrival interval was expected to saturate.
Once saturated, latency stops climbing and flattens at ~2700-2770ms instead
of growing further — arrivals beyond capacity are now rejected outright
rather than added to an ever-deeper backlog, exactly the behaviour a
hard-capacity queue should show. This is no longer an inferred risk: at
realistic burst rates, this firmware silently loses race events today.

**Re-run under WebSockets (recommendation 1), 2026-06-30 (raw data in
`test-data/trial-burst-ws-20260630-1036.txt`):** same `trial_burst` (40
`GOAL` pulses, fixed 90ms interval). All 40/40 pulses reached cerberus — zero
drops, no `[QUEUE OVERFLOW]` lines. Latency: mean 8.7ms, median 8.2ms, stdev
2.7ms, min 5.2ms, max 18.2ms, p99 18.2ms — flat throughout the burst, no
climbing/ramp signature at all (`tools/cerberus_log_stats.py
test-data/trial-burst-ws-20260630-1036.txt --event GOAL --gaps`). This
confirms the mechanism directly: the persistent WS connection eliminates the
~250-270ms per-event connect+POST+confirm service time that was the actual
bottleneck, so at this 90ms burst rate the depth-10 `networkQueue` now never
comes close to saturating.

### 9. Two real-world trigger patterns this queueing interacts with

- **A single gate re-triggering on one pass.** A robot with a gapped/slotted
  structure can break one gate's beam more than once during what should
  count as a single crossing. `DEBOUNCE_US` (50ms, ISR-level) suppresses
  electrical/mechanical bounce, but a structural gap can plausibly be wider
  than that, generating genuinely separate, correctly-timestamped triggers
  for what should be one logical event. The race state machine already
  tolerates this once it's out of `RUNNING` (a later duplicate is dropped
  by state, not corrupted), so this isn't a correctness bug today — but per
  #8, needlessly sending/queueing extra events adds avoidable network-side
  pressure for no benefit, since only the first trigger of a crossing is
  ever wanted for a race time. A short post-trigger lock-out on hesperus
  itself (deactivate that sensor for ~300ms after it fires, distinct from
  and longer than the 50ms electrical debounce) would suppress these at the
  source. Cheap to add, does no harm once persistent connections (rec. 1)
  make per-event overhead small, and reduces load in the meantime.

  **Confirmed by bench test** (`ares-pulse-generator`'s `trial_double_trigger`,
  two `GOAL` pulses on the same pin, edges 150ms apart, 100 trials; raw
  data/analysis in `test-data/trial-double-trigger-20260630-1604*.txt`): of
  98 cleanly-paired trials, the first trigger's latency averaged 267.3ms
  (stdev 5.9ms) — matching baseline — and the second averaged 371.2ms
  (stdev 6.0ms), a consistent **~104ms** queueing delay, the same mechanism
  as the `ARM`/`START` case below, scaled up because 150ms of spacing
  overlaps more of the ~267ms send cycle than `ARM`/`START`'s 200ms does
  (267−150=117ms forced overlap vs. 267−200=67ms, roughly matching the
  ~104ms vs. ~53ms difference between the two tests).

  **Open question, not yet settled:** 198 of the expected 200 events arrived.
  One is a confirmed genuine drop — `gate_us` (captured at the true trigger
  instant, unaffected by transmission delay) shows the very first trial's
  second edge is simply missing from the sequence, not just delayed; its
  first edge had an unusually slow ~816ms cold-start connection, which may
  have tied up the worker long enough to cause it, but this wasn't confirmed
  against hesperus's own serial log the way the #8 burst-overflow drops were,
  so the exact mechanism is unknown. The second missing event is a lone,
  ordinary-looking trailing trigger with no partner following it, most likely
  just the log capture ending before the response came back rather than a
  real drop, but that's not confirmed either. Noted here as a real, open
  question rather than settled — dropped messages generally are an accepted
  risk at this stage, expected to be addressed once a retry mechanism is
  implemented, so not pursued further for now.

  **Re-run under WebSockets (recommendation 1), 2026-07-31 (raw data in
  `test-data/trial-double-trigger-ws-20260631-1104.txt`):** same
  `trial_double_trigger` (100 trials, edges 150ms apart). All 200/200 events
  arrived — no drops, resolving in WS's favour the open question above
  (that run's baseline drop/ambiguous-trailing-event was specific to HTTP's
  per-event connection cost; not reproduced here). First-trigger latency —
  mean 7.7ms, median 6.6ms, stdev 3.9ms. Second-trigger latency — mean
  8.5ms, median 7.5ms, stdev 2.4ms. The ~104ms queueing offset is gone
  (down to **~0.7ms**), consistent with #8 and the `ARM`/`START` case
  above: removing HTTP's per-event connect+POST+confirm cost removes the
  send-cycle overlap that caused it. One first-trigger outlier at 41.6ms
  (trial 51 of 100, isolated, paired second-trigger event unaffected) plus
  a handful of smaller 10-20ms spikes scattered through the run (more
  frequent than the single 37.7ms outlier seen in the `ARM`/`START` re-run)
  — same low-priority, unexplained, single-event-jitter character as noted
  there, not investigated further.

- **One board serving both `ARM` and `START` gates.** Observation indicates
  a robot can cross the `ARM` gate and then the `START` gate as little as
  ~200ms apart. Both sensors share one hesperus board's `networkQueue`, so
  per #8 this routinely (not just occasionally) queues the `START` event
  behind the `ARM` event's in-flight send cycle — at ~230-270ms per cycle, a
  200ms gap is squarely inside the range where the second event queues.

  **Confirmed by bench test** (`ares-pulse-generator`'s `trial_arm_then_start`,
  99 trigger pairs, edges 200ms apart, raw data/analysis in
  `test-data/arm-then-start-test-20260630-1522*.txt`): `ARM` latency —
  mean 266.6ms, median 266.2ms, stdev 3.4ms — matches the solo-trigger
  baseline (#8, Test B) exactly, as expected. `START` latency — mean 319.7ms,
  median 318.8ms, stdev 6.3ms — consistently ~53ms higher than `ARM`,
  on essentially every pair (one outlier at 373.7ms, the first trial right
  after the button press, matching the same cold-connection pattern seen at
  the start of every other test here). The tightness of both stdevs (3-6ms,
  not tens of ms) confirms this is a near-deterministic queueing offset from
  `START` waiting out the remainder of `ARM`'s in-flight send cycle, not
  ordinary network jitter.

  **Re-run under WebSockets (recommendation 1), 2026-07-31 (raw data/analysis
  in `test-data/arm-then-start-test-ws-20260631-1045*.txt`):** same
  `trial_arm_then_start` (100 pairs, edges 200ms apart). `ARM` latency —
  mean 6.7ms, median 5.8ms, stdev 2.0ms. `START` latency — mean 8.2ms,
  median 6.7ms, stdev 3.9ms. The `ARM`/`START` offset drops from ~53ms
  (HTTP) to ~1.5ms — consistent with #8's finding that WS removes the
  in-flight send-cycle duration `START` was queueing behind, leaving almost
  nothing left to wait out. Unlike the HTTP baseline, no cold-connection
  outlier appears at the first pair — steady-state latency from pair 1
  onward, confirming the persistent connection removes that penalty too.
  One isolated `START` outlier at 37.7ms (pair 39 of 100, p99): its paired
  `ARM` event and the surrounding pairs were all normal (~6-8ms), so this
  is a single-frame delay, not a connection drop — and at ~30ms above
  baseline it's two orders of magnitude smaller than the ~468-531ms
  connect/disconnect/reconnect blip noted in the WS bring-up section below,
  so it's not the same phenomenon. Not investigated further (single
  occurrence, no shared cause found, doesn't affect committed times) — a
  new, distinct, low-priority curiosity, noted here rather than explained.

  Order is preserved (FIFO drain, `ARM`'s full cycle completes before
  `START`'s begins), so this doesn't risk `START` arriving before `ARM` or
  otherwise confusing the state machine — it only delays *when* `START`'s
  HTTP request lands, never *what* timestamp it carries. Since
  `START.tsf_us` is captured at the true trigger instant regardless of when
  it's transmitted, this delay never touches the committed
  `GOAL.tsf_us − START.tsf_us` result at all — recommendation 4 (as actually
  shipped, see #6) computes that independently of `run_sw`. The *live*
  display will show this ~53ms as an ordinary part of its receipt-time lag
  (recommendation 4's revised design deliberately left `run_sw` on plain
  receipt time rather than concealing network delay behind a backdated
  start, precisely to avoid a worse overrun/snap-back problem at the other
  end of the run). No separate fix needed here beyond what's already
  recommended.

### 10. A radio-level Wi-Fi interruption can silently gate hesperus's events for up to 5 minutes

Found during recommendation-1 (persistent WebSocket) bring-up bench testing,
while deliberately testing reconnect-after-Wi-Fi-drop — unrelated to
WebSockets itself, this is a pre-existing property of hesperus's clock-
disciplining code (`main.cpp`'s "TIMELINE SANITY AUDIT ENGINE") that the
test happened to expose clearly for the first time.

`MIN_PLAUSIBLE_TSF = 300000000` (`hesperus-timing-gate/src/main.cpp`) is the
threshold a fresh `tsf_observed` reading must clear before hesperus will
trust it as an initial baseline (`has_initial_baseline`) — until that
baseline exists, every trigger is unconditionally dropped
(`[CRITICAL DROP] Baseline missing or un-synchronized. Packet dropped.`),
before ever reaching the network send code (HTTP or WS). The assumption
behind the threshold is that a implausibly-small TSF value means the Wi-Fi
stack hasn't synced yet (e.g. very early boot) — but **`esp_wifi_get_tsf_time()`
tracks time since the AP's own TSF epoch, not since hesperus associated**,
so anything that resets the AP's TSF resets this for every station on it,
regardless of how briefly.

**Confirmed by direct bench test**: toggling only the AP's radio off and
back on (not a full router reboot/power-cycle) was enough to reset its TSF
epoch. hesperus's own log shows the exact boundary:
```
[T=297670ms] [PLAUSIBILITY REJECT] TSF 297670304 too low. Wi-Fi stack un-synchronized.
[T=297670ms] [CRITICAL DROP] Baseline missing or un-synchronized. Packet dropped.
[T=302817ms] [INITIALIZED] Valid Baseline Coordinates Locked: 302817313
```
Rejected at TSF=297,670,304 (<300,000,000), accepted 5,147,009us later
(matching the heartbeat timer's ~5147ms period exactly) at
TSF=302,817,313 (>300,000,000) — recovery is bounded and self-healing, but
takes the *entire* 300-second threshold from the moment the AP's radio was
disrupted, not from whenever hesperus itself reconnects (hesperus's own
Wi-Fi reconnected in ~3.4s in this same test; TSF wasn't trusted again for
another ~4.9 minutes after that).

Separately, this same outage also triggered hesperus's existing "stuck in
SYN mode >10s" watchdog (`main.cpp`'s "PATCH 2", `ESP.restart()`) — a full
board reboot, not just a Wi-Fi radio reset — since a real outage keeps the
board in SYN/extrapolated-time mode well past the 10s threshold. Worth
noting alongside the TSF gate above since both fire from the same kind of
event and compound the total time-to-recovery, but they're two independent
mechanisms.

**Practical implication**: any AP-side radio interruption — not just a full
power outage — can leave every gate on the network dropping triggers
silently for up to 5 minutes, with no operator-visible indication beyond
the serial debug log. At a real contest this reads as "the gates just
stopped working" for a very long five minutes.

## Summary and conclusions

- The dominant source of everyday latency (~130-270ms) is TCP connection
  establishment, paid fresh on every event. This was previously invisible
  because the timeout being tuned (`HTTP_TIMEOUT_MS`) never actually bounded
  that phase.
- Worst-case outliers (up to ~3s) have two distinct, additive causes now
  confirmed separately: single-attempt TCP connect-phase trouble (#3), and
  — confirmed directly by bench test in #8 — queueing on hesperus's own
  single-worker send cycle when triggers arrive faster than one ~230-270ms
  connect+POST+confirm cycle can complete. At realistic trigger spacing
  (#8, Test B) latency stays in the everyday ~130-270ms range; the queueing
  contribution only appears once triggers outrun that per-event cost, which
  #9 identifies as a routine (not just edge-case) occurrence for a board
  serving both `ARM` and `START`.
- `WIFI_PS_NONE` removes one class of latency spike (radio wake-up) but not
  the connect-phase/packet-loss or queueing classes, and comes at a real,
  hard-constrained power cost given the sub-500mAh battery budget.
- The displayed race time's credibility to a spectator is threatened by
  *variance/asymmetry* between the two legs' latency, not by latency's
  absolute size — a small, consistent latency would be invisible in the
  displayed number even though every message is technically "late."
- Separately, and more seriously: the state machine currently has no defense
  against a severely delayed event from one attempt being silently
  misattributed to a later attempt — a correctness risk, not just a display
  one.

## Recommendations

Ordered by leverage, not necessarily implementation order.

1. **Persistent connection per gate board** (highest leverage). Open one TCP
   connection (or WebSocket) per hesperus board once it becomes ready (Wi-Fi
   connected + cerberus discovered), and hold it open across all subsequent
   trigger events instead of opening/closing per event. Eliminates the
   repeated TCP-handshake cost — and its associated packet-loss-driven
   multi-second retries — for every event after the first. Compatible with
   Wi-Fi modem sleep: TCP session state persists across the radio's
   sleep/wake cycles since they operate at different layers; only the first
   transmission after a sleep period would pay a smaller, more bounded radio
   wake-up cost instead of a full handshake. This is the single change most
   likely to address both the power budget and the latency/variance problem
   together.

   Two implementation details worth getting right from the start:

   - **A held-open connection alone does not give rapid failure detection.**
     If the peer silently disappears (power loss, WiFi drops with no clean
     FIN/RST) while the socket is idle, plain TCP has no built-in way to
     notice — nothing is being sent to fail. Getting proactive, fast
     detection on both ends needs either TCP keepalive (tuned to a short
     interval; most stacks default to hours) or an application-level
     heartbeat. A WebSocket gives this for free via its ping/pong control
     frames and library-level disconnect callbacks (e.g. ESPAsyncWebServer's
     `AsyncWebSocket::onDisconnect`), rather than needing to be built by hand.
   - **The gate should remain the connection initiator**, matching today's
     client/server roles. mDNS discovery already runs in that
     direction (gate resolves `cerberus.local`; cerberus has no equivalent
     way to discover a gate's address), cerberus is already the server, and
     it scales better with the actual shape of the system — each gate
     independently owns reconnecting itself, rather than cerberus tracking
     and re-initiating toward a variable set of remote gates. Note that who
     initiates the connection doesn't restrict which direction data flows
     afterward — a WebSocket is full-duplex once established regardless of
     who dialed, so this doesn't rule out cerberus ever pushing something to
     a gate over the same connection later.
   - **Prefer WebSocket specifically over a bare kept-open TCP/HTTP socket.**
     The ping/pong-and-disconnect-detection property above comes essentially
     free with a WebSocket library on both ends, rather than needing to be
     hand-built as custom keepalive/timeout logic on a plainer transport.
     Cerberus already depends on `ESPAsyncWebServer`, which includes
     `AsyncWebSocket` — adding a WS endpoint alongside the existing
     `/api/event` route needs no new library there. Message framing is
     native (one JSON event per WS message) instead of managing HTTP
     status lines/headers per message or hand-rolling boundaries on a raw
     socket. And the existing dispatch logic doesn't change at all —
     `race_command_from_http()`/`serial_protocol_handle_info_message()` and
     the JSON schema stay exactly as they are; only the transport underneath
     moves from "a new HTTP request per event" to "a WS message per event
     over an already-open connection." The honest cost: hesperus has no
     WebSocket *client* today, so that's a new dependency to add and verify
     (e.g. Links2004/arduinoWebSockets) — and a WS connection still needs an
     initial HTTP-upgrade handshake, so there's still a first-connection
     cost, just paid once per board instead of once per event.

2. **Reintroduce a lighter modem-sleep mode once persistent connections are
   in place.** `WIFI_PS_NONE` was adopted to fix a problem a persistent
   connection addresses more directly and without the full power cost.
   `WIFI_PS_MIN_MODEM` (wakes every beacon interval, ~100ms typically) is a
   reasonable middle ground to test empirically against `MAX_MODEM` and
   `NONE` for both latency and current draw. Requires real on-hardware
   measurement before committing to a specific mode.

3. **Configure both HTTP timeouts explicitly.** Call
   `http.setConnectTimeout()` (currently unset, silently defaulting to
   5000ms) alongside `http.setTimeout()`. Suggested starting point, pending
   (1): ~600ms connect / ~700ms read — comfortably above the ~270-300ms
   typical round trip observed, far below the 5000ms default, so a lost SYN
   fails fast into the existing retry loop instead of stalling for seconds
   inside one attempt. Remains useful even after adopting persistent
   connections, for whatever cold-connect path still exists (first
   connection, or reconnect after a drop).

4. **Use `tsf_us` directly for the committed run time, but not for the live
   display** (implemented and bench-confirmed 2026-07-30 — see #6). The
   originally-proposed version of this recommendation backdated `run_sw`
   itself at both `START` and `GOAL` (via `Stopwatch::restart(timestamp)`/
   `stop(timestamp)`) so the committed time came out exact. It did — but the
   live display, ticking in real time from a backdated start, then had to
   keep counting past the true finish for as long as the `GOAL` message's own
   network leg took to arrive, and visibly snapped backward the instant it
   did. That read worse to a spectator than the original problem (a display
   that's merely a bit late is far less alarming than one that runs, then
   suddenly rewinds).

   **What shipped instead**: `run_sw` stays exactly as it was before this
   recommendation — plain receipt-time `restart()`/`stop()`, no backdating —
   so the live display is smooth and monotonic, just consistently
   latency-late (the original, milder problem). Separately, cerberus records
   the `START` event's `tsf_us` (`g_run_start_tsf_us` in `race-timer.h`) and,
   on `GOAL`, computes the committed time directly as
   `round((GOAL.tsf_us − START.tsf_us) / 1000)` — exact to the millisecond,
   completely independent of `run_sw` and unaffected by either leg's latency.
   The display then shows that exact committed value from the moment the run
   ends (`RaceState::GOAL`) instead of `run_sw`'s own frozen reading, so the
   only visible "snap" left is the small residual return-leg jitter (single
   digits to a few tens of ms) rather than a whole network round trip.
   Falls back to `run_sw.time()` unchanged for a locally-buttoned run, which
   has no `tsf_us` at all and no network hop to correct for.

5. **Add a run/attempt identifier to the event contract**, to structurally
   close the stale-event misattribution risk. Tag each armed attempt with a
   monotonically increasing id, include it in the event payload, and have the
   state machine reject `START`/`GOAL` events that don't match the currently
   armed attempt. This is the only option here that makes the "stale GOAL
   wrongly ends a later run" scenario provably impossible rather than merely
   improbable. Higher implementation cost (touches the event schema on both
   boards and the state machine's acceptance logic) — recommended last, once
   the lower-effort mitigations are in place and its residual necessity can
   be judged against observed real-world reliability.

   **Separable from recommendation 1, but not unrelated.** Persistent
   connections and this attempt-id are independent workstreams — neither
   blocks the other, and they touch different code (transport vs. event
   contract/state machine). They do interact one way: shorter latency from
   (1) shrinks the window during which a stale event can still plausibly be
   in flight when a new attempt gets armed, which makes any timeout-based
   staleness judgment (here or in the tsf-ordering refinement just below)
   easier to call correctly and lets it be tuned more aggressively without
   risking false positives against genuinely-late-but-valid events.

   **Refinement worth folding in, whether or not an explicit attempt-id is
   added**: `tsf_us` is already a shared, ordered clock across boards, so
   staleness can be detected without any new field at all — a `GOAL` whose
   `tsf_us` precedes the *current* attempt's own recorded `START.tsf_us`
   cannot possibly belong to the current attempt, since that attempt's clock
   only starts counting forward from its own start. That comparison alone
   is enough to recognize a late event as belonging to a previous,
   already-superseded attempt. The catch: it's not a simple equality check
   the way an attempt-id is — cerberus would need to retain a short history
   of recent attempts' start (and eventual goal) timestamps and find which
   window a late event's `tsf_us` actually falls into, rather than comparing
   against a single current value. Fine as long as at most one attempt is
   ever abandoned while something is still in flight (likely, given retries
   top out in seconds), but it is a small ring-buffer-of-recent-attempts
   problem, not a one-liner. This is a genuine complement to an explicit
   attempt-id rather than a straight substitute for one: an id tells you
   unambiguously *which* run a late event belongs to; tsf-ordering
   additionally lets cerberus *do something useful with it* once
   identified — retroactively complete what otherwise looked like an
   abandoned run — rather than only being able to reject it as noise.

   **Implemented (reject-only scope), 2026-07-31**
   (`cerberus-gate-controller/src/race/race-timer.h`, the `RaceState::RUNNING`
   `GOAL` branch): a `GOAL` is now rejected (logged as `[RACE] rejected stale
   GOAL: tsf_us=... < start_tsf_us=...`, state stays `RUNNING`, nothing
   committed) whenever its `event_tsf_us` is less than the current attempt's
   own `g_run_start_tsf_us`. On review, a ring-buffer-of-recent-attempts
   turned out **not** to be needed for this reject-only scope, correcting the
   paragraph above: `g_run_start_tsf_us` is a single scalar that's always
   overwritten by the most recent `START`, and `tsf_us` is monotonic across
   every attempt, so comparing only against the *current* attempt's start
   already catches a stale event regardless of how many attempts were
   abandoned in between — no history needed just to reject. A ring buffer
   would still be needed for the fancier follow-on this paragraph also
   describes (retroactively completing an abandoned run from a late `GOAL`
   instead of merely rejecting it), which remains unimplemented and is still
   a genuine complement to, not a substitute for, an explicit attempt-id.
   Native unit tests added (`test_race_timer.cpp`) covering the stale-reject
   case and the `<` vs `<=` boundary (an exactly-equal tsf still commits).
   Builds clean (`pio test -e native`, `pio run -e
   cerberus-cyd2usb-diymalls-ili9341`); not yet flashed or bench-verified.
   Bench-reproducing the #7 scenario deliberately (Experiment 6 below) is the
   natural next step now that there's a defense to verify.

6. **Event de-duplication on cerberus, as a cheap safety net** (optional,
   lower priority). A simple `gate_id` + `event` + `tsf_us` de-duplication
   check on `/api/event` — mirroring the existing Python test stub's
   `last_event_tsf` pattern — costs little and guards against any remaining
   retry-driven duplicate processing, independent of the deeper timing
   questions above.

7. **Redundant "hedged" sends for the first few attempts** (speculative).
   Gate triggers are sparse — on the order of one every 20+ seconds per gate,
   sometimes minutes apart — so the traffic-volume cost of this idea is much
   smaller than it would be for a chatty protocol, which makes it worth
   trying. Idea: send the first few attempts (e.g. 5) as a rapid burst rather
   than gating each on the previous one failing, then fall back to the
   existing sequential retry loop for the remaining budget (e.g. up to 15
   attempts total). Whichever attempt returns first is used; the rest are
   redundant. This is the same principle as "hedged requests" in distributed
   systems — trading some redundant work for a large cut in tail latency —
   and could meaningfully reduce the chance of a multi-second stall reaching
   the state machine, without needing the bigger architectural change of
   persistent connections.

   Two things this depends on, not just nice-to-haves:
   - **Cerberus-side de-duplication (item 6) becomes a hard prerequisite,
     not optional.** Under this design, multiple attempts from the same
     burst will routinely both succeed, not just occasionally.
   - **Number each attempt sequentially and log, on both hesperus and
     cerberus, which attempt number actually "hit" first** — for every
     event, as a matter of course, not just when investigating a problem.
     This is what would turn the technique from a plausible idea into a
     measured one: it directly shows whether failures across a burst are
     actually independent (supporting the technique) or correlated (e.g. all
     5 failing together under channel congestion, undermining it) — see
     Experiment 7 below.

8. **Gate-side post-trigger lock-out (~300ms)** (see #9). Deactivate a
   sensor for a short window after it fires, on top of the existing 50ms
   `DEBOUNCE_US` electrical debounce — aimed at a robot's structure (e.g. a
   gapped chassis) producing more than one genuine, correctly-spaced trigger
   for what should count as a single crossing. The state machine already
   tolerates the duplicate once it's left `RUNNING`, so this isn't fixing a
   correctness bug, only avoiding needless queued/sent events per #8 — cheap,
   does no harm once persistent connections (rec. 1) are in place, and helps
   in the meantime.

9. **Replace `MIN_PLAUSIBLE_TSF`'s magnitude gate with trust-on-reconnect**
   (see #10). **Decided, 2026-07-31** (design reasoning, not yet
   implemented): the original 300-second value's justification isn't
   documented anywhere in the code and isn't recalled either — re-examined
   from first principles instead of just re-tuning the number.

   The scenario the gate is actually trying to guard against is the AP's
   TSF epoch resetting *mid-run*. On this system's single-AP topology, the
   only plausible way for that to happen is an AP radio failure/reset —
   which takes down Wi-Fi for every gate on the network simultaneously, a
   total system failure regardless of what hesperus's TSF logic does. A
   magnitude gate that stays shut for 5 minutes doesn't protect against
   that scenario (there's nothing left to protect at that point); it only
   adds unnecessary downtime on top of it, and — per #10 — a much smaller,
   *more common* trigger (a brief AP radio blip well short of a full
   outage) already demonstrates this.

   The residual risk a gate might still be defending against is a
   genuinely tiny/implausible `tsf_observed` value being trusted right
   after reconnect — but the client radio's own reconnection already takes
   several seconds, during which the AP's TSF clock (if it's running at
   all) will have advanced well past any near-zero danger zone. No
   plausible mechanism for a large spurious jump was identified for a
   single, non-mesh AP.

   **Decision: trust `tsf_observed` immediately on a fresh Wi-Fi
   association event** rather than gating on the value's absolute
   magnitude at all. **Implemented, 2026-07-31**
   (`hesperus-timing-gate/src/main.cpp`, the initial-baseline gate at
   `!has_initial_baseline`): the `tsf_observed >= MIN_PLAUSIBLE_TSF` check
   is replaced with `WiFi.status() == WL_CONNECTED`, matching the
   connectivity check already used elsewhere in this file — since this
   branch only ever runs while no baseline exists yet, checking current
   connectivity right there achieves "trust once actually connected"
   without needing a separate `WiFi.onEvent` handler/flag. Builds clean for
   `hesperus-gate-c3-super-mini`; flashed and bench-verified 2026-07-31 (see
   below).
   Scoped narrowly to this one gate — a second, separate use of
   `MIN_PLAUSIBLE_TSF` exists in the "escape hatch" SYN-mode recovery
   heuristic (PATCH 1, `main.cpp:401-415`) and was deliberately left
   untouched, not being what #10 documents as the bug.

   **Verification plan, proposed 2026-07-31, superseded by a direct test the
   same day:** the original idea was a second ESP32 as a soft AP, fully
   under test control, to check the load-bearing assumption above — whether
   there is *any* way to take an AP's radio down (even briefly) without
   resetting its TSF epoch. No spare ESP32 was available to dedicate as a
   test-controlled soft AP, so instead the real venue AP's radio was toggled
   off and back on directly (its normal ~30s recovery time) while collecting
   logs from cerberus and the GOAL-board hesperus unit.

   **Bench-confirmed, 2026-07-31.** hesperus's GOAL board logged
   `[INITIALIZED] Valid Baseline Coordinates Locked: 2408999` — a TSF value
   far below the old `MIN_PLAUSIBLE_TSF` (300,000,000) that would previously
   have been rejected outright — accepted immediately because the new gate
   only checks `WiFi.status() == WL_CONNECTED` (`main.cpp:326`), not
   magnitude. This is the fix working exactly as designed, on the real AP,
   confirming the soft-AP assumption above didn't need to be separately
   proven: the real AP's radio-only toggle did reset its TSF epoch (matching
   #10's original finding) and the new code recovered from it immediately.

   Full recovery sequence observed (hesperus GOAL board's own TSF-based
   `[T=]` log clock reads near-zero until re-associated, so times below are
   relative to each stage, not one continuous clock): losing AP sync tripped
   the existing "PATCH 2" `[ROLLOVER FAULT]` watchdog (`main.cpp:502-511`)
   after 10s stuck in `DISCIPLINED SYN` mode, forcing a full `ESP.restart()`
   — expected, and independent of #10/recommendation 9. Post-reboot, the
   separate 15s "Wi-Fi link dead" watchdog (`main.cpp:563-570`) cycled at
   least 3 times (~45s+) while the AP radio was still coming back up,
   consistent with its normal ~30s recovery. Once reconnected: `[NETWORK]
   Link Active!` at T=1136ms, mDNS resolved at T=2051ms, baseline locked at
   T=2775ms — **under 2 seconds after Wi-Fi actually came back**, not the
   old 5-minute magnitude-gate wait. Cerberus itself (never rebooted, just
   lost/regained Wi-Fi) reconnected in ~916ms and had both gates' WS clients
   back within a few seconds. Total time-to-recovery in this test is now
   governed by hesperus's own 10s-SYN-timeout reboot plus actual Wi-Fi
   reassociation time, not by an arbitrary threshold.

   One new, unexplained data point from this same test: cerberus logged
   `[E][ESPmDNS.cpp:148] addService(): Failed adding service http.tcp.`
   immediately after reconnecting (non-fatal — mDNS started cleanly on the
   very next line). Not confirmed, but plausibly the same underlying
   mechanism as the already-open WS connect/disconnect/reconnect blip noted
   in the WS bring-up section below (`net/wifi-manager.h`'s
   `AsyncServer::begin()` PCB-orphaning gotcha) — flagged here rather than
   investigated further.

## Alternative considered: ESP-NOW

Worth recording explicitly, since it was raised and weighed rather than
overlooked: an earlier, separate experiment used ESP-NOW as the message
transport and found it unimpressive at getting messages through in a
genuinely congested radio environment.

The reasoning for sticking with infrastructure Wi-Fi/TCP (recommendation 1)
despite that isn't "TCP is more reliable over the air" — it likely isn't.
ESP-NOW operates near the raw 802.11 MAC layer with no AP association, no
DHCP, no TCP handshake — it skips essentially all the overhead infrastructure
Wi-Fi carries. If it still performed poorly under congestion, that points to
the underlying RF channel itself (collisions, interference, noise floor) as
the bottleneck, not protocol overhead — and that's shared physics: infra
Wi-Fi transmits on the same spectrum, subject to the same contention, plus
everything ESP-NOW sidesteps. So infra Wi-Fi is not obviously going to get an
individual frame through a congested channel any better than ESP-NOW did —
it could plausibly be worse per attempt, given it has strictly more that can
go wrong.

What decides it instead is a build-vs-buy tradeoff. TCP already contains a
mature, extensively tested answer to "detect and recover from a lost
packet" — retransmission, ordering, connection-liveness — none of which has
to be designed, implemented, or debugged from scratch. ESP-NOW would be
leaner and lower-latency when a message gets through, but when it
inevitably loses one, a comparable raft of detection-and-recovery measures
would need to be built by hand on top of it. The extra airtime infra
Wi-Fi/TCP costs is the price of not having to invent and maintain that
machinery — several existing layers of resilience today, and a well-understood
path to adding more (recommendations 5-7), rather than a bespoke protocol
built up one edge case at a time.

## Proposed experiments

The observations above were mostly gathered opportunistically from bench
tests, not from deliberately reproducing each effect. The following
experiments are aimed at characterising each one directly and repeatably,
roughly in order of how directly they target the open questions above.

### 1. Isolate connect-phase latency from the rest of the round trip

Everything in this document about "the connect phase dominates" is inferred
from the TSF-correlated one-way/return-leg split (#3) plus the
`HTTPClient` source reading (#2) — never measured directly as its own number.
Add temporary instrumentation on hesperus that times a bare
`WiFiClient::connect()` to cerberus's IP/port separately from the subsequent
request/response, logged alongside the existing `[Async Worker]` line. Run a
few hundred trigger cycles at realistic race spacing and histogram the
connect-only durations. This would convert "almost certainly TCP connect" into
a directly measured distribution, and would also make it possible to see
whether the ~130-270ms "typical" figure itself has a connect-phase component
worth caring about, not just the multi-second outliers.

### 2. Reproduce packet loss / connect stalls on demand, rather than waiting for one

The 856ms/2993ms outliers happened opportunistically; a controlled repro
would let the connect-timeout values (recommendation 3) and the persistent-
connection design (recommendation 1) actually be validated against a known
failure rate instead of guessed at. If the venue router (or a Linux box
bridging/routing for the test bench) supports it, Linux's `tc`/`netem` can
inject a configurable percentage of dropped or delayed packets on the link
between the AP and the test network (`tc qdisc add dev <if> root netem loss
2% delay 50ms 20ms`) — this reproduces the "occasional lost SYN" mechanism
directly and repeatably, with a known, tunable loss rate, rather than relying
on it happening by chance during a test run.

### 3. Congested-airtime testing — matches your own proposal

Putting a stressor AP on the same or an overlapping channel plus running
heavy transfers is a good, cheap proxy for a busy contest venue, and lines up
with stress-test ideas already sketched in `hesperus-timing-gate/review.md`:

- **Airtime saturation**: several extra ESP32s (or laptops) running a tight
  loop of HTTP requests against a local server, associated to the same AP —
  packs the AP's transmit queue and forces beacon deferral, which is exactly
  what would stress TSF sync and (separately) connect-phase reliability.
- **Bulk throughput**: `iperf3 -s` on one machine, `iperf3 -c <ip> -t 300` from
  another on the same AP — sustained throughput competing for airtime, higher
  per-device impact than many small HTTP requests.
- **Channel interference**: a second, unrelated router broadcasting on an
  overlapping channel (venue AP on channel 6, stressor on channel 4 or 8) —
  RF contention the AP can't back off from, closer to a real busy-venue
  environment than same-channel congestion alone.
- **Broadband noise**: a 2.4GHz-band noise source (review.md notes a
  microwave oven ~30cm from the AP works surprisingly well) as a blunt,
  worst-case stressor.

Worth running the full ARM/START/GOAL bench sequence under each of these
independently (not just one combined "as noisy as possible" test), so the
resulting latency/outlier data can be attributed to a specific kind of
congestion rather than "network was busy" in general.

### 4. Characterise power-save modes against real traffic patterns

For each of `WIFI_PS_NONE`, `WIFI_PS_MIN_MODEM`, and `WIFI_PS_MAX_MODEM`, with
a persistent connection open (recommendation 1) once it exists: measure (a)
current draw via a USB power meter or multimeter, at idle and during a
trigger, and (b) wake-to-first-byte latency for a trigger fired after a
range of idle gaps (1s, 5s, 15s, 30s, 60s+ — spanning realistic ARM-to-START
and START-to-GOAL spacing). This directly answers both the "well under 50mA"
question and whether a lighter sleep mode's wake latency is actually small
and bounded, rather than assumed from generic ESP32 figures.

### 5. Directly validate the display-catch-up model with injected asymmetric latency (done)

To test the `L_goal − L_start` model (#6) and a future `tsf_us`-based fix
(recommendation 4) directly rather than waiting for it to occur naturally:
deliberately delay only one leg (e.g. hold the start board's request in a
proxy/shim for a fixed extra 300ms while leaving the goal board unmodified),
then compare cerberus's displayed/committed time against the
`GOAL.tsf_us − START.tsf_us` truth. This should let the predicted error
(`L_goal − L_start`) be checked against the actual observed display error
before and after adopting a `tsf_us`-based `run_sw`.

**Done differently, 2026-07-30**: rather than injecting artificial asymmetric
latency, `ares-pulse-generator`'s `trial_four_runs` (four back-to-back
ARM-START-GOAL runs of known 2s/3s/4s/5s duration) exercised real gate
hardware/network latency directly. This is how the recommendation-4 flaw
(live display overrun-then-snap-back) was actually caught, and how the
revised fix was confirmed to commit exact 2000/3000/4000/5000ms results. See
#6/recommendation 4.

### 6. Reproduce the state-machine misattribution scenario deliberately

The stale-`GOAL`-lands-during-a-later-run scenario (#7) is currently only a
code-reading finding, not something observed happening. Using the same
delay-injection shim as experiment 5 (or `netem` from experiment 2), hold a
`GOAL` message back deliberately, manually re-arm and start a new attempt
during the delay, then release the held `GOAL`. Confirming this actually
produces a bogus committed time for the second attempt (rather than being
rejected or ignored) would turn recommendation 5 (run/attempt identifier)
from a theoretical concern into a confirmed, reproducible bug with a known
trigger condition.

### 7. Validate the hedged-burst-send technique (recommendation 7)

With attempts numbered sequentially and the "hit" attempt logged on both
sides (as recommendation 7 requires regardless), send a burst of, say, 5
attempts per event over a large number of trigger cycles and record which
attempt number wins each time. Two things to check specifically:

- **How often does more than one attempt in a burst succeed?** This is the
  real-world rate cerberus-side de-duplication needs to handle — not a rare
  edge case under this design, but routine.
- **How often do all attempts in a burst fail together?** Run this under
  quiet conditions and again under each of the congestion scenarios from
  Experiment 3. A meaningfully non-zero all-fail rate under congestion would
  indicate the failures are correlated (shared RF conditions) rather than
  independent, directly testing the assumption the technique's benefit
  depends on.

### 8. Long single-gate sequence to characterise the WS jitter spikes

The `trial_arm_then_start` and `trial_double_trigger` WS re-runs (#8/#9)
each showed one or a few isolated single-event latency spikes (10-40ms,
vs. ~6-9ms baseline) with no shared cause with their paired event and no
correlation to connection start — cause not identified (see discussion:
candidates include `uploadWorkerTask`'s `wsClient.loop()` occasionally
blocking on a keepalive/partial read, a single 802.11 MAC-layer retry, or
cerberus-side contention from its own busier workload). n=100-200 per run
so far is too small to see a pattern, if one exists.

Proposed: a single-gate `GOAL`-only sequence, much longer and steadier than
the existing trials — e.g. 10,000 messages at a fixed 250ms interval (well
clear of any queueing effect, per #8's WS results) — logged and run through
`tools/cerberus_log_stats.py --gaps`. At that volume, look for: periodicity
(a spike every N messages, which would point at a fixed-period task like
the heartbeat timer or a WS keepalive interval), clustering in time (bursts
of spikes vs. uniformly scattered), or drift/rate correlation. Would need
`ares-pulse-generator`'s `MAX_COUNT`/interval reconfigured for this (currently
100 at 1s spacing) — a straightforward change, but a 10,000-message run at
250ms is ~42 minutes per pass, worth planning for.

Would not, on its own, isolate *which* stage the delay happens in (hesperus
pickup vs. network transit vs. cerberus processing) — the current log only
has `tsf_us` (hesperus ISR time) and `recv_ms` (cerberus receipt time), which
bundles all three. If the pattern-hunt above doesn't point at an obvious
cause, the next step would be adding a hesperus-side send timestamp to the
log to split the latency into legs.

## Status as of 2026-07-30 and what's next

**Bench-testing phase (this document's #8/#9) is substantially done.** Four
controlled tests were run using `ares-pulse-generator` (now has four
`trial_*` functions: solo pulse, `ARM`-then-`START`, controlled burst,
same-pin double-trigger) and `tools/cerberus_log_stats.py` (extracts
`recv_ms`/`event`/`tsf_us`/`gate_us` from cerberus's debug log and reports
latency stats, with a `--gaps` mode for the queueing diagnostic). Raw data
and analysis for all of them are in `test-data/`. Confirmed, not just
inferred:
- Queueing (not TCP-connect alone) explains worst-case latency under burst
  triggering (#8), and the depth-10 `networkQueue` really does overflow and
  silently drop events at realistic burst rates (40 sent, 24 received, 16
  confirmed dropped both via hesperus's own overflow log and independently
  via `gate_us` gap analysis).
- The `ARM`/`START` shared-board scenario (#9) adds a consistent, near-
  deterministic ~53ms delay to `START` at 200ms trigger spacing.
- The single-gate double-trigger scenario (#9) adds ~104ms to the second
  trigger at 150ms spacing, plus two still-unresolved single-event drops
  (one confirmed real via `gate_us`, cause unknown; one likely just a
  capture-boundary artifact) — noted as an open question, not pursued
  further since dropped messages are an accepted risk pending the retry
  mechanism below.
- Experiment 6 (deliberately reproducing the #7 misattribution scenario) was
  considered and explicitly deferred: expected to be rare in practice and
  its effects easy to infer without a dedicated test, and better revisited
  after the event-contract/protocol changes below land, since those may
  change how it plays out anyway.

**Next, in the order this session left off on:**
1. Implement a retry/reliability mechanism for dropped events (the accepted
   gap noted above) — not yet designed as of this writing.
2. Implement persistent connections on hesperus (recommendation 1) and
   re-run the bench `ARM`/`START`/`GOAL`/burst sequence with the same tools;
   compare the resulting latency distribution (expect it to tighten to
   roughly the ~15-30ms return-leg figures in #3, with no multi-second
   outliers or queue overflow at the burst rates tested here) against this
   document's baseline data.

   **In progress, 2026-07-30/31 (`websockets` branch, not yet merged)**:
   cerberus's `/ws` `AsyncWebSocket` endpoint landed and is bench-verified
   (new `tools/testing/ws_send_event.py`, mirrors the existing `send-*.sh`
   HTTP scripts but reuses one persistent connection). hesperus's WS client
   landed, built for `hesperus-gate-c3-super-mini`, and **both physical
   hesperus test boards (ARM/START, GOAL) are now flashed and running WS
   successfully**. Bench-measured WS latency settled at **~5-12ms** typical
   (an initial run saw a one-time ~1.1s first-connection warm-up that didn't
   recur on later runs), against **~115-560ms** for the still-HTTP
   comparison runs earlier in bring-up — beats this section's own ~15-30ms
   prediction. Committed times stayed exact (2000/3000/4000/5000ms)
   throughout, confirming recommendation 4 is unaffected. Reconnect-after-
   Wi-Fi-drop and reconnect-after-cerberus-reboot are both bench-confirmed
   working (self-healing, no manual intervention needed either time).
   Reconnect testing is what surfaced #10 (5-minute `MIN_PLAUSIBLE_TSF`
   lockout) — unrelated to WebSockets itself, but found here first;
   recommendation 9 covers it. Fix designed and implemented 2026-07-31
   (trust `tsf_observed` on Wi-Fi reconnect instead of gating on
   magnitude, see recommendation 9) — flashed and bench-confirmed
   2026-07-31 against a real AP radio toggle: baseline locked under 2s
   after Wi-Fi reassociation, versus the old 5-minute lockout. One low-priority,
   self-healing WS connect/disconnect/reconnect blip was also observed
   during a cerberus-reboot test, investigated but not conclusively
   explained (see `net/wifi-manager.h`'s documented `AsyncServer::begin()`
   PCB-orphaning gotcha for the closest known-analogous issue) — watch for
   recurrence rather than fixed speculatively.

   The other 3 hesperus board envs (`hesperus-gate-s3-zero`,
   `hesperus-gate-s3-super-mini`, `hesperus-gate-c3-xiao`) don't need
   separate testing. The full bench suite re-run for the complete
   before/after comparison this item originally asked for is **done**:
   `trial_burst` (see #8 above — 40/40 delivered, 8.7ms mean latency, vs.
   24/40 delivered climbing to ~2.7s under HTTP), `trial_arm_then_start`
   (see #9 above — `ARM`/`START` offset dropped from ~53ms to ~1.5ms), and
   `trial_double_trigger` (see #9 above — 200/200 delivered with no drops,
   vs. 198/200 under HTTP; queueing offset dropped from ~104ms to ~0.7ms).
   All three confirm the same mechanism: WS's persistent connection removes
   the ~250-270ms per-event connect+POST+confirm cost that caused HTTP's
   queueing, drops, and latency ramp alike.
3. Measure actual current draw on real hardware across `WIFI_PS_NONE` /
   `WIFI_PS_MIN_MODEM` / `WIFI_PS_MAX_MODEM` with a persistent connection
   open, to make an evidence-based power/latency tradeoff instead of relying
   on generic ESP32 figures.
4. Experiment 5 (validate the display-catch-up model) is done: recommendation
   4 was implemented, found to have a worse spectator-facing flaw than the
   problem it fixed, revised, and bench-confirmed (see #6/recommendation 4).
   Experiment 4 / item 3 above (power-save mode characterization) is
   **deferred** — decided 2026-07-30 to wait until persistent connections
   (recommendation 1) land before measuring current draw across
   `WIFI_PS_NONE`/`WIFI_PS_MIN_MODEM`/`WIFI_PS_MAX_MODEM`, since that's the
   configuration it's actually meant to be measured under; measuring the
   current per-event-connection behaviour first would be throwaway work.
5. Return to the misattribution test (Experiment 6) once the protocol
   changes are in.
