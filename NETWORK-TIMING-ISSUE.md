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

### 6. Displayed run time vs. true TSF-based time

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

## Summary and conclusions

- The dominant source of both everyday latency (~130-270ms) and worst-case
  outliers (up to ~3s) is TCP connection establishment, paid fresh on every
  event. This was previously invisible because the timeout being tuned
  (`HTTP_TIMEOUT_MS`) never actually bounded that phase.
- `WIFI_PS_NONE` removes one class of latency spike (radio wake-up) but not
  the connect-phase/packet-loss class, and comes at a real, hard-constrained
  power cost given the sub-500mAh battery budget.
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

4. **Use `tsf_us` directly for the displayed/committed run time**, instead of
   cerberus's local receipt-time clock. On `START`, initialize the live
   stopwatch's elapsed baseline to `cerberus_tsf_now − start_event.tsf_us`
   (via `Stopwatch::restart(timestamp)`) so the display "catches up"
   immediately rather than starting at zero and running permanently behind.
   Compute the final committed run time directly as
   `GOAL.tsf_us − START.tsf_us`, already accurate to near-microsecond
   precision regardless of network latency, symmetric or not. This is a fix
   by construction, complementary to (1)/(2) rather than a substitute for
   them.

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

### 5. Directly validate the display-catch-up model with injected asymmetric latency

To test the `L_goal − L_start` model (#6) and a future `tsf_us`-based fix
(recommendation 4) directly rather than waiting for it to occur naturally:
deliberately delay only one leg (e.g. hold the start board's request in a
proxy/shim for a fixed extra 300ms while leaving the goal board unmodified),
then compare cerberus's displayed/committed time against the
`GOAL.tsf_us − START.tsf_us` truth. This should let the predicted error
(`L_goal − L_start`) be checked against the actual observed display error
before and after adopting a `tsf_us`-based `run_sw`.

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

## Suggested next steps

- Implement persistent connections on hesperus and re-run the bench
  ARM/START/GOAL sequence; compare the resulting latency distribution
  (expect it to tighten to roughly the ~15-30ms return-leg figures above,
  with no multi-second outliers) against this document's baseline data.
- Measure actual current draw on real hardware across `WIFI_PS_NONE` /
  `WIFI_PS_MIN_MODEM` / `WIFI_PS_MAX_MODEM` with a persistent connection
  open, to make an evidence-based power/latency tradeoff instead of relying
  on generic ESP32 figures.
- Decide, based on the above, whether items (4) and (5) are pursued now or
  deferred — both are correctness/appearance fixes rather than blockers
  given current bench-test success.
