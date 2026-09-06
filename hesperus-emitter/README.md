# hesperus-emitter

ESP32-S3-Zero bench emitter: a button-driven, deep-sleeping stimulus box
for the emitter side of a Hesperus gate. It shares its enclosure with
`hesperus-timing-gate` but carries a much simpler board.

See [CLAUDE.md](CLAUDE.md) for firmware design details and
[../PLATFORMIO.md](../PLATFORMIO.md) for the workspace build guide.

## Build

```
pio run
pio run -t upload
```
from `firmware/`.
