# Network Reliability and Guaranteed Delivery

> **Section 1 reflects what's actually implemented (2026-07-31). Section 2
> (congestion avoidance) remains a proposed design, not built — verified: no
> exponential backoff/jitter in either codebase as of this writing. Section
> 3 (Nagle/`TCP_NODELAY`) turned out to already be true by default (2026-08-02)
> — see that section for the library-level evidence; no app code change was
> ever needed.** The original version of this document proposed a
> transaction-ID-based idempotent-retry scheme before any of this existed;
> what actually shipped uses a different mechanism (below). Full
> investigation, measurements, and bench-confirmation are in
> `NETWORK-TIMING-LOG.md`'s "Reliable delivery over persistent WS
> connection" issue — this page is a summary, not the source of truth.

Capturing sensor events with microsecond accuracy is only half the battle. The other half is guaranteeing 100% delivery of the event notification to the server in a wireless environment.

Gates hold one persistent WebSocket connection open to the controller (see `NETWORK-TIMING-LOG.md`'s "Per-event connection latency & queueing" issue) rather than opening a fresh HTTP connection per event. That connection sits on top of TCP, so still benefits from TCP's own delivery guarantees, but retries now happen as repeated messages over the *same* held-open connection rather than fresh sockets.

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

Both the dedup table and the ack/retry loop are bench-confirmed on real hardware, including the specific "ack genuinely lost, sensor retries, retry recovers it" scenario via a temporary fault-injection test switch — see `NETWORK-TIMING-LOG.md` for the full log evidence.


## 2. Congestion Avoidance

To prevent sensors from flooding the Access Point during network recovery, we can implement Exponential Backoff with Jitter (Randomization):

- **Backoff**: On each successive transmission failure, the sensor increases the delay before the next retry (e.g., 50ms, 120ms, 250ms).

- **Jitter**: A small, randomized time offset (e.g., ±10–30ms) is added to the backoff delay. This prevents multiple sensors from retrying at the exact same millisecond.

- **Traffic Note**: While congestion is highly unlikely on a dedicated AP with sparse gate transition events, these measures provide robust insurance against transient local interference (like router beaconing).


## 3. Low Latency Tuning — Already True By Default

By default, the TCP protocol uses Nagle's Algorithm to bundle small outgoing payloads into larger network packets to save bandwidth. For real-time event reporting, this can introduce unacceptable delays — but analysis of the actual traffic pattern (see `NETWORK-TIMING-LOG.md`) found Nagle was never likely to bite here anyway: hesperus's ack/retry loop is stop-and-wait (never two un-acked writes in flight on the same socket) and events are naturally spaced seconds to minutes apart, not bursty.

Moot either way — both ends already disable Nagle, as a side effect of the exact library versions this project pins, not app code:

- **Sensor (client)**: `links2004/WebSockets @ ^2.4.1` — `WebSocketsClient::connectedCb()` calls `_client.tcp->setNoDelay(true)` unconditionally on ESP32 whenever the connection is (re)established.
- **Server**: `esp32async/ESPAsyncWebServer @ ^3.11.2` — `AsyncWebServer::begin()` calls `_server.setNoDelay(true)` unconditionally, which `esp32async/AsyncTCP @ ^3.5.0` then applies to every accepted connection (`tcp_accept()`). Note AsyncTCP's own `AsyncServer` defaults `_noDelay` to `false` — it's specifically `ESPAsyncWebServer::begin()` that turns it on, so this is a property of this library stack, not something to assume from AsyncTCP alone if it's ever swapped out.

No implementation needed. Only a risk if a future library upgrade or a switch away from `ESPAsyncWebServer`/`WebSockets` silently drops this default — worth a quick re-check of both libraries' source if either is ever upgraded or replaced.


---
[^1]: Idempotent operations are those which can be applied many times without changing the outcome.