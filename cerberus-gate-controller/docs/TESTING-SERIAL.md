# Serial (RATS V2) Manual Test Plan

Manual test script for the host-PC serial link (`src/net/serial-protocol.h`,
`src/net/messages.h`, `src/race/race-command-source.h`), covering the
legacy `<98,0>` NewMouse message plus the RATS V2 inbound messages added
from `docs/preferredMessageSequencesV2.pdf`.

## Setup

- **Noise suppression (optional):** `g_watchdog_tx_enabled`
  (`race/race-serial-telemetry.h`) and `g_wifi_rssi_report_enabled`
  (`net/wifi-manager.h`) default to `true` (production-safe). Flip either
  to `false` temporarily if the once-a-second `MSG_WATCHDOG` line or the
  5s Wi-Fi RSSI line makes a manual terminal session hard to read -- just
  remember to set both back to `true` afterward. RATS expects a watchdog
  at least every 2s and flags a fault if it stops.
- Connect the board over USB, open a serial terminal at **9600 baud**,
  line ending **CR+LF** (or plain LF/CR -- all three are accepted, see
  Section 5).
- `g_serial_protocol_rx_echo` is on by default, so every line the RX task
  receives is echoed back as `#[serial-protocol] rx: "<line>"` -- use this
  to confirm the exact bytes the device saw, independent of local terminal
  echo settings.
- All ordinary telemetry lines (`#`-prefixed lines are comments/debug;
  bare `<type,value>` lines are the protocol) will keep streaming in the
  background (`MSG_WATCHDOG` every second, `MSG_CURRENT_STATE` on state
  change, etc.) -- ignore these unless a step below calls them out.
- Run the steps in order top to bottom; later steps assume earlier ones
  passed.

---

## 1. Legacy compatibility (must not regress)

- [x] **1.1** Power on. Confirm `<0,N>` (`MSG_WATCHDOG`) appears roughly
      once a second with an incrementing value.
- [x] **1.2** Send `<98,0>` (legacy zero-value NewMouse). Confirm:
      - RX echo shows `rx: "<98,0>"`.
      - Device transitions to a fresh mouse / `WAITING` state regardless
        of whatever state it was in before (screen + NeoKey LEDs reflect
        this).
      - `<4,1>` (`MSG_CURRENT_STATE` = WAITING) is sent shortly after.
- [x] **1.3** Drive a full ARM -> START -> GOAL sequence with the physical
      buttons and confirm serial telemetry (`<4,2>`, `<4,4>`, `<12,0>`,
      `<13,...>` x2) still looks normal -- confirms the parser change
      didn't disturb the TX side.

## 2. NewMouse with a name (RATS V2)

- [x] **2.1** From any state, send `<98,MightyMouse>`. Confirm:
      - RX echo shows the full name, unmodified: `rx: "<98,MightyMouse>"`.
      - Same RESTART/new-mouse transition as 1.2 happens (name doesn't
        block the transition).
- [x] **2.2** Send `<98,Name With Spaces>` (a name containing spaces).
      Confirm the echo captures the whole thing up to `>` and the
      transition still fires -- the parser must not stop at the first
      space.
- [x] **2.3** Send `<98,>` (empty name). Confirm this is handled gracefully
      -- either treated as a valid (nameless) NewMouse, or silently
      ignored; **must not crash or hang**.

## 3. Session metadata messages (informational, no expected state change)

For each of the following, confirm the RX echo shows the line was
received, and that **race state does not change** (no unexpected
transition, no telemetry burst beyond what step 2 already caused).

- [x] **3.1** `<96,Senior Maze Solver>` (`MSG_CONTEST_NAME`) -- received,
      no state change.
- [x] **3.2** `<95,Minos 2026>` (`MSG_EVENT_NAME`) -- received, no state
      change.
- [x] **3.3** `<94,3>` (`MSG_ALLOWED_RUNS`) -- received, no state change.
- [x] **3.4** `<93,300>` (`MSG_ENTRY_TIME_S`) -- received, no state change.
- [x] **3.5** `<92,1>` (`MSG_EXTRA_RUN`) -- received; confirm the
      `#[serial-protocol] ExtraRun received (not enforced this round)`
      debug line appears; no state change, run count unaffected (expected
      -- enforcement is explicitly out of scope this round).

## 4. SetMode

- [x] **4.1** Send `<99,TIMER>`. Confirm the
      `#[serial-protocol] SetMode(TIMER) received (no action taken this round)`
      debug line appears; no state change.
- [x] **4.2** Send `<99,CALIBRATION>`. Confirm the equivalent debug line
      appears for `CALIBRATION`; no state change. (This is expected to be
      a no-op for now -- see the plan's open question on CALIBRATION
      semantics. Flag here if you expected different behaviour.)

## 5. RequestType / TimerType reply

- [x] **5.1** Send `<97,0>` (`MSG_REQUEST_TYPE`). Confirm a `<96,1CH>`
      reply (`MSG_TIMER_TYPE`) appears **immediately** (within one RX
      task tick, well under a second) with no other visible side effect.
- [x] **5.2** Repeat 5.1 a few times back to back (e.g. 5 requests, no
      delay).  Confirm each gets its own `<96,1CH>` reply, none dropped or
      corrupted, and no interleaving with the periodic watchdog line.

## 6. Line-ending tolerance

- [ ] **6.1** Send `<98,LFMouse>` terminated with LF only. Confirm normal
      processing (same as Section 2).
- [ ] **6.2** Send `<98,CRMouse>` terminated with CR only. Confirm normal
      processing, and confirm no spurious blank-line parse follows it.
- [ ] **6.3** Send `<98,CRLFMouse>` terminated with CRLF. Confirm normal
      processing, and confirm no spurious blank-line parse follows it
      (the CR/LF pair must not be seen as two lines).

## 7. Malformed / edge-case input

- [x] **7.1** Send garbage with no angle brackets, e.g. `hello world`.
      Confirm it's echoed (`rx: "hello world"`) but produces no command,
      no reply, no crash.
- [x] **7.2** Send an unknown message type, e.g. `<250,1>`. Confirm it's
      echoed and silently ignored (no reply, no crash).
- [x] **7.3** Send a line longer than 63 bytes (the RX buffer size).
      Confirm the device doesn't crash or hang -- excess bytes may be
      dropped/truncated, but recovery on the next line must be clean.
- [x] **7.4** Send `<98,` with no closing `>` at all, then send a normal
      `<98,RecoveryMouse>` right after. Confirm the device recovers and
      processes the second line correctly (doesn't get stuck waiting for
      a `>` that never came in the first one).

## 8. End-to-end combined sequence

- [x] **8.1** Send, in order, with short pauses between each:
      ```
      <98,CheckMouse>
      <96,Senior Maze Solver>
      <95,Minos 2026>
      <94,3>
      <93,300>
      <97,0>
      <98,FinalCheckMouse>
      ```
      Confirm: NewMouse transition happens on line 1; lines 2-5 are
      received with no state change; line 6 gets an immediate `<96,1CH>`
      reply; then drive ARM -> START -> GOAL via the physical buttons and
      confirm a normal run completes and appears correctly (right mouse,
      right time) -- confirming the metadata messages didn't leave any
      stray state behind.
