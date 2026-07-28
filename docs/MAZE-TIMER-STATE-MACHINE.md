# Maze timer state machine (superseded)

This was an early draft of the CERBERUS gate-controller state machine —
unnumbered states, no `Started`/`TIMED_OUT` states, no `FirstRun`
course-timer guard, no run-limit logic, no wire-protocol detail.

It has been superseded by
[`cerberus-gate-controller/docs/RACE-STATE-MACHINE.md`](../cerberus-gate-controller/docs/RACE-STATE-MACHINE.md),
which is reconciled against the RATS V2 protocol spec and the actual
firmware. Use that as the authoritative state machine reference.
