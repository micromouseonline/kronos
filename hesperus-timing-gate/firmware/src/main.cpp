// #include "esp_wifi.h"
#include <Adafruit_NeoPixel.h>
#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include <WebSocketsClient.h>
#include <WiFi.h>

#include "esp_wifi.h"

#include "beam-sensor.h" // dual-EMA reflective/occlusion beam-break detector
#include "board-role.h" // BoardRole, role -> event-name mapping
#include "boards.h" // contains the board MAC addresses to look up the identifiers
#include "cli.h"    // serial command line interface
#include "debug-log.h"            // TSF-timestamped debug_println/debug_printf
#include "network-health-stats.h" // NVS-persisted stall/drop/disconnect counters
#include "provisioning-commands.h" // `wifi`/`role` serial commands
#include "secrets.h" // these are the network credentials neede to connect to the AP
#include "wifi-credentials.h" // NVS-persisted wifi creds from the `wifi` command

#if HAS_HTTP
#include "net/debug-http-server.h" // on-demand /logs, /status diagnostics endpoint
#endif

// --- Hardware Pin Configuration ---
// STATUS_LED and NEOPIXEL_COLOR_ORDER come from the per-board build_flags in
// platformio.ini/boards.ini - do not hardcode a pin/order here, it varies
// per target (e.g. GPIO21/NEO_RGB on ESP32-S3-Zero, GPIO48/NEO_GRB on the
// S3 Super Mini).
Adafruit_NeoPixel led(1, STATUS_LED, NEOPIXEL_COLOR_ORDER + NEO_KHZ800);

char gate_id[16];

// Power/link status indicator LEDs, active-low. RED on at power-up, GREEN on
// once Wi-Fi is connected (RED off); reverts to RED on disconnect. Sole
// owner of these two pins is statusLedTask, below (see led_set()/
// led_flash()) -- do not digitalWrite() them from anywhere else, or the two
// writers will race on the same GPIOs.
const int LED_RED_PIN = 10;
const int LED_GREEN_PIN = 11;

// --- RED/GREEN indicator LED control queue ---
// LedTarget/LedCommand/LedControlMsg, statusLedQueue and statusLedTask
// (defined further down, near ledDiagnosticTask) let any code turn either
// LED on/off "at will" (led_set()) or force one on for a bounded number of
// 100ms ticks then revert to its steady state (led_flash()) -- e.g. to
// signal a one-off event without disturbing the ongoing connection-status
// indication. Named distinctly from the unrelated NeoPixel
// ledQueue/ledDiagnosticTask/ledTaskHandle below (different LEDs, different
// subsystem).
enum class LedTarget { RED, GREEN };
enum class LedCommand { SET_ON, SET_OFF, FLASH };

struct LedControlMsg {
  LedTarget target;
  LedCommand command;
  uint8_t flash_ticks; // only used when command == FLASH; ticks of 100ms each
};

QueueHandle_t statusLedQueue;
TaskHandle_t statusLedTaskHandle = NULL;

// Non-blocking; mirrors ledQueue's xQueueSend(...,0) + logged-overflow
// convention used elsewhere in this file.
bool led_set(LedTarget target, bool on) {
  LedControlMsg msg{target, on ? LedCommand::SET_ON : LedCommand::SET_OFF, 0};
  if (xQueueSend(statusLedQueue, &msg, 0) != pdTRUE) {
    debug_println("[QUEUE OVERFLOW] statusLedQueue full; LED set dropped.");
    return false;
  }
  return true;
}

// Forces `target` to the OPPOSITE of its current steady state for `ticks` *
// 100ms, then reverts to its steady state (whatever led_set() last set it to
// -- may change during the flash, which is picked up correctly since steady
// state and the flash countdown/value are tracked independently per LED in
// statusLedTask). E.g. calling this on RED (currently off) and GREEN
// (currently on) back-to-back swaps them for the flash's duration.
bool led_flash(LedTarget target, uint8_t ticks) {
  LedControlMsg msg{target, LedCommand::FLASH, ticks};
  if (xQueueSend(statusLedQueue, &msg, 0) != pdTRUE) {
    debug_println("[QUEUE OVERFLOW] statusLedQueue full; LED flash dropped.");
    return false;
  }
  return true;
}

// Channel A / channel B -- see board-role.h for how these map to ARM/START/
// GOAL depending on the board's provisioned role. Sourced from the
// analogue dual-EMA detector (see beam-sensor.h and
// ARM_SENSOR_PIN/START_SENSOR_PIN below) rather than a digital edge.
// ARM_SENSOR_PIN/START_SENSOR_PIN themselves come from the per-board
// build_flags in platformio.ini/boards.ini (same convention as STATUS_LED
// above) -- they vary per target.

BoardRole board_role = BoardRole::UNSET;
Cli cli;

int state = 1;

enum EventType { TRIGGER_A, TRIGGER_B, HEARTBEAT };

struct GateEvent {
  EventType type;
  uint64_t tsf_observed;    // Volatile native Wi-Fi TSF timeline
  uint64_t processor_clock; // Monotonic internal 64-bit microsecond uptime
};

/// ISR-to-task handoff for a trigger edge (NETWORK-TIMING-LOG.md "ISR
/// calling esp_wifi_get_tsf_time()" issue) -- carries only what's safe to
/// capture directly in an ISR (the event type and the ISR-safe
/// esp_timer_get_time() processor clock). The real TSF read happens in
/// tsfCaptureTask(), in task context, where blocking briefly on the WiFi
/// driver's internal lock is legal instead of fatal.
struct PendingCapture {
  EventType type;
  uint64_t processor_clock;
};

// Tracking Memory Management
GateEvent last_good_state = {HEARTBEAT, 0, 0};
bool has_initial_baseline = false;

/** --- Dynamic Clock Disciplining Parameters --- */
double clock_alpha = 1.00000000; // Dynamic drift scaling factor
bool alpha_calibrated = false;   // Explicit state flag
const double EMA_ALPHA = 0.10;   // Weights 10% new sample, 90% history
uint64_t cal_prev_tsf = 0;
uint64_t cal_prev_proc = 0;

const uint64_t DRIFT_MARGIN_US = 500;
const uint64_t MIN_PLAUSIBLE_TSF = 300000000;

// --- WS ACK/RETRY (NETWORK-TIMING-LOG.md status section item 1) ---
// Blocking, single-in-flight retry: uploadWorkerTask waits for cerberus's
// ack before dequeuing the next networkQueue event, matching the existing
// one-worker/one-event-at-a-time architecture exactly (no pending-events
// table needed). WS_ACK_TIMEOUT_MS is comfortably above the ~5-15ms typical
// WS round trip measured during rec. 1's bring-up, generous against the
// occasional ~40ms single-event jitter spike also seen there.
const uint32_t WS_ACK_TIMEOUT_MS =
    500; // per-attempt base: resend if no ack within this --
         // raised from 300, 2026-08-04 session 10: end-to-end
         // instrumentation on both boards (cerberus's
         // pending/space, hesperus's [WS-ACK-RECV]) traced every
         // retry in a two-spammer+BT trial and found none were
         // lost -- each was the original ack arriving fine on a
         // round trip (258-458ms) that simply outran the old
         // 240-360ms window under heavy channel congestion
         // (neither board's own processing was slow -- cerberus's
         // dispatch stayed <15ms and wsPumpTask's own
         // wsClient.loop() canary logged only one >50ms stall in
         // the whole trial, uncorrelated with these events -- so
         // the time is most likely spent on-air/at the WiFi MAC
         // layer itself, not in either board's software). 500ms
         // clears the observed 458ms worst case with margin.
const uint32_t WS_ACK_TIMEOUT_JITTER_MS =
    100; // +/- randomised against the base above (see below) --
         // kept at the same ~20% of base as before (was 60/300)
         // -- 2026-08-03 beacon-spam stress testing (session 3)
         // found cerberus's own ack dispatch reliably fast
         // (<15ms) even under heavy congestion, yet acks still
         // failed to arrive back in time; a fixed retry schedule
         // means both gate boards' retries (or retries vs. ARES's
         // own periodic traffic) can lock-step and collide
         // repeatedly on an already-congested channel -- jitter
         // breaks that synchronisation. Deliberately widening the
         // interval under congestion (not shortening it) rather
         // than retrying faster into contention.
const uint8_t WS_MAX_SEND_ATTEMPTS =
    10; // raised from 5, 2026-08-03 session 3 -- extra attempts
        // only ever fire on a timeout, so a healthy send (the
        // normal case) pays none of this cost; only the tail
        // under congestion gets more chances
const uint32_t WS_ACK_WAIT_TICK_MS =
    5; // real vTaskDelay between poll iterations -- see below
const uint32_t WS_ACK_OVERALL_DEADLINE_MS =
    5200; // hard wall-clock cap, applies even while disconnected
          // -- without this a real Wi-Fi outage would park
          // uploadWorkerTask on one stale event for the whole
          // outage while fresh events overflow-drop from
          // networkQueue behind it. Raised from 3200 to 5200,
          // 2026-08-04, alongside WS_ACK_TIMEOUT_MS's 300->500 --
          // scaled proportionally so WS_MAX_SEND_ATTEMPTS's 10
          // attempts still fit inside the deadline (10x500=5000,
          // +200ms margin, same margin the original 3200 left
          // over 10x300=3000) rather than quietly shrinking to
          // ~7 attempts' worth of real-loss recovery depth as a
          // side effect of the per-attempt timeout increase.
          // Real-race events are sparse (one every 20+ seconds)
          // and networkQueue is depth 10, so this remains
          // comfortably inside that budget.

// --- CERBERUS DISCOVERY (mDNS) ---
// Resolved once after Wi-Fi connects and cached for the rest of this boot --
// the venue IP won't change mid-contest, so there's no need to re-query
// mDNS for every event.
IPAddress cerberus_ip;
bool cerberus_ip_valid = false;

// --- PERSISTENT CONNECTION TO CERBERUS (NETWORK-TIMING-LOG.md rec. 1) ---
// One connection opened per boot (see loop()'s g_ready edge-detection) and
// held open across every subsequent event, replacing the old per-event
// HTTPClient connect+POST+close cycle.
//
// .loop() is pumped exclusively by wsPumpTask (below) -- confirmed via
// NETWORK-TIMING-LOG.md's "wsClient.loop() blocking under congestion"
// investigation that a single .loop() call can itself block for several
// seconds (internal TCP reconnect under RF interference), which used to
// silently blow past uploadWorkerTask's own 300ms/2000ms ack/retry bounds
// since those were only checked *between* .loop() calls on the same task.
// wsClient itself isn't safe to call from two tasks without a mutex, so
// every call site (this task's .loop(), and uploadWorkerTask's/loop()'s
// .sendTXT()/.isConnected()/.begin()/.enableHeartbeat()/.onEvent()) goes
// through ws_client_mutex -- see the wsXxxBounded() helpers below.
WebSocketsClient wsClient;

// ws_client_mutex: guards every call into wsClient. wsPumpTask holds it for
// the full duration of each .loop() call (the one place allowed to block for
// however long a stall runs); every other caller takes it with a short
// bounded timeout via the wsXxxBounded() helpers below and treats a failed
// acquisition as "skip this attempt / not connected" rather than blocking --
// the only way that timeout fires is wsPumpTask being mid-stall, which the
// 2026-08-02 stress test showed coincides with a genuine outage anyway, so
// "not connected" is the right conclusion, reached without blocking.
SemaphoreHandle_t ws_client_mutex = xSemaphoreCreateMutex();
const uint32_t WS_MUTEX_SEND_TIMEOUT_MS = 20;
const uint32_t WS_MUTEX_SETUP_TIMEOUT_MS = 50;

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

// Ack-tracking state for the WS retry mechanism, guarded by ws_ack_state_mutex
// as one atomic (received flag, tsf) unit. Used to run on the assumption that
// wsClient.onEvent()'s callback and uploadWorkerTask's wait loop shared a
// task; now that .loop() (and so this callback) runs exclusively on
// wsPumpTask while uploadWorkerTask reads it from a different task, that
// assumption no longer holds -- g_ws_ack_tsf_us in particular is a uint64_t,
// a non-atomic two-word read/write on this 32-bit hardware, so a torn read
// is a real (if narrow-consequence) possibility without the mutex. Use
// wsTakeAckIfReceived()/wsClearAck() (below) rather than touching these
// directly.
bool g_ws_ack_received = false;
uint64_t g_ws_ack_tsf_us =
    0; // last acked tsf_us, compared by value against the pending send
// TSF-timeline (debug_timestamp_ms(), same clock cerberus's own [WS-ACK]
// recv/dispatch/sent timestamps use) capture time of the ack above --
// NETWORK-TIMING-LOG.md "acks not arriving back at hesperus in time"
// issue, added 2026-08-04 once cerberus-side instrumentation ruled out
// AsyncTCP write-completion lag on cerberus's own end: this is the
// matching receive-side timestamp needed to see where the remaining time
// actually goes, directly comparable to cerberus's sent= with no NTP/offset
// needed. debug_timestamp_ms() itself is a cheap esp_wifi_get_tsf_time()
// read, not I/O, so it's safe to call here despite the latency-sensitive
// context below -- unlike a debug_printf/Serial write, which is
// deliberately NOT done in this handler (see its own comment).
uint64_t g_ws_ack_recv_t_ms = 0;
SemaphoreHandle_t ws_ack_state_mutex = xSemaphoreCreateMutex();

/// @brief wsClient.onEvent() handler -- receives cerberus's per-event ack
/// ({"ack_tsf_us": ...}, see net/http-server.h's ws_event_handler() on that
/// repo) so uploadWorkerTask's send-and-wait loop knows an event landed.
/// Also clears a stale ack flag on disconnect so a reconnect mid-wait can't
/// leave old bookkeeping lying around into the next connection.
///
/// Fires synchronously nested inside wsPumpTask's wsClient.loop() call, which
/// holds ws_client_mutex for the duration -- this handler must only ever take
/// ws_ack_state_mutex, never ws_client_mutex (a non-recursive FreeRTOS mutex
/// already held by the same task would deadlock). It has no reason to call
/// back into wsClient, so keep it that way. Deliberately does no logging of
/// its own for the same reason: a debug_printf() here would add a
/// serial_write_mutex take + blocking Serial write directly into
/// wsPumpTask's call stack, the exact latency-critical path the
/// wsClient.loop()-blocking fix exists to protect -- the receive timestamp
/// captured below is cheap to take here, but its logging is deferred to
/// uploadWorkerTask (see wsTakeAckIfReceived()'s call site), which already
/// does its own unhurried Serial I/O for "[WS Worker] Sent/Resent" today.
void wsClientEventHandler(WStype_t type, uint8_t *payload, size_t length) {
  if (type == WStype_TEXT) {
    JsonDocument doc;
    if (deserializeJson(doc, payload, length) == DeserializationError::Ok) {
      uint64_t recv_t_ms = debug_timestamp_ms();
      xSemaphoreTake(ws_ack_state_mutex, portMAX_DELAY);
      g_ws_ack_tsf_us = doc["ack_tsf_us"] | 0ULL;
      g_ws_ack_recv_t_ms = recv_t_ms;
      g_ws_ack_received = true;
      xSemaphoreGive(ws_ack_state_mutex);
    }
  } else if (type == WStype_DISCONNECTED) {
    xSemaphoreTake(ws_ack_state_mutex, portMAX_DELAY);
    g_ws_ack_received = false;
    xSemaphoreGive(ws_ack_state_mutex);
    // Plain RAM increment only, no I/O -- see network-health-stats.h's
    // header comment for why NVS writes never happen at an increment site.
    g_network_health.disconnect_count++;
    g_network_health_dirty = true;
  }
}

/// @brief Bounded-timeout wrapper around wsClient.isConnected(). Returns
/// false (treated as "not connected") if ws_client_mutex can't be acquired
/// within timeout_ms -- the only way that happens is wsPumpTask being
/// mid-stall in .loop(), which the 2026-08-02 stress test showed coincides
/// with a genuine outage, so this isn't a false negative, just an early one.
bool wsIsConnectedBounded(uint32_t timeout_ms) {
  if (xSemaphoreTake(ws_client_mutex, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
    return false;
  }
  bool connected = wsClient.isConnected();
  xSemaphoreGive(ws_client_mutex);
  return connected;
}

/// @brief Bounded-timeout wrapper around wsClient.sendTXT(). Returns false
/// (attempt skipped, caller's own deadline bookkeeping is unaffected) if
/// ws_client_mutex can't be acquired within timeout_ms.
bool wsSendTxtBounded(String &payload, uint32_t timeout_ms) {
  if (xSemaphoreTake(ws_client_mutex, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
    return false;
  }
  wsClient.sendTXT(payload);
  xSemaphoreGive(ws_client_mutex);
  return true;
}

/// @brief Atomically reads the pending ack, if one has arrived, into out_tsf
/// and out_recv_t_ms (the TSF-timeline timestamp wsClientEventHandler()
/// captured it at -- directly comparable to cerberus's own [WS-ACK] sent=).
bool wsTakeAckIfReceived(uint64_t &out_tsf, uint64_t &out_recv_t_ms) {
  bool got = false;
  xSemaphoreTake(ws_ack_state_mutex, portMAX_DELAY);
  if (g_ws_ack_received) {
    out_tsf = g_ws_ack_tsf_us;
    out_recv_t_ms = g_ws_ack_recv_t_ms;
    got = true;
  }
  xSemaphoreGive(ws_ack_state_mutex);
  return got;
}

/// @brief Clears the ack flag (a fresh send starting, or a stale/mismatched
/// ack that wasn't ours).
void wsClearAck() {
  xSemaphoreTake(ws_ack_state_mutex, portMAX_DELAY);
  g_ws_ack_received = false;
  xSemaphoreGive(ws_ack_state_mutex);
}

volatile uint32_t networkq_overflow_count =
    0; ///< Incremented by ISR/timer on dropped networkQueue send
volatile uint32_t triggerCaptureq_overflow_count =
    0; ///< Incremented by ISR on dropped triggerCaptureQueue send

// --- WATCHDOG STATE SHARING VARIABLES ---
volatile bool global_is_stuck_in_syn = false; // Shared flag to notify main loop

// --- FreeRTOS Queues ---
QueueHandle_t networkQueue; // stores network activities - sending notifications
QueueHandle_t ledQueue;     // stored neopixel commands
QueueHandle_t
    triggerCaptureQueue; // ISR-to-tsfCaptureTask handoff, see PendingCapture

// --- FreeRTOS Task Handles (for stack instrumentation) ---
TaskHandle_t ledTaskHandle = NULL;
TaskHandle_t uploadTaskHandle = NULL;
TaskHandle_t wsPumpTaskHandle = NULL;
TaskHandle_t tsfCaptureTaskHandle = NULL;

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
  led.show(); // Initialize to OFF
  LedPattern requested_pattern;
  bool blink_on = false;
  uint32_t last_blink_toggle = millis();

  // Colour for whatever led_base currently is -- SEARCHING blinks (alternates
  // blue/off every ~300ms via blink_on), OFF/READY are steady.
  auto base_color = [&]() -> uint32_t {
    switch (led_base) {
    case LedBase::READY:
      return led.Color(0, 24, 0); // solid green
    case LedBase::SEARCHING:
      return blink_on ? led.Color(0, 0, 32)
                      : led.Color(0, 0, 0); // blinking blue
    default:
      return led.Color(0, 0, 0); // off
    }
  };

  while (1) {
    // Bounded wait, not portMAX_DELAY -- SEARCHING's blink needs to keep
    // alternating even when no trigger/heartbeat flash ever arrives, so
    // this loop can't just block forever waiting for one.
    if (xQueueReceive(ledQueue, &requested_pattern, pdMS_TO_TICKS(300)) ==
        pdPASS) {
      if (requested_pattern == FLASH_TRIGGER_1) {
        led.setPixelColor(0, led.Color(0, 32, 0)); // Bright Green
        led.show();
        vTaskDelay(pdMS_TO_TICKS(50));
      } else if (requested_pattern == FLASH_TRIGGER_2) {
        led.setPixelColor(0, led.Color(32, 0, 0)); // Bright Red
        led.show();
        vTaskDelay(pdMS_TO_TICKS(50));
      } else if (requested_pattern == SHOW_HEARTBEAT) {
        led.setPixelColor(0, led.Color(16, 16, 16)); // Dim Cyan pulse
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

// --- LOW-PRIORITY RED/GREEN STATUS LED TASK ---
// Sole owner of LED_RED_PIN/LED_GREEN_PIN -- see led_set()/led_flash()
// above for the queue producer side. Each 100ms tick, drains every pending
// command (cheap, and lets a SET_ON immediately followed by a FLASH both
// take effect within the same tick rather than one tick apart), then drives
// each pin from its own steady state, forced to the opposite value instead
// for as many ticks as a flash still has remaining.
void statusLedTask(void *pvParameters) {
  // Indexed by LedTarget. Starts matching the power-up default (RED on,
  // GREEN off) that setup()'s own digitalWrite() already applied directly
  // before this task's first tick -- so there's no discontinuity, and no
  // led_set() call is needed at boot just to reach this state.
  bool steady_on[2] = {true, false};
  uint8_t flash_remaining[2] = {0, 0}; // ticks left forced, 0 = not flashing
  bool flash_state[2] = {false,
                         false}; // value forced while flash_remaining > 0

  auto write_pin = [](LedTarget target, bool on) {
    int pin = (target == LedTarget::RED) ? LED_RED_PIN : LED_GREEN_PIN;
    digitalWrite(pin, on ? LOW : HIGH); // active-low
  };

  while (1) {
    LedControlMsg msg;
    while (xQueueReceive(statusLedQueue, &msg, 0) == pdPASS) {
      int i = static_cast<int>(msg.target);
      switch (msg.command) {
      case LedCommand::SET_ON:
        steady_on[i] = true;
        break;
      case LedCommand::SET_OFF:
        steady_on[i] = false;
        break;
      case LedCommand::FLASH:
        // Toggle, not force-on: an LED currently off flashes on, one
        // currently on flashes off -- always visible either way, and
        // flashing two LEDs with differing steady states (e.g. RED
        // off/GREEN on) swaps them for the duration.
        flash_state[i] = !steady_on[i];
        flash_remaining[i] = msg.flash_ticks;
        break;
      }
    }

    for (int i = 0; i < 2; i++) {
      LedTarget target = static_cast<LedTarget>(i);
      if (flash_remaining[i] > 0) {
        write_pin(target, flash_state[i]);
        flash_remaining[i]--;
      } else {
        write_pin(target, steady_on[i]);
      }
    }

    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

// --- REFLECTIVE BEAM-BREAK DETECTION (dual-EMA fast/slow ratio) ---
// See beam-sensor.h for the ExpFilter/BeamSensor algorithm itself (ported
// from legacy/gate-detector's dual-EMA occlusion detector). A hardware
// timer fires at BEAM_SAMPLE_RATE_HZ and wakes beamSampleTask via a task
// notification -- the timer ISR itself does no ADC reads or float math,
// matching this file's existing ISR-minimalism convention (see
// tsfCaptureTask()'s comment below on why esp_wifi_get_tsf_time() stays out
// of ISR context; the same reasoning motivates keeping this timer ISR as
// short as possible). beamSampleTask runs in ordinary task context (woken
// by, not running inside, the timer ISR), so pushing to triggerCaptureQueue
// uses the plain xQueueSend(), not the ISR-only xQueueSendFromISR() the old
// GPIO-interrupt path needed.
//
// Channel mapping matches board-role.h's ARM/START split:
// ARM_SENSOR_PIN -> TRIGGER_A, START_SENSOR_PIN -> TRIGGER_B.
BeamSensor armSensor{"ARM", ARM_SENSOR_PIN};
BeamSensor startSensor{"START", START_SENSOR_PIN};

hw_timer_t *beamSampleTimer = NULL;
TaskHandle_t beamSampleTaskHandle = NULL;

void IRAM_ATTR onBeamSampleTimer() {
  BaseType_t xHigherPriorityTaskWoken = pdFALSE;
  vTaskNotifyGiveFromISR(beamSampleTaskHandle, &xHigherPriorityTaskWoken);
  if (xHigherPriorityTaskWoken) {
    portYIELD_FROM_ISR();
  }
}

// esp_wifi_get_tsf_time() is NOT called here -- see PendingCapture and
// tsfCaptureTask(): it takes an internal WiFi-driver lock that can block,
// which is illegal (and, under heavy WiFi-stack load, fatal -- an
// Interrupt WDT panic, confirmed via crash backtrace 2026-08-03) from ISR
// context. That constraint is about blocking generally, not specifically
// ISR context, so it applies here too even though beamSampleTask is a
// normal task: only the esp_timer_get_time() processor clock is captured
// here; the real TSF read happens one task-context hop later, in
// tsfCaptureTask(), as close to this instant as the scheduler allows.
void dispatchBeamTrigger(EventType type) {
  PendingCapture pc;
  pc.type = type;
  pc.processor_clock = esp_timer_get_time();
  if (xQueueSend(triggerCaptureQueue, &pc, 0) != pdTRUE) {
    triggerCaptureq_overflow_count++;
  }
}

void beamSampleTask(void *parameter) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    if (armSensor.update(analogRead(armSensor.pin))) {
      dispatchBeamTrigger(TRIGGER_A);
    }
    if (startSensor.update(analogRead(startSensor.pin))) {
      dispatchBeamTrigger(TRIGGER_B);
    }
  }
}

/// @brief Sole task-context consumer of triggerCaptureQueue -- the other
/// half of the ISR-safety fix described above.
/// Performs the actual esp_wifi_get_tsf_time() read (safe here: blocking
/// briefly on the WiFi driver's internal lock is legal in task context)
/// as close to the original ISR instant as the scheduler allows, then hands
/// the completed GateEvent to networkQueue exactly as the ISRs used to do
/// directly. Highest priority in the app (above uploadWorkerTask/wsPumpTask)
/// so it's scheduled immediately off the ISR's portYIELD_FROM_ISR(), keeping
/// the added latency to a task-switch (typically low microseconds) rather
/// than trading precision away for safety.
void tsfCaptureTask(void *pvParameters) {
  PendingCapture pc;
  while (1) {
    if (xQueueReceive(triggerCaptureQueue, &pc, portMAX_DELAY) == pdPASS) {
      GateEvent ev;
      ev.type = pc.type;
      ev.tsf_observed = esp_wifi_get_tsf_time(WIFI_IF_STA);
      ev.processor_clock = pc.processor_clock;
      if (xQueueSend(networkQueue, &ev, 0) != pdTRUE) {
        networkq_overflow_count++;
      }
    }
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

  // Serial.printf("[STACK] LED min free: %u bytes | Upload min free: %u
  // bytes\n",
  //               uxTaskGetStackHighWaterMark(ledTaskHandle) * 4,
  //               uxTaskGetStackHighWaterMark(uploadTaskHandle) * 4);
}

// --- WS SOCKET PUMP TASK ---
// Sole caller of wsClient.loop() (see the design note above wsClient's
// declaration). Deliberately allowed to block on ws_client_mutex for as long
// as a single .loop() call takes -- that's the point: whatever it stalls on,
// uploadWorkerTask's own ack-wait deadline logic keeps running on schedule
// on its own task instead of waiting on this one.
void wsPumpTask(void *pvParameters) {
  while (1) {
    xSemaphoreTake(ws_client_mutex, portMAX_DELAY);
    // Kept as a permanent low-cost canary (only logs above 50ms) rather than
    // stripped once-fixed -- see NETWORK-TIMING-LOG.md's "wsClient.loop()
    // blocking under congestion" issue for why this is worth watching for
    // long-term, not just during the original investigation.
    uint64_t loop_call_start = esp_timer_get_time();
    wsClient.loop();
    uint64_t loop_call_us = esp_timer_get_time() - loop_call_start;
    if (loop_call_us > 50000) {
      uint32_t stall_ms = loop_call_us / 1000;
      // Plain RAM increments only, no I/O -- still holding ws_client_mutex
      // here, see network-health-stats.h's header comment for why the NVS
      // write itself is deferred to loop().
      g_network_health.stall_count++;
      if (stall_ms > g_network_health.max_stall_ms) {
        g_network_health.max_stall_ms = stall_ms;
      }
      g_network_health_dirty = true;
      debug_printf("[WS Pump] wsClient.loop() blocked %llums (wifi_status=%d, "
                   "ws_connected=%d)\n",
                   loop_call_us / 1000, WiFi.status(), wsClient.isConnected());
    }
    xSemaphoreGive(ws_client_mutex);
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// --- CORE NETWORK WORKER TASK WITH DISCIPLINED OSCILLATOR MATH ---
void uploadWorkerTask(void *pvParameters) {
  GateEvent current_ev;

  while (1) {
    // wsPumpTask now owns pumping wsClient.loop() -- this task only touches
    // wsClient through the bounded wsXxxBounded() helpers below. The 10ms
    // bound here (vs. portMAX_DELAY) is kept anyway: nothing else needs it
    // today, but there's no reason to widen this wait as a side effect of an
    // unrelated change.
    if (xQueueReceive(networkQueue, &current_ev, pdMS_TO_TICKS(10)) == pdPASS) {
      // TODO: Serial logging is unavailable in field deployment. Future
      // options:
      //   - RGB LED pattern to signal overflow (extend LedPattern enum)
      //   - Write overflow count to SPIFFS for post-session retrieval
      //   - Append &overflow=N to next HTTP GET to notify server
      uint32_t drops = networkq_overflow_count;
      if (drops > 0) {
        networkq_overflow_count = 0;
        debug_printf("[QUEUE OVERFLOW] %lu networkQueue event(s) dropped.\n",
                     drops);
      }

      LedPattern pattern =
          (current_ev.type == TRIGGER_A) ? FLASH_TRIGGER_1 : FLASH_TRIGGER_2;
      if (current_ev.type == HEARTBEAT) {
        pattern = SHOW_HEARTBEAT;
      }
      if (xQueueSend(ledQueue, &pattern, 0) != pdTRUE) {
        debug_println("[QUEUE OVERFLOW] ledQueue full; LED feedback dropped.");
      }

      uint64_t tsf_to_transmit = current_ev.tsf_observed;
      bool trust_observed_tsf = false;
      String clock_mode = "TSF";

      // --- TIMELINE SANITY AUDIT ENGINE ---
      if (current_ev.tsf_observed != 0 && has_initial_baseline) {
        uint64_t elapsed_processor_time =
            current_ev.processor_clock - last_good_state.processor_clock;
        uint64_t expected_tsf_delta =
            (uint64_t)(elapsed_processor_time * clock_alpha);
        uint64_t expected_tsf =
            last_good_state.tsf_observed + expected_tsf_delta;

        uint64_t drift_variance =
            (current_ev.tsf_observed > expected_tsf)
                ? (current_ev.tsf_observed - expected_tsf)
                : (expected_tsf - current_ev.tsf_observed);

        if (drift_variance <= DRIFT_MARGIN_US) {
          trust_observed_tsf = true;
          consecutive_audit_failures = 0;

          if (cal_prev_tsf != 0 &&
              (current_ev.processor_clock - cal_prev_proc) > 4000000) {
            double actual_tsf_delta =
                (double)(current_ev.tsf_observed - cal_prev_tsf);
            double actual_proc_delta =
                (double)(current_ev.processor_clock - cal_prev_proc);
            double instant_alpha = actual_tsf_delta / actual_proc_delta;
            instant_alpha = constrain(instant_alpha, 0.9990, 1.0010);

            if (!alpha_calibrated) {
              clock_alpha = instant_alpha;
              alpha_calibrated = true;
            } else {
              clock_alpha = (EMA_ALPHA * instant_alpha) +
                            ((1.0 - EMA_ALPHA) * clock_alpha);
            }
            // Serial.printf("[CALIBRATION] Dynamic Alpha Stabilized: %.8f\n",
            // clock_alpha);
          }

          cal_prev_tsf = current_ev.tsf_observed;
          cal_prev_proc = current_ev.processor_clock;
        } else {
          debug_printf("[AUDIT ALERT] Temporal Disruption! Drift: %llu us. "
                       "Rejecting stack value.\n",
                       drift_variance);
          consecutive_audit_failures++;
          if (consecutive_audit_failures >= 5) {
            debug_println("[AUDIT RECOVERY] Jitter is persistent. Accepting "
                          "new router baseline shift.");
            trust_observed_tsf = true;
            consecutive_audit_failures = 0;
            cal_prev_tsf = current_ev.tsf_observed;
            cal_prev_proc = current_ev.processor_clock;
          }
        }
      } else if (current_ev.tsf_observed != 0 && !has_initial_baseline) {
        // NETWORK-TIMING-LOG.md recommendation 9: trust the first TSF
        // reading as soon as the radio is actually associated, rather than
        // gating on the TSF value's absolute magnitude. esp_wifi_get_tsf_time()
        // tracks time since the AP's own TSF epoch, not since hesperus
        // associated -- a magnitude gate assumes a small value means "Wi-Fi
        // stack not synced yet", but any AP radio interruption resets it for
        // every station, and the old MIN_PLAUSIBLE_TSF=300000000 threshold
        // then silently dropped every trigger for up to 5 minutes (#10),
        // regardless of how quickly hesperus itself reconnected.
        if (WiFi.status() == WL_CONNECTED) {
          trust_observed_tsf = true;
          has_initial_baseline = true;
          cal_prev_tsf = current_ev.tsf_observed;
          cal_prev_proc = current_ev.processor_clock;
          debug_printf(
              "[INITIALIZED] Valid Baseline Coordinates Locked: %llu\n",
              current_ev.tsf_observed);
        } else {
          debug_printf("[PLAUSIBILITY REJECT] Wi-Fi not connected. Baseline "
                       "not established.\n");
        }
      }

      // --- EXECUTION ENGINE CORE DETERMINATION ---
      if (trust_observed_tsf) {
        last_good_state = current_ev;
        clock_mode = "TSF";
        global_is_stuck_in_syn = false; // Clear state flag
      } else if (has_initial_baseline) {
        uint64_t elapsed_processor_time =
            current_ev.processor_clock - last_good_state.processor_clock;
        uint64_t disciplined_delta =
            (uint64_t)(elapsed_processor_time * clock_alpha);

        tsf_to_transmit = last_good_state.tsf_observed + disciplined_delta;
        clock_mode = "SYN";
        global_is_stuck_in_syn =
            true; // Alert main loop that we are using fallback tracking
        debug_printf("[DISCIPLINED SYN] Extrapolated TSF: %llu (Alpha: %.8f)\n",
                     tsf_to_transmit, clock_alpha);

        last_good_state.type = current_ev.type;
        last_good_state.tsf_observed = tsf_to_transmit;
        last_good_state.processor_clock = current_ev.processor_clock;
      } else {
        debug_println("[CRITICAL DROP] Baseline missing or un-synchronized. "
                      "Packet dropped.");
        continue;
      }

      // --- HTTP TRANSMISSION ENGINE ---
      // HEARTBEAT is only used above for local clock discipline/LED
      // feedback -- cerberus's /api/event has no HEARTBEAT event type
      // (see board-role.h's header comment), so it's never sent.
      if (current_ev.type == HEARTBEAT) {
        continue;
      }

      const char *event_name =
          board_event_name(board_role, current_ev.type == TRIGGER_A);
      if (event_name == nullptr) {
        debug_println("[Async Worker] Role not set -- run `role start` or "
                      "`role goal`. Event dropped.");
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
        debug_println(
            "[WS Worker] cerberus.local not resolved yet. Event dropped.");
        continue;
      }

      JsonDocument doc;
      doc["gate_id"] = gate_id;
      doc["event"] = event_name;
      doc["tsf_us"] = tsf_to_transmit;
      doc["gate_us"] = current_ev.processor_clock;
      doc["rssi"] = WiFi.RSSI();
      String payload;
      serializeJson(doc, payload);

      // --- WS TRANSMISSION, WITH BOUNDED RETRY-ON-LOST-ACK ---
      // Sends the exact frozen payload built above; a retry NEVER
      // re-derives tsf_to_transmit, since re-running the TSF-audit engine
      // a second time for what should be one logical event would both give
      // a different answer and incorrectly re-mutate clock_alpha/
      // cal_prev_*/last_good_state again. Matches the pre-existing
      // "drop, no requeue" behaviour for the down/unresolved cases above --
      // this only adds recovery for a message lost after this point.
      // Blocks uploadWorkerTask (matching the existing one-worker/
      // one-event-at-a-time model, no new pending-events table) for up to
      // WS_ACK_OVERALL_DEADLINE_MS -- see net/http-server.h's ws_event_
      // handler() on cerberus for the ack this waits on, and
      // net/gate-event-dedup.h for why a resend after a lost ack is safe
      // (recognised as a duplicate there, not double-processed).
      bool sent = wsIsConnectedBounded(WS_MUTEX_SEND_TIMEOUT_MS);
      if (sent) {
        wsClearAck();
        wsSendTxtBounded(payload, WS_MUTEX_SEND_TIMEOUT_MS);
        debug_printf("[WS Worker] Sent %s (%s), attempt 1.\n", event_name,
                     clock_mode.c_str());

        uint64_t expected_ack = tsf_to_transmit;
        uint8_t attempt = 1;
        uint32_t attempt_start = millis();
        uint32_t wait_start = attempt_start;
        bool acked = false;
        // Jittered per-attempt timeout (WS_ACK_TIMEOUT_MS +/-
        // WS_ACK_TIMEOUT_JITTER_MS)
        // -- re-rolled on every attempt, see WS_ACK_TIMEOUT_JITTER_MS above.
        uint32_t attempt_timeout_ms =
            WS_ACK_TIMEOUT_MS + random(-(int32_t)WS_ACK_TIMEOUT_JITTER_MS,
                                       (int32_t)WS_ACK_TIMEOUT_JITTER_MS + 1);

        // No wsClient.loop() call here -- wsPumpTask pumps the socket on its
        // own task now, so this loop's millis()-based deadline checks below
        // run on their own schedule regardless of how long a stall in that
        // pump takes. This is the actual fix for the defeated-deadline bug:
        // previously a single wsClient.loop() call in this loop could itself
        // block for seconds, so the checks below never got a chance to fire
        // on time.
        while (!acked) {
          uint64_t acked_tsf;
          uint64_t acked_recv_t_ms;
          if (wsTakeAckIfReceived(acked_tsf, acked_recv_t_ms)) {
            if (acked_tsf == expected_ack) {
              acked = true;
#if WS_EVENT_LOG_DETAIL
              debug_printf("[WS-ACK-RECV] tsf_us=%llu recv_t=%llu attempt=%u\n",
                           acked_tsf, acked_recv_t_ms, attempt);
#endif
              break;
            }
            wsClearAck(); // stale/mismatched ack -- keep waiting for ours
          }

          uint32_t now = millis();
          if (now - wait_start > WS_ACK_OVERALL_DEADLINE_MS) {
            g_network_health.drop_ack_deadline++;
            g_network_health_dirty = true;
            debug_println("[WS Worker] Event dropped after ack deadline.");
            break;
          }
          if (now - attempt_start > attempt_timeout_ms) {
            if (attempt >= WS_MAX_SEND_ATTEMPTS) {
              g_network_health.drop_max_retries++;
              g_network_health_dirty = true;
              debug_println("[WS Worker] Event dropped after max retries.");
              break;
            }
            if (wsIsConnectedBounded(WS_MUTEX_SEND_TIMEOUT_MS) &&
                wsSendTxtBounded(payload, WS_MUTEX_SEND_TIMEOUT_MS)) {
              attempt++;
              debug_printf("[WS Worker] Resent %s (%s), attempt %u.\n",
                           event_name, clock_mode.c_str(), attempt);
            }
            // else: ws_client_mutex busy (wsPumpTask mid-stall) or link
            // down -- this resend is skipped, but the deadline checks above
            // keep running on schedule regardless.
            attempt_start = now;
            attempt_timeout_ms = WS_ACK_TIMEOUT_MS +
                                 random(-(int32_t)WS_ACK_TIMEOUT_JITTER_MS,
                                        (int32_t)WS_ACK_TIMEOUT_JITTER_MS + 1);
          }
          // Real block, not a spin -- uploadWorkerTask runs at a higher
          // FreeRTOS priority than ledDiagnosticTask/loop() on the same
          // core, so without this LED feedback/cli.poll()/the Wi-Fi
          // watchdogs would starve for the whole wait.
          vTaskDelay(pdMS_TO_TICKS(WS_ACK_WAIT_TICK_MS));
        }
      } else {
        g_network_health.drop_link_down++;
        g_network_health_dirty = true;
        debug_println("[WS Worker] Link down. Event dropped.");
      }

      // --- PATCH 1: NETWORK RECEIPT ESCAPE HATCH ---
      // Only trigger TSF re-entry once we've actually sent in SYN mode --
      // "sent" here still means only "had connectivity and attempted at
      // least one send" (evaluated from the first attempt only, above),
      // independent of whether an ack ultimately arrived -- this escape
      // hatch is about local clock resync given Wi-Fi is up, unrelated to
      // delivery confirmation.
      if (sent && clock_mode == "SYN") {
        uint64_t raw_tsf_check = esp_wifi_get_tsf_time(WIFI_IF_STA);
        if (raw_tsf_check > MIN_PLAUSIBLE_TSF) {
          debug_println("[ESCAPE HATCH] Server confirmed online. Attempting "
                        "TSF re-entry.");
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

  // Safe default for the brief window before statusLedTask's first 100ms
  // tick (it isn't created until further down in setup()) -- statusLedTask
  // itself starts with the same RED-on/GREEN-off state, so there's no
  // discontinuity once it takes over as sole owner of these two pins.
  pinMode(LED_RED_PIN, OUTPUT);
  pinMode(LED_GREEN_PIN, OUTPUT);
  digitalWrite(LED_RED_PIN, LOW);    // active-low -- RED on at power-up
  digitalWrite(LED_GREEN_PIN, HIGH); // GREEN off at power-up

#ifdef NEOPIXEL_POWER_PIN
  // Only defined for boards where the onboard NeoPixel's power rail is
  // gated by a separate GPIO rather than always-on (e.g. the QT Py ESP32
  // Pico's GPIO8/"NEOPIX_PWR") -- must be driven HIGH before `led`
  // (Adafruit_NeoPixel) is used below, or the pixel never lights at all.
  pinMode(NEOPIXEL_POWER_PIN, OUTPUT);
  digitalWrite(NEOPIXEL_POWER_PIN, HIGH);
#endif

  network_health_load(); // before wsPumpTask/uploadWorkerTask exist -- nothing
                         // can increment these yet

  strlcpy(gate_id, identifyBoard(), sizeof(gate_id));

  // Analogue front-end for the dual-EMA reflective/occlusion detector (see
  // beam-sensor.h) -- must be configured before beam_sensor_seed_from_adc()
  // runs, below, once triggerCaptureQueue exists.
  analogReadResolution(12);
  analogSetPinAttenuation(START_SENSOR_PIN, ADC_11db);
  analogSetPinAttenuation(ARM_SENSOR_PIN, ADC_11db);

  // Hardware jumper is authoritative -- re-read and re-save on every boot,
  // so a `role` command from a previous session never lingers past a power
  // cycle once the jumper says otherwise.
  board_role = board_role_from_jumper();
  board_role_save(board_role);
  debug_printf("[SYSTEM] Board role (jumper): %s\n",
               board_role_name(board_role));

  cli.begin(PROVISIONING_COMMANDS, PROVISIONING_COMMAND_COUNT);

  networkQueue = xQueueCreate(10, sizeof(GateEvent));
  ledQueue = xQueueCreate(5, sizeof(LedPattern));
  statusLedQueue =
      xQueueCreate(5, sizeof(LedControlMsg)); // depth mirrors ledQueue
  triggerCaptureQueue = xQueueCreate(10, sizeof(PendingCapture));

#if HAS_HTTP
  debug_http_server_register_status_sources(gate_id, networkQueue,
                                            &networkq_overflow_count);
#endif

  if (wifi_credentials_load(stored_ssid, sizeof(stored_ssid), stored_pass,
                            sizeof(stored_pass))) {
    connect_ssid = stored_ssid;
    connect_pass = stored_pass;
    debug_println("[SYSTEM] Using saved Wi-Fi credentials from NVS");
  }

  WiFi.persistent(false);
  WiFi.disconnect(true);
  WiFi.begin(connect_ssid, connect_pass);
  // NETWORK-TIMING-LOG.md "Wi-Fi power-save vs. battery budget" --
  // testing WIFI_PS_MIN_MODEM (wakes every beacon interval, ~100ms
  // typically) 2026-08-04 against the WIFI_PS_NONE baseline (measured
  // ~110mA), now that persistent connections exist -- the original
  // problem WIFI_PS_NONE was adopted to fix (WIFI_PS_MAX_MODEM's 2.9s
  // wake-up latency spike, radio asleep between sparse ARM/START/GOAL
  // events) was under the old per-event-connection model; MIN_MODEM's much
  // shorter, more frequent wake cycle is expected to avoid that, but
  // hasn't been measured on this hardware. Swap back to WIFI_PS_NONE
  // (matches cerberus's own net/wifi-manager.h) if current-draw/latency
  // testing doesn't support keeping this.
  esp_wifi_set_ps(WIFI_PS_NONE);

  TimerHandle_t hbTimer = xTimerCreate("HB_Timer", pdMS_TO_TICKS(5147), pdTRUE,
                                       (void *)0, heartbeatTimerCallback);
  if (hbTimer != NULL) {
    xTimerStart(hbTimer, 0);
  }

  xTaskCreatePinnedToCore(ledDiagnosticTask, "LED_Task", 2048, NULL, 1,
                          &ledTaskHandle, 1);
  xTaskCreatePinnedToCore(statusLedTask, "StatusLed_Task", 2048, NULL, 1,
                          &statusLedTaskHandle, 1);
  xTaskCreatePinnedToCore(wsPumpTask, "WsPump", 8192, NULL, 2,
                          &wsPumpTaskHandle, 1);
  xTaskCreatePinnedToCore(uploadWorkerTask, "UploadWorker", 8192, NULL, 2,
                          &uploadTaskHandle, 1);
  // Highest priority in the app -- see tsfCaptureTask()'s own comment for
  // why: it needs to be scheduled immediately off the ISRs'
  // portYIELD_FROM_ISR() to keep the TSF read as close to the true trigger
  // instant as possible.
  xTaskCreatePinnedToCore(tsfCaptureTask, "TsfCapture", 4096, NULL, 3,
                          &tsfCaptureTaskHandle, 1);

  // Seed both dual-EMA channels from real ADC samples (see beam-sensor.h)
  // now that triggerCaptureQueue exists -- beamSampleTask, started right
  // after, pushes into it. A dark seed means the sensor is unlit/miswired/
  // obstructed at boot; flash RED so a technician without a laptop at the
  // gate still gets a signal (a normal seed logs quietly and flashes
  // nothing, so it's never mistaken for a race-timing event).
  bool arm_seed_ok = beam_sensor_seed_from_adc(armSensor);
  bool start_seed_ok = beam_sensor_seed_from_adc(startSensor);
  if (!arm_seed_ok || !start_seed_ok) {
    led_flash(LedTarget::RED, 10); // 10 * 100ms = 1s
  }

  // beamSampleTaskHandle must exist before the timer ISR can notify it, so
  // the task is created first; onBeamSampleTimer() only starts firing once
  // timerAlarmEnable() runs below, by which point the task is already
  // parked on ulTaskNotifyTake() waiting for it.
  xTaskCreatePinnedToCore(beamSampleTask, "BeamSample", 4096, NULL, 2,
                          &beamSampleTaskHandle, 0);

  // Hardware timer 0, divider 80 against the 80MHz APB clock -> 1MHz tick
  // (1us/tick), independent of the FreeRTOS 1000Hz tick (which can't
  // express BEAM_SAMPLE_RATE_HZ if it isn't an integer divisor of 1000).
  beamSampleTimer = timerBegin(0, 80, true);
  timerAttachInterrupt(beamSampleTimer, &onBeamSampleTimer, true);
  timerAlarmWrite(beamSampleTimer, 1000000 / BEAM_SAMPLE_RATE_HZ, true);
  timerAlarmEnable(beamSampleTimer);
}

// --- MAIN LOOP EXECUTION TASK ---
void loop() {
  static uint32_t last_connected_time = millis();
  static uint32_t last_reconnect_attempt = millis();
  static uint32_t last_cerberus_attempt = 0;
  static uint32_t syn_mode_start_time = 0; // Tracks duration of fallback timing
  static bool was_connected = false;
  static bool ws_was_ready =
      false; // g_ready edge-detection, opens wsClient once per readiness

  uint32_t current_time = millis();

  // Ahead of the reboot-capable watchdog below, so a dirty counter from a
  // stall/drop this same iteration is persisted before any possible restart.
  network_health_flush_if_dirty();

  // --- PATCH 2: EXTENDED SYN-MODE WATCHDOG ---
  // If the gate is trapped in synthetic time for >10 s, the Wi-Fi
  // recovery mechanism has stalled. Reboot to force a clean restart.
  if (global_is_stuck_in_syn) {
    if (syn_mode_start_time == 0) {
      syn_mode_start_time = current_time; // Start the clock on the breakdown
    } else if (current_time - syn_mode_start_time > 10000) {
      // If the gate is stuck calculating synthetic time for more than 10
      // seconds, the hardware clock register has hit an overflow boundary.
      // Clear stacks and reboot.
      debug_println("[ROLLOVER FAULT] Trapped in synthetic time loop for 10s. "
                    "Forcing hardware reboot...");
      delay(500);
      ESP.restart();
    }
  } else {
    syn_mode_start_time = 0; // Reset watchdog when operations are normal
  }

  if (WiFi.status() == WL_CONNECTED) {
    if (!was_connected) {
      debug_println("[NETWORK] Link Active! IP: " + WiFi.localIP().toString());
      if (!MDNS.begin(gate_id)) {
        debug_println("[MDNS] failed to start");
      }
#if HAS_HTTP
      MDNS.addService("http", "tcp", 80);
      debug_http_server_init(); // idempotent route registration; .begin()
                                // re-issued every edge -- see
                                // debug-http-server.h's AsyncServer::begin()
                                // note
#endif
      cerberus_ip_valid = false; // re-resolve on this connection
      last_cerberus_attempt = 0; // force an immediate resolve attempt below
      was_connected = true;
      led_set(LedTarget::RED, false); // link up: RED off, GREEN on
      led_set(LedTarget::GREEN, true);
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
        // Bounded acquisition -- if wsPumpTask is mid-stall holding
        // ws_client_mutex, ws_was_ready stays false and this is retried on
        // the next loop() tick (10ms later, below) rather than blocking
        // loop() itself for the stall's duration.
        if (xSemaphoreTake(ws_client_mutex,
                           pdMS_TO_TICKS(WS_MUTEX_SETUP_TIMEOUT_MS)) ==
            pdTRUE) {
          wsClient.begin(cerberus_ip.toString(), 80, "/ws");
          wsClient.enableHeartbeat(
              5000, 3000, 2); // 5s ping, 3s pong timeout, disconnect after 2
                              // misses (widened to 5000,3 2026-08-05, reverted
                              // same day -- session 14 showed it makes the
                              // MIN_MODEM stall/disconnect cascade dramatically
                              // worse, not better. NETWORK-TIMING-LOG.md)
          wsClient.onEvent(
              wsClientEventHandler); // receives cerberus's per-event ack
          xSemaphoreGive(ws_client_mutex);
          ws_was_ready = true;
        }
      }
    }
  } else {
    if (was_connected) {
      led_set(LedTarget::RED, true); // link down: RED on, GREEN off
      led_set(LedTarget::GREEN, false);
    }
    was_connected = false;
    cerberus_ip_valid = false;
    led_base = LedBase::OFF;
    g_ready = false;
    // Force a fresh begin() next time readiness is reached -- cerberus may
    // resolve to a different IP after this WiFi drop/rejoin.
    ws_was_ready = false;

    if (current_time - last_connected_time > 15000) {
      debug_println("[WATCHDOG FAULT] Wi-Fi link dead for 15s. Smashing "
                    "network stack...");
      WiFi.disconnect(true, true);
      delay(500);
      debug_println(
          "[WATCHDOG RECOVERY] Re-initializing hardware radio interface...");
      WiFi.begin(connect_ssid, connect_pass);
      last_connected_time = current_time;
      last_reconnect_attempt = current_time;
    } else if (current_time - last_reconnect_attempt > 3000) {
      Serial.print(".");
      last_reconnect_attempt = current_time;
    }
  }

  cli.poll();

  delay(10);
}