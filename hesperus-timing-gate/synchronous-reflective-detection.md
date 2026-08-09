Here is a complete, structured system architecture specification designed specifically to serve as a precise prompt/context document for an agentic coding assistant (e.g., Cursor, Claude Dev, GitHub Copilot Workspace).

---

# Technical Specification: Self-Calibrating Reflective Light Gate (ESP32-S3)

## 1. System Overview

Design an optical reflective light gate on the ESP32-S3 to detect objects passing through a gap with a minimum dwell time of 7 ms. The system uses an IR LED and IR Phototransistor pair facing a reflective baseline.

To eliminate ambient light calibration and saturation issues, the system employs a closed-loop control scheme: the LED is driven via hardware PWM whose duty cycle is dynamically controlled by a low-pass-filtered feedback loop to maintain the phototransistor output at a target voltage (~35% Full-Scale Deflection). Fast optical transitions (passing objects) bypass the slow feedback loop and are detected as transient spikes in the error signal.

```
                    +-------------------------------------------------+
                    |                   ESP32-S3                      |
                    |                                                 |
  +--------------+  |  +-----------+    +-------------+               |
  |  IR LED      |<-+--| MCPWM     |    | Dual-Core   |               |
  +--------------+  |  | Timer/Op  |    | FreeRTOS    |               |
         |          |  +-----+-----+    +------+------+               |
   Optical Path     |        | Sync            |                      |
         v          |        v                 | Core 1 High-Pri Task |
  +--------------+  |  +-----------+           v                      |
  | Photo-Trans. |  |  | DIG ADC1  |----> [ Ring Buffer ]             |
  +------+-------+  |  | DMA       |           |                      |
         |          |  +-----------+           v                      |
  +------v-------+  |                 +------------------+            |
  | Hardware LPF |--+---------------->| 4-Tap FIR Notch  |            |
  +--------------+  |                 +--------+---------+            |
                    |                          |                      |
                    |            +-------------+-------------+        |
                    |            |                           |        |
                    |            v                           v        |
                    |  +-------------------+   +-------------------+  |
                    |  | High-Pass / Diff  |   | 2Hz LPF (Feedback)|  |
                    |  +---------+---------+   +---------+---------+  |
                    |            |                       |            |
                    |            v                       v            |
                    |    Threshold Detect         Duty Cycle Adjust   |
                    |   (Object Event >7ms)     (MCPWM Duty Reload)   |
                    +-------------------------------------------------+

```

---

## 2. Hardware Interface & Pin Configuration

* **Microcontroller:** ESP32-S3
* **LED Driver Pin (GPIO_LED):** Output driving the IR LED via a low-side MOSFET or BJT.
* **Phototransistor Input Pin (GPIO_ADC):** Connected to ADC1 (e.g., `ADC1_CHANNEL_2` / GPIO3).
* **Front-End Hardware Filter:** Passive RC low-pass filter ($R = 1.5\text{ k}\Omega$, $C = 100\text{ nF}$, $f_c \approx 1.06\text{ kHz}$) placed between the phototransistor emitter/collector tap and the ESP32-S3 ADC pin to attenuate high-frequency edges and external RF spikes.

---

## 3. Peripheral Configuration

### 3.1 MCPWM Peripheral (Timer0, Operator0, Generator0)

* **Frequency:** $20\text{ kHz}$ (Period $T = 50\ \mu\text{s}$).
* **Initial Duty Cycle:** 25% (Adjustable across 0.5% to 85% range).
* **Hardware Trigger Output:** Configure MCPWM Operator to produce a sync event (using `mcpwm_gen_timer_event` or timer sync) at the **center of the active PWM pulse** (or PWM timer peak) to trigger the ADC conversion phase-locked to the illumination.

### 3.2 Continuous ADC with DMA

* **Unit & Channel:** ADC1, assigned to `GPIO_ADC`.
* **Sampling Trigger:** Driven via internal GDMA / I2S DIG ADC controller linked to the hardware timer/sync signal at an effective rate of **$4\text{ kHz}$** ($4000\text{ samples/sec}$).
* **Buffer Architecture:** Continuous conversion into a circular DMA buffer (frame size: 64 samples).

---

## 4. Signal Processing Pipeline

Processing occurs on every incoming DMA buffer frame inside a dedicated, high-priority FreeRTOS task.

### Step 1: 250 Hz & Harmonic Noise Rejection (16-Tap FIR Moving Average)
To eliminate synchronous interference at 250 Hz and all its harmonics (500 Hz, 750 Hz, 1.0 kHz, etc.), pass each raw sample x[n] through a 16-point rolling average filter. 

* **Algorithm Requirement:** Implement as an O(1) recursive moving average buffer to ensure computational time remains identical to shorter filters:
      
      y[n] = y[n-1] + (x[n] - x[n-16]) >> 4

* **System Impact Note:** Extending the filter length to 16 points adds zero execution overhead per sample. It increases the deterministic filter group delay from 0.375 ms to 1.875 ms (a slight increase in detection latency), but significantly improves SNR by stripping out 250 Hz ripple. Subtract a fixed offset of 1.875 ms from timestamps when logging event times to compensate for the group delay.

### Step 2: Error Signal Generation

Compute the instantaneous difference between the fixed target baseline $V_{\text{target}}$ (e.g., 35% of $4095 \approx 1433\text{ counts}$) and the filtered phototransistor reading $y[n]$:


$$E[n] = y[n] - V_{\text{target}}$$

### Step 3: Fast Path — Object Detection Thresholding

Pass $E[n]$ into an edge detection and thresholding module:

1. **Magnitude Condition:** Trigger an event state if $\vert{}E[n]\vert{} > V_{\text{thresh}}$ for at least $N$ consecutive samples (e.g., 8 samples $\approx 2\text{ ms}$ threshold window to validate a 7 ms event).
2. **Derivative Condition (Leading Edge):** Compute slope $D[n] = y[n] - y[n-2]$. If $\vert{}D[n]\vert{} > D_{\text{thresh}}$, flag a rapid reflectance change event.
3. **Event Qualification:** Declare an object detected when threshold conditions are sustained for $2\text{ ms} \le t_{\text{event}} \le 100\text{ ms}$.

### Step 4: Slow Path — Closed-Loop Feedback Control (2 Hz LPF)

Pass the smoothed error $E[n]$ through a First-Order Infinite Impulse Response (IIR) Low-Pass Filter to update the LED duty cycle:

$$\text{FilteredError}[n] = \alpha \cdot E[n] + (1 - \alpha) \cdot \text{FilteredError}[n-1]$$

* **Filter Parameter:** Set $\alpha$ such that the filter cutoff $f_c \approx 2.0\text{ Hz}$ at a $4\text{ kHz}$ update rate:

$$\alpha = \frac{2\pi f_c}{f_s + 2\pi f_c} \approx \frac{2\pi (2)}{4000 + 2\pi (2)} \approx 0.00313$$


* **Integrator / Duty Update:**

$$\text{Duty}[k] = \text{Duty}[k-1] - (K_p \cdot \text{FilteredError}[n])$$



*Where $K_p$ is a conservative loop gain factor.*
* **Safety Clamping:** Soft-clamp the calculated duty cycle to a safe range (e.g., $1.0\% \le \text{Duty} \le 80.0\%$) before writing to the MCPWM register using `mcpwm_comparator_set_compare_value()`.

---

## 5. FreeRTOS Task Architecture

### Task 1: Optical Engine Task (`optical_engine_task`)

* **Core Affinity:** Pinned to **Core 1**.
* **Priority:** `configMAX_PRIORITIES - 2` (High Priority).
* **Execution Model:** Blocked on ESP-IDF DMA ADC event queue (`adc_continuous_evt_cntg_pool`). Unblocks immediately upon DMA buffer completion.
* **Responsibilities:**
1. Read DMA buffer.
2. Execute 4-tap FIR filter.
3. Compute $E[n]$.
4. Run 2 Hz IIR control loop and update MCPWM duty cycle.
5. Evaluate trigger conditions; if an object is detected, dispatch an event notification or message queue payload to system handlers.



### Task 2: Application / Communication Tasks

* **Core Affinity:** Pinned to **Core 0**.
* **Responsibilities:** Wi-Fi/BT management, logging, user interfaces, or higher-level state machines. Isolating these to Core 0 guarantees zero interrupt/scheduling jitter on the optical sampling task.

---

## 6. Verification & Edge-Case Requirements for Code Generation

The generated C++/ESP-IDF code must explicitly address the following requirements:

1. **Phase-Locked Control Loop Isolation:** The 2 Hz IIR filter cutoff ensures that any transient event lasting less than $50\text{ ms}$ (including the 7 ms minimum object passage) causes negligible shift in the LED duty cycle, leaving the optical spike entirely visible in $E[n]$.
2. **Buffer Overrun Protection:** ADC DMA queue sizes must be sufficient (minimum 4 frame descriptors) to prevent queue overflow during transient system interrupts.
3. **Integer Arithmetic Efficiency:** Use fixed-point or raw integer operations for the 4-tap FIR filter and 2 Hz IIR filter inside the high-frequency ISR/DMA callback path to maximize performance.
4. **Saturation Guard:** If the ambient light level is so high that $y[n]$ reaches maximum ADC count ($4095$) even at minimum LED duty cycle ($0.5\%$), flag an `AMBIENT_SATURATION_ERROR` and disable auto-tuning until light levels drop.

## 7. Latency and Accuracy
### 1. Expected Detection Latency

**Total Latency: 2.25 ms to 2.5 ms**

The delay between an object physically entering the beam and the system triggering a detection is determined by three main stages:

* **Hardware RC Filter Delay (~0.15 ms):** The passive input filter ($R = 1.5\text{ k}\Omega, C = 100\text{ nF}$) has a time constant $\tau = RC = 150\ \mu\text{s}$. The analog signal reaches ~63% of its new level within this window.
* **FIR Notch Filter Delay (0.5 ms):** The 4-tap moving average filter operating at $4\text{ kHz}$ ($0.25\text{ ms}$ per sample) introduces a fixed group delay of:

$$\text{Delay}_{\text{FIR}} = \frac{N - 1}{2} \times T_s = \frac{4 - 1}{2} \times 0.25\text{ ms} = 0.375\text{ ms}$$


* **Threshold Verification Window (1.75 ms to 2.0 ms):** To prevent false triggers from random noise, the algorithm requires $N = 7 \text{ to } 8$ consecutive samples to confirm an event:

$$\text{Window} = 8 \times 0.25\text{ ms} = 2.0\text{ ms}$$



---

### 2. Detection Time Accuracy (Timing Jitter)

**Timing Accuracy (Jitter): $\pm 0.125\text{ ms}$ to $\pm 0.25\text{ ms}$**

This determines how precisely you can timestamp the exact moment an object passed. The primary source of uncertainty is the discrete sample interval:

* **Sampling Resolution ($\pm 0.25\text{ ms}$):** At a $4\text{ kHz}$ ADC sample rate, the physical arrival of an object can fall anywhere within a $250\ \mu\text{s}$ sampling window.
* **Edge Interpolation ($\pm 0.125\text{ ms}$):** By using a derivative trigger ($\frac{dE}{dt}$) alongside magnitude thresholding, you can mathematically interpolate between the two samples straddling the threshold crossing, cutting the effective timing jitter in half.

*Note: Software-induced jitter is negligible ($< 1\ \mu\text{s}$) because the ADC conversions are triggered directly by hardware DMA on Core 1.*

---

### Key Trade-Off

Because the total object dwell time is **7 ms**, a **2.5 ms latency** means the system will flag the event when the object is roughly **35% of the way through** the beam—leaving plenty of margin to register the passage accurately before the object clears the sensor.