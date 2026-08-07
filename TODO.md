# Kronos Workspace TODO

Index of outstanding work across the workspace. Each sub-project keeps its
own task/planning docs; this file doesn't duplicate their content, it just
points at them and tracks the cross-project (interoperability, testing)
items that don't belong to any single sub-project.

Current focus: interoperability and testing between the already-working
per-project pieces, not new per-project features.

---

## Priorities, in order

Condensed cross-workspace list, reviewed 2026-08-07. Full detail/rationale
for each is in the sections below — this is the fast-scan version.

**#1-3 settled, 2026-08-07.** Session 16 was the genuinely clean
two-spammer+BT trial under `WIFI_PS_NONE` these were all waiting on
(5000 runs, ~5.97h, both spammers and BT confirmed continuous throughout
— see [NETWORK-TIMING-ISSUE.md](NETWORK-TIMING-ISSUE.md#issue-wi-fi-power-save-vs-battery-budget)).
Results: 4940 armed+started, 4911 committed (0.59% lost); of 923 events
needing a retry, only 8 exhausted all attempts (99.1% recovered); the
5002ms `WEBSOCKETS_TCP_TIMEOUT` stall signature recurred 25 times but
every stall/disconnect recovered automatically with zero crashes and zero
corrupted results. Net effect: the ack-path fix
([issue](NETWORK-TIMING-ISSUE.md#issue-acks-not-arriving-back-at-hesperus-in-time-despite-cerberus-receiving-the-event))
is confirmed under real sustained load, and `NONE`
([issue](NETWORK-TIMING-ISSUE.md#issue-wi-fi-power-save-vs-battery-budget))
is confirmed to pass the two-spammer+BT smoke-test bar (graceful
degradation + full auto-recovery, not zero-loss) — while also confirming
`NONE` isn't immune to the severe stall signature either, just apparently
less exposed than `MIN_MODEM`. The `MIN_MODEM`-stays-unadopted decision is
unchanged (it was never contingent on `NONE`'s own result).

1. **Duplicate-trigger lock-out window design** — [issue](NETWORK-TIMING-ISSUE.md#issue-duplicate-triggers-from-gapped-robot-structure).
   Standalone design question, no dependencies — the top open item now.
2. **`BUTTON_COMMAND_MAP` review — reviewed, one action item, deferred to a
   future session** ([race-command-source.h:37](cerberus-gate-controller/src/race/race-command-source.h#L37)):
   a short TOUCH press in race mode should send `<91,1>` to RATS (new
   message type — nearest analog is `MSG_EXTRA_RUN=92`'s shape), replacing
   its current bare-`NEW_MOUSE` mapping that `race-timer.h` deliberately
   no-ops. TOUCH's long-press ("return to menu") is already correct,
   no change needed there. Not implemented yet.
   (`send_run_time()`'s double-send, formerly listed alongside this, is
   explained — a RATS-side belt-and-braces requirement, not a bug here —
   and the code comment updated to say so, 2026-08-07.)
3. **ARES reset-glitch investigation** (see
   [ares-pulse-generator](#ares-pulse-generator) below) — both gates fire
   when ARES resets while wired directly to a pair of hesperus boards.
   Isolated, doesn't block running test trials.

Deprioritized, not pursued right now (see the relevant sections below for
why): GOAL-board retry asymmetry under heavy stress (deferred to
production-board testing — session 16 saw the same asymmetry again at a
smaller 2.5x ratio vs. session 13's 4.6x, doesn't change the deferred
status), congested-airtime stress testing (judged sufficient 2026-08-04),
hedged burst sends for tail latency (watching telemetry only), and all
net-new feature work — cerberus's supervisor state machine / SD-card
logging / HTTP log streaming / `race_runs[]` concurrency guard,
hesperus's NVS config store / OTA / SSD1306 display / stack telemetry /
NVS event buffering / multi-AP BSSID fallback / standalone scoring, and
ARGUS (not started at all).

**Now due: a thorough documentation review.** With #1-3 settled, this is
the next thing — `NETWORK-TIMING-ISSUE.md` and this file have both grown
complex and hard to follow, a long story told poorly, accumulated session
by session. Not started yet.

---

## Cross-project: interoperability & testing

Primary tracker: **`NETWORK-TIMING-ISSUE.md`** (root) — its "Outstanding
work, prioritized" section is the ranked list, kept up to date there rather
than mirrored here. As of 2026-08-07, in priority order:

1. Duplicate triggers from gapped robot structure — lock-out window design unresolved
2. GOAL board retries far more than ARM/START under heavy stress (session
   13: 18.3% vs. 4.0%; session 16: 10.3% vs. 4.2%, same asymmetry at a
   smaller ratio) — cause (physical position vs. role) not yet
   distinguished; the planned start/goal swap is deferred to
   production-board testing
3. Congested-airtime stress testing — judged sufficient 2026-08-04 and
   deprioritized (not formally closed); bulk-throughput contention and
   broadband noise remain genuinely untested, not pursued without a
   concrete reason to
4. Hedged burst sends for tail latency — deprioritized, watch retry telemetry first

Resolved/closed since the last pass: the `wsClient.loop()`-blocking
congestion bug (hardware-verified 2026-08-03), the unexplained WS
jitter/reconnect blip (characterized as benign, closed 2026-08-04), the
ack-path timeout mismatch (confirmed under real sustained load by session
16's clean trial, 2026-08-07), and Wi-Fi power-save vs. battery budget's
own open sub-question — whether `NONE` cleanly passes the two-spammer+BT
smoke test — which session 16 also settled (it does; `MIN_MODEM` still
doesn't get adopted, that decision was separate). See the "Priorities, in
order" section at the top of this file for the full summary.

Other cross-project items not tracked in that file:

- **ARGUS** (data collector/analysis layer) — not started at all. See `docs/KRONOS-SYNCHRONIZED-TIMING-GATES-ARCHITECTURE.md`.
- **Provisioning** — sensor-side mDNS discovery and Wi-Fi credential override are already implemented (`resolveCerberus()`, serial `wifi` command + NVS on hesperus); only deep-sleep power cycling and the sensor registration/auth-token handshake remain proposed. See `docs/PROVISIONING.md`, which now also has a "Recovery from Failure" section (per-scenario: what's automatic vs. needs manual action, including the known "cerberus restarts on a new IP" gap — untested, no repro yet).
- **Reliability: congestion avoidance** (exponential backoff+jitter) — proposed design only, not implemented. See `docs/RELIABILITY.md` (section 2). (`TCP_NODELAY`, formerly listed alongside this, turned out to already be enabled by default via the WebSockets/ESPAsyncWebServer library versions in use — no action needed, see that doc's section 3.)

### Testing gaps

- Congested-airtime stress harness — airtime saturation has extensive
  coverage (spam-test sessions 2-16); bulk throughput contention and
  broadband noise layers are untested and deprioritized, not tooled
  (`docs/TEST-TOOLING.md`)
- Duplicate-trigger lock-out re-test (`trial_double_trigger`) — blocked until the lock-out feature above is built

Closed: the 10,000-message WS jitter test and the power-save
current-draw/latency measurement were both completed (the latter via
sessions 11-15a's spam-test trials rather than the originally-proposed
dedicated bench method); the genuinely confound-free two-spammer+BT
`WIFI_PS_NONE` trial (sessions 15/15a were both compromised by unrelated
equipment issues) was finally delivered by session 16, 2026-08-07 — see
`NETWORK-TIMING-ISSUE.md` for results.

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

- `BUTTON_COMMAND_MAP` reviewed against the state machine, 2026-08-07 —
  one action item, deferred to a future session: a short TOUCH press in
  race mode should send `<91,1>` to RATS (new message type, nearest analog
  `MSG_EXTRA_RUN=92`), replacing its current bare-`NEW_MOUSE` mapping
  that's deliberately a no-op today. Long-press ("return to menu") is
  already correct. Not implemented yet. — `src/race/race-command-source.h:37`

(`send_run_time()`'s double-send, formerly listed here, is explained —
RATS-side belt-and-braces, not a cerberus bug — comment updated
2026-08-07, `src/messages-reference.h:111-116`.)

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
