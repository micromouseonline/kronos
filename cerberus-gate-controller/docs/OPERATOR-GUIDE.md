# Cerberus operator guide

A guide for running a race session on a CERBERUS gate controller — what the
main menu and Wi-Fi setup look like, what the physical buttons and LEDs do,
what the screen shows, and how to work through a normal race day. For how
the software implements any of this, see `docs/RACE-STATE-MACHINE.md`
(state-by-state spec) and `docs/INPUT-SYSTEM.md` (input wiring).

## Contents

- [Cerberus operator guide](#cerberus-operator-guide)
  - [Contents](#contents)
  - [Startup and main menu](#startup-and-main-menu)
  - [Connecting to a Wi-Fi network](#connecting-to-a-wi-fi-network)
  - [Using the box from a PC or phone](#using-the-box-from-a-pc-or-phone)
  - [The physical controls](#the-physical-controls)
  - [What the LEDs mean](#what-the-leds-mean)
  - [Power-on and Calibrating](#power-on-and-calibrating)
  - [Running a race](#running-a-race)
  - [Remote control](#remote-control)
  - [Troubleshooting](#troubleshooting)

## Startup and main menu

On power-up the controller shows a main menu with a 2x3 grid of buttons
(five are active; the sixth slot is currently unused):

| Button | What it does |
|---|---|
| **WiFi SETUP** | Shows the network last joined and the controller's current IP address, and lets you join a different network (see below). |
| **GATE TEST** | Lets you test that the gates respond, without running a full race. |
| **MAZE TIMER** | Goes to the race timer, passing through Calibrating on the way (see "Power-on and Calibrating"). Long-press ARM, or have RATS send a New Mouse message, to get started. |
| **RESET** | Resets the controller. |
| **SETTINGS** | Toggles serial/debug **verbosity** and **watchdog** message output. |

From any race screen, long-press key 3 (TOUCH) to return to this menu at
any time.

## Connecting to a Wi-Fi network

1. From the main menu, press **WiFi SETUP**.
2. Select **Join New Network**.
3. The screen shows the name and password of a temporary access point
   (`CERBERUS-SETUP`).
4. On your phone or laptop, join that network, then browse to
   `192.168.4.1/wifi`. This opens a page where you can enter the SSID and
   password of the network you actually want the controller on.
5. Submit the form. The controller reboots and attempts to join that
   network — the screen flashes briefly while it connects.

If the controller can't reach the saved network at all, key 3 (TOUCH)
blinks cyan indefinitely rather than giving up — see
[Troubleshooting](#troubleshooting).

## Using the box from a PC or phone

Once connected, the controller's IP address is shown on the WiFi Setup
screen. From any device on the same network, browse to:

- `http://<controller-ip>/` — a simple clock, synchronized to the box.
- `http://<controller-ip>/leaderboard` — an automatically updated
  leaderboard.

## The physical controls

CERBERUS is operated with a **NeoKey 1x4** keypad — four buttons, each with
its own LED, wired in a fixed order:

| Key | Label | Short press | Long press (hold) |
|---|---|---|---|
| 0 | **ARM** | Arm — confirms a mouse is in the start cell, ready to run | **Restart** — abandon the current mouse and start a fresh entry from Calibrating/Waiting/Armed/Running/Goal |
| 1 | **START** | Start — the mouse has left the start cell, begin timing | (no effect, except on the main menu screen — see below) |
| 2 | **GOAL** | Goal — the mouse has reached the finish | (no effect) |
| 3 | **TOUCH** | Currently has no effect on race progress | **Return to menu** (leaves the current race screen; does not affect race state) |

The touchscreen itself is **not** used for ARM/START/GOAL during a race —
it only navigates between the menu, main timer, and settings screens.
Tapping the WiFi-status area on the main screen opens the setup menu.

**On the main menu screen only**, long-pressing START launches the touch
calibration wizard — see [Troubleshooting](#troubleshooting) if the
touchscreen is misbehaving.

If the NeoKey module is unplugged or not detected, all four LEDs stay
off and **none of the local buttons work** — the controller still responds
to remote ARM/START/GOAL/new-mouse commands over serial or HTTP (see
[Remote control](#remote-control)) until the keypad is reconnected.

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

Key 3 (TOUCH) is reserved for WiFi status but stays off either way — WiFi
connection status is shown on the main screen's status bar instead (WIFI and
a dBm reading when connected, red **MANUAL** when not — see below). The only
exception to key 3 staying off is during Calibrating (see below), where it
briefly reflects a keypress instead.

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

- **Touchscreen taps land in the wrong place, or don't register at all:**
  from the main menu screen, long-press START (key 1) to re-run the touch
  calibration wizard. This is a physical NeoKey gesture deliberately, so
  it works even when the touchscreen is too miscalibrated to use — no
  flash erase needed.
- **No LEDs at all, buttons don't respond:** the NeoKey module isn't
  detected (unplugged, bad I2C connection). The controller still runs
  and still accepts remote commands; reseat/reconnect the keypad to
  restore local control — no reboot needed, it's detected automatically.
- **Status bar shows red "MANUAL" instead of a dBm reading:** WiFi isn't
  connected. There's no timeout and no forced hand-off to WiFi setup — the
  controller keeps racing on the physical buttons indefinitely, and keeps
  retrying the network in the background on its own. Local racing is
  unaffected (WiFi is only needed for remote gates, the HTTP leaderboard,
  and HTTP-driven remote control). To set up WiFi, hold TOUCH to reach the
  main menu, then press **WiFi SETUP**.
- **Controller stuck showing Calibrating:** it's waiting for a genuine
  new-mouse event — long-press ARM, or send `<98,...>` from the host.
- **Touchscreen unresponsive after a screen change:** there's a brief
  (250ms) lockout after every screen switch to avoid a stray tap
  registering twice; wait a moment and try again.
- **Menu locked out after a botched touch calibration:** this is a known
  gap with no on-device recovery yet — see `docs/PLANNED-UPDATES.md`.
  Race control via the NeoKey keypad is unaffected either way, since it
  doesn't depend on touch.
