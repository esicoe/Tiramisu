# aim1k tablet — sensing calculations

All numbers below are derived from the KiCad files in `pcb/`
(120 × HAL403SO, RP2354B, 8 × CD74HC4067). Re-run the extraction in
`scripts/gen_sensor_map.py` if the board changes.

## 1. Grid geometry (measured from the PCB)

| Quantity | Value |
|---|---|
| Sensors | **120** (HAL403SO, SOT-23) |
| Layout | **12 columns × 10 rows** |
| Pitch | **10.0 mm** in X and Y |
| Sensor-centre span | **110 mm × 90 mm** (col 0→11, row 0→9) |
| Aspect ratio | 110:90 = 1.22 (osu! playfield is 4:3 = 1.33 — crop in software) |

**Usable active area.** A magnet is localised by interpolating between the
sensors that surround it. Near the outer edge the magnet is only "seen" from one
side, so accuracy degrades within ~½ pitch of the border. Plan for a reliable
interior of about **100 mm × 80 mm**, and let OpenTabletDriver map a smaller
osu! area inside that.

## 2. Electrical / scan architecture (from the schematic)

- **Select lines S0–S3 are a shared bus** → `S0=GPIO32, S1=GPIO31, S2=GPIO30, S3=GPIO29`.
- **Mux enables (E) are tied low in hardware** (10 kΩ to GND, not on the MCU),
  so **all 8 muxes are always enabled** and present their selected channel
  simultaneously.
- **8 mux COM outputs → the RP2350 ADC pins**, but the net names are **reversed**:
  net `ADC0` (mux A) is on **GPIO47 = ADC input 7**, … net `ADC7` (mux H) is on
  **GPIO40 = ADC input 0**. Encoded as `MUX_ADC_INPUT[A..H] = {7,6,5,4,3,2,1,0}`.
- **Each mux drives 15 sensors** (channels 0–14); **channel 15 is unused** on all
  eight. So a full frame is **15 select addresses × 8 muxes = 120 reads**.

The RP2350 has **one** ADC behind an 8-input front mux, so the 8 mux COMs are read
*round-robin*, not truly in parallel.

## 3. Scan-rate budget

Per select address: set 4 GPIOs, wait for the analog line to settle, then convert
8 channels round-robin.

| Item | Time |
|---|---|
| RP2350 ADC conversion | ~2.0 µs / sample (500 kSa/s) |
| 8 channels / address | ~16 µs |
| Mux + line settle (conservative) | ~5 µs |
| **Per address** | **~21 µs** |
| **15 addresses = full 120-sensor frame** | **~315 µs → ~3.2 kHz** |

Even at a pessimistic 2× overhead you clear **1.5 kHz full-grid**, so:

- **1000 Hz USB report rate is comfortably met** (USB Full-Speed, 1 ms interval).
- There is headroom to **oversample** (average 2–3 frames) to cut noise ~1.4–1.7×
  and still report at 1 kHz.
- Free-run the scan on **core 1** (DMA-fed round-robin), solve position on **core 0**.

## 4. Magnet & working height (this changes your earlier plan)

Pen magnet: **6 mm ⌀ × 10 mm** axially-magnetised NdFeB cylinder (N42≈Br 1.32 T).
On-axis field of a cylinder at distance `z` from the pole face:

```
B(z) = (Br/2) · [ (L+z)/√(R²+(L+z)²) − z/√(R²+z²) ],   R=3 mm, L=10 mm
```

| z (magnet face → sensor plane) | B |
|---|---|
| 5 mm | ~81 mT |
| 8 mm | ~33 mT |
| **10 mm** | **~21 mT** |
| 12 mm | ~14 mT |
| 15 mm | ~8 mT |
| 17 mm | ~6 mT |

**Key correction.** Earlier I said "get the magnet as close to the tip as
possible." That is right for a *single* sensor, but **wrong for a 10 mm-pitch
array.** If the magnet is 2–3 mm away the field is a sharp spike over essentially
one sensor and you **cannot interpolate** where between sensors it sits. For an
array the field "spot" at the sensor plane should span ~2–3 pitches, i.e. the
**working height should be ≈ the pitch, ~8–12 mm.**

So the target is:

```
working height  =  (tip-to-magnet-face gap)  +  (cover thickness)  ≈  8–12 mm
```

With a ~1.5–2 mm cover sheet, the **magnet face should sit ~6–10 mm behind the
pen-tip contact point** — not 2–5 mm, and not 15 mm.

- **Your current 15 mm** puts the magnet ~17 mm from the sensors → only ~6 mT and
  a broad, low-amplitude peak. Workable but noise-limited; pull it forward.
- **Sweet spot ≈ 8 mm behind the tip** → ~10 mm working height, ~21 mT, peak
  spread across a 3×3 neighbourhood. That is the number to build to.

## 5. Expected resolution

- Raw pitch is 10 mm, but with weighted-centroid / dipole interpolation over a
  3×3–5×5 neighbourhood and a 12-bit ADC, position resolution of **~0.05–0.2 mm**
  is realistic when SNR is good (another reason to favour ~21 mT over ~6 mT).
- Mapped to a 1920 px width over a ~100 mm area (~0.05 mm/px), that is roughly
  one-screen-pixel precision — adequate for osu!, **noise-limited, not
  pitch-limited**, which is why working height and calibration matter most.

## 6. Open hardware questions to confirm

1. **Is HAL403SO the linear/ratiometric Hall (analog output), not a switch?**
   The ADC readout only works with an analog-output part. Confirm against
   `datasheets/HAL403.pdf`.
2. **Pen click / tip switch.** A rigid magnet-in-pen gives *hover only*. osu!
   players usually click with the keyboard, so hover + keyboard is fine, but if
   you want a pen "tap = click" you need either a tip micro-switch (add a GPIO) or
   a z-threshold gesture. The firmware exposes a `tip_switch` bit that is
   currently stubbed off — wire it to whichever you choose.
