# Kronos Workspace TODO

Index of planned work across the workspace. Each sub-project keeps its own
task/planning docs; this file doesn't duplicate their content, it just
points at them and tracks the cross-project (interoperability, testing)
items that don't belong to any single sub-project.

Current focus: with the core network-timing/reliability work settled (see
`NETWORK-TIMING-LOG.md`), the shift is toward planned per-project features
and enhancements.

**What this document is, 2026-08-07.** An index of *planned* work —
features and enhancements not yet built, kept short enough to scan at a
glance. Bug fixes and external feature requests are tracked as GitHub
Issues instead, not duplicated here. Investigation history and analysis
(what was tried, what the data showed, why a decision was made) lives in
`NETWORK-TIMING-LOG.md`'s lab notebook, not here — this file links to
specific findings there rather than restating them.

---

## Open bugs / investigation gaps

Not planned-work in the sense this file otherwise tracks — these are
tracked properly in `NETWORK-TIMING-LOG.md`'s own "Outstanding work,
prioritized" section (kept up to date there, not mirrored here) and will
move to GitHub Issues going forward. 

As of 2026-08-07: 
 - GOAL board retry asymmetry under heavy stress (deferred to production-board 
 testing), 
 - the two untested congested-airtime stressor layers (bulk-throughput contention,
broadband noise — deprioritized, not formally closed), 
 - hedged burst sends for tail latency (deprioritized, watching retry telemetry).

Not a task, just a standing note: the ARES pulse generator can produce a
spurious trigger on both gates if reset while wired directly to a pair of
hesperus boards — reset it fully before starting a trial or a new log. See
[ares-pulse-generator](#ares-pulse-generator) below. Bench-test tooling
only, no bearing on production use.

---

## Cross-project planned work

- **ARGUS** (data collector/analysis layer) — not started at all. See `docs/KRONOS-SYNCHRONIZED-TIMING-GATES-ARCHITECTURE.md`.
- **Provisioning** — sensor-side mDNS discovery and Wi-Fi credential override are already implemented (`resolveCerberus()`, serial `wifi` command + NVS on hesperus); only deep-sleep power cycling and the sensor registration/auth-token handshake remain proposed. See `docs/PROVISIONING.md`, which now also has a "Recovery from Failure" section (per-scenario: what's automatic vs. needs manual action, including the known "cerberus restarts on a new IP" gap — untested, no repro yet).
- **Reliability: congestion avoidance** (exponential backoff+jitter) — proposed design only, not implemented. See `docs/RELIABILITY.md` (section 2). (`TCP_NODELAY`, formerly listed alongside this, turned out to already be enabled by default via the WebSockets/ESPAsyncWebServer library versions in use — no action needed, see that doc's section 3.)

---

## ALL boards

 - Add non volatile run logs. Cerberus can use an SD card. Hesperus can easily store perhaps 8000 runs at full logging

## cerberus-gate-controller

Full list: **`cerberus-gate-controller/docs/PLANNED-UPDATES.md`** —
supervisor state machine, SD-card CSV logging, HTTP log streaming/
MAINTENANCE mode, `race_runs[]` concurrency guard.

## hesperus-timing-gate

Full list: **`hesperus-timing-gate/review.md`**'s "Future Development Path"
table — Wi-Fi modem sleep, NVS config store, configurable debounce, OTA
update, SSD1306 display, stack telemetry, NVS event buffering, multi-AP
BSSID fallback, local standalone scoring.

## ares-pulse-generator

- **Note, not an open task** (removed from the priorities list 2026-08-07):
  resetting the ARES board while wired directly to a pair of hesperus
  boards can produce a spurious trigger glitch on both gates. Just make
  sure the board has fully reset before starting a trial or a new log —
  ARES is bench-test tooling only, this has no bearing on production use.
  (originally noted 2026-08-02)
