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

- [x] **2.1** From any state, send `<98,MightyMouse Might>`. Confirm:
      - RX echo shows the full name, unmodified: `rx: "<98,MightyMouse>"`.
      - Same RESTART/new-mouse transition as 1.2 happens (name doesn't
        block the transition).
      - **Changed:** the on-screen mouse-name label now shows
        "MightyMouse" (previously showed a canned name regardless of
        what was sent). See Section 9 for the full name-threading checks.
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
- [ ] **3.3** `<94,3>` (`MSG_ALLOWED_RUNS`) -- received, no *immediate*
      state change. **Changed:** this value is no longer inert -- it now
      overrides the run-count limit for the current mouse (see Section 9).
- [x] **3.4** `<93,300>` (`MSG_ENTRY_TIME_S`) -- received, no immediate
      state change. **Changed:** now drives the on-screen countdown once
      ARMED (see Section 9) -- still no forced state change at zero.
- [x] **3.5** `<92,1>` (`MSG_EXTRA_RUN`) -- **Changed:** this now actually
      decrements `mouse_run_count` by 1 (floor 0) via the Main Event
      Queue, instead of just being logged. See Section 9 for the
      grant-one-more-attempt check. No standalone visible effect if the
      mouse isn't currently exhausted.

## 4. SetMode

- [x] **4.1** Send `<99,TIMER>` while in `CALIBRATE`. **Changed:** this now
      forces `race_state` to `WAITING` (previously logged only, no state
      change) -- confirm the screen/NeoKeys reflect `WAITING`. See Section
      9 for the full CALIBRATION/TIMER round-trip check.
- [x] **4.2** Send `<99,CALIBRATION>` from any state. **Changed:** this now
      forces `race_state` to `CALIBRATE` (previously logged only) --
      confirm the screen/NeoKeys reflect `CALIBRATE` (NeoKeys go dark, per
      `neokey_reflect_race_state()`'s existing `CALIBRATE` case).

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

## 9. New behaviour: SetMode, run limits, EntryTimeS, mouse name

Covers the four items implemented after the initial parsing-only
round (see `docs/PLANNED-UPDATES.md`'s Unresolved Protocol Behaviors section and the
plan file's Decisions section).

- [x] **9.1 SetMode round-trip.** Send `<99,CALIBRATION>` from `WAITING`
      (or any state). Confirm the screen and NeoKeys show `CALIBRATE`
      (NeoKeys dark). Send `<99,TIMER>`. Confirm it returns to `WAITING`
      (NeoKeys green, `<4,1>` telemetry). Try it again from `RUNNING` --
      confirm `CALIBRATION` interrupts the run unconditionally (host
      override, same spirit as `RESTART`).
- [x] **9.2 NewMouse name -- display.** Send `<98,SpeedyGonzales>`.
      Confirm the on-screen mouse-name label shows "SpeedyGonzales", not
      a canned name.
- [x] **9.3 NewMouse name -- leaderboard.** After 9.2, ARM -> START -> GOAL
      a run via the physical buttons. Check `http://cerberus.local/leaderboard`
      (or the device's IP) -- confirm the row shows "SpeedyGonzales", not
      a canned name.
- [x] **9.4 `<98,0>` is treated as a real name, not a fallback trigger.**
      Send `<98,0>`. **Known/accepted behaviour (not a bug to fix right
      now):** the mouse name becomes the literal string `"0"`, both
      on-screen and on the leaderboard -- `race_timer_enter_new_mouse()`
      only falls back to a canned `mouse_names[]` pick when the value is
      genuinely empty (`name[0] == '\0'`), and `"0"` is a non-empty
      string like any other. True fallback-to-canned-name only happens
      via `<98,>` (empty value, see 2.3) or a producer that supplies no
      name at all (local button, HTTP without a name).
- [x] **9.5 AllowedRuns enforcement.** Send `<98,LimitTestMouse>` then
      `<94,2>` (`AllowedRuns=2`). Complete 2 runs (ARM -> START -> GOAL
      twice). On the 3rd ARM attempt, confirm the mouse is treated as
      exhausted (drops back to `WAITING`, same behaviour as hitting the
      default `MAX_RUNS_PER_MOUSE=5` today) -- i.e. it now stops at 2, not
      5.
- [x] **9.6 ExtraRun grants one more.** Immediately after 9.5 (mouse
      still exhausted at 2/2), send `<92,1>` (`ExtraRun`). Confirm ARM now
      succeeds and a 3rd run can be completed.
- [x] **9.7 AllowedRuns resets per mouse.** After 9.5/9.6, send a fresh
      `<98,NextMouse>` with no `AllowedRuns` follow-up. Confirm this new
      mouse is limited by the default `MAX_RUNS_PER_MOUSE=5`, not the
      previous mouse's limit of 2 (confirms the reset-on-NewMouse logic).
- [x] **9.8 EntryTimeS starts on first ARM, not NewMouse.** Send
      `<93,60>` (`EntryTimeS=60`) then `<98,TimerTestMouse>`. Confirm
      `lbl_time_remaining` shows a static "01:00" in `WAITING` -- **not**
      counting down yet. ARM the mouse. Confirm it now counts down live
      from 60s. **Changed:** the countdown only starts on the mouse's
      first `WAITING`->`ARMED` transition, not at `NewMouse`/in `WAITING`.
- [x] **9.8b Countdown continues across runs, doesn't restart.** After
      9.8, complete a run (START -> GOAL) and ARM again for run 2 without
      sending another `<93,...>`. Confirm the countdown keeps counting
      down from wherever it left off (does NOT jump back up to 60s) --
      it only ever starts once per mouse entry.
- [x] **9.8c Stops and turns red at zero.** Let the countdown from 9.8/9.8b
      reach 0. Confirm `lbl_time_remaining` shows "00:00", turns **red**,
      and stays at "00:00" (does not wrap or go negative) -- confirm no
      crash and no forced state transition (track-and-display only).
- [x] **9.9 EntryTimeS persists across mice.** After 9.8c, send a fresh
      `<98,NextTimerMouse>` with **no** new `<93,...>`. Confirm
      `lbl_time_remaining` resets to the full "01:00" (static, not red)
      in `WAITING` -- **changed:** EntryTimeS now persists as the
      starting entry time for every subsequent mouse until the host sends
      a new value, instead of needing to be resent each time.
- [x] **9.9b Explicitly unset.** Send `<93,-1>` (negative value -- an
      emergent way to request "unset" behaviour, since the countdown code
      only requires a value `>= 0` to treat it as active). Confirm
      `lbl_time_remaining` reverts to counting *up* from 0 while
      ARMED/RUNNING/GOAL (the pre-EntryTimeS raw-elapsed display), normal
      colour. **Note:** this is no longer the boot default -- see 10.6.
- [x] **9.10 Run-number label format.** Confirm `lbl_run_number` shows
      "current/max" (e.g. "0/5" at a fresh mouse, "1/5" after one run) and
      is visually centered within its parent panel (`pnl_run_number`).
      Send `<94,8>` (`AllowedRuns=8`) and confirm the label immediately
      updates to show ".../8" -- both halves (current and max) must
      refresh whenever either changes.

## 10. State-table reconciliation (docs/updated-state-table.md alignment)

Covers the round of fixes that brought the firmware into line with the
now-confirmed `updated-state-table.md` (T-key restrictions, CALIBRATE
display reset, `CourseTimeMs` timing, `C1RunTime` x3, `EntryTimeS`
default). None of this was covered by Sections 1-9 above.

- [x] **10.1 CALIBRATE display reset.** With a mouse name, a non-default
      run count, and at least one run already recorded (e.g. finish
      Section 9's steps first), send `<99,CALIBRATION>`. Confirm:
      mouse-name label goes **blank**, run-number label shows **"0/5"**
      (back to the default cap, not whatever `AllowedRuns` was set to),
      `lbl_time_remaining` shows a fixed **"00:00"**, and the run-times
      list is **empty**. Leaderboard is left alone (open question in the
      doc itself, not touched by this fix).
- [x] **10.2 T key does nothing in CALIBRATE.** While in `CALIBRATE`
      (from 10.1, or fresh boot), press the physical Touch/`T` key (short
      press). Confirm **no transition** happens -- only `<98,xxxx>` or a
      long press on `A` should exit Calibrating now.
- [x] **10.3 T key does nothing in ARMED.** ARM a mouse, then press `T`
      (short press). Confirm the armed mouse is **not** abandoned -- no
      new-mouse transition, state stays `ARMED`. (Previously this would
      abandon the armed mouse and start a new one.)
- [x] **10.4 C1RunTime sent three times.** Complete a run (ARM -> START ->
      GOAL). Confirm **three** `<13,mmmm>` lines appear in the terminal
      (was two before this round).
- [x] **10.5 CourseTimeMs timing.** Send `<98,CourseTimingMouse>`. Confirm
      **no** `<30,0>` appears yet (device is in `WAITING`, mouse not yet
      armed). Now ARM the mouse. Confirm `<30,0>` appears now, close
      together with `<4,2>` (`TimerState`=`ARMED`) -- **changed:**
      previously `<30,0>` fired immediately on the `<98,...>` line itself.
      Complete a run and ARM again for run 2 -- confirm `<30,0>` is **not**
      resent (fires once per entry only).
- [ ] **10.6 EntryTimeS default is 600s.** Requires a genuine power-cycle
      first -- **`g_entry_time_s_limit` persists by design (that's the
      whole point of the persistence behaviour confirmed earlier), so
      once anything has sent `<93,-1>` (e.g. 9.9b) it stays "unset" until
      another `<93,...>` arrives; it does NOT revert to the 600s default
      on its own.** After a fresh boot, send a plain `<98,0>` with **no**
      `<93,...>` follow-up at all. Confirm `lbl_time_remaining` shows a
      real countdown starting from **"10:00"** once armed (600s default),
      not the count-up-from-0 behaviour -- **changed:** `EntryTimeS` now
      defaults to 600s baked in at boot, rather than "unset until the host
      sends one." **Test ordering note:** run this check *before* 9.9b in
      the same session, or power-cycle between them -- otherwise 9.9b's
      `<93,-1>` will still be in effect and this step will (correctly, by
      design) show the count-up behaviour instead.
- [x] **10.7 Messages 93-96 ignored outside WAITING.** ARM a mouse, then
      send `<94,2>` (`AllowedRuns=2`). Confirm the run-number label's
      max does **not** change (still whatever it was before ARM) --
      confirms 93-96 are only applied while `WAITING`, ignored in `ARMED`
      (and, by the same rule, `RUNNING`/`GOAL`/`CALIBRATE` too). Return to
      `WAITING` (finish the run) and resend `<94,2>` -- confirm it **does**
      apply now.
- [x] **10.8 GOAL requires manual ARM (no auto-advance).** Complete a run
      and reach `GOAL`. Wait at least 10-15 seconds without pressing
      anything. Confirm the device stays in `GOAL` indefinitely -- it does
      **not** automatically fall through to `WAITING`/`ARMED` on its own.
      Then press `ARM` and confirm it advances normally.
