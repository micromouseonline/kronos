# Provisioning the Kronos network

This document originally described a proposed provisioning approach for
KRONOS sensors generally. Since then, CERBERUS has implemented its own
version of the network-join half of it (config-portal fallback + mDNS
server advertising) — those sections below are now marked **Implemented
(CERBERUS)** and describe what's actually there, with the specifics
corrected against the code. The sensor/gate-side sections (mDNS client
discovery, deep-sleep wake cycling, the registration handshake) remain
**Proposed** — hesperus-timing-gate today just connects with hardcoded
credentials from `secrets.h` and has none of this yet, though it's the
expected direction for that project.

## Sensor Connection to Network — Implemented (CERBERUS)

When a device powers on and cannot connect to a saved or compiled-in Wi-Fi
network within a timeout, it falls back to an Access Point (AP) mode so
the user can input credentials. CERBERUS implements this directly (no
WiFiManager library dependency) in `cerberus-gate-controller/src/net/
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

--- 

## Server Discovery

Unless we hard-code the server's address in the router, it is probably easiest to use mDNS so that the server can provide its connection details for the sensors to find and make a connection.

### Server-Side Setup — Implemented (CERBERUS)

CERBERUS advertises itself via mDNS exactly as originally proposed here,
in `cerberus-gate-controller/src/net/mdns.h`:

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

### Sensor-Side Discovery & Power Cycling — Proposed, not yet built

Nothing below this point exists in hesperus-timing-gate yet — it currently
connects with a hardcoded SSID/password from `secrets.h` and has no mDNS
client, no sleep-wake cycling, and no dynamic server discovery. Kept here
as the intended direction for that project.

**NOTE**: if we use deep sleep, the cached service locators are lost and the discovery must start again when the radio wakes up. Modem sleep is much simpler

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


### Client-Side Setup (Sensors) — Proposed, not yet built

1. The sensor connects to Wi-Fi.
2. It performs an mDNS query for the service `_http._tcp.local` or directly targets `myserver.local`.
3. Once the IP is resolved, the sensor begins sending its HTTP POST requests to that destination.

---

### Sensor Registration & Security — Proposed, not yet built

No `/api/register` endpoint or auth-token handshake exists anywhere in
the repo today — CERBERUS's HTTP server only implements `/`, `/api/event`,
`/leaderboard`, `/time`, and `/events` (see `cerberus-gate-controller/
docs/SYSTEM-DESCRIPTION.md`), none of which validate a registered sensor
identity. This section remains a proposal for if/when that's needed.

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
        [ Sensor Node ] ──(HTTP POST)──> [ ESP32 Server ]
```        
