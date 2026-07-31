# Network Reliability and Guaranteed Delivery

> **Section 1 reflects what's actually implemented (2026-07-31); sections 2
> and 3 remain proposed designs, not yet built** (verified: no exponential
> backoff/jitter or `TCP_NODELAY` in either codebase as of this writing).
> The original version of this document proposed a transaction-ID-based
> idempotent-retry scheme before any of this existed; what actually shipped
> uses a different mechanism (below). Full investigation, measurements, and
> bench-confirmation are in `NETWORK-TIMING-ISSUE.md`'s "Reliable delivery
> over persistent WS connection" issue — this page is a summary, not the
> source of truth.

Capturing sensor events with microsecond accuracy is only half the battle. The other half is guaranteeing 100% delivery of the event notification to the server in a wireless environment.

Gates hold one persistent WebSocket connection open to the controller (see `NETWORK-TIMING-ISSUE.md`'s "Per-event connection latency & queueing" issue) rather than opening a fresh HTTP connection per event. That connection sits on top of TCP, so still benefits from TCP's own delivery guarantees, but retries now happen as repeated messages over the *same* held-open connection rather than fresh sockets.

## 1. Idempotent Retries and De-duplication

A held-open connection doesn't eliminate the "lost ACK" problem: the server can receive and process an event, but the acknowledgement back to the sensor can still be lost, leaving the sensor believing the send failed.

To handle this without risking duplicate records, each event is de-duplicated by content rather than by a dedicated transaction ID: cerberus keys on `(gate_id, event)` → last-seen `tsf_us` (each gate's disciplined Wi-Fi-TSF timestamp, already carried in every event payload for timing purposes) in a small fixed-size table
(`cerberus-gate-controller/src/net/gate-event-dedup.h`). No separate sequential ID field was added to the event schema — `tsf_us` already uniquely identifies an event well enough for this purpose, and reusing it avoided a schema change.

``` mermaid
sequenceDiagram
    participant Sensor
    participant Controller as Gate Controller

    Note right of Sensor: Sensor Detects Event
    Sensor->>Controller: WS event (tsf_us: 99)
    Note right of Controller: Server Processes And Records
    rect rgb(250, 220, 220)
    Controller-->>Sensor: ack_tsf_us (lost in transit)
    Note over Sensor,Controller: Per-attempt timeout expires
    end
    Sensor->>Controller: Resend identical WS event (tsf_us: 99)
    Note right of Controller: Dedup recognises (gate_id, event, tsf_us) already seen
    Controller->>Sensor: ack_tsf_us (successful)
    Note right of Sensor: Sensor stops retrying
```

 - **Server-Side Guard**: cerberus checks incoming `(gate_id, event, tsf_us)` against its dedup table. If already seen, it skips re-dispatching the event into the race state machine but still sends back the ack, so the sensor can stop retrying — the most likely reason an event arrives twice is that the *original* was already processed and only its *ack* was lost, so re-acking without reprocessing is the correct response, not an error.

 - **Sensor-Side Guard**: hesperus's `uploadWorkerTask` waits for the ack after sending; if none arrives within a 300ms per-attempt timeout, it resends the exact same frozen payload over the *same* open connection (never re-derived, since recomputing it would mutate shared clock-drift-audit state as a side effect) — up to 5 attempts, bounded by a hard 2000ms overall deadline that applies even while disconnected. Unlike the original fresh-socket-per-retry proposal, there's no new connection to tear down and reopen; the retry is just another message on the same connection.

Both the dedup table and the ack/retry loop are bench-confirmed on real hardware, including the specific "ack genuinely lost, sensor retries, retry recovers it" scenario via a temporary fault-injection test switch — see `NETWORK-TIMING-ISSUE.md` for the full log evidence.


## 2. Congestion Avoidance

To prevent sensors from flooding the Access Point during network recovery, we can implement Exponential Backoff with Jitter (Randomization):

- **Backoff**: On each successive transmission failure, the sensor increases the delay before the next retry (e.g., 50ms, 120ms, 250ms).

- **Jitter**: A small, randomized time offset (e.g., ±10–30ms) is added to the backoff delay. This prevents multiple sensors from retrying at the exact same millisecond.

- **Traffic Note**: While congestion is highly unlikely on a dedicated AP with sparse gate transition events, these measures provide robust insurance against transient local interference (like router beaconing).


## 3. Low Latency Tuning
By default, the TCP protocol uses Nagle's Algorithm to bundle small outgoing payloads into larger network packets to save bandwidth. For real-time event reporting, this can introduce unacceptable delays.

- **Optimization**: We disable Nagle's Algorithm on both the sensor (client) and the server.

- **Implementation**: Set the TCP_NODELAY socket option immediately after establishing a connection. This forces the TCP stack to transmit our tiny event payloads instantly.


---
[^1]: Idempotent operations are those which can be applied many times without changing the outcome.