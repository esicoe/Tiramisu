# aim1k — a 1000 Hz osu! tablet

Absolute-position graphics tablet built around a 120-sensor Hall-effect grid and
an RP2354B. The pen is a passive magnet; there is no active electronics in it.
Reports at a measured **1000 Hz** over USB, works with no driver installed, and
hands off cleanly to OpenTabletDriver.

```
logo/                        logo sources + generated KiCad footprints
tiramisu-dev/                the current board revision
  firmware/                  RP2354B firmware (Pico SDK, C)
  pcb/                       KiCad PCB + schematic
  cad/                       pen STLs
  scripts/                   host-side Python utilities (stdlib only, no pip installs)
  opentabletdriver/          OTD configuration
  docs/                      sensing calculations
  manufacturing/             gerbers, BOM, pick-and-place
  datasheets/                part datasheets
```

## Current state

| | |
|---|---|
| Report rate | **1000 Hz** (999.8 Hz measured, 2 gaps in 15,000) |
| Grid scan rate | ~1610 Hz, 120 sensors, DMA on core 1 |
| Sensor noise | σ ≈ 5.8 ADC counts |
| Signal with pen down | ~2066 counts (≈356σ) |
| Localisation | weighted centroid seeded into a Levenberg-Marquardt dipole fit |
| Sensor map | verified against hardware (magnet on U88 → col 5 row 4, x 50.86 y 39.58 mm) |
| Windows Ink | not in the path — the device is not a Digitizer/Pen |
| OpenTabletDriver | works, no plugin DLL needed |

Open items: pen working height is ~14 mm where `tiramisu-dev/docs/CALCULATIONS.md` §4 wants
8–12 mm for best interpolation; tip switch is unwired (hover-only, click with the
keyboard as osu! players normally do).

## Setting up on a new machine

**Toolchain.** The firmware expects the Pico SDK and ARM GCC under `C:\pico`:

```
C:\pico\pico-sdk        Raspberry Pi Pico SDK 2.1.1
C:\pico\armgcc          ARM GCC 13.3.1
C:\pico\cmake           CMake 3.30.5
C:\pico\ninja           Ninja
C:\pico\picotool-dist   prebuilt picotool
```

The official Raspberry Pi *Pico setup for Windows* installer lays this out
exactly. To put it elsewhere, edit the `$Pico` path at the top of
`tiramisu-dev/firmware/build.ps1`.

**Build:**

```powershell
cd tiramisu-dev/firmware
.\build.ps1              # configure (first time) + build
.\build.ps1 -Clean       # wipe build/ and reconfigure
```

Output is `tiramisu-dev/firmware/build/aim1k.uf2`.

Build through Git Bash rather than PowerShell if you script it — PowerShell 5.1
wraps CMake's stderr in `NativeCommandError` and reports a spurious exit 1 on a
perfectly successful build.

**Flash:** hold **SW1** (BOOTSEL), tap **SW2** (RESET), release SW1, then copy
`aim1k.uf2` onto the `RP2350` drive that appears.

**Host tools** need only Python 3 (standard library — they talk to Win32
directly through `ctypes`, so there is nothing to `pip install`):

```powershell
cd tiramisu-dev
python scripts/rate_test.py 20     # report rate + jitter distribution
python scripts/grid_view.py        # live 120-sensor heatmap over USB CDC
python scripts/grid_view.py noise  # per-sensor noise floor
python scripts/set_mode.py status  # absolute-mouse collection on/off
python scripts/gen_sensor_map.py   # regenerate sensor_map.h from the KiCad PCB
```

## How it presents itself over USB

Two HID top-level collections plus a debug serial port:

- **absolute mouse** — Windows moves the cursor with no driver at all, mapped to
  the whole virtual desktop. A mouse is not the pen/ink stack, so Windows Ink
  never enters the picture.
- **vendor-defined** — OpenTabletDriver reads this one. It has to be separate:
  Windows opens Digitizer/Pen *and* mouse collections exclusively, so OTD would
  be refused a handle on either. Windows claims collections individually, so the
  vendor one stays open.
- **USB CDC** — the debug console `scripts/grid_view.py` drives.

OTD switches the mouse collection off when it connects (a feature report, see
`opentabletdriver/aim1k.json`) so the two never fight over the cursor.
Replugging restores mouse mode, so the tablet still works driverless anywhere.

See `tiramisu-dev/firmware/README.md` for the firmware internals and bring-up
notes, and `tiramisu-dev/opentabletdriver/README.md` for OTD setup.
