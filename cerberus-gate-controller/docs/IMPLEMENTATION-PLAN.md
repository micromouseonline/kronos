# Comprehensive Development & Testing Plan: CERBERUS Multi-Gate Race Timer

Execute this plan sequentially. Do not move to a subsequent step until the "Verification Test" for the current step passes completely.

---

## Phase 1: Core Framework & Inter-Task Communication
The goal of this phase is to establish the thread-safe foundation of the application without any physical I/O or graphics hardware.

### Step 1.1: Data Structures & The Main App Task
* **Action:** Define the `SystemEvent` struct (`EventType type; uint64_t timestamp_us; char payload[32];` -- see `DESIGN-REQUIREMENT.md`). Initialize the `Main Event Queue` with a depth of 32. Create the `Main App Task` on Core 1 at a high priority (`configMAX_PRIORITIES - 1`). Inside this task, loop on `xQueueReceive()` with a bounded timeout (e.g. 30-50ms) rather than an indefinite block -- this tick is what later drives the live Run Timer redraw (Step 4.1), so get the shape right now even though nothing uses the timeout branch yet. Print received event details to the primary Serial monitor for now (temporary -- see Step 2.3 for when this must move to the display).
* **Verification Test:** Write a temporary loop in `setup()` that pushes 10 mock events of alternating types into the queue. Verify via the Serial Monitor that the Main App Task pops and prints all 10 events sequentially in perfect FIFO order, and that idle periods between pushes produce timeout ticks without errors.

### Step 1.2: The Supervisory State Machine Core
* **Action:** Implement the top-level supervisor state logic (`READY`, `RACING`, `MAINTENANCE`) inside the Main App Task. Add parsing logic so that when specific command events are popped from the queue, the internal state updates. 
* **Verification Test:** Manually push a sequence of mock command events into the queue (e.g., `CMD_START_RACE`, `CMD_TRIGGER_LAP`, `CMD_ENTER_MAINTENANCE`). Verify via Serial prints that the system transitions correctly between states and that timing events are processed during `RACING` but explicitly ignored or dropped during `MAINTENANCE`.

---

## Phase 2: Input Layer Integration
The goal of this phase is to bring asynchronous and synchronous input sources online, directing them all into the Core 1 queue.

### Step 2.1: Local Input Polling Task (Core 1)
* **Action:** Already built -- see `USER-INPUT-SYSTEM.md`. The Local Input Polling Task on Core 1 polls touch/GPIO/NeoKey every `INPUT_POLL_PERIOD_MS`, debounces in software, and posts a `ButtonID` (`BTN_ARM/BTN_START/BTN_GOAL/BTN_RESET`). For RACING mode, `on_button_event()` maps these to `EV_ARM/EV_START/EV_GOAL/EV_NEW_MOUSE`, wraps them in a `SystemEvent` timestamped with `esp_timer_get_time()`, and pushes to the main queue with `xQueueSend()`.
* **Verification Test:** Press the physical buttons and the NeoKey buttons. Watch the Serial Monitor to ensure the Main App Task intercepts the events instantly, showing the correct button IDs with zero bounce or double-triggering.

### Step 2.2: Asynchronous HTTP Input Handler (Core 0)
* **Action:** Connect to the shared WiFi network (provided by an external travel router, not CERBERUS) as a station, then spin up the `ESPAsyncWebServer` library on that connection, configuring it to pin its internal infrastructure to Core 0. Implement `POST /api/event`, accepting the JSON body documented in `DESIGN-REQUIREMENT.md` (`gate_id`, `event`, `tsf_us`, `gate_us`). In the request callback, parse the JSON into a `SystemEvent` (`timestamp_us = tsf_us`, `payload = gate_id`) and push it to the Main Event Queue.
* **Verification Test:** Use a tool like `curl` or a script to POST 5 JSON events separated by only 20 milliseconds. Verify that the Main App Task on Core 1 captures and prints all 5 events without dropping data or crashing the network stack.

### Step 2.3: Serial Monitor Task (Core 1)
* **Action:** Create a dedicated task on Core 1 that maintains the continuous serial link with the host PC, and becomes the sole owner of the host UART from this point forward. Use a non-blocking read mechanism that accumulates incoming characters into a local 64-byte buffer until an EOL (`\n`) character is discovered (e.g. `NEW_MOUSE:MightyMouse\n`). Parse the resulting string command, map it to a `SystemEvent`, and push it to the main queue. TX side: mirror every generated event and calculated run time back to the host in the same canonical line format the Logging Task writes to SD (Step 3.2) -- one formatter, two destinations. Migrate any ad-hoc `Serial.print` debug output added in Steps 1.1/1.2/2.1/2.2 to the display; from here on Serial is reserved exclusively for host protocol traffic (see Debug Output Policy, `DESIGN-REQUIREMENT.md`).
* **Verification Test:** Type commands directly into your serial terminal (e.g., `START\n`). Confirm, via an on-screen debug readout (not Serial -- that link is now host-only), that the Serial Task processes the string and that the Main App Task transitions states accordingly.

---

## Phase 3: Storage, NVS, & Logging Infrastructure
The goal of this phase is to safely establish file system routines and offload blocking disk I/O to a lower-priority worker thread.

### Step 3.1: NVS Boot Counter & Storage Initialization
* **Action:** Initialize the ESP32 Non-Volatile Storage (NVS) flash partition. Write a routine that reads an integer key named `boot_count`, increments it by 1, saves it back to NVS, and initializes the SD Card over the SPI bus. Use this incremented number to construct a unique, persistent filename (e.g., `/logs/RACE_NUM_[count].CSV`). Create a file named `/logs/LATEST.TXT` and write the active filename string inside it.
* **Verification Test:** Power-cycle the CYD board 5 times consecutively. Pull the SD card, insert it into a computer, and verify that 5 empty files (`RACE_NUM_1.CSV` through `RACE_NUM_5.CSV`) exist, and that `LATEST.TXT` correctly contains the text string `RACE_NUM_5.CSV`.

### Step 3.2: The Logging Queue & Task (Core 1 - Low Priority)
* **Action:** Initialize a 64-item `Logging Queue` passing fixed-size `LogMessage` structs by value. Create a low-priority `Logging Task` on Core 1. This task blocks on the logging queue, pops incoming data packets, formats them into a single canonical CSV row string, appends it to the active SD card file handle, and hands the same string to the Serial Monitor Task (Step 2.3) to mirror to the host during `RACING`.
* **Verification Test:** Modify the Main App Task so that when a simulated lap trigger event occurs, it passes a data struct to the Logging Queue. Fire a dense burst of inputs. Verify that the system writes data continuously to the SD card without causing any latency hiccups in the Main App Task loop, and that the host terminal receives the identical line in real time. Then replay the saved `.CSV` file back over the serial link (e.g. `cat session.csv > /dev/ttyUSBn`) and confirm the host receives byte-identical lines to the original live run.

---

## Phase 4: UI, Maintenance, & Extraction
The final phase glues the visual components to the system and safely implements the file delivery mechanism.

### Step 4.1: Exclusive Display Updates
* **Action:** Initialize the display hardware on Core 1 utilizing `LovyanGFX` (see `display.h`). Grant the Main App Task exclusive ownership over writing to the screen. Update the UI layout dynamically inside the Main App Task based on state machine shifts (e.g., static menus during `READY`). During `RACING`, draw both timers concurrently: the Session Countdown Timer (configurable duration, ticks down at roughly 1Hz) and the Run Timer (zeroed on `EV_START`, stopped on `EV_GOAL`, redrawn every timeout tick from Step 1.1's bounded `xQueueReceive` loop so it visibly runs live while waiting for `EV_GOAL`/`EV_ARM`/`EV_RESTART`).
* **Verification Test:** Run the timer while bombarding the network stack with HTTP requests. Ensure the display updates fluently with zero graphical glitching, artifact tearing, or multi-core memory panics. Separately, start a run and confirm the Run Timer visibly increments smoothly (no multi-second stalls) purely from timeout ticks when no gate/button events are arriving, while the Session Countdown Timer counts down alongside it.

### Step 4.2: Maintenance Guarding & HTTP Log Streaming
* **Action:** Implement the `MAINTENANCE` behavior routine. When a `CMD_ENTER_MAINTENANCE` event is processed, the Main App Task must update the display to read "Syncing Data...", and command the Logging Task to explicitly flush its buffers and close its open SD card file handle. Then, expose an HTTP GET route `/download`. The web server callback on Core 0 opens the targeted CSV file in read-only mode and streams it to the client using a chunked wrapper, inserting a `vTaskDelay(1)` between blocks to satisfy the Task Watchdog Timer (TWDT).
* **Verification Test:** Generate a 500KB log file. Trigger `MAINTENANCE` mode, and download the file via a web browser. Confirm that the download finishes with zero errors, the contents perfectly match the card data, the device watchdog does not trip, and the display remains functional throughout the transfer.