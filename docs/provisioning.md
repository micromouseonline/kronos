# Provisioning the Kronos network

A flexible approach might work in a similar fashion to that used by ESPHome.


## Sensor Connection to Network
When a sensor powers on and cannot find a saved Wi-Fi network, it must fall back to an Access Point (AP) mode so the user can input credentials.

```
[New Sensor] ──(No Wi-Fi)──> Spins up own AP (e.g., "Sensor-Setup")
                                    │
[Your Phone] ──(Connects)───> Opens Captive Portal (192.168.4.1)
                                    │
[Your Phone] ──(Submits)────> Sends KRONOS Wi-Fi SSID & Password
                                    │
[New Sensor] ──(Saves)──────> Stores to NVS/EEPROM & Reboots into STA Mode
```

If there is some kind of button that can be pressed while the sensor boots, that can be used to trigger a re-configure for use on a new network.

For this, there already exists the WiFiManager library. It is designed specifically to automate this process.

--- 

## Server Discovery

Unless we hard-code the server's address in the router, it is probably easiest to use mDNS so that the server can provide its connection details for the sensors to find and make a connection.

### Server-Side Setup (ESP32 Server):

1. Start the mDNS responder: `mdns_init()` or `MDNS.begin("myserver")`.
2. Advertise the HTTP service: `MDNS.addService("http", "tcp", 80)`.

- Note that the mDNS service is a query-response mechanism. On startup, it sends out a short series of adverts and then goes quite. 
- The server does not constantly advertise after that, it goes quiet, waiting for requests for its identity.
- Then, clients can ask who has a particular service and the server responds.
- The responses have a time-to-live (TTL) telling clients how long the information is valid for. We are not able to change the TTL, which is typically relatively short.
  - Hostname resolution for IP addresses ((e.g., myserver.local pointing to an IP address)) TTLs are about 2 minutes
  - Service records (e.g., _http._tcp.local advertising a service) are about 75 minutes
- Because fof this, we need some more overhead, checking that a failed POST is not simply a result of the mDNS records becoming stale

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


### Client-Side Setup (Sensors):

1. The sensor connects to Wi-Fi.
2. It performs an mDNS query for the service `_http._tcp.local` or directly targets `myserver.local`.
3. Once the IP is resolved, the sensor begins sending its HTTP POST requests to that destination.

---

### Sensor Registration & Security

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
