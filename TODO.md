# Kronos Workspace TODO

Index of outstanding work across the workspace. Each sub-project keeps its
own task/planning docs; this file doesn't duplicate their content, it just
points at them and tracks the cross-project (interoperability, testing)
items that don't belong to any single sub-project.

Current focus: interoperability and testing between the already-working
per-project pieces, not new per-project features.

---

## Cross-project: interoperability & testing

Primary tracker: **`NETWORK-TIMING-ISSUE.md`** (root) — its "Outstanding
work, prioritized" section is the ranked list, kept up to date there rather
than mirrored here. As of 2026-08-02, in priority order:

1. Wi-Fi power-save vs. battery budget — unblocked, highest-value open item
2. Duplicate triggers from gapped robot structure — lock-out window design unresolved
3. Congested-airtime stress testing — sketched in `hesperus-timing-gate/review.md` / `docs/TEST-TOOLING.md`, never built
4. Hedged burst sends for tail latency — deprioritized, watch retry telemetry first
5. Unexplained minor WS jitter / reconnect blip — low-priority curiosity

Other cross-project items not tracked in that file:

- **ARGUS** (data collector/analysis layer) — not started at all. See `docs/KRONOS-SYNCHRONIZED-TIMING-GATES-ARCHITECTURE.md`.
- **Provisioning** — sensor-side mDNS discovery/power cycling, client-side setup, registration & auth handshake all still "Proposed, not yet built". See `docs/PROVISIONING.md`.
- **Reliability** — congestion avoidance (exponential backoff+jitter) and `TCP_NODELAY` are proposed designs only, not implemented in either codebase. See `docs/RELIABILITY.md` (sections 2-3).

### Testing gaps

- 10,000-message steady-state WS jitter characterization test — proposed, never run (`NETWORK-TIMING-ISSUE.md`)
- Power-save current-draw/latency measurement across PS modes — method proposed, not yet run (`NETWORK-TIMING-ISSUE.md`)
- Congested-airtime stress harness (airtime saturation, bulk throughput, channel interference, broadband noise) — no tooling built, manual setup only (`docs/TEST-TOOLING.md`)
- Duplicate-trigger lock-out re-test (`trial_double_trigger`) — blocked until the lock-out feature above is built

---

## cerberus-gate-controller

Full list: **`cerberus-gate-controller/docs/PLANNED-UPDATES.md`** (supervisor
state machine, SD-card CSV logging, HTTP log streaming/MAINTENANCE mode,
TSF-based drift compensation, `race_runs[]` concurrency guard, touch
calibration NVS escape hatch).

Smaller items tracked only in-code, not in that doc:

- `BUTTON_COMMAND_MAP` flagged as needing review against the state machine — `src/race/race-command-source.h:37`
- Legacy serial sends `send_run_time()` twice for unclear reasons — `src/messages-reference.h:111-112`

## hesperus-timing-gate

Full list: **`hesperus-timing-gate/review.md`**'s "Future Development Path"
table (OTA update, SSD1306 display, NVS config store, configurable
debounce, stack telemetry, NVS event buffering, multi-AP BSSID fallback,
local standalone scoring).

Note: two rows in that table are stale — "mDNS server discovery" and "HTTP
keep-alive/WebSocket" are both already implemented; don't treat that table
as current status for those two.

Smaller item tracked only in-code:

- Field-deployable overflow-drop notification (LED pattern / SPIFFS log / HTTP flag) — currently serial-debug-only. `src/main.cpp:293-296`

## ares-pulse-generator

Nothing open.
