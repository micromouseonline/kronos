# Cerberus operator guide

This is a guide for running a race session on a CERBERUS gate controller —
what the physical buttons and LEDs do, what the screen shows, and how to
work through a normal race day. For how the software implements any of
this, see `docs/RACE-STATE-MACHINE.md` (state-by-state spec) and
`docs/INPUT-SYSTEM.md` (input wiring).

## The physical controls

CERBERUS is operated with a **NeoKey 1x4** keypad — four buttons, each with
its own LED, wired in a fixed order:

| Key | Label | Short press | Long press (hold) |
|---|---|---|---|
| 0 | **ARM** | Arm — confirms a mouse is in the start cell, ready to run | **Restart** — abandon the current mouse and start a fresh entry from Calibrating/Waiting/Armed/Running/Goal |
| 1 | **START** | Start — the mouse has left the start cell, begin timing | (no effect) |
| 2 | **GOAL** | Goal — the mouse has reached the finish | (no effect) |
| 3 | **TOUCH** | Currently has no effect on race progress | **Return to menu** (leaves the current race screen; does not affect race state) |

The touchscreen itself is **not** used for ARM/START/GOAL during a race —
it only navigates between the menu, main timer, and settings screens.
Tapping the WiFi-status area on the main screen opens the setup menu.

If the NeoKey module is unplugged or not detected, all four LEDs stay
off and **none of the local buttons work** — the controller still responds
to remote ARM/START/GOAL/new-mouse commands over serial or HTTP (see
"Remote control" below) until the keypad is reconnected.

## What the LEDs mean

Keys 0-2 (ARM/START/GOAL) reflect the current race state:

| Race state | Key 0 (ARM) | Key 1 (START) | Key 2 (GOAL) |
|---|---|---|---|
| Calibrating | off | off | off |
| New mouse (transient) | magenta | magenta | magenta |
| Waiting for robot | green | green | green |
| Armed | green | off | off |
| Running | off | green | off |
| Goal reached | off | off | green |

Key 3 (TOUCH) is normally reserved for **WiFi status**, not race state:
blinking cyan while connecting, off once connected. The only exception is
during Calibrating (see below), where it briefly reflects a keypress
instead.

## Power-on and Calibrating

On power-up (or after a Restart from any other state) the controller
starts in **Calibrating**. This is a self-test screen, not a race state:

- Time display shows `.........`, entry time shows `00:00`, run count
  shows `0/5`, mouse name and run times are blank.
- Pressing any of the four physical keys lights that key's LED **yellow**
  for as long as it's held — use this to confirm each button (and, on
  systems with separate gate hardware, each gate signal feeding this
  controller) is actually registering before a race starts.
- **Calibrating only exits on a genuine "new mouse" event**: a long press
  of ARM, or a `<98,...>` NewMouse message from the host over serial/HTTP.
  A short press of TOUCH does **not** exit Calibrating (it currently has
  no effect in any state — see the controls table above).

## Running a race

1. **Start a new mouse.** Long-press ARM (or send a NewMouse command from
   the host). The controller briefly shows New Mouse (all three LEDs
   magenta), then settles into **Waiting**, all three LEDs green. The
   screen shows the mouse's name, the maximum entry time, and a `0/5`
   run counter (defaults; the host can override both via serial/HTTP
   before the first Arm).
2. **Place the mouse in the start cell and press ARM.** The controller
   moves to **Armed** — key 0 (ARM) lights, keys 1/2 go off. The run timer
   is reset and waiting; an overall entry-time countdown starts (only on
   the very first Arm of this mouse, not on re-arms).
3. **Press START when the mouse leaves the start cell.** The controller
   moves to **Running** — key 1 (START) lights, run timer counts up.
4. **Press GOAL when the mouse reaches the finish.** The controller moves
   to **Goal** — key 2 (GOAL) lights, the run time is recorded and sent to
   the host.
   - If the mouse doesn't finish (crashes, needs to be picked up and
     restarted), press **ARM** instead of GOAL — this abandons the current
     attempt and returns to Armed for another try, without recording a
     run.
5. **Press ARM again to continue.** From Goal, ARM re-arms for the next
   run if the mouse hasn't used up its allowed run count, or drops back to
   **Waiting** if it has. There is no automatic advance out of Goal —
   it always waits for an explicit ARM.
6. **Long-press ARM at any point** to abandon the current mouse entirely
   and start a fresh entry (back to step 1). This is also how you recover
   if something has gone wrong mid-run.

A false start can be undone remotely: the host can send an ExtraRun
message (`<92,...>`) to give back a run that shouldn't have counted,
without a local button for it.

## Remote control

Everything above can equally be driven remotely — a host PC connected
over serial (RATS V2 protocol) or another device on the network posting
to the HTTP API produces the exact same ARM/START/GOAL/new-mouse commands
as the physical keypad, and the display/LEDs update the same way
regardless of source. Local and remote control can be mixed freely during
a session; there's no mode switch to select between them.

## Troubleshooting

- **No LEDs at all, buttons don't respond:** the NeoKey module isn't
  detected (unplugged, bad I2C connection). The controller still runs
  and still accepts remote commands; reseat/reconnect the keypad to
  restore local control — no reboot needed, it's detected automatically.
- **Key 3 blinking cyan and staying that way:** WiFi isn't connecting.
  Local racing is unaffected (WiFi is only needed for remote gates, the
  HTTP leaderboard, and HTTP-driven remote control).
- **Controller stuck showing Calibrating:** it's waiting for a genuine
  new-mouse event — long-press ARM, or send `<98,...>` from the host.
- **Touchscreen unresponsive after a screen change:** there's a brief
  (250ms) lockout after every screen switch to avoid a stray tap
  registering twice; wait a moment and try again.
- **Menu locked out after a botched touch calibration:** this is a known
  gap with no on-device recovery yet — see `docs/PLANNED-UPDATES.md`.
  Race control via the NeoKey keypad is unaffected either way, since it
  doesn't depend on touch.
