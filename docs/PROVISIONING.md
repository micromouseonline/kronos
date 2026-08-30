# Provisioning the Kronos network

This document originally described a proposed provisioning approach for
KRONOS sensors generally. Since then, both projects have implemented most
of it, each with its own mechanism — those sections below are now marked
**Implemented (CERBERUS)** or **Implemented (HESPERUS)** and describe what's
actually there, with the specifics corrected against the code (verified
2026-08-02). Only two pieces remain **Proposed**: deep-sleep wake cycling
(hesperus runs continuously today, no sleep/wake logic at all) and the
sensor registration/auth-token handshake (no `/api/register` endpoint or
identity validation exists on cerberus).

## Sensor Connection to Network — Implemented (CERBERUS)

When a device powers on and cannot connect to a saved or compiled-in Wi-Fi
network within a timeout, it falls back to an Access Point (AP) mode so
the user can input credentials. CERBERUS implements this directly (no
WiFiManager library dependency) in `cerberus-gate-controller/firmware/src/net/
wifi-provisioning.h` and `wifi-credentials.h`:

```
[CERBERUS] ──(No Wi-Fi after 60s)──> Spins up own AP ("CERBERUS-SETUP")
                                    │
[Your Phone] ──(Connects)───> Browse to 192.168.4.1 (no auto-redirect)
                                    │
[Your Phone] ──(Submits)────> Sends Wi-Fi SSID & Password via HTML form
                                    │
[CERBERUS] ──(Saves)──────> Stores to NVS (Preferences) & Reboots into STA Mode
```

The AP name and password are shown directly on the device's own screen
(bypassing the normal UI), so no separate documentation is needed to find
them. Two things can trigger this flow: the 60-second no-connect timeout
above, or an on-demand "Wi-Fi Setup" menu action that forces the portal
open even while already connected, for switching to a different network.
Holding the TOUCH key cancels and reboots back to normal operation.

### Sensor Connection to Network — Implemented (HESPERUS, different mechanism)

hesperus has no captive-portal AP mode. Instead, credentials are set over
its serial CLI (a `wifi` command, see `hesperus-timing-gate/firmware/src/cli.h`) and
persisted to NVS via `wifi-credentials.h`; on boot, `wifi_credentials_load()`
is tried first and, if present, overrides the compiled-in default from
`secrets.h` (`hesperus-timing-gate/firmware/src/main.cpp:517-553`). So it's not
"hardcoded credentials, no provisioning" — it's a serial-driven mechanism
rather than a self-hosted AP + web form.

--- 

## Server Discovery

Unless we hard-code the server's address in the router, it is probably easiest to use mDNS so that the server can provide its connection details for the sensors to find and make a connection.

### Server-Side Setup — Implemented (CERBERUS)

CERBERUS advertises itself via mDNS exactly as originally proposed here,
in `cerberus-gate-controller/firmware/src/net/mdns.h`:

1. Start the mDNS responder: `MDNS.begin("cerberus")`.
2. Advertise the HTTP service: `MDNS.addService("http", "tcp", 80)`.

This makes the device reachable at `http://cerberus.local/` without
needing its DHCP-assigned IP. It's started only once Wi-Fi actually has an
IP (not at boot), to avoid racing mDNS init against an unconnected radio.

- Note that the mDNS service is a query-response mechanism. On startup, it sends out a short series of adverts and then goes quite. 
- The server does not constantly advertise after that, it goes quiet, waiting for requests for its identity.
- Then, clients can ask who has a particular service and the server responds.
- The responses have a time-to-live (TTL) telling clients how long the information is valid for. We are not able to change the TTL, which is typically relatively short.
  - Hostname resolution for IP addresses ((e.g., myserver.local pointing to an IP address)) TTLs are about 2 minutes
  - Service records (e.g., _http._tcp.local advertising a service) are about 75 minutes
- Because fof this, we need some more overhead, checking that a failed POST is not simply a result of the mDNS records becoming stale

### Sensor-Side Discovery & Connection — Implemented (HESPERUS)

hesperus resolves and connects to cerberus dynamically, no hardcoded
server IP: `resolveCerberus()` (`hesperus-timing-gate/firmware/src/main.cpp:99-111`)
calls `MDNS.queryHost("cerberus")` and caches the result in
`cerberus_ip`/`cerberus_ip_valid`. It's re-triggered (throttled to once per
second) whenever `cerberus_ip_valid` is false, and gets explicitly
invalidated — forcing a fresh mDNS query — on every Wi-Fi
disconnect/reconnect (`main.cpp:610,645,650`), since cerberus could come
back on a different IP after that. Once resolved, `wsClient.begin()` opens
the persistent WebSocket connection to it (`main.cpp:637`).

**Known gap**: this re-resolve only fires on a hesperus-side Wi-Fi drop.
If cerberus alone restarts while hesperus's own Wi-Fi link stays up,
hesperus never re-queries mDNS — it relies on the WS client library's own
reconnect loop retrying the *same cached IP*. Fine as long as cerberus's
IP is stable across its reboot (typical with a normal DHCP lease), but if
it ever came back on a different IP without hesperus's Wi-Fi also
cycling, hesperus would keep retrying a dead address indefinitely. Not yet
hit in practice, not yet tested.

### Power Cycling (Deep Sleep) — Proposed, not yet built

hesperus runs continuously today — no `esp_sleep`/`forceSleepBegin`/
`forceSleepWake` calls exist anywhere in its source. If deep-sleep power
saving is added later, the discovery model above would need to change: a
deep sleep cycle drops the cached mDNS result, so discovery would need to
re-run on every wake, not just on reconnect as it does now. Modem sleep
(radio-off, RAM-retained) would be simpler than deep sleep here, since it
doesn't lose that state.

```
// Pseudocode for Sensor Loop
void loop() {
    // 1. Wake up the Wi-Fi modem
    WiFi.forceSleepWake(); 
    delay(1); // Small yield to let the radio stabilize
    
    // Wait briefly for Wi-Fi connection to re-establish
    while (WiFi.status() != WL_CONNECTED) {
        delay(10);
    }

    // 2. Attempt data transmission
    bool success = send_http_post(cached_server_ip, data);
    
    // 3. Fallback: If IP changed during sleep, resolve mDNS and retry
    if (!success) {
        IPAddress new_ip = discover_server_via_mdns("myserver");
        if (new_ip != IPAddress(0,0,0,0)) {
            cached_server_ip = new_ip;
            send_http_post(cached_server_ip, data); // Retry
        }
    }

    // 4. Put the Wi-Fi modem back to sleep to save power
    WiFi.forceSleepBegin(); 
    
    // 5. Wait for the next sensor read interval (CPU stays active)
    delay(sensor_interval); 
}
```

---

## Recovery from Failure

Covers failures in the network relationship between hesperus and cerberus
— what an operator sees, what's automatic, and what needs manual action.
Failures local to one project with no network component (e.g. cerberus's
touch-calibration NVS lockout) belong in that project's own operator
documentation instead — see `cerberus-gate-controller/docs/OPERATOR-GUIDE.md`.

hesperus's onboard NeoPixel reports discovery/connection state directly
(`main.cpp:155-186`), so most of this is diagnosable from the LED alone:

| LED | Meaning |
|-----|---------|
| Off | Wi-Fi not connected |
| Blinking blue | Wi-Fi connected, still resolving `cerberus.local` |
| Solid green | Wi-Fi connected and cerberus's IP is cached (`g_ready`) |

### Hesperus can't find cerberus at boot

**Symptom**: LED stuck blinking blue.

**Automatic**: `resolveCerberus()` retries the mDNS query once per second,
indefinitely — no timeout, no operator action needed once cerberus becomes
reachable.

**If it persists**: check that cerberus is actually powered on and joined
to the *same* Wi-Fi network (not stuck in its own `CERBERUS-SETUP` AP
fallback — see above), and that the AP/router isn't blocking mDNS
multicast between clients (some consumer routers with "AP/client
isolation" enabled do this, which would make discovery fail permanently
until that setting is changed).

### Hesperus's own Wi-Fi link drops

**Symptom**: LED goes off, then blinks blue again once reconnected (before
cerberus re-resolves).

**Automatic**: a 15-second dead-link watchdog forces `WiFi.disconnect()` +
`WiFi.begin()` if the radio doesn't recover on its own (`main.cpp:652-659`).
Either way, reconnection unconditionally invalidates the cached cerberus
IP and re-triggers `resolveCerberus()`, and reopens the WebSocket fresh
once resolved (`main.cpp:645,650`) — this path assumes cerberus may have
come back on a different IP, so it always re-discovers rather than
trusting the old address. No operator action needed.

### Cerberus restarts while hesperus's Wi-Fi link stays up

**Symptom**: LED stays solid green throughout (hesperus doesn't know
cerberus is gone yet); any event that occurs during the outage gets
dropped after hesperus's ack/retry budget is exhausted (5 attempts over a
hard 2-second deadline, `main.cpp:67-70`, logged as `"Event dropped after
max retries"` / `"...after ack deadline"`).

**Automatic, if cerberus comes back on the same IP**: the WebSocket client
library (`WebSocketsClient`) auto-reconnects on its own — this can't be
disabled and isn't app code — retrying every 500ms by default (verified in
`links2004/WebSockets@2.4.1` source). Since `resolveCerberus()` is never
re-invoked here, hesperus is still pointed at the same cached IP, so this
recovers with no operator action as long as that IP is unchanged, which is
the normal case (stable DHCP lease).

**Manual action needed, if cerberus comes back on a *different* IP**:
hesperus has no way to detect this on its own — it will keep retrying the
old, now-dead address forever. Power-cycle the hesperus board (or
otherwise force its Wi-Fi to drop and reconnect) to make it re-run
`resolveCerberus()` against the new IP. Not yet observed in practice, but
follows directly from the code path above.

### Hesperus has wrong or stale Wi-Fi credentials

**Symptom**: LED stays off; `WiFi.begin()` never succeeds.

**Manual action required — serial only, no wireless recovery path**:
connect over USB and re-run `wifi <ssid> <passphrase>`
(`provisioning-commands.h`), which saves to NVS and reboots. There's no
separate "forget credentials" command — re-running `wifi` is also how you
revert to a different network. If NVS is corrupted or credentials need to
be fully cleared back to the compiled-in `secrets.h` default, a flash
erase (`pio run -e <env> -t erase`) is the only option today.

### Cerberus can't join Wi-Fi at boot

Covered above under "Sensor Connection to Network — Implemented
(CERBERUS)": falls back to its own `CERBERUS-SETUP` AP after 60 seconds,
with the AP name/password shown on its own screen. Its live connection
state (once joined) is also shown on NeoKey LED position 3
(`cerberus-gate-controller/docs/SYSTEM-DESCRIPTION.md`).

### Everything restarts together (e.g. a power outage)

No special case — this is just the "hesperus's own Wi-Fi link drops"
scenario above (the AP going down drops every device's link at once) plus
cerberus's own boot-time Wi-Fi join, both of which are already
self-recovering. No particular power-on order is required.

---

### Sensor Registration & Security — Proposed, not yet built

No `/api/register` endpoint or auth-token handshake exists anywhere in
the repo today — CERBERUS's HTTP/WebSocket server only implements `/`,
`/ws`, `/api/event`, `/leaderboard`, `/time`, and `/events` (see
`cerberus-gate-controller/docs/SYSTEM-DESCRIPTION.md`), none of which
validate a registered sensor identity. This section remains a proposal for
if/when that's needed.

The KRONOS architecture has sensors pushing data *to* the server. We need a handshake to register and validate new sensors.

#### The Provisioning Handshake:
```
[ Sensor Node ]                             [ ESP32 Server ]
       │                                            │
       │ ── 1. POST /api/register (MAC, Type) ────> │
       │                                            │  [ Holds in Pending UI ]
       │                                            │  [ Admin Approves Node ]
       │                                            │
       │ <── 2. Response: Auth Token ────────────── │
       │
 [Saves Token to NVS]
       │
       │ ── 3. POST /api/data (Header: Token) ────> │ (Validates & Records Data)

```

* **Step 1: First Contact (Unregistered):** The sensor sends its unique MAC address and sensor type to the server's `/api/register` endpoint.
* **Step 2: Server Approval:** The server holds this in a "Pending Devices" list in its UI.
* **Step 3: Access Granted:** Once you approve the device in the server UI, the server assigns it an authentication token. The sensor stores this token in its flash memory and includes it in the header of all future HTTP data payloads to prove its identity.

---

```
[ Router Boot ] ──> Starts DHCP Server (assigns random IPs)
                         │
         ┌───────────────┴───────────────┐
         ▼                               ▼
 [ ESP32 Server ]                [ Sensor Node ]
   IP: 192.168.1.42                IP: 192.168.1.115
   Advertises:                     Performs mDNS query for:
   "myserver.local is              "Where is myserver.local?"
    at 192.168.1.42"                     │
         │                               │
         └─────────── Resolved ──────────┘
                         │
                         ▼
        [ Sensor Node ] ──(persistent WS connection)──> [ ESP32 Server ]
```        
