# aim1k tablet firmware

RP2354B firmware that scans the 120-sensor HAL403 grid, localises the pen magnet,
and reports absolute position over USB through two HID collections — an absolute
mouse (driverless cursor, no Windows Ink) and a vendor-defined one for
OpenTabletDriver. Deliberately NOT a Digitizer/Pen device; see config.h for why.

## Layout

```
include/config.h        build-time config (USB IDs, area, rates, DSP knobs)
include/sensor_map.h    AUTO-GENERATED (mux/channel -> grid) from the KiCad PCB
include/hid_report.h    the 7-byte pen report struct
src/sensors.[ch]        core-1 DMA round-robin scan -> 120 deflections (seqlock),
                        per-sensor baseline + noise sigma (Welford)
src/solve.[ch]          presence (sigma + spatial coherence + hysteresis) and
                        localisation (centroid seed -> LM dipole fit)
src/filter.[ch]         1-Euro adaptive output filter
src/usb_descriptors.c   mouse + vendor collections, optional CDC console,
                        TinyUSB glue
src/tusb_config.h       TinyUSB config
src/main.c              core-0: USB + solve + filter + 1 kHz report loop
../scripts/grid_view.py host-side live heatmap over the CDC console
```

## Debug console

`DEBUG_CONSOLE 1` (default) adds a USB CDC serial port alongside the HID
interface. It is how the sensor map and the noise thresholds get verified:

```bash
python ../scripts/grid_view.py        # live heatmap
```

Touch the magnet to one physical sensor and confirm the lit cell appears at the
matching (col,row). If it lights elsewhere, that entry in `SENSOR_GRID` is wrong
and the heatmap shows where it actually landed. `noise`, `raw`, `baseline`, and
`cal` are the other subcommands. Set `DEBUG_CONSOLE 0` for a release build to
drop the CDC interface entirely.

Over a raw serial terminal the console also takes single keys — `i` for a status
line and `j` for a jitter measurement:

```
i    scan 1812 Hz | sigma min 0.75 mean 1.31 max 2.04 counts | solve 61 us avg 88 us max (budget 1000)
     pen IN  peak 197.4 snr 96.8 coh 0.11  x 59.51 y 40.02 z 10.3 (dipole) rms 1.9
```

The four numbers to read when something is wrong:

- **`solve ... us`** against the 1000 µs budget. The report loop runs at 1 kHz;
  if the average approaches that, the fit is not finishing in its slot.
- **`rms` against `peak`.** This is the dipole fit's residual — how well a real
  magnet is described by the model. It should be a few percent of `peak`. If it
  sits near `FIT_MAX_RESID_FRAC` (0.25) with the pen plainly on the tablet, the
  fit-quality gate is rejecting good frames and that constant needs raising.
- **`coh`.** Spatial coherence, which falls as the pen gets *closer* (see the
  table in `config.h`). Under `COHERENCE_MIN` the pen is not detected at all.
- **`z`.** Fitted hover height. Over `HOVER_MAX_Z_MM` the pen drops out.

`j` parks a 2-second capture and reports the standard deviation and
peak-to-peak of the reported position, before and after the output filter:

```
j    jitter over 2000 frames (mm)
       solver   sd 0.1103 0.1147  p2p 0.612 0.658
       filtered sd 0.0129 0.0134  p2p 0.083 0.094
```

Hold the pen still and read the **filtered p2p** — that is how far the cursor
wanders on its own. A large p2p next to a small sd means isolated glitches
rather than noise, which is the one case `MEDIAN_TAPS 3` is for.

## Build

```powershell
.\build.ps1            # configure (first time) + build
.\build.ps1 -Clean     # wipe build/ and reconfigure
```

`build.ps1` finds the toolchain in either place one ends up: a standalone
`C:\pico` layout, or `%USERPROFILE%\.pico-sdk\` — which is what the Raspberry Pi
Pico VS Code extension installs, and which brings its own SDK, arm-none-eabi,
CMake, Ninja and picotool. It picks the newest version of each.

Output is `build/aim1k.uf2`. To flash: hold **BOOTSEL**, plug in the RP2354B,
copy `aim1k.uf2` onto the drive that appears.

Current build: **46 KB flash, 15 KB RAM** — trivially fits the RP2354B's 2 MB /
520 KB. (Most of that is the SDK 2.3.0 / GCC 15 toolchain rather than this code;
the same source was 20.7 KB under SDK 2.1.1 / GCC 13.)

Building the firmware is not the only way to work on the solver — see *Testing
the solver without hardware* below.

## Why the cursor holds still

A 10 mm sensor pitch means the position between sensors is *interpolated*, so
every discrete decision the solver makes is a chance to turn a stationary magnet
into a moving cursor. Five things exist purely to stop that, in rough order of
how much they matter (all measured against synthetic frames — peak 200 counts,
sigma 2, magnet at 8 mm — with the harness described below):

1. **Priors on the fit's nuisance parameters** (`FIT_NUISANCE_PRIOR`). Magnet
   strength `A` and hover height `z` cannot change at 1 kHz, but the fit
   re-derived them every frame, and because the model's parameters are
   correlated their noise came back out as *position* noise. Tying them to the
   previous frame while leaving x/y completely free: 0.131 → 0.058 mm rms with
   `z` pinned outright, 0.111 mm with the adaptive prior actually shipped. No
   latency cost, because position is never constrained.
2. **Coherence threshold** (`COHERENCE_MIN`, 0.18 → **−0.10**). This one took two
   goes to get right, and the reason is worth stating plainly: **coherence is a
   monotonically decreasing function of how close the pen is**, because the
   peak's neighbours 10 mm away sit in the dipole's *negative skirt*. So any
   positive threshold is a minimum hover height in disguise — press the pen down
   harder and the tablet cuts out. At 0.18 the pen did not exist below ~10 mm.
   Lowering it to 0.05 moved the wall rather than removing it: a left-edge drag
   measured 100% usable at 7 mm, 99.8% at 6 mm, and **79% at 5 mm with presence
   chattering 213 times in four seconds** — every failure the coherence test,
   with amplitude, residual, height and area never firing. Only a negative floor
   removes the wall entirely. It now sits below what a magnet produces at ~3 mm,
   which is below anything a 10 mm pitch can localise anyway.
3. **Baseline time constant** (`BASELINE_ALPHA`, 0.0005 → 3.0e-5) and freezing
   it while a pen is present. At ~1.8 kHz the old value was a 1.1 *second* time
   constant sitting directly under the pen: rest the magnet anywhere and the
   baseline crawled up to meet it, eating the blob's skirt first and deforming
   the shape the solver was fitting.
4. **Peak-cell hysteresis** (`PEAK_HOLD_MARGIN_SIGMA`). The analysis window
   hangs off the argmax; a magnet parked between two sensors let noise re-decide
   that every frame, sliding the window a full pitch each time.
5. **Frame-count debounce** on presence, so a marginal frame costs nothing
   instead of a filter reset and a cursor jump.

End to end, worst position found by sweeping the magnet over a 10 × 10 mm cell
at 0.5 mm steps, cursor peak-to-peak with the output filter in place:

| hover | before | after |
|-------|--------|-------|
| 8 mm  | 0.54 mm | 0.25 mm |
| 12 mm | 1.02 mm | 0.51 mm |

Latency is unchanged: 0.88 ms at 400 mm/s (was 0.87), 3.81 ms at 40 mm/s (was
4.13, i.e. slightly better).

## Out of range

`solve_position` reports `in_range` false — and `main.c` then stops sending
reports entirely, freeing the cursor — when any of these hold for
`PRESENCE_EXIT_FRAMES` consecutive frames:

- **the peak has collapsed to under `PRESENCE_RELEASE_FRAC` of what this contact
  has been reading.** This is the test that actually releases the pen, and it is
  relative because no absolute one can work: the threshold has to sit low enough
  for the weakest pen worth seeing, and a strong magnet reading 865 counts would
  have to travel nearly 4x further away before it fell through
  `PRESENCE_EXIT_SNR * sigma` ≈ 16 counts. The reference is a symmetric average
  over `PEAK_REF_TAU_S`, so any hover height held for longer than that simply
  becomes the new reference and only a change faster than it — lifting the pen —
  trips the test;
- peak below `PRESENCE_EXIT_SNR` sigma, or coherence below `COHERENCE_EXIT_MIN`
  (pen gone, or a single drifted sensor pretending to be one);
- fitted `z` over `HOVER_MAX_Z_MM` (pen held too high to localise);
- fit residual over the `FIT_MAX_RESID_*` gate (whatever is on the grid is not
  a magnet on the surface);
- the fit places the pen more than `AREA_MARGIN_MM` outside the sensor-centre
  span (**pen off the edge of the tablet**).

That last one is only possible because the fit's x/y are unconstrained: push the
magnet past the last column and the peak cell stays pinned to that column, but
the fit — which knows what the field's shape should look like — reports a
position out beyond the edge. A centroid cannot do this; it is trapped inside
the grid, which is why a centroid-only tablet smears and jitters along its
border instead of admitting the pen has left.

### The phantom pen

A phantom is a POSITIVE blob on the grid with no magnet present, and there is
exactly one way to make one: the baseline learned a value that is too LOW,
which means it was learned while a NEGATIVE field sat on the tablet. A negative
field is the magnet's other pole — the pen resting flipped, or lying on its
side, which is the natural way to put a pen down. Presence only searches for
positive peaks, so a negative field is invisible to it, and any ungated "no pen
detected, track freely" baseline rate will absorb it wholesale. Pick the pen up
and its positive mirror image stands up out of the corrected-for field: full
pen amplitude, dipole-shaped to within noise (it IS the pen's field, frozen),
fitting a plausible hover height. It passes every test the solver has, because
it is a perfect recording of something that passed every test. One was captured
on hardware holding peak ~900 / z 12.0, motionless, for five unbroken minutes.

Weak phantoms exist too (drift accumulated under a long-parked pen, the
positive ring around the hole left by calibrating with the magnet on), but the
full-amplitude image is the one that cannot be beaten after the fact: a real
pen between sensors reads LESS than the image does, so no amplitude contest,
threshold, or release rule can prefer the pen once the image exists. The only
winning move is to make images impossible to create. Defences, in order:

1. `BASELINE_GATE_SNR` — no baseline rate may follow a deflection beyond a few
   sigma, in either polarity. Drift creeps, so a tracking baseline never
   legitimately sees more; anything bigger is a field, and fields are never
   absorbed. This kills the formation path outright.
2. The post-release re-zero pass (`BASELINE_ALPHA_REZERO`) is the single
   exception, and it is ungated for POSITIVE deflections only — its targets
   (leftover images, drift from under a parked pen) are positive, and
   absorbing a large negative deflection is precisely the act of minting a
   phantom.
3. `PRESENCE_REACQUIRE_BLOCK_MS` — after a release, nothing may be acquired,
   flat, no exceptions, while the re-zero runs. An image is at full pen
   amplitude, so any exception lenient enough to admit a returning pen admits
   the image, which then freezes its own neighbourhood and cancels the erase.
4. The peak tracker never walks to a FAR rival while its own cell's signal has
   collapsed. One magnet cannot jump several pitches in a millisecond, so that
   situation is always "the pen left while a second source exists" — hold the
   dying cell, release, and let the re-zero deal with whatever the rival is.
   This is both how weak phantoms get erased by ordinary use (one contact and
   lift heals them) and why the cursor no longer teleports onto a second blob
   whenever the pen lifts.

What remains: a full image can still be BAKED IN by booting — the calibration
runs ~0.5 s after power-up and trusts whatever it sees — with a magnet resting
on (or flipped on) the tablet. Nothing at runtime can distinguish that from a
clean zero, so keep the tablet clear when plugging in; `c` on the console
re-zeros and fixes it immediately if it happens.

## Testing the solver without hardware

`solve.c` and `filter.c` have no SDK dependencies beyond `sensor_map.h`, so they
compile natively against a synthetic grid. The magnet is modelled as a pair of
magnetic charges (a finite cylinder), *not* as the point dipole the solver fits,
so the fit sees real model mismatch; per-sensor gain spread is included for the
same reason. Stub `sensors_sigma()` and friends, generate frames, and measure.
This is how every number above was obtained, and it is much faster than
guessing at a tablet.

## Bring-up order (do these on a scope/logic analyser first)

1. **Confirm HAL403SO is the linear/ratiometric part** (analog output). The ADC
   path is meaningless for a switch-type Hall. See `../datasheets/HAL403.pdf`.
2. **Select-line sanity.** Toggle S0..S3 (GPIO32/31/30/29) and verify each mux
   walks channels 0..14. `sensor_map.h` assumes address = channel index.
3. **Settle time.** `sensors.c: set_address()` waits 5 µs. Scope one COM line
   after an address change and shrink/grow this until it is just clean at 12-bit.
4. **Baseline + polarity.** With no pen, `sensors_calibrate_baseline()` should
   leave deflections near zero. Bring the magnet down over one sensor and confirm
   the deflection goes **positive** (the code weights on positive deflection). If
   it goes negative, flip the magnet, or negate in `raw_frame()`.
5. **Grid orientation.** Move the pen right/down and confirm reported X/Y move the
   right way. Add axis flips in `main.c: send_pen()` if the board is mounted
   rotated/mirrored.

## Known stubs / next steps

- **Clicks are keyboard-only, by design.** The tablet is hover-only: it reports
  position and never asserts a tip switch. Bind osu! K1/K2 on the keyboard as
  usual. Pressure stays 0.
- **DMA round-robin on core 1** is done: core 1 scans the grid (DMA drains the 8
  ADC inputs per address) and publishes frames through a seqlock; core 0 only
  runs USB + solve. Core-1 frame rate is ~1.4 kHz (2x oversampled), independent
  of the 1 kHz report loop, with headroom to push higher (drop OVERSAMPLE or
  trim the 5 us settle in `sensors.c`).
- **Dipole refine** in `solve.c` is implemented (Levenberg-Marquardt over the
  5x5 window, falling back to the centroid if it fails to converge or wanders
  outside the window). It removes the centroid's edge bias and its bias from the
  field's negative skirt.
- Regenerate `sensor_map.h` whenever the PCB changes:
  `python ../scripts/gen_sensor_map.py --write`. It reads placements, net
  connectivity, and pin functions out of `pcb/tiramisu-dev.kicad_pcb`, so it never
  assumes a pinout — which matters, because the CD74HC4067 footprint here does
  not use the pad order the datasheet would suggest (pad 1 is COM, not I5), and
  the ADC net names are reversed relative to the ADC input numbers.
- Confirm the map on hardware with `../scripts/grid_view.py` after regenerating.
