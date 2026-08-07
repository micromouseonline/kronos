# Network Timing Issue: Observations, Resolutions, and Outstanding Work

Date: 2026-07-30. Restructured 2026-07-31 — see "Restructuring note" at the
bottom for what changed and why.

**What this document is, 2026-08-07.** A chronological investigation log
(lab notebook), not a live issue tracker — dated, session-by-session
findings and decisions from bench-testing the gate/controller network
stack. Entries are historical record: when a later session supersedes an
earlier claim, the old text is struck through and annotated rather than
silently rewritten, so the document stays an honest account of how
understanding changed over time. Because of that, don't treat an
`[OPEN]`/`[RESOLVED]`/`[Reassessed ...]` tag as necessarily *current*
without checking its date against anything later in the same section —
staleness here is a known failure mode, not a hypothetical one (several
tags and cross-references in this doc have been found stale and corrected
after the fact). For genuinely current open work, see `TODO.md`. New bugs
or feature requests going forward are tracked as GitHub Issues, not added
here — this document's job going forward is to stay a record of what was
investigated and found, not a live status board.

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

- **One beacon spammer is extreme, but not adversarial.** The tool itself is
  nominally an attack device, but the load one produces is plausibly in the
  same range a busy exhibition hall's own ordinary infrastructure could
  generate organically (dozens of exhibitor/vendor APs each beaconing at the
  standard rate — see the SSID-density estimate under the outstanding-work
  item below). It's a severe environment, not a hostile one: nothing about
  it requires a deliberate actor. **This is the pass/fail bar.**
- **Two simultaneous spammers (~1000 beacon frames/sec combined) is
  adversarial** — that combination of severity *and* deliberate placement
  (both devices parked right next to the gate at full signal strength) is
  well beyond what a venue's own organic traffic would reproduce; it
  requires someone deliberately trying to disrupt the system. Every radio on
  the channel still has to process each beacon frame at the hardware/MAC
  level regardless of payload, which is why it hits the ESP32's software
  WiFi stack disproportionately hard (see the `wsClient.loop()`-blocking
  issue below) even though a laptop on the same air noticed nothing. Treated
  as a **smoke test** (does the system degrade gracefully and recover
  automatically, without crashing or corrupting a result) rather than a
  pass/fail gate — zero-loss under deliberate, sustained ~1000fps beacon
  flooding is not a requirement.

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

**Planned next steps, 2026-08-03.** The AP has been deliberately placed some
distance from the gates for this round of testing — typical reported `rssi`
sits around -70 to -80dBm, a genuinely weak-to-marginal link (rough rule of
thumb: -50 to -60dBm is good, -60 to -70 usable, -70 to -80 is where 802.11
MAC-layer retransmission rates start climbing on their own, independent of
any beacon-storm software overhead). `rssi` is already carried in every
event payload, so no new instrumentation is needed:

1. Once the current single-spammer 5000-run trial (session 6's counterpart)
   finishes, cross-reference retry/drop occurrences against the `rssi`
   values of the events involved, to check whether failures cluster at the
   low end of the -70/-80dBm range (pointing at marginal signal margin as a
   contributing factor) or look independent of it (supporting the
   beacon-frame-count-overhead theory on its own, per the `wsClient.loop()`
   issue above).
   - **Unconfirmed hunch to check alongside this**: the user suspects their
     own physical position near the test rig during the first ~1000 or so
     runs may itself have been attenuating `rssi` (body blocking
     line-of-sight to the AP), an effect that would fade once they stepped
     away for the remainder of the multi-hour unattended run. Flagged as
     possibly fanciful, not asserted — the way to check it is the plot
     below, not assumption.
   - **Known extra confound, same window**: for this specific trial, the
     user's own BT headphones were also streaming audio within a couple of
     feet of the rig during roughly that same first ~1000 runs, on top of
     the body-proximity hunch above. Mechanistically different from the
     beacon-spam issue this doc otherwise tracks — BT interference would
     show up as ordinary co-channel RF collision/corruption (raised noise
     floor, standard 802.11 MAC retransmissions), not the CPU/management-
     frame-processing overhead the beacon-storm issues are about — so if the
     first ~1000 runs do look worse, it's confounded between body position
     and BT and not attributable to either alone. The following ~2 hours of
     the same trial should be free of both. Noted for the record because
     good experiment hygiene means tracking every condition in play, even
     ones that turn out not to matter.
   - **Curiosity plot, not a formal requirement**: average `rssi` vs. time
     (or run index) across the trial, per board, to see whether there's a
     visible early-run dip/trend consistent with the hunch above, independent
     of the retry/drop correlation in point 1.
2. Afterwards, the AP will be moved to give an `rssi` more representative of
   the planned deployment environment, and the **exact same trial protocol
   re-run** for a direct comparison against the current (deliberately weak
   signal) baseline.

**Results, session 7 (2026-08-03)** — from the single-spammer 5000-run trial
before it was cut short by the ISR/TSF crash bug above (~2882 armed+started
runs completed, 2881 committed, only 1 true loss — see that issue for the
crash itself):

- **RSSI level does not predict retries.** Joining each hesperus-side send
  record to its cerberus-side `rssi` (ordered join per board/event-type,
  tolerant of the one dropped `GOAL`): mean `rssi` for events that needed a
  retry is statistically indistinguishable from events that didn't (ARM:
  -74.9 vs -75.0dBm; START: -75.3 vs -75.0; GOAL: -72.8 vs -73.0). Within
  the -66 to -82dBm range actually seen, `rssi` level itself isn't
  predictive of which specific events fail.
- **Time is a much stronger predictor.** Binning retries by run index:
  ARM retries ran 2.75% in runs 0-800 vs. 0.14% in runs 800-3000 (~20x
  lower); GOAL retries ran 4.5% vs. 0.55% (~8x lower). Essentially all
  retry activity — and the one genuine drop, at run 252 (~20.6 minutes in)
  — falls inside the first ~800-1000 runs (~1-1.2 hours), matching the
  body-position/BT-headphones confound window flagged before this trial.
  The remaining ~2000 runs were close to flawless.
- **This actually points more at the BT headphones specifically than body
  position.** Average `rssi` stayed flat through the early window (no dip)
  — `rssi` measures the WiFi AP signal's own strength, not ambient
  interference, so a body attenuating line-of-sight *would* show up as
  reduced `rssi`; BT co-channel interference degrades packet success
  *without* touching the `rssi` reading at all. Retries elevated with
  `rssi` flat fits "something interfering" better than "something
  attenuating." Not proven (no confound-free control run to compare
  against directly), but a coherent, self-consistent reading of the data.
- Curiosity plot (average `rssi` vs. run index) done as part of the above
  rather than as a separate artifact — see the binned figures; no visible
  early-run dip, per the point above.

**Results, session 8 (2026-08-04)** — the AP-repositioned, confound-free
single-spammer 5000-run trial planned above
(`test-data/spam-tests/{cerberus-8,hesperus-start-8,hesperus-goal-8}.log`),
run to completion (no crash cutoff this time):

- **ISR/TSF crash fix hardware-verified.** Zero panics, zero reboots, on
  both boards, across the full ~6.53-hour, ~5346-run trial under the same
  sustained single-spammer load that surfaced the bug in session 7. See the
  issue below — now marked resolved.
- **Realistic-deployment RSSI, as intended.** Mean `rssi` -65.8dBm (range
  -56 to -76dBm), a clear improvement over session 7's deliberately-weak
  -66 to -82dBm/mean ~-75dBm baseline, confirming the AP repositioning
  worked.
- **Retries near-zero, and — the key check — no longer front-loaded.** Only
  5 distinct events needed a retry out of 16,034 total (0.03%), identified
  via cerberus-side duplicate receipts of the same `tsf_us`. Binned across
  the full trial: **zero retries in the first ~2 hours**, the few that
  occurred fall in hours ~2-4.4, none after. This is the confound-free
  control session 7 lacked: with the body-position/BT-headphones confound
  genuinely absent this time, retries do not show the ~20x/~8x early-window
  spike session 7 saw — supporting the session-7 reading that that spike
  was confound-driven, not RSSI- or time-of-trial-driven. RSSI level still
  isn't predictive of which events retry (e.g. ARM: -67.0dBm retried vs.
  -66.8dBm clean, statistically indistinguishable).
- **Item 1 (ack-path) recurred once, same signature as before.** A burst at
  T≈14.09-14.13M ms: three ARM events needed retries within a 40-second
  window, one needing 9 attempts before succeeding. Cerberus's `[WS-ACK]`
  fired within 3-8ms on every single delivery (again ruling out "cerberus
  is slow"), yet hesperus didn't recognise the ack in time — consistent
  with item 1's still-open two candidate causes, not yet distinguished
  further by this session.
- **Race outcome, once one operator-caused artifact is excluded (confirmed
  with the user, see below): effectively lossless.** Start board
  armed+started 5346 runs; cerberus received 5345 of the ARM+START pairs
  over the network (the one gap lines up exactly with the artifact below);
  goal board sent 5344 GOALs (2 runs left incomplete by the same artifact),
  and cerberus received all 5344 of them — **zero GOAL-side network loss**.
- **Explained anomaly, not a bug.** At T≈2.6M ms (~43 minutes in), both
  hesperus boards and cerberus went completely silent simultaneously for
  ~8.7 minutes (no sends, no receipts, no crash/reboot/disconnect logged by
  any of the three) before resuming cleanly — 2 runs left incomplete (armed
  and started, no GOAL), one of which also never reached cerberus at all.
  **Confirmed with the user**: this was a deliberate mid-trial adjustment,
  with the log not reset before resuming — which also explains the run
  count landing at ~5346 rather than the nominal 5000. Excluded as an
  operator artifact, same treatment as the cold-start/cutoff artifacts
  excluded in sessions 3-5; not a system fault. One of the goal board's
  `[AUDIT ALERT] Temporal Disruption!` self-corrections (see the "AP radio
  interruption" issue's recurrence note) lands at the exact resume instant
  and is fully explained by it (a large TSF gap after 8+ minutes without a
  fresh sample). The other 6 (5 goal-board, 1 start-board) occur during
  otherwise-normal running with no nearby gap — each a single rejected
  sample, immediately self-corrected, no operational impact — extending the
  session-6 recurrence note: this now happens during active running too,
  not just post-trial idle. Still unexplained, still low-priority and
  self-healing.

## Outstanding work, prioritized

1. **[Acks not arriving back at hesperus in time](#issue-acks-not-arriving-back-at-hesperus-in-time-despite-cerberus-receiving-the-event)**
   — **[RESOLVED, tuning applied 2026-08-04, not yet hardware-verified.]**
   End-to-end instrumentation on both boards (cerberus's `pending`/`space`,
   hesperus's `[WS-ACK-RECV]` receive timestamp, both on the shared TSF
   clock) traced all 10 retried events in a two-spammer+BT marginal trial
   start to finish: **every one of them was the original ack arriving
   fine, just with a round trip (258-458ms) that happened to exceed
   hesperus's fixed ~300ms per-attempt timeout.** Not loss, not a stack
   stall on either side (ruled out via `wsPumpTask`'s own blocking canary,
   which fired only once in the whole trial and didn't line up with any of
   the 10), not asymmetric (both legs contribute comparably) — ordinary
   bidirectional jitter under heavy interference, occasionally summing
   past a fixed timeout, most likely at the WiFi MAC layer itself given
   both boards' software was cleared. Retry/dedup already handled it
   correctly throughout (zero data loss in any session this showed up in),
   and it only ever appeared under the explicitly-adversarial two-spammer
   smoke test, not the single-spammer pass bar (0.03% retry rate there).
   **`WS_ACK_TIMEOUT_MS` raised 300→500ms** (jitter and
   `WS_ACK_OVERALL_DEADLINE_MS` scaled proportionally alongside it, so
   `WS_MAX_SEND_ATTEMPTS`'s retry depth for genuine loss isn't quietly
   reduced) rather than the previously-considered redundant-ack-burst
   mitigation, which was built around a loss model this data doesn't
   support. Build-verified; next step is a session-9/10-style marginal
   trial confirming the retry count drops **under `NONE`** — session 13
   (2026-08-05) ran the harsher two-spammer+BT combination but under
   `MIN_MODEM`, so it doesn't isolate this fix cleanly (see item 2); the
   forward-leg delivery held up well there regardless (every genuinely
   transmitted event eventually reached cerberus), which is a good sign
   but not the controlled re-run this item still needs. **Session 15**
   (2026-08-06) attempted this under `NONE` but one spammer dropped out
   ~30 minutes in (equipment issue, not a finding) — both segments'
   retry rates looked healthy, but it's suggestive, not the clean
   confirmation this item needs. **Session 15a** (2026-08-06, the planned
   3000-run re-run) was *also* compromised — BT was found off partway
   through, back on at a known timestamp — but showed sustained stress
   throughout regardless, with overall retry rates (1.46%/9.47%) well
   under session 13's `MIN_MODEM` baseline (4.0%/18.3%) over a comparable
   exposure. Two compromised trials in a row is enough supportive signal
   to lower this item's urgency, but a genuinely clean confound-free run
   still hasn't happened. **Session 16 (2026-08-07) is that clean run**:
   full 5000-run, ~5.97h two-spammer+BT trial under `NONE`, both spammers
   and BT confirmed continuous throughout, no equipment confound. Retry
   rates 4.15%/10.32% (ARM/START vs. GOAL) — of the 923 events that needed
   at least one retry, only 8 exhausted all retries (99.1% recovered).
   **Confirms the fix**: the widened timeout is doing real work under
   sustained real adversarial load, not just the shorter/compromised
   trials this item was leaning on before. Full per-event round-trip
   tracing (the session 9/10-style breakdown showing *why* each retry
   happened) wasn't repeated for this session — the aggregate recovery
   rate is the evidence here, not a re-derivation of the mechanism, which
   sessions 9/10 already established. Full detail in the issue below.
2. **[Wi-Fi power-save vs. battery budget](#issue-wi-fi-power-save-vs-battery-budget)**
   — Measure current draw across `WIFI_PS_NONE`/`MIN_MODEM`/`MAX_MODEM`
   with persistent connections in place. Real, measured 110mA cost today;
   the thing this was explicitly deferred pending (persistent connections)
   has now landed, so this is unblocked. **Decision, 2026-08-04: `NONE`
   stays the shipped default regardless of what the overnight trial
   below shows.** Session 11 found a real, if not-yet-fully-explained,
   `MIN_MODEM` reliability regression (see the issue below); a clean
   overnight repeat wouldn't be enough to overturn that — absence of a
   recurrence in one more trial is not evidence of absence of a rare tail
   risk, and the asymmetry between "run `NONE` at higher power
   indefinitely" and "have an outage during a real contest" clearly favours
   caution. The overnight trial is now diagnostic-only (does the mechanism
   recur, at what rate), not a pass/fail gate for adoption — `MIN_MODEM`
   would need to be root-caused, fixed, and *then* re-verified extensively
   before this decision would be revisited. **Update, 2026-08-05**: session
   12 (single-spammer repeat) showed zero recurrence; session 13
   (two-spammer+BT smoke test) showed the mechanism recurring twice in one
   trial — occurrence count now tracks stress severity directly (0/1/2
   across sessions 12/11/13), which is itself evidence for a genuine
   mechanism rather than coincidence. **Update, same day**: the candidate
   fix (widen `enableHeartbeat()`'s pong-timeout/miss-count) was applied
   and tested (session 14) — it made every measure dramatically worse
   (drops, stalls, disconnects, stale-GOALs, retry rates all 2-136x worse
   per hour than session 13's unmodified baseline), not better. Reverted.
   The mechanism remains unexplained and unmitigated; this specific
   direction (widen tolerance) is now ruled out, not just untried — any
   future attempt needs a different theory of the failure. **Update,
   2026-08-06**: session 15 (`NONE`, same nominal two-spammer+BT stress)
   compromised by a spammer dropping out ~30 minutes in, but the ~30
   minutes of genuine exposure it did get showed no stall approaching the
   5000ms signature that defines the `MIN_MODEM` mechanism — suggestive
   that it's `MIN_MODEM`-specific, on weaker evidence (30min vs. 6h) than
   the recurrence finding it's being weighed against. **Revised, same
   day**: session 15a (the planned clean re-run, also compromised — BT
   found off partway through) recorded one genuine **5002ms** stall under
   `NONE` during its confirmed-no-BT period, hitting the exact
   `WEBSOCKETS_TCP_TIMEOUT` signature. **The severe cascade is not
   `MIN_MODEM`-exclusive after all** — it's a general two-spammer
   WiFi-stack-overhead property `NONE` isn't immune to, just apparently
   less frequent/severe under it (one occurrence in ~3.6h vs. repeated
   occurrences per `MIN_MODEM` trial) rather than absent. Both `NONE`
   trials to date have been confound-compromised; a genuinely clean run
   under either power mode still hasn't happened. **Session 16
   (2026-08-07) is that clean run for `NONE`**: full 5000-run, ~5.97h
   two-spammer+BT trial, both interference sources confirmed continuous
   throughout. **The 5002ms signature is not rare under `NONE` either**
   — 25 of ~~158~~ **79** (see raw-log recount, 2026-08-07, in the issue
   detail below — the 158 figure's source wasn't identified) total stalls
   landed in the 4900-5100ms band across the trial, ~~spread throughout
   rather than clustered in one window (unlike `MIN_MODEM`'s acute
   2-minute/14-15s episodes in sessions 11/13)~~ **actually clustered: ~85%
   of the large stalls fall inside one ~9.7-minute episode at the very
   start of the trial, comparable in kind to `MIN_MODEM`'s acute episodes
   just front-loaded rather than mid-run — see the corrected finding
   below.** 34 WS disconnects total (confirmed correct). Despite that,
   race outcome was 4940
   armed+started, 4911 committed (29 lost, 0.59%) — no crash, no reboot,
   no corrupted result, fully automatic recovery every time. **This is a
   clean pass of the smoke-test bar this trial was always held to**
   ("degrades gracefully and recovers automatically" — not zero-loss,
   see the acceptance-criteria section above) — the first genuinely
   confound-free confirmation that `NONE` survives the harshest tested
   condition without manual intervention. Does not overturn the
   `MIN_MODEM`-stays-unadopted decision (that was never contingent on
   `NONE`'s own smoke-test result) — it closes out the parallel question
   of whether `NONE` itself has ever been cleanly verified under this
   condition. It hadn't been; now it has.
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
   remains open too, folded into item 5 below. **Proposed experiment,
   2026-08-07 (not yet run)**: session 16's raw-log recount found the
   severe stalls cluster into episodes rather than spreading evenly, with
   the fastest-onset episodes landing suspiciously close to trial start —
   but not in every session. A cheap two-trial pair (~75min total) is
   designed to isolate whether traffic-onset or connection-freshness (both
   normally coincide at trial start) is the actual trigger — see the
   dedicated write-up at the end of the issue below.
4. **[Duplicate triggers from gapped robot structure](#issue-duplicate-triggers-from-gapped-robot-structure)**
   — **[Reassessed 2026-08-07 — deprioritized, lock-out design not being
   pursued.]** The proposal's own justification (needless network pressure
   from extra events) was written under HTTP's per-event connection cost;
   this issue's own WS re-run data (200/200 arrived, ~7-8ms latency, the
   104ms queueing offset gone) already shows that premise doesn't hold
   anymore. Multiple triggers from one robot are rare in practice, and
   under WS a redundant trigger costs almost nothing to send and is already
   harmlessly ignored once out of `RUNNING` — if anything, sending
   duplicates is closer to a reliability feature (a free extra delivery
   attempt) than a problem worth suppressing at the source. Not formally
   closed (a genuinely gapped robot could still exist), just no longer the
   standing design question it was.
5. **Congested-airtime stress testing** (cross-cutting, not tied to one
   issue) — **[Judged sufficient, 2026-08-04 — deprioritized, not
   formally closed.]** The four stressor layers sketched in
   `hesperus-timing-gate/review.md`: **airtime saturation** has extensive
   coverage (beacon-spam sessions 2-10, up to two simultaneous spammers)
   and **channel interference** partial coverage (BT streaming co-tested
   alongside spammers, sessions 9-10); **bulk throughput contention** and
   **broadband noise** remain genuinely untested. Decision: given the
   system handled the harshest combination actually tested (two spammers +
   BT streaming, deliberately parked next to the gate) with zero drops,
   zero crashes, and the one real issue that surfaced (the ack-path
   timeout mismatch) understood and fixed, further chasing the two
   untested layers is diminishing returns relative to what a real contest
   venue is actually likely to produce — other exhibitors' AP
   beacon/management-frame overhead, not deliberate bulk-throughput
   hogging or broadband jamming. Not pursuing further unless a specific
   venue's known conditions give a concrete reason to.
6. **[Hedged burst sends](#issue-hedged-burst-sends-for-tail-latency)** —
   deprioritized; watch real-world retry counts/rate from the now-shipped
   ack/retry mechanism before building this. No action needed unless that
   telemetry shows frequent retries.
7. **[Unexplained minor WS jitter / reconnect blip](#issue-unexplained-minor-ws-jitter--reconnect-blip)**
   — **[Characterized and closed, 2026-08-04 — no periodic cause found.]**
   Rather than running the proposed purpose-built 10,000-message GOAL-only
   trial, used the existing confound-free 5000-run no-spammer baseline
   (session 6, `test-data/spam-tests/cerberus-6.log`) instead — 5000 real
   `GOAL` events at a uniform ~4.3s cadence, arguably a better substitute
   than the synthetic proposal since it's real protocol traffic, not
   synthetic pulses, and 5000 comfortably exceeds the proposed 10,000
   *messages* once ARM/START are counted too (this pass used `GOAL` alone
   to match the original single-event-type proposal). Latency: mean 8.8ms,
   p90 12.0ms, p95 14.6ms, p99 19.0ms, max 63.8ms; 30/5000 (0.6%) exceeded
   20ms. Checked those 30 outliers' index spacing for periodicity (the
   original leading hypothesis — "a spike every N messages, pointing at a
   fixed-period task like the heartbeat timer or a WS keepalive interval")
   — spacing is irregular (1 to 697 events apart, no repeating interval),
   ruling that out across a sample 25-50x larger than the n=100-200 that
   originally flagged this. Consistent with ordinary non-periodic
   scheduling/RF jitter, not a hidden periodic bug. The original issue's
   own caveat ("would not on its own isolate which stage the delay happens
   in") is now moot rather than resolved — sessions 8-10's ack-path work
   has since added exactly the hesperus-side send/receive timestamp this
   would have needed as a follow-up, but with no pattern found there's
   nothing left to isolate. Closed as characterized-and-benign; watch for
   recurrence rather than investigate further.
8. **[GOAL board retries far more than ARM/START board under heavy
   stress](#issue-goal-board-retries-far-more-than-armstart-board-under-heavy-stress)**
   — **[OPEN, deferred 2026-08-05]** Session 13's two-spammer+BT smoke
   test showed the GOAL board retrying 4.6x more often than the ARM/START
   board (18.3% vs. 4.0%). Cause not yet distinguished between physical
   placement and role/traffic-pattern. The planned start/goal position
   swap is deferred to the production-board testing stage — the current
   hand-wired breadboard rig has too many uncontrolled physical variables
   for a swap result to be trustworthy.

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

**Revised, 2026-08-07** (more accurate monitoring): **113mA**, not 110mA
— a small (~2.7%) upward correction to the baseline above. Every
`MIN_MODEM`-vs-`NONE` percentage-reduction figure elsewhere in this issue
(sessions 11/12: ~34%/~42% reduction, etc.) was computed against the
original 110mA figure and hasn't been recalculated against 113mA — treat
those percentages as slightly optimistic (by roughly this same ~2.7%)
rather than re-deriving each one for a correction this small.

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
`MAX_MODEM` is not part of this round of testing — already ruled out (see
Observation above).

**`WIFI_PS_MIN_MODEM` enabled for testing, 2026-08-04**
(`hesperus-timing-gate/src/main.cpp`, one-line change from the existing
`esp_wifi_set_ps(WIFI_PS_NONE)`). Build-verified (`pio run -e
hesperus-gate-s3-zero`); not yet hardware-verified. Plan: (1) current-draw
comparison against the 110mA `NONE` baseline (user, physical measurement);
(2) a single-spammer trial — deliberately the established pass-bar
condition (session 8's confound-free baseline: 0.03% retry rate), not a
harsher one, since the question here is specifically whether `MIN_MODEM`
regresses the already-accepted baseline, not a fresh stress
characterization — checking retry rate for a regression signal. **Both
hesperus units (start and goal) need reflashing** — this line is in the
shared `main.cpp`, not board-role-specific code. Revert to
`esp_wifi_set_ps(WIFI_PS_NONE)` if either measurement doesn't support
keeping it.

**Session 11 results (2026-08-04)** (`test-data/spam-tests/{cerberus-11,
hesperus-start-11,hesperus-goal-11}.log`) — single-spammer, `MIN_MODEM`,
~5h active + ~90min idle (~6h34m total, 14,968 total ARM+START+GOAL
events):

- **Current draw: 480mAh / 6h34m ≈ 73mA average** — a real ~34% reduction
  from the 110mA `NONE` baseline. Blended across active + idle time, not a
  clean single-condition number — a dedicated idle-only baseline is the
  planned next measurement.
- **Ordinary retry rate elevated ~47x.** Excluding the acute episode below,
  210/14,968 events (1.4%) needed a retry, vs. session 8's confound-free
  `NONE` baseline of 0.03% under the same single-spammer condition. All
  still succeeded (same harmless jitter-vs-timeout mechanism session 10
  characterized) — consistent with modem sleep adding latency to most wake
  cycles, not just rare outliers, so more events graze the timeout even
  though `WS_ACK_TIMEOUT_MS` was already widened.
- **One acute ~2-minute episode broke the single-spammer zero-loss bar.**
  At T≈50.66M-50.79M ms, both boards simultaneously logged repeated
  `wsClient.loop()` stalls from 217ms up to **7.6 seconds**
  (`wifi_status=3`/WiFi-associated, `ws_connected=0`), 6 WS disconnects,
  and **8 genuine event drops** (`"dropped after ack deadline"`/`"after
  max retries"`). This exact `wsClient.loop()`-blocking signature (the
  same one from the original congestion investigation) occurred **zero
  times** in session 8's equivalent-length `NONE` single-spammer baseline.
  Every single-spammer trial to date has been zero-loss until this one.
  Not proven causal from a single trial (can't fully rule out an
  unrelated environmental event coinciding with this run), but the
  contrast with session 8 makes `MIN_MODEM` the leading suspect, not a
  coincidence, pending a repeat trial.
- **Reading**: `MIN_MODEM`'s ~34% power saving currently comes with a real
  reliability cost that breaks the established single-spammer pass bar —
  not yet a case for adopting it as-is. Next steps before any decision:
  the clean idle-only current-draw baseline (in progress), and ideally a
  second single-spammer `MIN_MODEM` trial to see whether the acute-episode
  pattern recurs (systematic problem) or was a one-off (still concerning
  given the elevated ordinary retry rate regardless, but a different
  severity picture).
- **Mechanism, on closer look — not just "channel congestion."** The 17
  stall durations aren't uniformly random: 8 of them land within 2ms of
  exactly **5000ms** (5001-5002ms, one outlier at 4880ms). Checked the
  actual `links2004/WebSockets` library source (pinned `^2.4.1`, resolved
  to 2.7.x) rather than guessing: `WEBSOCKETS_TCP_TIMEOUT = 5000`
  (`WebSockets.h`) is the library's own hardcoded TCP socket timeout,
  applied during connect/reconnect. Combined with hesperus's own heartbeat
  config (`main.cpp:936`, `enableHeartbeat(5000ms ping, 3000ms pong
  timeout, disconnect after 2 misses)`), a coherent chain emerges: (1)
  **trigger** — `MIN_MODEM`'s sleep window is a liability only if it
  happens to coincide with a burst of missed/corrupted real-AP beacons
  severe enough to blow through the 3s pong timeout twice in a row (a rare
  probabilistic coincidence with the already-documented beacon-processing-
  overhead effect, not a standing condition — `WIFI_PS_NONE` is never
  exposed to this since it's always listening); (2) **cascade** — once
  disconnected, several consecutive *reconnect* attempts can each
  independently hit the library's fixed 5s TCP timeout if conditions are
  still degraded, chaining into the observed ~2-minute total outage — a
  `WebSockets`-library property, not firmware-specific; (3) **why it
  doesn't recur** — it needs that specific rare coincidence, so going
  quiet for the remaining hours is consistent with a low-probability tail
  event, not evidence against the mechanism. Still a hypothesis, not
  proof — the overnight repeat trial is the actual test: recurrence at
  some low rate supports it, continued absence over a much longer run
  weakens it.

**Decision: `NONE` stays the shipped default, 2026-08-04.** Decided
*before* running the overnight trial, deliberately, to avoid rationalizing
a marginal result after the fact: given the asymmetric stakes (extra
~35-55mA running `NONE` indefinitely vs. a genuine outage during a real
contest), a clean overnight trial would not be sufficient evidence to
adopt `MIN_MODEM` — absence of a recurrence in one more trial is not
evidence of absence of a rare tail risk, especially given the mechanism
above is understood well enough to predict occasional low-probability
recurrence, not ruled out entirely. The overnight trial's role from here
is diagnostic only (does it recur, at roughly what rate, does the
5000ms-timeout signature repeat) — not a pass/fail gate for production
adoption. `MIN_MODEM` would need the mechanism actually fixed (e.g. a
widened heartbeat pong-timeout/miss-count, discussed but deliberately not
yet applied so as not to confound this trial) and then re-verified
extensively before this decision would be revisited.

**Idle-only current draw, 2026-08-04**: 17mAh over 18 minutes ≈ **56mA**
average idle draw under `MIN_MODEM` (short sample, but a real number, and
cleaner than the blended 73mA above since no spammer or events were
active). Of that, the status NeoPixel (solid green when `READY`, `main.cpp`'s
`ledDiagnosticTask`) accounts for ~20mA — over a third of total idle draw.
**Considered and declined, 2026-08-04**: reducing it via a duty-cycled
flash (e.g. 100ms on/900ms off, ~10% duty, ~2mA) instead of solid-on.
Decision: keep it solid — the LED is the operator's at-a-glance
"gate alive" signal, and a brief once-a-second flash trades real
visibility (especially in bright outdoor daylight at an actual contest)
for a comparatively small (~18mA) saving next to the larger open question
above (the `MIN_MODEM` reliability regression). Not revisited unless the
power budget becomes tight enough that every mA matters.

**Scope note applying to every current-draw figure in this issue**
(110mA `NONE` baseline, 73mA/63.6mA/62.8mA `MIN_MODEM` blended figures,
56mA idle-only): each includes this same ~20mA constant for the single
WS2812 status NeoPixel, held solid (green or red) continuously regardless
of power-save mode or trial condition — it's gate electronics, not radio
behaviour, so it's a fixed offset present in every measurement here, not
something that varies with `NONE` vs. `MIN_MODEM` or with trial severity.
The radio+MCU portion alone is therefore each headline figure minus ~20mA
(e.g. `NONE`: ~90mA; `MIN_MODEM` idle-only: ~36mA) — worth keeping in mind
before comparing these numbers to any other project's bare radio power
figures.

**Session 12 results (2026-08-05)** (`test-data/spam-tests/{cerberus-12,
hesperus-start-12,hesperus-goal-12}.log`) — the planned second single-spammer
`MIN_MODEM` repeat, checking whether session 11's acute episode recurs:
9999 runs, ~11.94 hours.

- **Current draw: 769mAh / 12h6m ≈ 63.6mA average** — closer to the ~56mA
  idle-only figure than session 11's 73mA blended figure (this trial's
  active/idle mix differed), and still a clear ~42% reduction from the
  110mA `NONE` baseline.
- **Session 11's acute episode did not recur.** Zero `wsClient.loop()`
  stalls, zero WS disconnects, zero "dropped after ack deadline"/"max
  retries" lines in either hesperus log. Ordinary retry rate 222/29,997
  events (0.74%) — elevated vs. session 8's `NONE` baseline (0.03%) but
  roughly half session 11's ordinary rate (1.4%); no front-loaded confound
  pattern in the binned breakdown. Max attempts needed to succeed: 4 (two
  events).
- **One race-outcome loss, judged a trial-protocol artifact, not a system
  fault (confirmed with the user).** A GOAL (`tsf_us=89063696892`) needed 4
  retry attempts spanning ~1.69s before its ack landed; by the time it
  reached cerberus the same pulser had already fired ARM+START for the
  *next* run (`start_tsf_us=89064996879`, ~1.1s after the GOAL trigger), so
  the stale-GOAL guard correctly rejected it (`[RACE] rejected stale GOAL:
  tsf_us=89063696892 < start_tsf_us=89064996879`) — working as designed,
  the first time this guard has fired on a real congestion-delayed event
  rather than a synthetic `--tsf-us` test. **Not counted against the
  single-spammer pass bar**: no real run can have a GOAL followed by the
  next ARM within ~1 second — the trial's rapid-fire back-to-back cycling
  is purely a convenience of the test protocol, not a scenario the system
  needs to survive. A different failure mode from session 11's (a single
  retry-delay outrunning the next run's own ARM/START, not a
  stall/disconnect cascade), so this doesn't confirm session 11's
  `wsClient.loop()`/5000ms-timeout mechanism recurring — it's a separate,
  narrower way `MIN_MODEM`'s added latency can bite, and one that requires
  unrealistically fast run cycling to trigger.
- **Reading**: strengthens the case that session 11's acute episode was a
  rare tail event rather than a standing `MIN_MODEM` property, and adds a
  second independent current-draw data point in the low-60s mA range. Does
  not change the 2026-08-04 decision (`NONE` stays shipped default) —
  that decision explicitly treats repeat trials as diagnostic, not a
  pass/fail gate; `MIN_MODEM` still needs the session-11 mechanism
  root-caused and fixed before re-adoption is reconsidered.

**Session 13 results (2026-08-05)** (`test-data/spam-tests/{cerberus-13,
hesperus-start-13,hesperus-goal-13}.log`) — a deliberately adversarial
smoke test, not a pass-bar trial: `MIN_MODEM`, two spammers + BT
streaming simultaneously (the harshest condition in the acceptance
criteria), current firmware as-shipped (500ms/5200ms ack-timeout
settings from the item-1 fix). 5000 runs, ~6.06 hours. Current draw not
measured this run (reliability-only).

- **Session 11's acute stall/disconnect mechanism recurred, twice, and
  worse.** Two episodes (T is approx 146744-146758ms, ~14s;
  T is approx 148894-148909ms, ~15s), both boards hitting the same 5002ms
  `wsClient.loop()` block signature session 11 identified (the
  `WebSockets` library's hardcoded `WEBSOCKETS_TCP_TIMEOUT`),
  disconnect/reconnect on both boards, some events exhausting all 10
  retry attempts. 17 genuine network-level event losses total (12
  ARM/START-side, 5 GOAL-side) out of 15,000 physical triggers,
  concentrated in these two windows.
- **Conclusion: this is a real, stress-scaling mechanism, not a rare
  coincidence.** Occurrence count tracks stress severity directly across
  the three `MIN_MODEM` trials to date: 0 times (session 12,
  single-spammer), 1 time (session 11, single-spammer), 2 times (session
  13, two-spammer+BT). That gradient is itself evidence for a genuine
  congestion-triggered mechanism, not an unrelated coincidence.
- **Mitigation available, not yet applied: widen the heartbeat
  pong-timeout/miss-count** (`enableHeartbeat()`,
  `hesperus-timing-gate/src/main.cpp:936`). Proposed when session 11 first
  found this, deliberately withheld through sessions 12-13 so those trials
  would cleanly show whether the mechanism recurs unmodified. It now has:
  applying it and re-running a comparable two-spammer+BT trial is the
  concrete next step, not further unmodified repeats.
- **New, unexplained finding: the GOAL board retried 4.6x more often than
  the ARM/START board under this specific stress combination** (18.3% vs.
  4.0%, 912/4993 vs. 403/9984). Not seen at this magnitude in any prior
  session. Two live hypotheses, not yet distinguished: physical placement
  relative to the spammers/BT source (RF-level; would predict the
  asymmetry follows *position*, not the specific board unit), or something
  about the GOAL board's single-event-type send pattern versus the
  ARM/START board's interleaved two-event pattern (would predict the
  asymmetry follows *role*, which is coupled to position in the current
  rig wiring and can't be fully separated from it by a swap alone). A
  physical position swap (start/goal roles exchanged between the two
  physical hesperus units) is planned to test whether the asymmetry moves
  with the position or stays with the specific board — won't fully isolate
  role from position, but will rule out "specific unit's hardware" as the
  sole cause either way.
- **Objection-#4-relevant data point, still bounded.** The stale-GOAL
  misattribution mechanism (see that issue below) fired 3 times this
  trial; worst resolution time for anything that actually succeeded was
  2.39s (5 attempts) — under the roughly-3s assumed-safe goal-to-rearm
  gap, but closer to it than anything seen pre-fix (session 10: 462ms
  max). **Conclusion: doesn't yet justify widening the ack timeout/deadline
  further** (nothing observed exceeded the current safe margin), but does
  confirm the risk is real under this specific adversarial combination,
  not hypothetical. See the stale-event-misattribution issue below for a
  more targeted fix under consideration instead of a further timeout
  widening.
- **Caveat, explicitly**: this was the deliberately harshest condition in
  the acceptance-criteria framework (two-spammer+BT), run specifically to
  surface issues that might not appear under realistic load — none of
  these findings say anything about behaviour under the single-spammer
  pass bar (sessions 3/4/5/8/12, still the operative standard) or normal
  contest conditions. Treat as smoke-test diagnostics, not a reason to
  revisit the pass-bar conclusion.

**Session 14 results (2026-08-05)** (`test-data/spam-tests/{cerberus-14,
hesperus-start-14,hesperus-goal-14}.log`) — the heartbeat-widening fix
proposed above (`enableHeartbeat` pong-timeout 3000ms→5000ms, miss-count
2→3), tested under the same `MIN_MODEM` + two-spammer+BT smoke test as
session 13, ~2900-3000 runs, 3.58 hours.

- **The fix made things dramatically worse, not better — tested and
  rejected.** Every measure moved the wrong way versus session 13's
  unmodified baseline (normalized per-hour, since durations differ):
  genuine drops 3.8→63.7/hour (16.8x), `wsClient.loop()` stalls
  2.6→27.4/hour (10.4x, max single stall 5002ms→**15,968ms**), WS
  disconnects 0.8→4.6/hour (5.5x), stale-GOAL rejections 0.5→67.3/hour
  (136x), ARM/START retry rate 4.0%→9.9%, GOAL retry rate 18.3%→**42.6%**.
  Live-observed by the user mid-trial before the full-log analysis
  confirmed it at scale.
- **Coherent mechanism, not noise**: widening the pong-timeout/miss-count
  only changes how long hesperus tolerates silence before declaring the
  link dead and forcing a reconnect — it doesn't improve connection
  *quality*. Against a genuinely degraded (not merely jittery) connection,
  a longer tolerance window means hesperus sits on a zombie connection
  longer before tearing it down and recovering, which is consistent with
  the 3x jump in max stall duration and the explosion in drops/stale-GOALs
  (more retries and more late GOALs piling up during a now-longer
  not-yet-recognized-as-dead window, instead of a faster clean reconnect).
  The mitigation optimized for the wrong side of the tradeoff: it aimed to
  stop a merely-jittery connection being killed unnecessarily, but this
  stress condition is dominated by the opposite case (genuinely dead
  connections recovering slower).
- **Reverted, 2026-08-05** (`hesperus-timing-gate/src/main.cpp:936`), back
  to `(5000, 3000, 2)`. Build-verified. Both boards need reflashing before
  any further `MIN_MODEM` trial — session 14's build should not be reused.
- **Where this leaves the `MIN_MODEM` mechanism**: still unexplained,
  still unmitigated. The heartbeat-widening hypothesis is now
  disconfirmed, not just unconfirmed — this was a real test of a real
  candidate fix, and it failed clearly enough to rule out "just widen the
  tolerance" as a viable direction. Any future mitigation attempt needs a
  different theory of the failure, not a bigger version of this one.

**25-hour blended current draw, 2026-08-06** (spanning sessions 11-14
continuously, `MIN_MODEM`, gates never powered off between trials, mixed
single-spammer/two-spammer+BT conditions plus idle gaps): **1569mAh over
25h ≈ 62.8mA average.** Consistent with session 12's 63.6mA and the
dedicated 56mA idle-only baseline — reinforces the ~55-65mA range as
`MIN_MODEM`'s real-world draw regardless of stress condition, including
across session 14's reconnect-heavy pathological period.

**`NONE` blended current draw, 2026-08-06** (spanning sessions 15/15a,
`WIFI_PS_NONE`, two-spammer+BT stress): **1202mAh over 12h15m ≈ 98.1mA
average.** A real-conditions `NONE` figure to set directly against the
`MIN_MODEM` one above, both measured the same way (blended across actual
stress trials, not a clean single-condition bench figure) — refines the
power-saving estimate to **~36%** (98.1→62.8mA) under comparably harsh
conditions, close to but a bit under the original ~40%+ figure derived
from the 2026-07-31 110mA idle/light-load baseline. Either way, the
open question stays reliability, not power — power was never in doubt.

**Session 15 results (2026-08-06)** (`test-data/spam-tests/{cerberus-15,
hesperus-start-15,hesperus-goal-15}.log`) — `WIFI_PS_NONE` (heartbeat
reverted to `(5000,3000,2)`), intended two-spammer+BT, 5000 runs, ~5.97h.
**Compromised trial: one spammer went offline partway through** (confirmed
by the user; not caught until after the run). Written up anyway with the
caveat stated plainly, per the same treatment past confounded sessions
(5, 7, 8) got — a spotted, locatable confound doesn't make the data
meaningless, it just changes what question it can answer. **A clean
same-condition re-run (3000 runs) is planned.**

- **The dropout is precisely locatable.** Nearly all problems (11/16 WS
  disconnects, 13/18 drops, 4/5 stale-GOAL rejections) cluster into one
  ~17.7-minute window, T≈13.5-31 minutes into the trial — matching the
  user's own estimate ("possibly within the first 30 minutes") almost
  exactly. Outside that window, the remaining ~5.7h has only two small
  isolated blips, no clustering.
- **Segmenting at that boundary gives two genuine (if unplanned) results**,
  not one intended one:
  - **First ~30min (genuine two-spammer+BT+`NONE`)**: ARM/START retry 4.20%
    (n=858), GOAL retry 6.73% (n=431), max `wsClient.loop()` stall ~1.2s,
    zero "dropped after max retries" (only ack-deadline/link-down). Against
    session 13 (`MIN_MODEM`, full 6h, same nominal stress): ARM/START retry
    is almost identical (4.20% vs 4.0%) — that side of the congestion hit
    seems power-save-independent — but GOAL retry is much lower (6.73% vs
    18.3%) and, most notably, **no stall ever approached the 5000ms
    `WEBSOCKETS_TCP_TIMEOUT` signature** that defined sessions 11/13/14.
    Supportive of the acute cascade being `MIN_MODEM`-specific (or at least
    far less severe under `NONE`) — but resting on ~30 minutes of exposure
    against sessions 13/14's full 6h+3.6h, so much weaker evidence than the
    `MIN_MODEM`-recurs finding it's being compared against.
  - **Remaining ~5.7h (one spammer + BT only, `NONE`)**: retry rates crash
    to 0.02%/0.09% (n=9127/4566) — at or below the single-spammer `NONE`
    pass-bar baseline (session 8: 0.03%). Not the intended two-spammer
    condition, but a real, fairly large single-spammer+BT+`NONE` data
    point that hadn't specifically been characterized at this scale before
    (prior single-spammer pass-bar sessions didn't run BT alongside) —
    incidental value, not wasted.
- **Item 1 (ack-path fix) still not cleanly verified.** This was supposed
  to be the controlled two-spammer+BT re-run confirming the retry-count
  drop under `NONE`; the shortened hard-stress window means it's
  suggestive (both segments' retry rates look healthy) rather than the
  clean confirmation intended. Remains open pending the planned re-run.

**Session 15a results (2026-08-06)** (`test-data/spam-tests/{cerberus-15a,
hesperus-start-15a,hesperus-goal-15a}.log`) — the planned clean re-run,
`WIFI_PS_NONE`, two spammers + BT, 3000 runs, ~3.58h. **Also compromised**:
the BT stream was found not running partway through and switched back on
at T=208430864 (user-confirmed); exact off-start time unknown, both
WiFi spammers were verified operational from ~5 minutes in per the trial's
own setup procedure. Written up anyway, same reasoning as session 15 — the
confound is identified and bounded, so the data isn't meaningless, just
answering a slightly different question than intended.

- **Different character from session 15**: instead of one brief severe
  cluster followed by calm, this trial shows **sustained, continuous
  churn across nearly the entire run** — disconnects from ~5 minutes in
  through to near the end, not a localized spike. Whole-session (~3000
  runs): 43 genuine drops, 22 stale-GOAL rejections, 33 stalls, retry
  rate 1.46% (ARM/START) / 9.47% (GOAL).
- **Splitting at the BT-resume timestamp shows no clear worsening from
  BT** — if anything several metrics improve after it comes on
  (disconnects/hour 18.1→12.5, ARM/START retry 2.72%→0.37%; GOAL retry
  roughly flat at ~9.5% either way). **Not read as "BT doesn't matter"** —
  this isn't a controlled comparison (true BT-off start unknown, RF
  conditions drift over hours regardless, n=1 trial) — read as "two
  spammers alone already produce sustained meaningful stress under
  `NONE`," which the data does support regardless of BT's contribution.
- **The one finding that changes prior conclusions**: a genuine **5002ms**
  `wsClient.loop()` stall (T=202785556) — the exact `WEBSOCKETS_TCP_TIMEOUT`
  signature that defined the `MIN_MODEM` cascade in sessions 11/13/14 —
  occurred here under `NONE`, during the confirmed-no-BT period. **Revises
  session 15's tentative "maybe `MIN_MODEM`-specific" reading**: the severe
  stall signature is not exclusive to `MIN_MODEM`, it's evidently a general
  two-spammer WiFi-stack-overhead property that `NONE` is not immune to —
  just apparently less frequent/severe under `NONE` (one occurrence here
  vs. repeated occurrences per trial under `MIN_MODEM`) rather than absent.
- **Item 1 (ack-path fix)**: overall retry rates (1.46%/9.47%) are
  meaningfully lower than session 13's `MIN_MODEM` full-6h rates
  (4.0%/18.3%) over a comparable (if shorter, ~3.6h) exposure — reasonably
  supportive the fix is working, still not the fully clean, confound-free
  confirmation this item has been waiting on since session 10.

**Session 16 results (2026-08-07)** (`test-data/spam-tests/{cerberus-16,
hesperus-start-16,hesperus-goal-16}.log`) — the clean re-run sessions 15
and 15a couldn't deliver: `WIFI_PS_NONE`, two spammers + BT, 5000 runs,
~5.97h (T=244434923→265930842ms). Both spammers and the BT stream
confirmed continuous throughout, no dropout or equipment issue on any
interference source — the first genuinely confound-free two-spammer+BT
`NONE` trial to date.

- **Race outcome: 4940 armed+started, 4911 committed — 29 lost (0.59%).**
  Higher than session 13's `MIN_MODEM` loss rate (17/15,000 physical
  triggers, ~0.11%) by count, though on a smaller physical-trigger base
  here (~14,868) the two aren't a strictly like-for-like comparison. No
  crash, no reboot, on either board, across the full trial.
- **Retry rates, whole trial: ARM/START 4.15% (411/9906), GOAL 10.32%
  (512/4962).** ARM/START sits almost exactly on session 13's `MIN_MODEM`
  figure (4.0%) — consistent with the standing read that this side of the
  congestion hit is power-save-independent. GOAL is meaningfully lower
  than session 13's 18.3% but higher than session 15a's (compromised,
  shorter) 9.47%.
- **Sustained, not localized.** Time-binned into sixths: ARM/START retry
  ran 9.0% / 2.8% / 2.2% / 2.5% / 3.1% / 5.6%; GOAL ran 16.4% / 7.8% /
  8.5% / 7.1% / 8.2% / 14.1%. Elevated at both ends, real throughout the
  middle — genuine sustained stress across the whole ~6h trial, not one
  isolated episode (session 15's pattern) or a front-loaded confound
  (session 7's). Mean RSSI -65.4dBm (range -82 to -55), in line with prior
  sessions — not a signal-strength confound either.
- **Recovery rate is the strongest evidence for the ack-path fix**: of the
  923 distinct events that needed at least one retry, only 8 exhausted all
  retry attempts and were genuinely dropped (99.1% recovered). This is the
  clean, sustained-load confirmation item 1 has been waiting on since
  session 10 — the widened timeout is doing real work, not just avoiding
  drops in a short or partially-compromised window.
- **The 5002ms `WEBSOCKETS_TCP_TIMEOUT` signature recurred 25 times** (of
  ~~158~~ total `wsClient.loop()` stalls >50ms), ~~spread across the trial
  rather than clustered into the acute 2-minute-scale episodes `MIN_MODEM`
  produced in sessions 11/13~~. 34 WS disconnects total, both boards
  combined. **This is the clean confirmation of session 15a's tentative
  finding**: the severe stall signature is a real, recurring `NONE`
  property under sustained two-spammer load, not a rare one-off. Despite
  that, every stall and disconnect recovered automatically with no
  operator intervention and no corrupted result (16 stale-GOAL rejections,
  all handled correctly by the existing guard — see that issue below).

  **Correction, 2026-08-07 (raw-log recount against the actual
  `[WS Pump] wsClient.loop() blocked` canary lines in
  `hesperus-start-16.log`/`hesperus-goal-16.log`).** The "158 total
  stalls" figure doesn't match the raw logs: actual count is **79** (45
  start-board, 34 goal-board) — the source of the original 158 figure
  wasn't identified, flagged rather than silently corrected without
  explanation. More significantly, the distribution is clustered, not
  spread: **49 of the 79 events (27 of them ≥4000ms) fall inside one
  ~9.7-minute episode, T=244443571–245020561ms, starting ~9 seconds into
  the log capture** — i.e. essentially the very start of the trial, not
  mid-run. Within that episode, start- and goal-board large stalls
  interleave almost metronomically (~2.6-2.9s apart — e.g. goal 5002ms,
  start 5002ms, goal 5002ms, start 5002ms...), and consecutive same-board
  stalls repeat at a near-exact **~5513ms** cadence for 6-7 cycles running
  (5002ms block + ~500ms retry-loop overhead) — a fixed reconnect-retry
  period, not random spacing. Cross-checked independently against
  cerberus's own WS disconnect log (34 total, confirming that count is
  correct): **19 of the 34 disconnects land inside the same opening
  window.** Outside it: ~21 minutes of silence, then ~2.5h of only small
  (<1.5s) isolated blips, then two much smaller flare-ups (2-3 events
  each) at T≈258.47M and T≈263.32M–264.28M, separated by hours of near-
  quiet. **Reading**: this episode is comparable in kind to `MIN_MODEM`'s
  acute 2-minute-scale episodes (sessions 11/13) — just front-loaded at
  the very start of the trial rather than occurring mid-run, and ~5x
  longer. The tight cross-board interleaving during the episode points at
  a shared external condition (both radios contending with the same bad
  channel window simultaneously) rather than independent per-board
  coincidences. Does not change the race-outcome, recovery-rate, or
  smoke-test-pass conclusions elsewhere in this session's writeup (all
  independently re-verified and still correct) — only the "evenly spread,
  unlike `MIN_MODEM`" characterization, which is superseded by this
  finding.
- **Not done this session**: the session 9/10-style per-event round-trip
  trace (hesperus `[WS-ACK-RECV]` vs. cerberus `[WS-ACK] sent=`) that would
  show *why* each of the 8 genuine drops happened, the way sessions 9/10
  established the mechanism in the first place. The aggregate recovery
  rate above is solid evidence the fix works; it isn't a re-verification
  of the underlying mechanism, which wasn't in question here.
- **Reading**: closes out the "genuinely clean confound-free `NONE` trial"
  gap that items 1 and 2 in the outstanding-work list were both blocked
  on. Confirms the ack-path fix under real sustained load, and confirms
  `NONE` passes the two-spammer+BT smoke-test bar (graceful degradation,
  full automatic recovery, no corruption) — while also confirming, more
  robustly than session 15a could, that `NONE` is not immune to the severe
  stall mechanism either. Does not touch the separate `MIN_MODEM`-stays-
  unadopted decision, which was never contingent on `NONE`'s own result.

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

**Real-world recurrence, sessions 12-13 (2026-08-05).** First observed
outside synthetic `--tsf-us` testing: session 12 (single-spammer
`MIN_MODEM`), 1 occurrence; session 13 (two-spammer+BT `MIN_MODEM` smoke
test), 3 occurrences. All four were a genuine congestion-delayed GOAL
(2-5 retry attempts, up to ~2.4s to resolve) arriving after the pulser's
fast next-run ARM/START had already gone through — the guard rejected
each one correctly, but the true result was still lost, not recovered.
In every case the trigger was the trial protocol's unrealistically fast
goal-to-rearm cycling (session 12: confirmed operator artifact,
sub-1-second gap no real run can produce), not a normal-cadence failure.
**Decision, 2026-08-05: stay with reject-only scope, formally deferred.**
The full attempt-id + ring-buffer mechanism (tag each attempt, retroactively
attribute and commit a late GOAL to the superseded attempt it actually
belongs to, rather than just reject it) is real added complexity — a
payload schema change on both boards, a ring buffer of recent attempts on
cerberus instead of a single scalar, multi-attempt-back matching, and an
unresolved UX question (where does a retroactively-completed result even
surface once the operator has moved on two attempts later). Weighed
against that cost: **all four real firings to date trace to conditions
outside the pass bar** — session 12's was a confirmed test-protocol
artifact (sub-1-second goal-to-rearm, which no real run can produce), and
session 13's three were under the deliberately-adversarial two-spammer+BT
smoke test, not single-spammer. Zero occurrences under the actual accepted
operating condition. Same "improbable, not impossible" reasoning that
justified shipping reject-only scope over the full design in the first
place — left documented as an option, revisited only if a real-world
(non-artifact, single-spammer-or-milder) occurrence shows up, not
otherwise.

### Issue: Duplicate triggers from gapped robot structure

**[Reassessed 2026-08-07 — deprioritized, see outstanding-work #4]**
*(was part of Observation #9; Recommendation 8)*

**Observation.** A robot with a gapped/slotted structure can break one
gate's beam more than once during what should count as a single crossing.
`DEBOUNCE_US` (50ms, ISR-level) suppresses electrical/mechanical bounce,
but a structural gap can plausibly be wider than that, generating
genuinely separate, correctly-timestamped triggers for what should be one
logical event. The race state machine already tolerates this once it's out
of `RUNNING` (a later duplicate is dropped by state, not corrupted), so
this isn't a correctness bug today — ~~but needlessly sending/queueing
extra events adds avoidable network-side pressure for no benefit, since
only the first trigger of a crossing is ever wanted for a race time~~ **—
that "avoidable network-side pressure" framing was written under HTTP's
per-event connection cost and doesn't hold under WS; see the reassessment
below.**

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

**Resolution.** ~~Not yet implemented. Proposed: a gate-side post-trigger
lock-out (~300ms) — deactivate a sensor for a short window after it fires,
on top of the existing 50ms electrical debounce — would suppress these at
the source. Cheap to add, does no harm once persistent connections make
per-event overhead small (they now do).~~ **Not being pursued — see
Reassessed, 2026-08-07, below.**

**Two open concerns, 2026-07-31 (mooted by the reassessment below, kept
for the record rather than deleted):**
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

**Reassessed, 2026-08-07.** The original observation's premise — that
extra events are "avoidable network-side pressure for no benefit" — was
written when per-event transport was HTTP (a fresh TCP connection per
trigger, ~267ms baseline latency). This issue's own WS re-run, already
recorded above, shows that premise no longer applies: 200/200 duplicate
events arrived cleanly at ~7-8ms latency, no queueing, no drops. Under
that transport, a redundant trigger costs almost nothing to send and is
already handled correctly (harmlessly ignored once out of `RUNNING`).
Combined with multiple triggers from one robot being rare in practice,
building a source-side lock-out to suppress them is solving a problem
that, under the current transport, barely costs anything to leave alone —
if anything, a duplicate is a free extra delivery attempt, closer to a
reliability feature than a problem. Deprioritized, not formally closed (a
genuinely gapped robot could still exist and this reasoning would need
revisiting if one shows up in practice). The lock-out design (and its two
open concerns above) is not being pursued.

**Verification.** N/A — not being pursued (see Reassessed, 2026-08-07,
above), not merely not-yet-implemented. No planned re-run.

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

**Recurred, 2026-08-04** (session 8's single-spammer 5000-run trial, see
the acceptance-criteria section's "Results, session 8") — same idle-only
signature, ~65 minutes after the last race this time, goal board only. New
this session: `[AUDIT ALERT] Temporal Disruption!` also fired 6 further
times *during* active running (5 goal-board, 1 start-board), each a single
rejected sample with immediate self-correction and no reboot or operational
impact — one of the six is fully explained by an 8.7-minute operator pause
mid-trial (confirmed with the user; see "Results, session 8"), but the
other five occur with no identified trigger nearby. Still self-healing and
still not investigated further, but now established as something that
happens during normal operation too, not just post-trial idle.

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

**[CLOSED, characterized 2026-08-04 — no periodic cause found]**
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

**Characterized, 2026-08-04, using existing data instead of the proposed
purpose-built test below.** Rather than reconfiguring
`ares-pulse-generator` for a synthetic 10,000-message run, used the
existing confound-free 5000-run no-spammer baseline (session 6,
`test-data/spam-tests/cerberus-6.log`) — 5000 real `GOAL` events at a
uniform ~4.3s cadence, arguably a better substitute since it's real
protocol traffic rather than synthetic pulses (`python3
tools/cerberus_log_stats.py test-data/spam-tests/cerberus-6.log --event
GOAL --no-table`): mean 8.8ms, p90 12.0ms, p95 14.6ms, p99 19.0ms, max
63.8ms. 30/5000 (0.6%) exceeded 20ms. Extracted those 30 outliers' event
indices and checked the gaps between them for periodicity (the leading
hypothesis below — a fixed-period task like the heartbeat timer or a WS
keepalive interval): the gaps are irregular (1 to 697 events apart, no
repeating interval), ruling that hypothesis out across a sample 25-50x
larger than the n=100-200 that originally flagged this. Consistent with
ordinary non-periodic scheduling/RF jitter, not a hidden periodic bug. The
"would not on its own isolate which stage the delay happens in" caveat
below is now moot rather than resolved — sessions 8-10's ack-path
investigation has since added exactly the hesperus-side send/receive
timestamp (`[WS-ACK-RECV]`) this would have needed as a follow-up, but
with no periodic pattern found there's no specific lead left to chase with
it. Closed as characterized-and-benign.

**Original proposed test, superseded by the above rather than run as
specified.** A single-gate `GOAL`-only sequence, much longer and steadier
than the existing trials — e.g. 10,000 messages at a fixed 250ms interval
(well clear of any queueing effect) — logged and run through
`cerberus_log_stats.py --gaps`. At that volume, look for: periodicity (a
spike every N messages, pointing at a fixed-period task like the heartbeat
timer or a WS keepalive interval), clustering in time, or drift/rate
correlation. Would need `ares-pulse-generator`'s `MAX_COUNT`/interval
reconfigured (currently 100 at 1s spacing) — a 10,000-message run at 250ms
is ~42 minutes per pass. Would not, on its own, isolate *which* stage the
delay happens in (hesperus pickup vs. network transit vs. cerberus
processing) — the current log only has `tsf_us` (hesperus ISR time) and
`recv_ms` (cerberus receipt time), which bundles all three. If the
pattern-hunt doesn't point at an obvious
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

**Field-deployable overflow-drop notification shipped (folded in from
TODO.md, originally noted there before this document's own reassessment).**
This issue's own investigation repeatedly ran into `networkq_overflow_count`
not being available for a given session (e.g. the very first 2026-08-02
trial above, where only GOAL's serial output was kept). That gap is now
closed for future sessions: overflow/drop/stall/disconnect counters are
persistent across reboot (`hesperus-timing-gate/src/network-health-stats.h`)
and exposed over the diagnostics HTTP server (`GET /status`,
`src/net/debug-http-server.h`) — previously serial-debug-output-only, so a
real deployment with no serial cable attached for hours had no way to check
after the fact whether any of these adversarial-smoke-test-only failure
modes had actually fired in the field. `main.cpp`'s own `uploadWorkerTask`
comment (around line 561) still lists this as unaddressed; that comment is
now stale and should be removed next time that function is touched.

**Proposed experiment, 2026-08-07: isolating traffic-onset vs.
connection-freshness as the trigger for severe stall clustering. Not yet
run.**

**Motivation.** The session 16 raw-log recount (above) found the severe
(~5002ms `WEBSOCKETS_TCP_TIMEOUT`-signature) stalls cluster into distinct
multi-minute episodes rather than spreading evenly through a trial. The
fastest-onset episodes landed very close to trial start — session 16:
~17s in; session 15a: ~5.2min in — but sessions 13 and 15 show the same
nominal two-spammer+BT condition producing its first severe episode
226min and 275min in respectively, not near the start at all. Trial start
normally conflates two things that are usually simultaneous: the WS
connection is at its freshest right when a trial's log begins, *and* the
interference sources are typically switched on around the same time.
Existing sessions can't distinguish which of those two (if either) is
the actual trigger — this experiment decouples them.

**Hypotheses (stated in advance, per this doc's own practice elsewhere of
deciding before running to avoid post-hoc rationalization of a marginal
result):**
- **H1 — traffic-onset.** The trigger is interference stepping up to full
  severity, regardless of connection age. Predicts: Trial A (old
  connection, fresh interference) shows a severe episode soon after the
  spammers switch on; Trial B (old interference, fresh connection) does
  not.
- **H2 — connection-freshness.** The trigger is the WS connection itself
  being newly established, regardless of how long the interference has
  already been present. Predicts: Trial B shows a severe episode soon
  after the cerberus-forced reconnect; Trial A does not.
- **H3 — general transition-sensitivity.** Either kind of abrupt change
  (interference step *or* connection reset) elevates risk for a following
  window — a broader, less specific version of H1/H2. Predicts: both A
  and B show a severe episode near their own change point.
- **H0 — null.** Severe-episode timing is unrelated to either transition
  and is better modelled as a rare, roughly memoryless tail event
  (consistent with the "rare probabilistic coincidence" framing already
  used for the `MIN_MODEM` mechanism in the power-save issue above) — the
  early clustering seen in sessions 15a/16 was coincidental. Predicts:
  neither A nor B reliably shows a severe episode tied to its change
  point.

**Protocol.**
- **Trial A — 45min total, one continuous log.** Minutes 0-15: quiet
  baseline, no spammers, `WIFI_PS_NONE`. At minute 15: switch on both
  spammers and BT streaming together (matching the two-spammer+BT
  smoke-test condition used in sessions 13/15/15a/16). Minutes 15-45 (30
  min post-change): interference running, both boards' WS connections
  untouched — already ~15min old and idle-quiet at the moment of the step
  change.
- **Trial B — 15min unlogged pre-conditioning + 30min logged trial.**
  Switch on both spammers and BT first, leave running 15 minutes with
  both hesperus boards connected normally throughout (nothing captured as
  "the run" yet — interference reaching steady state before anything is
  measured). At minute 15: restart cerberus — forces both hesperus
  boards' WS connections to drop and re-establish fresh simultaneously
  (the bench-verified reconnect-after-cerberus-reboot path, `main.cpp`
  `loop()`'s watchdog/edge-trigger logic around lines 918-993 — see this
  turn's earlier discussion on why a cerberus restart was chosen over
  power-cycling the CDC-serial-connected gate boards). From that point,
  run the logged 30-minute trial with interference already steady-state
  for 15 minutes.

**Operational criterion for "clustering near the change."** One or more
`wsClient.loop()` blocks ≥4000ms (the canary threshold already logged
permanently by `wsPumpTask`) within 10 minutes of the relevant change
point (spammer-on for Trial A, cerberus-restart-triggered reconnect for
Trial B) — chosen to bound the fastest-onset episodes seen so far
(session 16: 17s; session 15a: 5.2min) with margin.

**Explicit caveat.** n=1 per condition, against a phenomenon that has
occurred only 1-3 distinct episodes per multi-hour trial historically —
this is a cheap first discriminating pass (~75min total bench time), not
a conclusive test. A result matching H1 or H2 is suggestive and would
need a confirming repeat before being treated as settled; a null result
on both trials doesn't rule the mechanism out either — it's equally
consistent with simply not having caught a rare event in the available
exposure window.

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

**TCP-level ack-pending instrumentation added, 2026-08-04**
(`cerberus-gate-controller/src/net/http-server.h`, `ws_event_handler()`'s
`WS_EVT_DATA` branch) — aimed at candidate (a) above. Deliberately does
**not** use `AsyncClient::onAck()`: checked the actual vendored library
source at the pinned versions (`esp32async/AsyncTCP` v3.5.0,
`esp32async/ESPAsyncWebServer` v3.11.2) first, since `AsyncClient::onAck()`
only holds a single callback slot, and `AsyncWebSocketClient`'s own
constructor already binds *its* `_onAck` handler onto the same underlying
`AsyncClient` to drive its outgoing message-queue (`_runQueue()`) —
registering our own handler there would silently replace theirs and break
queued-message delivery after the first frame per client, a much worse bug
than the one being investigated, and one `pio run` wouldn't catch. Instead,
right before each ack dispatch, two plain read-only getters on the
underlying `AsyncClient` (`client->client()`) are logged: `canSend()`
(`false` means a *previous* write on this connection hasn't been TCP-acked
by hesperus yet — i.e. genuinely still in flight below the WS layer, not
just unprocessed by hesperus's app code) and `space()` (remaining TCP send
buffer headroom). New `[WS-ACK]` fields: `pending=<0/1> space=<bytes>`.
Captured unconditionally on every ack (not just retried ones) to build a
normal-operation baseline to compare congested-window readings against.
Since hesperus retries roughly every 250-350ms during a stall, each retry's
arrival is itself an extra free sample of this state during exactly the
window that matters. Build-verified (`pio run -e
cerberus-cyd2usb-diymalls-ili9341`); not yet hardware-verified. **Next
step, per the session-8 conversation**: rather than another multi-hour
trial (session 8's realistic-RSSI retry rate was 0.03%, too rare to
efficiently distinguish causes), run a short (15-30 minute) deliberately
marginal trial — two-spammer congestion or the earlier weak-RSSI AP
placement — to reliably reproduce enough ack-path retries to read: if
`pending=1`/`space` near-zero shows up repeatedly during retry bursts, that
confirms candidate (a) (backlog inside AsyncTCP/lwIP, not yet on the air);
if `pending=0`/`space` healthy throughout even while hesperus keeps
retrying, that rules (a) out and points at (b) or a hesperus-side handling
delay instead.

**Hardware-verified, session 9 (2026-08-04)** (`test-data/spam-tests/
{cerberus-9,hesperus-start-9,hesperus-goal-9}.log`) — the short marginal
trial above, run as planned: ~28.6 minutes, 399 runs, spammer(s) and BT
streaming both active together and the AP moved physically closer than
session 8. This deliberately harsher combination worked as a retry
generator: 62 of 1197 distinct events needed at least one retry (5.2%,
~170x session 8's confound-free 0.03%), including bursts up to 7 receipts
for a single event — and **candidate (a) is ruled out, cleanly**:

- **`pending=0` on every single one of the 1272 `[WS-ACK]` lines this
  trial** — including all 62 retried events' acks, checked individually,
  not just in aggregate. Never once did cerberus have an unacked write
  outstanding on a client's connection at the moment it needed to dispatch
  another ack.
- **`space` (TCP send-buffer headroom) stayed within 2% of its max (5760)
  throughout** — min 5648 across the whole trial, including during the
  worst retry bursts. No sign of a growing backlog inside `AsyncTCP`/lwIP
  under this load.
- **`client->text()` itself stayed fast** — max 8ms even across the
  retried-event acks, consistent with session 3's original finding.
- **Race outcome: 399/399/399 ARM/START/GOAL, zero drops** — every one of
  the 62 retry situations eventually succeeded; the retry/dedup mechanism
  did its job even under this much heavier load than the pass-bar
  single-spammer scenario. Zero panics, zero WS disconnects, zero `[AUDIT
  ALERT]` recurrences.
- **Reading**: with cerberus's TCP-level write path this thoroughly cleared
  — no backlog, no pending acks, fast dispatch, even under conditions that
  produced 170x session 8's retry rate — candidate (a) (`AsyncTCP` write
  completion lagging behind `client->text()`) is no longer a plausible
  explanation. The remaining live candidate is (b): something on the
  return leg or hesperus's own receive-side handling is where the event
  needs to be traced next — this instrumentation has done what it can from
  cerberus's side alone. (Note: hesperus's WS client is the Links2004
  `WebSocketsClient` library, not `AsyncTCP` — a different stack from
  cerberus's server side, so this session's `AsyncTCP`-specific finding
  says nothing about hesperus's own library internals one way or another.)
- Incidentally reinforces the standing beacon-volume-not-RSSI theory: mean
  `rssi` this session (-65.9dBm) was statistically the same as session 8's
  clean-baseline reading (-65.8dBm) despite the AP being moved closer —
  yet the retry rate exploded, because what changed was beacon/interference
  *volume* (spammers + BT), not link quality.

**Hesperus-side receive timestamp added, 2026-08-04**
(`hesperus-timing-gate/src/main.cpp`) — the natural next step once
cerberus's side was cleared: `wsClientEventHandler()` (the
`WebSocketsClient` callback that receives cerberus's ack) now captures
`debug_timestamp_ms()` (same shared Wi-Fi-TSF clock cerberus's own
timestamps use, so directly comparable with no offset/NTP needed) at the
instant an ack arrives, alongside the existing `g_ws_ack_tsf_us`. New field
`g_ws_ack_recv_t_ms`, threaded through `wsTakeAckIfReceived()`'s existing
out-param pattern (now `(uint64_t &out_tsf, uint64_t &out_recv_t_ms)`, one
call site). Deliberately does **not** log from inside
`wsClientEventHandler()` itself: that handler runs synchronously nested
inside `wsPumpTask`'s `wsClient.loop()` call — the single most
latency-sensitive path in the app, the exact one the `wsClient.loop()`-
blocking fix exists to protect — and a `debug_printf()` there would add a
`serial_write_mutex` take plus a blocking `Serial` write directly into that
path. Capturing the timestamp is cheap (a plain `esp_wifi_get_tsf_time()`
read, no I/O) so it's safe to do inline; logging it is deferred to
`uploadWorkerTask`'s existing ack-wait loop (new `[WS-ACK-RECV]
tsf_us=... recv_t=... attempt=...` line, right where a matching ack is
recognised) — that task already does its own unhurried Serial I/O for
`[WS Worker] Sent/Resent` today, so this adds no new risk there. Once run
against a marginal trial like session 9's, `[WS-ACK-RECV] recv_t=...` on
hesperus can be lined up directly against cerberus's `[WS-ACK] sent=...`
for the same `tsf_us`, giving a true one-way return-leg latency instead of
an inference. Build-verified (`pio run -e hesperus-gate-s3-zero`); not yet
hardware-verified.

**Mitigation timing, decided 2026-08-04**: discussed jumping straight to
mitigation 1 below (redundant ack bursts) now that candidate (a) is ruled
out, given the suspicion has shifted toward hesperus's own stack — decided
against it for now, in favour of running the instrumentation above first.
Two reasons: the extra 2-3 small (~30-50 byte) frames are negligible next
to beacon-storm traffic volume, so "would it make things worse" isn't
really the concern; the real issue is that trying the mitigation now would
show *whether* it helped without showing *why*, and there's already a
suggestive data point against "redundant acks alone will fix this" —
session 3's specific case had cerberus's own dedup-triggered re-acks
already deliver the same ack **four separate times** (once per hesperus
resend, ~250-350ms apart) and hesperus missed all four, which looks more
like a hesperus-side handling gap than simple independent packet loss that
redundancy would fix. Get the one-way latency data first, then pick the
mitigation with evidence in hand.

**Root cause identified, session 10 (2026-08-04)** (`test-data/spam-tests/
{cerberus-10,hesperus-start-10,hesperus-goal-10}.log`) — the marginal
trial above, this time with two spammers plus BT streaming together
(harsher than session 9's setup), run against both new instrumentation
sets at once. 400 runs, 28.6 minutes, mean rssi -66.3dBm. Race outcome:
399/399/399, zero drops, zero panics, zero disconnects, zero `[AUDIT
ALERT]`s — the system handled the adversarial two-spammer case cleanly
despite the ack-path activity below. Only 10 of 1197 distinct events
needed a retry (0.84%) — every one of which was cross-referenced end to
end using both boards' timestamps (all on the same shared TSF clock, so
directly comparable with no offset correction):

- **Every single one of the 10 "retried" events' original ack was actually
  received and recognised by hesperus — none were lost.** For each, the
  full round trip was reconstructed: hesperus's own send time (`[WS
  Worker] Sent ...]`) → cerberus's `DATA` receipt → cerberus's ack
  `sent=` → hesperus's `[WS-ACK-RECV] recv_t=`. Forward leg (hesperus send
  → cerberus receipt) ranged 93-386ms; cerberus's own processing stayed
  fast as always (3-6ms); return leg (cerberus ack sent → hesperus
  recognises) ranged 30-322ms. **Total round trip for all 10: 258-458ms**
  — every one landing at or beyond hesperus's own jittered per-attempt
  timeout window (`WS_ACK_TIMEOUT_MS` 300ms ± 60ms, i.e. 240-360ms).
- **Compare against the 1187 clean (non-retried) events' return-leg
  latency alone: mean 44ms, p50 35ms, p90 80ms, p99 143ms, max 257ms —
  only 1 of 1187 exceeded 240ms.** The 10 retried events' return-leg
  latencies (30-322ms) mostly sit inside or just past that same
  distribution's tail, not in some separate, qualitatively different
  regime. Nothing points at loss, a stuck queue, or a hesperus-side stall:
  cerberus's dispatch is fast, hesperus's own `[WS-ACK-RECV]` fires
  promptly once the packet is actually there.
- **Conclusion: this isn't packet loss (candidate b) or a stack stall on
  either side — it's ordinary bidirectional network jitter, occasionally
  summing past a fixed ~300ms timeout under heavy interference.** Both
  legs contribute roughly symmetrically (forward-leg range 93-386ms is if
  anything wider than the return-leg's 30-322ms), so "asymmetric return-leg
  loss" specifically is not what's happening. The retry/dedup mechanism
  already handles this exactly as designed — every one of these 10 events
  still completed with zero data loss — so in practice this is a harmless
  "spurious resend under heavy interference" phenomenon, not a bug with a
  hidden failure mode. This also retroactively reframes session 3's
  original finding (cerberus re-acked the same event 4 times, "well inside
  hesperus's own wait window") — that analysis compared cerberus-side
  times against hesperus's own window without a directly-measured return
  transit time; today's instrumentation shows that transit time is exactly
  the piece that was missing, and is enough on its own to explain it.
- **Practical implication for the mitigations below**: redundant ack
  bursts (mitigation 1) were designed around a loss model and would only
  help here by chance (marginally raising the odds one of 2-3 copies beats
  the timeout, not by fixing anything structural). A better-targeted fix,
  if this is worth addressing at all, is **loosening `WS_ACK_TIMEOUT_MS`**
  to comfortably clear the round-trip tail actually observed under
  adversarial interference (session 10's worst case: 458ms) — a pure
  tuning change with no correctness downside, since a resend is always
  safe (dedup already handles it) and simply costs a little extra airtime.
  Whether it's worth doing at all is a judgement call: this only showed up
  under the explicitly-adversarial two-spammer+BT smoke-test scenario, not
  the single-spammer pass bar (session 8's confound-free single-spammer
  retry rate was 0.03%, essentially never), so the honest framing is
  "understood and harmless," not "must fix."
- **Before tuning it, checked whether the delay was hesperus's own
  software rather than the network**: `wsPumpTask`'s permanent
  `wsClient.loop()` blocking canary (logs any single call over 50ms) fired
  only once in the entire session-10 trial, and that one instance doesn't
  line up in time with any of the 10 retried events (nearest is 4.5s+
  away). So hesperus wasn't sitting on an already-arrived packet — the
  pump was polling normally throughout. Combined with cerberus's own
  dispatch staying under 15ms (confirmed since session 3), the delay is by
  elimination most likely happening on-air / at the WiFi MAC layer itself
  under two-spammer channel saturation (matching the "every radio on the
  channel pays a beacon-processing tax" mechanism already described in
  the acceptance-criteria section) — not directly instrumented, inferred
  by ruling out both boards' software.

**`WS_ACK_TIMEOUT_MS` tuned, 2026-08-04** (`hesperus-timing-gate/src/main.cpp`)
— raised 300→500ms (jitter scaled proportionally, 60→100ms, keeping the
same ~20% ratio), comfortably clearing session 10's 458ms worst case with
margin. `WS_ACK_OVERALL_DEADLINE_MS` raised 3200→5200ms alongside it,
proportional to the same 1.5-1.6x change, so `WS_MAX_SEND_ATTEMPTS`'s 10
attempts still fit inside the overall deadline (10×500=5000, +200ms
margin, matching the original's 3200 vs. 10×300=3000) rather than quietly
losing ~3 attempts of real-loss recovery depth as a side effect of loosening
the per-attempt timeout. Real-race events are sparse (one every 20+
seconds) and `networkQueue` is depth 10, so the larger worst-case
per-event stall remains comfortably inside budget there. Build-verified
(`pio run -e hesperus-gate-s3-zero`); not yet hardware-verified — next
step is re-running a session-9/10-style marginal trial and confirming the
retry count drops.

**Confirmed, session 16 (2026-08-07).** The clean, confound-free
two-spammer+BT `NONE` trial this item had been waiting on since session
10 finally landed — full detail in the "Wi-Fi power-save vs. battery
budget" issue above (same trial serves both items). Headline: of 923
events needing at least one retry, only 8 exhausted all attempts (99.1%
recovered) over a genuine ~5.97h sustained-stress trial. This is the
hardware verification the paragraph above was waiting on.

**Candidate further mitigations, 2026-08-03 — not yet tried.** Discussed
while waiting on the large-N single-spammer trial; deliberately held until
there's a disrupted-trial baseline to compare against, rather than tried
speculatively. None of these are a radical change to the existing scheme
(small packets, low connection overhead, persistent WS, up to 10 jittered
retries, auto-reconnect) — they're incremental additions on top of it. In
rough priority order:

1. **Redundant ack bursts on cerberus** (top pick). `ws_event_handler()`
   currently sends the ack exactly once per received DATA frame (even a
   duplicate delivery only triggers one re-ack). Since the ack-dispatch
   path is proven fast (<15ms, see above), sending the same ack payload 2-3
   times back-to-back costs almost nothing and directly targets the
   still-open "asymmetric loss on the return leg" candidate, without
   touching the retry/backoff design at all.
2. **Task/core placement — checked, already correct, nothing to gain.**
   Confirmed `wsPumpTask`, `uploadWorkerTask`, and `ledDiagnosticTask`
   (`hesperus-timing-gate/src/main.cpp`'s `xTaskCreatePinnedToCore` calls)
   are all pinned to core 1, same as Arduino's own `loop()`; WiFi/LWIP
   internals run on core 0. They're not fighting the WiFi stack for CPU
   time during a beacon storm. Recorded here so this isn't re-checked
   fruitlessly in a future session.
3. **LWIP/TCP buffer tuning via sdkconfig/build flags on cerberus** —
   addresses the other open item-1 candidate (`AsyncTCP`'s actual on-air
   flush lagging behind `client->text()` returning). Doesn't touch vendor
   source, just its configuration (send buffer/window sizes). Harder to
   isolate cleanly than #1 and payoff is genuinely uncertain — try second,
   only if #1 doesn't move the needle.
4. **Physical/RF, not code at all** — antenna placement/orientation and
   AP proximity/channel selection. Given the root cause is RF/hardware-level
   (ESP32 stack overhead from beacon volume), this is arguably the single
   biggest lever available, and free — but it's a per-venue operational
   practice, not something that ships in firmware.
5. **Hedged sends** — already on record as deprioritized (see that issue
   below); the 5000-run no-spammer baseline and single-spammer results so
   far show retries are rare-to-nonexistent under the actual pass bar, so
   there's no signal yet to justify it. Only worth revisiting if a large-N
   single-spammer trial shows a meaningful retry rate.
6. **Try different hesperus silicon, 2026-08-06 — noted, not attempted.**
   The `wsClient.loop()` stall only ever appears on hesperus (ESP32-S3);
   cerberus (plain `esp32dev`, confirmed in `boards.ini`) never shows the
   equivalent symptom, though that's confounded with a second real
   difference — hesperus's synchronous `WebSocketsClient` vs. cerberus's
   event-driven `AsyncWebServer`/`AsyncTCP`, so chip family alone isn't
   isolated by that comparison. **A same-library test on different
   hesperus silicon would isolate it — but not the existing `c3-super-mini`
   /`c3-xiao` PlatformIO environments**: the ESP32-C3 is single-core, and
   hesperus's task/core split (`wsPumpTask`/`uploadWorkerTask`/
   `ledDiagnosticTask` pinned to core 1, WiFi/LWIP on core 0) is already
   confirmed load-bearing (item 2 above) — moving to C3 would collapse
   that separation at the same time as changing silicon, confounding the
   result either direction. A clean test needs a **dual-core, non-S3**
   hesperus target (e.g. plain ESP32, matching cerberus's own chip) —
   not one of hesperus's existing environments, would need a new board
   target added, not just building an existing one.

### Issue: GOAL board retries far more than ARM/START board under heavy stress

**[OPEN — new, 2026-08-05]**
*(found in session 13, the `MIN_MODEM` + two-spammer + BT smoke test)*

**Observation.** Under the harshest tested combination, the GOAL board's
retry rate (18.3%, 912/4993) ran 4.6x the ARM/START board's (4.0%,
403/9984) — the largest such asymmetry seen in any session, and the first
time this comparison has stood out enough to flag. Not seen (or not
distinguishable from noise) in any single-spammer trial to date.

**Two live hypotheses, not yet distinguished:**
- **Physical placement.** The two boards sit in different positions
  relative to the spammers/BT source/AP; if RF exposure differs enough
  between the two spots, whichever board is physically closer/worse-angled
  would retry more, independent of which unit is there.
- **Role/traffic-pattern.** The GOAL board only ever sends one event type;
  the ARM/START board interleaves two, on a different internal timing
  pattern. If something about that difference (not placement) drives it,
  the asymmetry would follow the *role*, not the physical spot.

**Deferred, 2026-08-05.** The proposed diagnostic (swap which physical
hesperus unit sits in the start vs. goal position, a wiring-only change)
is sound in principle, but the current bench rig is hand-wired breadboards
with too many uncontrolled physical variables (cable runs, ad hoc
positioning relative to the spammers/BT source/AP, no fixed antenna
orientation) to draw a clean conclusion from a swap right now — any result
could as easily reflect breadboard-specific artifacts as either of the two
hypotheses above. Deferred to the production-board testing stage, where
the physical arrangement will be fixed and repeatable enough for a swap
result to actually mean something.

**Resolution.** Not yet — diagnostic deferred until production hardware is
available.

**Data point, session 16 (2026-08-07).** Same asymmetry, smaller ratio:
GOAL 10.32% (512/4962) vs. ARM/START 4.15% (411/9906), a 2.5x gap under a
clean full-length `NONE` two-spammer+BT trial, vs. session 13's 4.6x under
`MIN_MODEM`. Consistent with the asymmetry being real (shows up again,
same direction) but its magnitude varying with something else — power-save
mode, RF conditions on the day, or just trial-to-trial noise at this
sample size — none of which distinguishes the two hypotheses above.
Doesn't change the deferred status.

### Issue: ISR calling `esp_wifi_get_tsf_time()` causes an Interrupt WDT panic under heavy WiFi load

**[RESOLVED, hardware-verified 2026-08-04]**
*(new, found 2026-08-03 during the single-spammer 5000-run trial, session 6's
counterpart)*

**Observation.** ~3190 runs into the trial, the start-board hesperus unit
entered a genuine crash-reboot loop — 12 consecutive panics captured in the
log before the capture was stopped, all `rst:0xc (RTC_SW_CPU_RST)` (a
software-triggered reset, i.e. the panic handler's own reboot, not a power
issue). The board self-recovered a couple of minutes after the captured
window ended (confirmed by the user; cerberus's own log independently shows
no further disconnect/reconnect churn afterward, consistent with a clean
recovery). The goal board was unaffected throughout (0 reboots).

**Confirmation.** Every captured panic reads `Guru Meditation Error: Core
N panic'ed (Interrupt wdt timeout on CPUn)`, one instance explicitly `Core 1
was running in ISR context` at the moment of panic, and one occurrence shows
`Re-entered core dump! Exception happened during core dump!` — both cores
panicking in close succession, consistent with interrupts being stuck
system-wide rather than one task on one core alone. Symbolizing the crash
backtrace against the exact firmware build in `.pio/build/` (via
`xtensa-esp32s3-elf-addr2line`) gives an unambiguous call chain:

```
prvIdleTask -> esp_vApplicationIdleHook -> cpu_ll_waiti (CPU idle, waiting for interrupt)
  -> _xt_lowint1 -> gpio_intr_service -> gpio_isr_loop -> __onPinInterrupt   (GPIO ISR fires)
    -> handleSensor1()  [hesperus-timing-gate/src/main.cpp:359]
      -> esp_wifi_get_tsf_time() -> wifi_get_tsf_time_process -> esp_wifi_get_mode
        -> wifi_init_completed -> wifi_api_lock -> mutex_lock_wrapper
          -> xQueueTakeMutexRecursive -> xQueueSemaphoreTake -> vTaskPlaceOnEventList
            -> vListInsert   [panic PC]
```

`handleSensor1()`/`handleSensor2()` — the `IRAM_ATTR` GPIO trigger ISRs,
attached via `attachInterrupt(..., CHANGE)` — call `esp_wifi_get_tsf_time()`
directly to capture the TSF timestamp at the exact trigger instant. That
API takes an internal WiFi-driver mutex (`wifi_api_lock`). Blocking on a
mutex from ISR context is illegal (there's no task context for FreeRTOS to
suspend/resume around); normally the lock is uncontended and the call
returns instantly, so this has silently "worked" since the ISR/debounce code
was first written. Under sustained heavy WiFi-stack load — exactly what a
many-hour single-spammer beacon flood produces — the lock can occasionally
be held by the driver's own internal processing for long enough that a
GPIO ISR blocked on it starves interrupt servicing long enough to trip the
Interrupt Watchdog Timer. This is a **pre-existing bug**, unrelated to
anything else changed this session (the `wsPumpTask`/retry-schedule work)
— it just needed enough sustained trigger volume under enough WiFi
contention to hit the unlucky timing window, which is why it took ~3190
runs to surface despite the code being unchanged since well before this
investigation began.

One encouraging structural note that shaped the fix: the ISRs already
capture `processor_clock` via `esp_timer_get_time()` alongside the unsafe
TSF call — and that API *is* ISR-safe. The codebase also already has a
working mechanism (the `clock_alpha`-driven `DISCIPLINED SYN` extrapolation
used for holdover) for deriving a trustworthy TSF-equivalent timestamp from
the processor clock when a live TSF read isn't available.

**Resolution.** Implemented 2026-08-03
(`hesperus-timing-gate/src/main.cpp`), following the same decouple-the-
unsafe-call pattern already used for the `wsClient.loop()` fix above,
rather than falling back to always-extrapolated timestamps (which would
have traded away precision on every single trigger, not just rare
contention cases):

- New `PendingCapture` struct carries only what's ISR-safe (`EventType` +
  `processor_clock`) from the ISRs to a new dedicated task.
- `handleSensor1()`/`handleSensor2()` no longer call
  `esp_wifi_get_tsf_time()` at all — they push a `PendingCapture` onto a new
  `triggerCaptureQueue` (`xQueueSendFromISR`, same pattern as the existing
  `networkQueue` send, with its own `triggerCaptureq_overflow_count`) and
  `portYIELD_FROM_ISR()` as before.
- New `tsfCaptureTask` is the sole consumer of `triggerCaptureQueue`: it
  performs the actual `esp_wifi_get_tsf_time()` read — safe here, since
  blocking briefly on the WiFi driver's lock in task context is legal, not
  fatal — pairs it with the ISR-captured `processor_clock`, and forwards the
  completed `GateEvent` to `networkQueue` exactly as the ISRs used to do
  directly.
- `tsfCaptureTask` is created at **priority 3, the highest in the app**
  (above `uploadWorkerTask`/`wsPumpTask` at 2), specifically so it's
  scheduled immediately off the ISR's `portYIELD_FROM_ISR()` — this keeps
  the added latency between the true trigger instant and the TSF read down
  to a task-switch (typically low microseconds), preserving precision
  rather than trading it away for safety.
- `heartbeatTimerCallback()` was left unchanged — it already calls
  `esp_wifi_get_tsf_time()` from the FreeRTOS Timer Service task, not ISR
  context, so it was never affected by this bug.

All 4 hesperus envs (`pio run`) build clean.

**Verification.** Hardware-verified 2026-08-04: a full ~6.53-hour,
~5346-run single-spammer trial (`test-data/spam-tests/{cerberus-8,
hesperus-start-8,hesperus-goal-8}.log`) under the same sustained load that
originally surfaced the bug produced zero Interrupt WDT panics on either
board. See the acceptance-criteria section's "Results, session 8" for the
full trial writeup.

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
