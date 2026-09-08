# Serial Plotter

For live-plotting serial data during bring-up/calibration (e.g. ADC readings,
`clock_alpha` drift trace, ares-pulse-generator timing), use
[SerialPlotster](https://github.com/todbot/SerialPlotster) rather than the
Arduino IDE's built-in plotter.

Chosen over writing a custom tool because it's free, open-source
(GPL-3.0), cross-platform (Mac/Windows/Linux prebuilt binaries), and already
supports what the Arduino plotter doesn't: a per-run toggle between
autoscaling the Y-axis and locking it to a fixed manual min/max range. Useful
for comparing traces across runs without the axis jumping around.

No install/build needed in this repo - grab the prebuilt binary for your OS
from the [releases page](https://github.com/todbot/SerialPlotster/releases).

Usage is the same as the Arduino plotter: point it at the board's serial
port/baud rate and print space- or comma-separated numeric values (optionally
`label:value`) one line per sample.

If it turns out to be insufficient, the fallback options considered were
[serialplot](https://github.com/hyOzd/serialplot) (older, more mature Qt
tool, same autoscale/fixed-range feature) or a small custom pyserial +
pyqtgraph script here in `tools/`.
