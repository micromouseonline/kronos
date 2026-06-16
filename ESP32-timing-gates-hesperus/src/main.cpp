#include <Adafruit_NeoPixel.h>
#include <HTTPClient.h>
#include <WiFi.h>

#include "boards.h"   // contains the board MAC addresses to look up the identifiers
#include "secrets.h"  // these are the network credentials neede to connect to the AP

// --- Hardware Pin Configuration ---
#define PIN_LED_RGB 21  // Built-in RGB LED on GPIO 21
Adafruit_NeoPixel led(1, PIN_LED_RGB, NEO_RGB + NEO_KHZ800);

String gate_id;

const int LED_PIN = 2;
const int GATE_PIN = 1;

int state = 1;

enum EventType { TRIGGER_A,
                 TRIGGER_B,
                 HEARTBEAT };

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

static int consecutive_audit_failures = 0;

// --- WATCHDOG STATE SHARING VARIABLES ---
volatile bool global_is_stuck_in_syn = false;  // Shared flag to notify main loop

// --- FreeRTOS Queues ---
QueueHandle_t networkQueue;  // stores network activities - sending notifications
QueueHandle_t ledQueue;      // stored neopixel commands

enum LedPattern { FLASH_TRIGGER_1,
                  FLASH_TRIGGER_2,
                  SHOW_HEARTBEAT };

// --- LOW-PRIORITY DIAGNOSTIC LED TASK ---
void ledDiagnosticTask(void *pvParameters) {
    led.begin();
    led.show();  // Initialize to OFF
    LedPattern requested_pattern;
    while (1) {
        if (xQueueReceive(ledQueue, &requested_pattern, portMAX_DELAY) == pdPASS) {
            if (requested_pattern == FLASH_TRIGGER_1) {
                led.setPixelColor(0, led.Color(0, 32, 0));  // Bright Green
                led.show();
                vTaskDelay(pdMS_TO_TICKS(50));
                led.setPixelColor(0, led.Color(0, 0, 0));
                led.show();
            } else if (requested_pattern == FLASH_TRIGGER_2) {
                led.setPixelColor(0, led.Color(0, 0, 32));  // Bright Blue
                led.show();
                vTaskDelay(pdMS_TO_TICKS(50));
                led.setPixelColor(0, led.Color(0, 0, 0));
                led.show();
            } else if (requested_pattern == SHOW_HEARTBEAT) {
                led.setPixelColor(0, led.Color(16, 16, 16));  // Dim Cyan pulse
                led.show();
                vTaskDelay(pdMS_TO_TICKS(30));
                led.setPixelColor(0, led.Color(0, 0, 0));
                led.show();
            }
        }
    }
}

extern "C" uint64_t esp_wifi_get_tsf_time(wifi_interface_t interface);

// --- HARDWARE INTERRUPT SERVICE ROUTINES (ISRs) WITH DEBOUNCE ---
void IRAM_ATTR handleSensor1() {
    static uint64_t last_interrupt_time = 0;
    uint64_t current_time = esp_timer_get_time();

    if (current_time - last_interrupt_time < 20000) {
        return;
    }
    last_interrupt_time = current_time;

    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    GateEvent ev;
    ev.type = TRIGGER_A;
    ev.tsf_observed = esp_wifi_get_tsf_time(WIFI_IF_STA);
    ev.processor_clock = current_time;
    xQueueSendFromISR(networkQueue, &ev, &xHigherPriorityTaskWoken);
    if (xHigherPriorityTaskWoken) {
        portYIELD_FROM_ISR();
    }
}

void heartbeatTimerCallback(TimerHandle_t xTimer) {
    GateEvent hb;
    hb.type = HEARTBEAT;
    hb.tsf_observed = esp_wifi_get_tsf_time(WIFI_IF_STA);
    hb.processor_clock = esp_timer_get_time();

    xQueueSend(networkQueue, &hb, 0);
}

// --- CORE NETWORK WORKER TASK WITH DISCIPLINED OSCILLATOR MATH ---
void uploadWorkerTask(void *pvParameters) {
    GateEvent current_ev;
    HTTPClient http;

    while (1) {
        if (xQueueReceive(networkQueue, &current_ev, portMAX_DELAY) == pdPASS) {

            LedPattern pattern = (current_ev.type == TRIGGER_A) ? FLASH_TRIGGER_1 : FLASH_TRIGGER_2;
            if (current_ev.type == HEARTBEAT) pattern = SHOW_HEARTBEAT;
            xQueueSend(ledQueue, &pattern, 0);

            uint64_t tsf_to_transmit = current_ev.tsf_observed;
            bool trust_observed_tsf = false;
            String clock_mode = "TSF";
            String type_str = "TRA";

            if (current_ev.type == TRIGGER_B) type_str = "TRB";
            if (current_ev.type == HEARTBEAT) type_str = "HB";

            // --- TIMELINE SANITY AUDIT ENGINE ---
            if (current_ev.tsf_observed != 0 && has_initial_baseline) {
                uint64_t elapsed_processor_time = current_ev.processor_clock - last_good_state.processor_clock;
                uint64_t expected_tsf_delta = (uint64_t) (elapsed_processor_time * clock_alpha);
                uint64_t expected_tsf = last_good_state.tsf_observed + expected_tsf_delta;

                uint64_t drift_variance = (current_ev.tsf_observed > expected_tsf) ? (current_ev.tsf_observed - expected_tsf) : (expected_tsf - current_ev.tsf_observed);

                if (drift_variance <= DRIFT_MARGIN_US) {
                    trust_observed_tsf = true;
                    consecutive_audit_failures = 0;

                    if (cal_prev_tsf != 0 && (current_ev.processor_clock - cal_prev_proc) > 4000000) {
                        double actual_tsf_delta = (double) (current_ev.tsf_observed - cal_prev_tsf);
                        double actual_proc_delta = (double) (current_ev.processor_clock - cal_prev_proc);
                        double instant_alpha = actual_tsf_delta / actual_proc_delta;

                        if (!alpha_calibrated) {
                            clock_alpha = instant_alpha;
                            alpha_calibrated = true;
                        } else {
                            clock_alpha = (EMA_ALPHA * instant_alpha) + ((1.0 - EMA_ALPHA) * clock_alpha);
                        }
                        Serial.printf("[CALIBRATION] Dynamic Alpha Stabilized: %.8f\n", clock_alpha);
                    }

                    cal_prev_tsf = current_ev.tsf_observed;
                    cal_prev_proc = current_ev.processor_clock;
                } else {
                    Serial.printf("[AUDIT ALERT] Temporal Disruption! Drift: %llu us. Rejecting stack value.\n", drift_variance);
                    consecutive_audit_failures++;
                    if (consecutive_audit_failures >= 5) {
                        Serial.println("[AUDIT RECOVERY] Jitter is persistent. Accepting new router baseline shift.");
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
                    Serial.printf("[INITIALIZED] Valid Baseline Coordinates Locked: %llu\n", current_ev.tsf_observed);
                } else {
                    Serial.printf("[PLAUSIBILITY REJECT] TSF %llu too low. Wi-Fi stack un-synchronized.\n", current_ev.tsf_observed);
                }
            }

            // --- EXECUTION ENGINE CORE DETERMINATION ---
            if (trust_observed_tsf) {
                last_good_state = current_ev;
                clock_mode = "TSF";
                global_is_stuck_in_syn = false;  // Clear state flag
            } else if (has_initial_baseline) {
                uint64_t elapsed_processor_time = current_ev.processor_clock - last_good_state.processor_clock;
                uint64_t disciplined_delta = (uint64_t) (elapsed_processor_time * clock_alpha);

                tsf_to_transmit = last_good_state.tsf_observed + disciplined_delta;
                clock_mode = "SYN";
                global_is_stuck_in_syn = true;  // Alert main loop that we are using fallback tracking
                Serial.printf("[DISCIPLINED SYN] Extrapolated TSF: %llu (Alpha: %.8f)\n", tsf_to_transmit, clock_alpha);

                last_good_state.type = current_ev.type;
                last_good_state.tsf_observed = tsf_to_transmit;
                last_good_state.processor_clock = current_ev.processor_clock;
            } else {
                Serial.println("[CRITICAL DROP] Baseline missing or un-synchronized. Packet dropped.");
                continue;
            }

            // --- HTTP TRANSMISSION ENGINE ---
            if (WiFi.status() == WL_CONNECTED) {
                char tsf_buffer[21];
                sprintf(tsf_buffer, "%llu", tsf_to_transmit);
                String ap_bssid = WiFi.BSSIDstr();

                String request_path = String(server_url) + "?id=" + gate_id +
                                      "&tsf=" + tsf_buffer +
                                      "&bssid=" + ap_bssid +
                                      "&mode=" + clock_mode +
                                      "&type=" + type_str;

                http.begin(request_path);
                http.setTimeout(2500);
                int httpCode = http.GET();

                if (httpCode > 0) {
                    Serial.printf("[Async Worker] Sent (%s). Server Code: %d\n", clock_mode.c_str(), httpCode);

                    // --- PATCH 1: THE NETWORK RECEIPT ESCAPE HATCH ---
                    // If the server answered 200 OK and we are in SYN mode, force check
                    // if the hardware TSF layer has recovered.
                    if (httpCode == 200 && clock_mode == "SYN") {
                        uint64_t raw_tsf_check = esp_wifi_get_tsf_time(WIFI_IF_STA);
                        if (raw_tsf_check > MIN_PLAUSIBLE_TSF) {
                            Serial.println("[ESCAPE HATCH] Server confirmed online. Attempting TSF re-entry.");
                            // Forcing audit engine reset
                            has_initial_baseline = false;
                            cal_prev_tsf = 0;
                            cal_prev_proc = 0;
                            consecutive_audit_failures = 0;
                        }
                    }
                } else {
                    Serial.printf("[Async Worker] HTTP Error: %s\n", http.errorToString(httpCode).c_str());
                }
                http.end();
            } else {
                Serial.println("[Async Worker] Link down. Internal queue stacking.");
            }
        }
    }
}


void setup() {
    delay(2000);
    Serial.begin(115200);

    pinMode(LED_PIN, OUTPUT);

    gate_id = identifyBoard();
    pinMode(GATE_PIN, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(GATE_PIN), handleSensor1, FALLING);

    networkQueue = xQueueCreate(10, sizeof(GateEvent));
    ledQueue = xQueueCreate(5, sizeof(LedPattern));

    WiFi.persistent(false);
    WiFi.disconnect(true);
    WiFi.begin(ssid, password, 0, bssid);

    TimerHandle_t hbTimer = xTimerCreate("HB_Timer", pdMS_TO_TICKS(5147), pdTRUE, (void *) 0, heartbeatTimerCallback);
    if (hbTimer != NULL) {
        xTimerStart(hbTimer, 0);
    }

    xTaskCreatePinnedToCore(ledDiagnosticTask, "LED_Task", 2048, NULL, 1, NULL, 1);
    xTaskCreatePinnedToCore(uploadWorkerTask, "UploadWorker", 4096, NULL, 2, NULL, 1);
}

// --- MAIN LOOP EXECUTION TASK ---
void loop() {
    static uint32_t last_connected_time = millis();
    static uint32_t last_reconnect_attempt = millis();
    static uint32_t syn_mode_start_time = 0;  // Tracks duration of fallback timing
    static bool was_connected = false;

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
            Serial.println("\n[ROLLOVER FAULT] Trapped in synthetic time loop for 10s. Forcing hardware reboot...");
            delay(500);
            ESP.restart();
        }
    } else {
        syn_mode_start_time = 0;  // Reset watchdog when operations are normal
    }

    if (WiFi.status() == WL_CONNECTED) {
        if (!was_connected) {
            Serial.println("\n[NETWORK] Link Active! IP: " + WiFi.localIP().toString());
            was_connected = true;
        }
        last_connected_time = current_time;
    } else {
        was_connected = false;

        if (current_time - last_connected_time > 15000) {
            Serial.println("\n[WATCHDOG FAULT] Wi-Fi link dead for 15s. Smashing network stack...");
            WiFi.disconnect(true, true);
            delay(500);
            Serial.println("[WATCHDOG RECOVERY] Re-initializing hardware radio interface...");
            WiFi.begin(ssid, password, 0, bssid);
            last_connected_time = current_time;
            last_reconnect_attempt = current_time;
        } else if (current_time - last_reconnect_attempt > 3000) {
            Serial.print(".");
            last_reconnect_attempt = current_time;
        }
    }

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