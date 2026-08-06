# Kronos Workspace TODO

Index of outstanding work across the workspace. Each sub-project keeps its
own task/planning docs; this file doesn't duplicate their content, it just
points at them and tracks the cross-project (interoperability, testing)
items that don't belong to any single sub-project.

Current focus: interoperability and testing between the already-working
per-project pieces, not new per-project features.

---

## Priorities, in order

Condensed cross-workspace list, reviewed 2026-08-06. Full detail/rationale
for each is in the sections below — this is the fast-scan version.

1. **A genuinely clean two-spammer+BT trial under `WIFI_PS_NONE`**
   ([NETWORK-TIMING-ISSUE.md](NETWORK-TIMING-ISSUE.md)) — the
   highest-leverage single action available right now. One clean run of
   this trial is expected to settle #2 and #3 below as a side effect, not
   just itself — no need to treat them as three separate pieces of work.
   Sessions 15 and 15a were both compromised by unrelated equipment issues
   (a spammer dropping out, BT found off partway through); re-run with the
   setup double-checked before starting.
2. **Wi-Fi power-save vs. battery budget** — [issue](NETWORK-TIMING-ISSUE.md#issue-wi-fi-power-save-vs-battery-budget).
   `NONE` stays shipped default for now; waiting on #1 to know whether
   it's genuinely safe from the `MIN_MODEM`-associated stall mechanism or
   just less exposed to it.
3. **Ack-path timeout fix confirmation** — [issue](NETWORK-TIMING-ISSUE.md#issue-acks-not-arriving-back-at-hesperus-in-time-despite-cerberus-receiving-the-event).
   Tuning already applied and build-verified (`WS_ACK_TIMEOUT_MS`
   300→500ms); just needs #1's clean trial to confirm the retry-rate drop
   under `NONE`.
4. **Duplicate-trigger lock-out window design** — [issue](NETWORK-TIMING-ISSUE.md#issue-duplicate-triggers-from-gapped-robot-structure).
   Standalone design question, doesn't depend on #1-3 — pick up whenever.
5. **Two small cerberus in-code items**, cheap and independent:
   [`BUTTON_COMMAND_MAP` review](cerberus-gate-controller/src/race/race-command-source.h#L37),
   [`send_run_time()` sent twice for unclear reasons](cerberus-gate-controller/src/messages-reference.h#L111-L115).
6. **ARES reset-glitch investigation** (see
   [ares-pulse-generator](#ares-pulse-generator) below) — both gates fire
   when ARES resets while wired directly to a pair of hesperus boards.
   Isolated, doesn't block running test trials.

Deprioritized, not pursued right now (see the relevant sections below for
why): GOAL-board retry asymmetry under heavy stress (deferred to
production-board testing), congested-airtime stress testing (judged
sufficient 2026-08-04), hedged burst sends for tail latency (watching
telemetry only), and all net-new feature work — cerberus's supervisor
state machine / SD-card logging / HTTP log streaming / `race_runs[]`
concurrency guard, hesperus's NVS config store / OTA / SSD1306 display /
stack telemetry / NVS event buffering / multi-AP BSSID fallback / standalone
scoring, and ARGUS (not started at all).

**After a few of these land: a thorough documentation review.**
`NETWORK-TIMING-ISSUE.md` and this file have both grown complex and hard
to follow — a long story told poorly, accumulated session by session.
Deliberately deferred until after the trial above lands, so the rewrite
happens once against settled results instead of mid-investigation.

---

## Cross-project: interoperability & testing

Primary tracker: **`NETWORK-TIMING-ISSUE.md`** (root) — its "Outstanding
work, prioritized" section is the ranked list, kept up to date there rather
than mirrored here. As of 2026-08-06, in priority order:

1. Wi-Fi power-save vs. battery budget — still open. `NONE` stays the
   shipped default (decided 2026-08-04); `MIN_MODEM`'s stall/reliability
   regression is real but unexplained (sessions 11/13, worse after a
   reverted mitigation attempt in session 14). Sessions 15/15a
   (2026-08-06) found the same severe stall signature also occurs under
   `NONE`, just less often — so it's not `MIN_MODEM`-exclusive after all —
   but both `NONE` confirmation trials were confound-compromised; a clean
   run still hasn't happened.
2. Ack-path timeout mismatch — tuning applied 2026-08-04
   (`WS_ACK_TIMEOUT_MS` 300→500ms), build-verified, but still not
   confirmed by a genuinely clean trial: two attempted two-spammer+BT
   `NONE` re-runs (sessions 15, 15a) were each compromised by an
   unrelated equipment issue.
3. Duplicate triggers from gapped robot structure — lock-out window design unresolved
4. GOAL board retries far more than ARM/START under heavy stress (session
   13: 18.3% vs. 4.0%) — cause (physical position vs. role) not yet
   distinguished; the planned start/goal swap is deferred to
   production-board testing
5. Congested-airtime stress testing — judged sufficient 2026-08-04 and
   deprioritized (not formally closed); bulk-throughput contention and
   broadband noise remain genuinely untested, not pursued without a
   concrete reason to
6. Hedged burst sends for tail latency — deprioritized, watch retry telemetry first

Resolved/closed since the last pass: the `wsClient.loop()`-blocking
congestion bug (hardware-verified 2026-08-03) and the unexplained WS
jitter/reconnect blip (characterized as benign, closed 2026-08-04).

Other cross-project items not tracked in that file:

- **ARGUS** (data collector/analysis layer) — not started at all. See `docs/KRONOS-SYNCHRONIZED-TIMING-GATES-ARCHITECTURE.md`.
- **Provisioning** — sensor-side mDNS discovery and Wi-Fi credential override are already implemented (`resolveCerberus()`, serial `wifi` command + NVS on hesperus); only deep-sleep power cycling and the sensor registration/auth-token handshake remain proposed. See `docs/PROVISIONING.md`, which now also has a "Recovery from Failure" section (per-scenario: what's automatic vs. needs manual action, including the known "cerberus restarts on a new IP" gap — untested, no repro yet).
- **Reliability: congestion avoidance** (exponential backoff+jitter) — proposed design only, not implemented. See `docs/RELIABILITY.md` (section 2). (`TCP_NODELAY`, formerly listed alongside this, turned out to already be enabled by default via the WebSockets/ESPAsyncWebServer library versions in use — no action needed, see that doc's section 3.)

### Testing gaps

- A genuinely confound-free two-spammer+BT trial under `WIFI_PS_NONE` —
  sessions 15 and 15a (2026-08-06) both compromised by unrelated equipment
  issues; this is what items 1 and 2 above are both waiting on
  (`NETWORK-TIMING-ISSUE.md`)
- Congested-airtime stress harness — airtime saturation has extensive
  coverage (spam-test sessions 2-15a); bulk throughput contention and
  broadband noise layers are untested and deprioritized, not tooled
  (`docs/TEST-TOOLING.md`)
- Duplicate-trigger lock-out re-test (`trial_double_trigger`) — blocked until the lock-out feature above is built

Closed: the 10,000-message WS jitter test and the power-save
current-draw/latency measurement were both completed (the latter via
sessions 11-15a's spam-test trials rather than the originally-proposed
dedicated bench method) — see `NETWORK-TIMING-ISSUE.md` for results.

---

## cerberus-gate-controller

Full list: **`cerberus-gate-controller/docs/PLANNED-UPDATES.md`** (supervisor
state machine, SD-card CSV logging, HTTP log streaming/MAINTENANCE mode,
`race_runs[]` concurrency guard). Two items formerly listed here are gone:
TSF-based drift compensation was removed 2026-08-06 as superseded
(hesperus's own dual-clock holdover already disciplines `tsf_us` before
it's ever sent, and cerberus only ever diffs two already-trustworthy
timestamps, never extrapolates one, so there was nothing left for
cerberus-side compensation to do); the touch calibration NVS escape hatch
was implemented the same day — long-press START on the main menu now
re-runs the calibration wizard (`main.cpp`'s `input_event_handler`),
recoverable even with badly-miscalibrated touch since it's a physical
NeoKey gesture, not an on-screen button.

Smaller items tracked only in-code, not in that doc:

- `BUTTON_COMMAND_MAP` flagged as needing review against the state machine — `src/race/race-command-source.h:37`
- Legacy serial sends `send_run_time()` twice for unclear reasons — `src/messages-reference.h:111-115`

## hesperus-timing-gate

Full list: **`hesperus-timing-gate/review.md`**'s "Future Development Path"
table (Wi-Fi modem sleep, NVS config store, configurable debounce, OTA
update, SSD1306 display, stack telemetry, NVS event buffering, multi-AP
BSSID fallback, local standalone scoring) — refreshed 2026-08-06 alongside
this file (correct line numbers, Wi-Fi Modem Sleep row now reflects the
sessions-11-through-15a investigation instead of just "unblocked", and the
new diagnostics-HTTP-server row removed since it shipped).

Resolved since the last pass: field-deployable overflow-drop notification
(was serial-debug-only) now ships as both persistent NVS counters and an
HTTP status flag — see `network-health-stats.h` and `GET /status` in
`src/net/debug-http-server.h`. (`main.cpp`'s own `uploadWorkerTask` comment
listing this as still-unaddressed, around line 561, is now itself stale
and should be removed next time that function is touched.)

## ares-pulse-generator

- Resetting the ARES board while wired directly to a pair of hesperus boards
  tends to make both gates fire -- looks like an electrical glitch on reset,
  not yet investigated. Doesn't block using ARES to run test trials, so
  deprioritized for now. (noted 2026-08-02)
