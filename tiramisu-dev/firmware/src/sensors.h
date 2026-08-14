#pragma once
// Hall-sensor grid acquisition.
//
// The scan runs continuously on core 1: for each of the 15 used mux addresses it
// drives S0..S3, lets the analog lines settle, then DMAs the eight mux COM lines
// out of the ADC FIFO in round-robin. Completed frames (baseline-subtracted) are
// published through a lock-free seqlock so core 0 can grab the latest one at any
// time without stalling the scanner.
#include <stdbool.h>
#include <stdint.h>
#include "sensor_map.h"

typedef struct {
    // Signed deflection from baseline for every sensor, indexed by linear grid
    // index (row*SENSOR_COLS + col). Positive = magnet pole approaching.
    float value[SENSOR_COUNT];
} sensor_frame_t;

// One-time hardware init: select-line GPIOs, ADC round-robin + FIFO, DMA channel.
void sensors_init(void);

// Establish the no-magnet baseline AND measure each sensor's noise sigma. Call
// with the pen away, before sensors_start(). Sigma is what every downstream
// threshold is expressed in, so this measurement is load-bearing.
void sensors_calibrate(uint32_t frames);

// Launch the continuous DMA scan on core 1. Call once, after init + calibrate.
void sensors_start(void);

// Copy the most recently completed frame. Returns false only if no frame has been
// published yet (before the first core-1 scan finishes).
bool sensors_get_latest(sensor_frame_t *out);

// Per-sensor noise sigma in ADC counts, measured by sensors_calibrate().
const float *sensors_sigma(void);

// Per-sensor baseline in ADC counts. raw = frame.value[i] + baseline[i].
const float *sensors_baseline(void);

// Measured core-1 frame rate (Hz), for the debug console.
float sensors_frame_rate(void);

// Ask core 1 to re-run baseline + sigma calibration at the next frame boundary.
// Safe to call from core 0; returns once the request is latched, not once done.
void sensors_request_recalibrate(void);

// Tell the scanner WHERE the pen is, so it can freeze baseline drift tracking
// for that neighbourhood and keep tracking everywhere else. Core 0 owns this
// decision — it is the only side that runs the coherence and hysteresis tests
// that make it trustworthy — and core 1 only consumes it. Call once per solve
// with whatever solve_pen_cell() returned.
//
// Freezing the whole grid instead lets thermal drift accumulate everywhere the
// pen is not, until some corner of it looks more like a pen than the pen does.
#define SENSORS_PEN_NONE   (-1)   // no pen: re-learn the zero at the idle rate
#define SENSORS_PEN_REZERO (-2)   // no pen, and a stale zero is suspected:
                                  // re-learn it fast (see BASELINE_ALPHA_REZERO)
void sensors_set_pen_cell(int cell);
