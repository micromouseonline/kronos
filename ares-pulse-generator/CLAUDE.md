# ares-pulse-generator

## Off-limits directories

- `firmware/.pio/`: generated build artefacts and vendored libraries - do not read or modify

## Hardware design files

- `hardware/pcb/`: KiCad project. `hardware/mechanical/`: enclosure/mounting
  CAD (native source + STEP/STL exports). Both are user-maintained via
  KiCad/CAD tools directly - do not hand-edit as text.
