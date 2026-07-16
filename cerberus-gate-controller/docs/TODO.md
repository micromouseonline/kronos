# Cerberus gate controller -- future enhancements

Ideas and known gaps that aren't blocking current work. Not a commitment or
a schedule -- pull items into USER-INPUT-SYSTEM.md (or a new plan doc)
when actually picked up.

## Touch calibration escape hatch

If NVS already holds a `"calibrated"=true` entry with bad data (e.g. left
over from earlier testing on the same physical board), `calibrate()`
(`touch-calibration.h`) loads it and never re-launches the wizard --
Supervisor navigation is touch-driven, so bad calibration data can lock you
out of reaching "Recalibrate Touch" via the UI. Currently the only recovery
is a full flash erase (`pio run -e <env> -t erase`).

Possible fix: a way to force `re_calibrate()` without needing working
touch -- e.g. hold a screen corner (or any fixed physical action available
on that board) during boot, checked before `calibrate()` loads stored data
in `app_setup()`.

Found 2026-07-08 during Freenove S3 CYD bring-up.
