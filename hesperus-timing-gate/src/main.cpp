// #include "esp_wifi.h"
#include <Adafruit_NeoPixel.h>
#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include <WebSocketsClient.h>
#include <WiFi.h>

#include "esp_wifi.h"

#include "board-role.h"             // BoardRole, role -> event-name mapping
#include "boards.h"                 // contains the board MAC addresses to look up the identifiers
#include "cli.h"                    // serial command line interface
#include "debug-log.h"              // TSF-timestamped debug_println/debug_printf
#include "provisioning-commands.h"  // `wifi`/`role` serial commands
#include "secrets.h"                // these are the network credentials neede to connect to the AP
#include "wifi-credentials.h"       // NVS-persisted wifi creds from the `wifi` command

// --- Hardware Pin Configuration ---
// STATUS_LED and NEOPIXEL_COLOR_ORDER come from the per-board build_flags in
// platformio.ini/boards.ini - do not hardcode a pin/order here, it varies
// per target (e.g. GPIO21/NEO_RGB on ESP32-S3-Zero, GPIO48/NEO_GRB on the
// S3 Super Mini).
Adafruit_NeoPixel led(1, STATUS_LED, NEOPIXEL_COLOR_ORDER + NEO_KHZ800);

char gate_id[16];

const int LED_PIN = 2;
// Channel A / channel B -- see board-role.h for how these map to ARM/START/
// GOAL depending on the board's provisioned role. Active-low.
const int GATE_PIN = 7;
const int GATE_PIN_B = 6;

BoardRole board_role = BoardRole::UNSET;
Cli cli;

int state = 1;

enum EventType { TRIGGER_A, TRIGGER_B, HEARTBEAT };

struct GateEvent {
  EventType type;
  uint64_t tsf_observed;     // Volatile native Wi-Fi TSF timeline
  uint64_t processor_clock;  // Monotonic internal 64-bit microsecond uptime
};

// Tracking Memory Management
GateEvent last_good_state = {HEARTBEAT, 0, 0};
bool has_initial_baseline = false;

/** --- Dynamic Clock Disciplining Parameters --- */
double clock_alpha = 1.00000000;  // Dynamic drift scaling factor
bool alpha_calibrated = false;    // Explicit state flag
const double EMA_ALPHA = 0.10;    // Weights 10% new sample, 90% history
uint64_t cal_prev_tsf = 0;
uint64_t cal_prev_proc = 0;

const uint64_t DRIFT_MARGIN_US = 500;
const uint64_t MIN_PLAUSIBLE_TSF = 300000000;

// ISR debounce guard for both trigger pins -- one place to tune. 50ms
// comfortably absorbs mechanical switch bounce (bench-testing with a
// pushbutton) without masking two genuinely separate IR-beam triggers close
// together (e.g. a robot with transparent body sections producing more than
// one real break) -- the race state machine, not this guard, is what's
// responsible for deciding what multiple close triggers mean.
const uint64_t DEBOUNCE_US = 50000;

// --- CERBERUS DISCOVERY (mDNS) ---
// Resolved once after Wi-Fi connects and cached for the rest of this boot --
// the venue IP won't change mid-contest, so there's no need to re-query
// mDNS for every event.
IPAddress cerberus_ip;
bool cerberus_ip_valid = false;

// --- PERSISTENT CONNECTION TO CERBERUS (NETWORK-TIMING-ISSUE.md rec. 1) ---
// One connection opened per boot (see loop()'s g_ready edge-detection) and
// held open across every subsequent event, replacing the old per-event
// HTTPClient connect+POST+close cycle. .loop() is pumped from
// uploadWorkerTask (below) -- not safe to call into this from a second task
// without a mutex, so socket-pump and event-send deliberately share one task.
WebSocketsClient wsClient;

/// @brief Resolves cerberus.local via mDNS and caches the result. Safe to
/// call repeatedly (e.g. retried from uploadWorkerTask) until it succeeds --
/// each call is a fresh query, not a re-use of a stale failure.
void resolveCerberus() {
  IPAddress ip = MDNS.queryHost("cerberus");
  if (ip != IPAddress(0, 0, 0, 0)) {
    cerberus_ip = ip;
    cerberus_ip_valid = true;
    debug_printf("[MDNS] cerberus.local -> %s\n", ip.toString().c_str());
  } else {
    debug_println("[MDNS] cerberus.local not found");
  }
}

static int consecutive_audit_failures = 0;

volatile uint32_t networkq_overflow_count = 0;  ///< Incremented by ISR/timer on dropped networkQueue send

// --- WATCHDOG STATE SHARING VARIABLES ---
volatile bool global_is_stuck_in_syn = false;  // Shared flag to notify main loop

// --- FreeRTOS Queues ---
QueueHandle_t networkQueue;  // stores network activities - sending notifications
QueueHandle_t ledQueue;      // stored neopixel commands

// --- FreeRTOS Task Handles (for stack instrumentation) ---
TaskHandle_t ledTaskHandle = NULL;
TaskHandle_t uploadTaskHandle = NULL;

enum LedPattern { FLASH_TRIGGER_1, FLASH_TRIGGER_2, SHOW_HEARTBEAT };

// Baseline/idle colour the NeoPixel sits at between flashes, reflecting
// whether this board is actually ready to send events -- set from loop()
// as Wi-Fi/cerberus-discovery state changes, read by ledDiagnosticTask.
// OFF: Wi-Fi not connected. SEARCHING: Wi-Fi connected, still resolving
// cerberus.local. READY: g_ready -- Wi-Fi connected AND cerberus's IP is
// cached.
enum class LedBase { OFF, SEARCHING, READY };
volatile LedBase led_base = LedBase::OFF;

// True only once Wi-Fi is connected AND cerberus's IP has been resolved --
// mirrors led_base == READY, kept as its own flag so other code can check
// readiness without re-deriving it from led_base.
volatile bool g_ready = false;

// --- LOW-PRIORITY DIAGNOSTIC LED TASK ---
void ledDiagnosticTask(void *pvParameters) {
  led.begin();
  led.show();  // Initialize to OFF
  LedPattern requested_pattern;
  bool blink_on = false;
  uint32_t last_blink_toggle = millis();

  // Colour for whatever led_base currently is -- SEARCHING blinks (alternates
  // blue/off every ~300ms via blink_on), OFF/READY are steady.
  auto base_color = [&]() -> uint32_t {
    switch (led_base) {
      case LedBase::READY:
        return led.Color(0, 24, 0);  // solid green
      case LedBase::SEARCHING:
        return blink_on ? led.Color(0, 0, 32) : led.Color(0, 0, 0);  // blinking blue
      default:
        return led.Color(0, 0, 0);  // off
    }
  };

  while (1) {
    // Bounded wait, not portMAX_DELAY -- SEARCHING's blink needs to keep
    // alternating even when no trigger/heartbeat flash ever arrives, so
    // this loop can't just block forever waiting for one.
    if (xQueueReceive(ledQueue, &requested_pattern, pdMS_TO_TICKS(300)) == pdPASS) {
      if (requested_pattern == FLASH_TRIGGER_1) {
        led.setPixelColor(0, led.Color(0, 32, 0));  // Bright Green
        led.show();
        vTaskDelay(pdMS_TO_TICKS(50));
      } else if (requested_pattern == FLASH_TRIGGER_2) {
        led.setPixelColor(0, led.Color(32, 0, 0));  // Bright Red
        led.show();
        vTaskDelay(pdMS_TO_TICKS(50));
      } else if (requested_pattern == SHOW_HEARTBEAT) {
        led.setPixelColor(0, led.Color(16, 16, 16));  // Dim Cyan pulse
        led.show();
        vTaskDelay(pdMS_TO_TICKS(30));
      }
      // Flash is over -- return to the current baseline colour, not
      // hardcoded off, so a flash stays visible against a green "ready"
      // background instead of blanking it.
      led.setPixelColor(0, base_color());
      led.show();
    } else if (millis() - last_blink_toggle > 300) {
      blink_on = !blink_on;
      last_blink_toggle = millis();
      led.setPixelColor(0, base_color());
      led.show();
    }
  }
}

// --- HARDWARE INTERRUPT SERVICE ROUTINES (ISRs) WITH DEBOUNCE ---
void IRAM_ATTR handleSensor1() {
  static uint64_t last_interrupt_time = 0;
  uint64_t current_time = esp_timer_get_time();

  if (current_time - last_interrupt_time < DEBOUNCE_US) {
    return;
  }
  last_interrupt_time = current_time;

  BaseType_t xHigherPriorityTaskWoken = pdFALSE;
  GateEvent ev;
  ev.type = TRIGGER_A;
  ev.tsf_observed = esp_wifi_get_tsf_time(WIFI_IF_STA);
  ev.processor_clock = current_time;
  BaseType_t sent = xQueueSendFromISR(networkQueue, &ev, &xHigherPriorityTaskWoken);
  if (sent != pdTRUE) {
    networkq_overflow_count++;
  }
  if (xHigherPriorityTaskWoken) {
    portYIELD_FROM_ISR();
  }
}

void IRAM_ATTR handleSensor2() {
  static uint64_t last_interrupt_time = 0;
  uint64_t current_time = esp_timer_get_time();

  if (current_time - last_interrupt_time < DEBOUNCE_US) {
    return;
  }
  last_interrupt_time = current_time;

  BaseType_t xHigherPriorityTaskWoken = pdFALSE;
  GateEvent ev;
  ev.type = TRIGGER_B;
  ev.tsf_observed = esp_wifi_get_tsf_time(WIFI_IF_STA);
  ev.processor_clock = current_time;
  BaseType_t sent = xQueueSendFromISR(networkQueue, &ev, &xHigherPriorityTaskWoken);
  if (sent != pdTRUE) {
    networkq_overflow_count++;
  }
  if (xHigherPriorityTaskWoken) {
    portYIELD_FROM_ISR();
  }
}

void heartbeatTimerCallback(TimerHandle_t xTimer) {
  GateEvent hb;
  hb.type = HEARTBEAT;
  hb.tsf_observed = esp_wifi_get_tsf_time(WIFI_IF_STA);
  hb.processor_clock = esp_timer_get_time();

  if (xQueueSend(networkQueue, &hb, 0) != pdTRUE) {
    networkq_overflow_count++;
  }

  // Serial.printf("[STACK] LED min free: %u bytes | Upload min free: %u bytes\n",
  //               uxTaskGetStackHighWaterMark(ledTaskHandle) * 4,
  //               uxTaskGetStackHighWaterMark(uploadTaskHandle) * 4);
}

// --- CORE NETWORK WORKER TASK WITH DISCIPLINED OSCILLATOR MATH ---
void uploadWorkerTask(void *pvParameters) {
  GateEvent current_ev;

  while (1) {
    // Bounded, not portMAX_DELAY -- wsClient.loop() must be pumped
    // regularly (reconnect, ping/pong, disconnect detection) even when no
    // event is waiting; 10ms is well under DEBOUNCE_US/event-spacing scales
    // here, at negligible cost on an otherwise-idle core-1 task.
    wsClient.loop();
    if (xQueueReceive(networkQueue, &current_ev, pdMS_TO_TICKS(10)) == pdPASS) {
      // TODO: Serial logging is unavailable in field deployment. Future options:
      //   - RGB LED pattern to signal overflow (extend LedPattern enum)
      //   - Write overflow count to SPIFFS for post-session retrieval
      //   - Append &overflow=N to next HTTP GET to notify server
      uint32_t drops = networkq_overflow_count;
      if (drops > 0) {
        networkq_overflow_count = 0;
        debug_printf("[QUEUE OVERFLOW] %lu networkQueue event(s) dropped.\n", drops);
      }

      LedPattern pattern = (current_ev.type == TRIGGER_A) ? FLASH_TRIGGER_1 : FLASH_TRIGGER_2;
      if (current_ev.type == HEARTBEAT)
        pattern = SHOW_HEARTBEAT;
      if (xQueueSend(ledQueue, &pattern, 0) != pdTRUE) {
        debug_println("[QUEUE OVERFLOW] ledQueue full; LED feedback dropped.");
      }

      uint64_t tsf_to_transmit = current_ev.tsf_observed;
      bool trust_observed_tsf = false;
      String clock_mode = "TSF";

      // --- TIMELINE SANITY AUDIT ENGINE ---
      if (current_ev.tsf_observed != 0 && has_initial_baseline) {
        uint64_t elapsed_processor_time = current_ev.processor_clock - last_good_state.processor_clock;
        uint64_t expected_tsf_delta = (uint64_t)(elapsed_processor_time * clock_alpha);
        uint64_t expected_tsf = last_good_state.tsf_observed + expected_tsf_delta;

        uint64_t drift_variance =
            (current_ev.tsf_observed > expected_tsf) ? (current_ev.tsf_observed - expected_tsf) : (expected_tsf - current_ev.tsf_observed);

        if (drift_variance <= DRIFT_MARGIN_US) {
          trust_observed_tsf = true;
          consecutive_audit_failures = 0;

          if (cal_prev_tsf != 0 && (current_ev.processor_clock - cal_prev_proc) > 4000000) {
            double actual_tsf_delta = (double)(current_ev.tsf_observed - cal_prev_tsf);
            double actual_proc_delta = (double)(current_ev.processor_clock - cal_prev_proc);
            double instant_alpha = actual_tsf_delta / actual_proc_delta;
            instant_alpha = constrain(instant_alpha, 0.9990, 1.0010);

            if (!alpha_calibrated) {
              clock_alpha = instant_alpha;
              alpha_calibrated = true;
            } else {
              clock_alpha = (EMA_ALPHA * instant_alpha) + ((1.0 - EMA_ALPHA) * clock_alpha);
            }
            // Serial.printf("[CALIBRATION] Dynamic Alpha Stabilized: %.8f\n", clock_alpha);
          }

          cal_prev_tsf = current_ev.tsf_observed;
          cal_prev_proc = current_ev.processor_clock;
        } else {
          debug_printf("[AUDIT ALERT] Temporal Disruption! Drift: %llu us. Rejecting stack value.\n", drift_variance);
          consecutive_audit_failures++;
          if (consecutive_audit_failures >= 5) {
            debug_println("[AUDIT RECOVERY] Jitter is persistent. Accepting new router baseline shift.");
            trust_observed_tsf = true;
            consecutive_audit_failures = 0;
            cal_prev_tsf = current_ev.tsf_observed;
            cal_prev_proc = current_ev.processor_clock;
          }
        }
      } else if (current_ev.tsf_observed != 0 && !has_initial_baseline) {
        if (current_ev.tsf_observed >= MIN_PLAUSIBLE_TSF) {
          trust_observed_tsf = true;
          has_initial_baseline = true;
          cal_prev_tsf = current_ev.tsf_observed;
          cal_prev_proc = current_ev.processor_clock;
          debug_printf("[INITIALIZED] Valid Baseline Coordinates Locked: %llu\n", current_ev.tsf_observed);
        } else {
          debug_printf("[PLAUSIBILITY REJECT] TSF %llu too low. Wi-Fi stack un-synchronized.\n", current_ev.tsf_observed);
        }
      }

      // --- EXECUTION ENGINE CORE DETERMINATION ---
      if (trust_observed_tsf) {
        last_good_state = current_ev;
        clock_mode = "TSF";
        global_is_stuck_in_syn = false;  // Clear state flag
      } else if (has_initial_baseline) {
        uint64_t elapsed_processor_time = current_ev.processor_clock - last_good_state.processor_clock;
        uint64_t disciplined_delta = (uint64_t)(elapsed_processor_time * clock_alpha);

        tsf_to_transmit = last_good_state.tsf_observed + disciplined_delta;
        clock_mode = "SYN";
        global_is_stuck_in_syn = true;  // Alert main loop that we are using fallback tracking
        debug_printf("[DISCIPLINED SYN] Extrapolated TSF: %llu (Alpha: %.8f)\n", tsf_to_transmit, clock_alpha);

        last_good_state.type = current_ev.type;
        last_good_state.tsf_observed = tsf_to_transmit;
        last_good_state.processor_clock = current_ev.processor_clock;
      } else {
        debug_println("[CRITICAL DROP] Baseline missing or un-synchronized. Packet dropped.");
        continue;
      }

      // --- HTTP TRANSMISSION ENGINE ---
      // HEARTBEAT is only used above for local clock discipline/LED
      // feedback -- cerberus's /api/event has no HEARTBEAT event type
      // (see board-role.h's header comment), so it's never sent.
      if (current_ev.type == HEARTBEAT) {
        continue;
      }

      const char *event_name = board_event_name(board_role, current_ev.type == TRIGGER_A);
      if (event_name == nullptr) {
        debug_println("[Async Worker] Role not set -- run `role start` or `role goal`. Event dropped.");
        continue;
      }

      if (WiFi.status() != WL_CONNECTED) {
        debug_println("[WS Worker] Link down. Internal queue stacking.");
        continue;
      }

      if (!cerberus_ip_valid) {
        resolveCerberus();
      }
      if (!cerberus_ip_valid) {
        debug_println("[WS Worker] cerberus.local not resolved yet. Event dropped.");
        continue;
      }

      JsonDocument doc;
      doc["gate_id"] = gate_id;
      doc["event"] = event_name;
      doc["tsf_us"] = tsf_to_transmit;
      doc["gate_us"] = current_ev.processor_clock;
      String payload;
      serializeJson(doc, payload);

      // --- WS TRANSMISSION ---
      // Fire-and-forget over the persistent connection -- no ack expected
      // back from cerberus (see net/http-server.h's ws_event_handler()
      // comment on the same repo). A retry/reliability mechanism is a
      // separate, not-yet-designed item (NETWORK-TIMING-ISSUE.md status
      // section) -- this deliberately doesn't try to half-build one here.
      // Matches the pre-WS behaviour exactly for the down/unresolved cases
      // above: drop, no requeue.
      bool sent = wsClient.isConnected();
      if (sent) {
        wsClient.sendTXT(payload);
        debug_printf("[WS Worker] Sent %s (%s).\n", event_name, clock_mode.c_str());
      } else {
        debug_println("[WS Worker] Link down. Event dropped.");
      }

      // --- PATCH 1: NETWORK RECEIPT ESCAPE HATCH ---
      // Only trigger TSF re-entry once we've actually sent in SYN mode --
      // "sent" here means the socket was connected at send time, not a
      // server-confirmed receipt (no ack exists over WS, unlike the old
      // httpCode==200 check).
      if (sent && clock_mode == "SYN") {
        uint64_t raw_tsf_check = esp_wifi_get_tsf_time(WIFI_IF_STA);
        if (raw_tsf_check > MIN_PLAUSIBLE_TSF) {
          debug_println("[ESCAPE HATCH] Server confirmed online. Attempting TSF re-entry.");
          has_initial_baseline = false;
          cal_prev_tsf = 0;
          cal_prev_proc = 0;
          consecutive_audit_failures = 0;
        }
      }
    }
  }
}

// Saved credentials (wifi-credentials.h, set via the `wifi` serial command)
// take priority over secrets.h's compiled-in default -- mirrors cerberus's
// wifi_connect_task priority order.
char stored_ssid[33];
char stored_pass[65];
const char *connect_ssid = ssid;
const char *connect_pass = password;

void setup() {
  delay(2000);
  Serial.begin(115200);

  pinMode(LED_PIN, OUTPUT);

  strlcpy(gate_id, identifyBoard(), sizeof(gate_id));
  pinMode(GATE_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(GATE_PIN), handleSensor1, FALLING);
  pinMode(GATE_PIN_B, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(GATE_PIN_B), handleSensor2, FALLING);

  // Hardware jumper is authoritative -- re-read and re-save on every boot,
  // so a `role` command from a previous session never lingers past a power
  // cycle once the jumper says otherwise.
  board_role = board_role_from_jumper();
  board_role_save(board_role);
  debug_printf("[SYSTEM] Board role (jumper): %s\n", board_role_name(board_role));

  cli.begin(PROVISIONING_COMMANDS, PROVISIONING_COMMAND_COUNT);

  networkQueue = xQueueCreate(10, sizeof(GateEvent));
  ledQueue = xQueueCreate(5, sizeof(LedPattern));

  if (wifi_credentials_load(stored_ssid, sizeof(stored_ssid), stored_pass, sizeof(stored_pass))) {
    connect_ssid = stored_ssid;
    connect_pass = stored_pass;
    debug_println("[SYSTEM] Using saved Wi-Fi credentials from NVS");
  }

  WiFi.persistent(false);
  WiFi.disconnect(true);
  WiFi.begin(connect_ssid, connect_pass);
  // No modem sleep -- WIFI_PS_MAX_MODEM was traced to a 2.9s first-packet
  // wake-up latency spike on real hardware (radio asleep between sparse
  // events, exactly the gap pattern between ARM/START/GOAL in a real race).
  // Matches cerberus's own net/wifi-manager.h, which disables power-save
  // for the same reason. Trades idle power for rapid, consistent response;
  // an adaptive "sleep after N idle seconds" scheme is a possible future
  // enhancement, not built here.
  esp_wifi_set_ps(WIFI_PS_NONE);

  TimerHandle_t hbTimer = xTimerCreate("HB_Timer", pdMS_TO_TICKS(5147), pdTRUE, (void *)0, heartbeatTimerCallback);
  if (hbTimer != NULL) {
    xTimerStart(hbTimer, 0);
  }

  xTaskCreatePinnedToCore(ledDiagnosticTask, "LED_Task", 2048, NULL, 1, &ledTaskHandle, 1);
  xTaskCreatePinnedToCore(uploadWorkerTask, "UploadWorker", 8192, NULL, 2, &uploadTaskHandle, 1);
}

// --- MAIN LOOP EXECUTION TASK ---
void loop() {
  static uint32_t last_connected_time = millis();
  static uint32_t last_reconnect_attempt = millis();
  static uint32_t last_cerberus_attempt = 0;
  static uint32_t syn_mode_start_time = 0;  // Tracks duration of fallback timing
  static bool was_connected = false;
  static bool ws_was_ready = false;  // g_ready edge-detection, opens wsClient once per readiness

  uint32_t current_time = millis();

  // --- PATCH 2: EXTENDED SYN-MODE WATCHDOG ---
  // If the gate is trapped in synthetic time for >10 s, the Wi-Fi
  // recovery mechanism has stalled. Reboot to force a clean restart.
  if (global_is_stuck_in_syn) {
    if (syn_mode_start_time == 0) {
      syn_mode_start_time = current_time;  // Start the clock on the breakdown
    } else if (current_time - syn_mode_start_time > 10000) {
      // If the gate is stuck calculating synthetic time for more than 10 seconds,
      // the hardware clock register has hit an overflow boundary. Clear stacks and reboot.
      debug_println("[ROLLOVER FAULT] Trapped in synthetic time loop for 10s. Forcing hardware reboot...");
      delay(500);
      ESP.restart();
    }
  } else {
    syn_mode_start_time = 0;  // Reset watchdog when operations are normal
  }

  if (WiFi.status() == WL_CONNECTED) {
    if (!was_connected) {
      debug_println("[NETWORK] Link Active! IP: " + WiFi.localIP().toString());
      if (!MDNS.begin(gate_id)) {
        debug_println("[MDNS] failed to start");
      }
      cerberus_ip_valid = false;    // re-resolve on this connection
      last_cerberus_attempt = 0;  // force an immediate resolve attempt below
      was_connected = true;
    }
    last_connected_time = current_time;

    // Gates readiness (LED + actual event sending in uploadWorkerTask) on
    // cerberus's IP being resolved -- retried here on a throttled timer
    // rather than every loop() tick, but deliberately doesn't block
    // cli.poll()/the reconnect-watchdog below, so `wifi`/`role` serial
    // reprovisioning and Wi-Fi recovery still work even if cerberus is
    // genuinely unreachable.
    if (!cerberus_ip_valid) {
      led_base = LedBase::SEARCHING;
      g_ready = false;
      if (current_time - last_cerberus_attempt > 1000) {
        resolveCerberus();
        last_cerberus_attempt = current_time;
      }
    } else {
      led_base = LedBase::READY;
      g_ready = true;
      // Edge-triggered: open the persistent connection once per readiness
      // transition, not every loop() tick. wsClient handles its own
      // reconnect loop internally once begun, so this doesn't need to be
      // re-called on every tick while already ready.
      if (!ws_was_ready) {
        wsClient.begin(cerberus_ip.toString(), 80, "/ws");
        wsClient.enableHeartbeat(5000, 3000, 2);  // 5s ping, 3s pong timeout, disconnect after 2 misses
        ws_was_ready = true;
      }
    }
  } else {
    was_connected = false;
    cerberus_ip_valid = false;
    led_base = LedBase::OFF;
    g_ready = false;
    // Force a fresh begin() next time readiness is reached -- cerberus may
    // resolve to a different IP after this WiFi drop/rejoin.
    ws_was_ready = false;

    if (current_time - last_connected_time > 15000) {
      debug_println("[WATCHDOG FAULT] Wi-Fi link dead for 15s. Smashing network stack...");
      WiFi.disconnect(true, true);
      delay(500);
      debug_println("[WATCHDOG RECOVERY] Re-initializing hardware radio interface...");
      WiFi.begin(connect_ssid, connect_pass);
      last_connected_time = current_time;
      last_reconnect_attempt = current_time;
    } else if (current_time - last_reconnect_attempt > 3000) {
      Serial.print(".");
      last_reconnect_attempt = current_time;
    }
  }

  cli.poll();

  static bool last_state = false;
  if (networkQueue != NULL) {
    UBaseType_t items = uxQueueMessagesWaiting(networkQueue);
    if ((items > 0) != last_state) {
      last_state = (items > 0);
      digitalWrite(LED_PIN, last_state ? HIGH : LOW);
    }
  }
  delay(10);
}