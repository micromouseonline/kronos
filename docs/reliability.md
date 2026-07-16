# Network Reliability and Guaranteed Delivery

Capturing sensor events with microsecond accuracy is only half the battle. The other half is guaranteeing 100% delivery of the event notification to the server in a wireless environment.

We utilize HTTP POST requests over TCP. While this adds overhead for simple messages, it provides multiple layers of delivery guarantees, which we reinforce with an application-level transaction protocol.

## 1. Idempotent Retries and De-duplication

The TCP transport layer has a delivery guarantee and the HTTP layer provides additional acknowledgements.

In spite of that, messages may still be lost or, even if the message is delivered, the acknowledgement may be lost and the sensor sends the same message until it does get a response.

TCP guarantees packet delivery, but it cannot prevent a connection from dropping *after* the server receives the data but *before* the sensor receives the response (the "lost ACK" scenario). 

To ensure 0% data loss without risking duplicate records, we implement an **Idempotent Retry** [^1] flow. Every event is packaged with a unique transaction ID (e.g., `[Sensor_MAC] + [Sequential_Event_ID]`).



``` mermaid
sequenceDiagram
    participant Sensor
    participant Controller as Gate Controller

    Note right of Sensor: Sensor Detects and Queues Event
    Sensor->>Controller: POST gate event (ID: 99)
    Note right of Controller: Server Writes To Recorder
    rect rgb(250, 220, 220)
    Controller-->>Sensor: ACK (lost in transit)
    Note over Sensor,Controller: WiFi Connection Times Out
    end
    Sensor->>Controller: Retry POST gate event (ID: 99)
    Note right of Controller: Server Ignores Duplicate POST
    Controller->>Sensor: ACK (successful)
    Note right of Sensor: Sensor Clears Event From Queue
    
```

 - **Server-Side Guard**: The server must check incoming transaction IDs against its database. If the ID already exists, it ignores the duplicate payload but still sends back a successful ACK to allow the sensor to safely clear its queue.

 - **Sensor-Side Guard**: If an HTTP request times out, the sensor tears down the active socket, waits, and retries on a fresh connection. Late ACKs from orphaned sockets are automatically discarded by the network stack.


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