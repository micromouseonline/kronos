# CERBERUS Gate Controller Message Format (Legacy V7.0 Protocol)

> **Historical reference only.** This documents the older V7.0 message
> set (`src/messages-reference.h`, not included in the current build). The
> firmware today implements RATS V2 instead — see
> `docs/preferredMessageSequencesV2.pdf` and `docs/RACE-STATE-MACHINE.md`
> for the current, authoritative protocol and state machine. Kept here for
> comparison only; may not reflect current firmware behaviour.

Source: `messages-reference.h` (comment block + constants), original authors
David Hannaford & Ian Butterworth (2017-09-25), modified by Peter Harrison
(2022-06-07, v7.0).

## 1. Framing

```
<message_type,value>[optional trailing text]\r\n
```

- `<` and `>` are literal delimiters that bracket the message body.
- Body is exactly two fields separated by a comma: `message_type,value`.
- Trailing text after `>` (comments, debug annotations) MUST be ignored by
  the parser up to the line terminator.
- Line terminator is CRLF (`\r\n`), per `Serial.println()` on the Arduino side.
- Example: `<98,0>` = "new mouse" with value 0.

## 2. Field grammar

```
message  = "<" message_type "," value ">" trailer CRLF
message_type = 1*DIGIT              ; integer, see registry in section 3
value         = 1*DIGIT             ; unsigned long, decimal, no sign
trailer       = *CHAR                ; free text, discarded by parser
```

- Both fields are ASCII decimal integers, no leading `+`, no whitespace
  inside the brackets.
- `value` is transmitted as an `unsigned long` (up to 32-bit range on the
  Arduino/ESP32 side) but always rendered as plain decimal text.
- One exception to strict two-field framing: when `message_type == 4`
  (`MSG_CURRENT_STATE`), the sender appends a single extra space-separated
  character after the closing `>` before any comment text, e.g.
  `<4,2> A` (see section 5). Parsers that only need `type,value` can ignore
  this; parsers needing the extra state character must special-case type 4.

## 3. Message type registry

| Code | Symbol              | Direction      | Frequency        | Notes |
|-----:|----------------------|----------------|-------------------|-------|
| 0    | MSG_Watchdog         | Gate -> Host   | 1000 ms           | Incrementing counter as heartbeat/keepalive |
| 4    | MSG_CURRENT_STATE    | Gate -> Host   | on state change   | `value` is the state enum, see section 4 |
| 12   | MSG_C1SplitTime      | Gate -> Host   | event-driven      | ms elapsed for current run split; sent as 0 to reset host counter |
| 13   | MSG_C1RunTime        | Gate -> Host   | event-driven      | ms for a just-completed run; definitive score time; **sent twice** (see section 5) |
| 30   | MSG_CourseTimeMs     | Gate -> Host   | event-driven      | ms mouse has been active in maze; sent as 0 to reset host counter |
| 98   | MSG_NewMouse         | Host -> Gate   | event-driven      | `value` always 0; selects a new mouse in host app |
| 99   | MSG_SetMode          | Host -> Gate   | event-driven      | `value` selects mode: see section 4b |
| 71   | MSG_STrigger         | Gate -> Host   | event-driven      | *Not currently used.* Start Gate trigger, value in {0,1} |
| 72   | MSG_FTrigger         | Gate -> Host   | event-driven      | *Not currently used.* Finish Gate trigger, value in {0,1} |
| 73   | MSG_CTrigger         | Gate -> Host   | event-driven      | *Not currently used.* Mouse-in-start-cell trigger, value in {0,1} |
| 81   | MSG_SGLevel          | Gate -> Host   | 100 ms            | Calibration only. Start Gate phototransistor intensity |
| 82   | MSG_SGPot            | Gate -> Host   | 100 ms            | Calibration only. Start Gate potentiometer reading |
| 83   | MSG_FGLevel          | Gate -> Host   | 100 ms            | Calibration only. Finish Gate phototransistor intensity |
| 84   | MSG_FGPot            | Gate -> Host   | 100 ms            | Calibration only. Finish Gate potentiometer reading |
| 85   | MSG_SCLevel          | Gate -> Host   | 100 ms            | Calibration only. Start-cell phototransistor intensity |
| 86   | MSG_SCPot            | Gate -> Host   | 100 ms            | Calibration only. Start-cell potentiometer reading |

"Gate -> Host" corresponds to the original "Arduino to PC"; "Host -> Gate" to
"PC to Arduino". In this codebase the gate/timer device is the ESP32
controller and the host is the supervising application (originally Visual
Basic).

## 4. Enumerations

### 4a. MSG_CURRENT_STATE value (type 4)

| Value | Symbol    | Meaning |
|------:|-----------|---------|
| 0     | CALIBRATE | Calibrating gates |
| 1     | WAITING   | Looking for mouse in start cell |
| 2     | ARMED     | Mouse seen in start cell |
| 3     | STARTING  | Run started, start gate not yet cleared |
| 4     | RUNNING   | Run in progress |
| 5     | GOAL      | Run to centre completed (finish gate triggered) |
| 6     | NEW_MOUSE | New mouse selected |

### 4b. MSG_SetMode value (type 99)

Named modes `TIMER` and `CALIBRATION`; the reference header does not encode
numeric values for these -- confirm against current firmware
(`send_message`/mode-handling code) before assuming values, rather than
guessing.

## 5. Known quirks a parser must tolerate

- **Type 4 has a trailing state character.** `send_message()` appends a
  single-character token (`last_char`) after `<4,value>` when
  `type == MSG_CURRENT_STATE`, before any comment text. Format becomes
  `<4,value> X comment`. This is the only message type with content between
  `>` and end-of-line that is not free-form comment.
- **Type 13 (MSG_C1RunTime) is transmitted twice** in immediate succession
  (20 ms apart), by design, per `send_run_time()`. Consumers must
  deduplicate or expect two identical events per run completion.
- **Values 0 for types 12 and 30 are sentinels**, meaning "reset/start
  the host-side counter", not a real zero-duration measurement.
- **Trailing comment text is free-form** (e.g. `" RUN TIME"`) and must not
  be relied upon for parsing logic -- only the bracketed `type,value` pair
  and (for type 4) the single trailing state character are structured.

## 6. Minimal parser algorithm

1. Read a line up to CRLF.
2. Locate `<` ... `>`; if absent, discard line (not a protocol message).
3. Split bracket contents on first `,`; parse both sides as base-10 integers
   -> `(message_type, value)`.
4. If `message_type == 4`: the character immediately following `>` and a
   single space, if present, is the current-state's short display code
   (`last_char`); everything after that is free comment.
5. For all other types, everything after `>` is free comment and can be
   discarded.
6. Dispatch on `message_type` using the registry in section 3.

## 7. Open items to confirm against current firmware before relying on this spec

- Numeric values for `MSG_SetMode` (`TIMER` / `CALIBRATION`).
- Whether `messages-legacy-reference.h` (deleted in current working tree per
  git status) differs from this v7.0 reference in ways relevant to a parser.
- Whether the ESP32/ new codebase (`main.cpp`) still uses this exact framing
  or has since diverged -- this document reflects the *reference* comment
  only, not necessarily the currently active protocol.
