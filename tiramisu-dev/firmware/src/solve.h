#pragma once
// Turn one frame of 120 deflections into a pen position.
#include <stdbool.h>
#include "sensors.h"

typedef struct {
    bool  in_range;   // a pen is on the tablet and its position is trustworthy
    float x_mm;       // 0 .. (SENSOR_COLS-1)*pitch, from grid origin
    float y_mm;       // 0 .. (SENSOR_ROWS-1)*pitch
    float z_mm;       // fitted magnet height (dipole fit only), else 0
    float peak;       // strongest deflection (ADC counts)
    float snr;        // peak / sigma at the peak sensor
    float coherence;  // 4-neighbour mean / peak; ~0 for noise, ~0.2-0.7 for a magnet
    float fit_rms;    // RMS fit residual in counts; the "is this a magnet" number
    bool  refined;    // true if the dipole fit converged and was used
    bool  outside;    // the fit placed the pen beyond the edge of the active area
    bool  blob;       // a magnet is influencing the grid, wherever it may be
} pen_pos_t;

// Clear hysteresis and tracking state (call when the pen leaves range, after a
// recalibration, or any time frame-to-frame continuity has been broken).
void solve_reset(void);

// Localise the pen.
//
// Presence is judged on per-sensor noise sigma, spatial coherence, fit quality
// and fitted height, with amplitude hysteresis AND a frame-count debounce on
// top. Position is a background-subtracted centroid refined by a warm-started,
// Huber-robust Levenberg-Marquardt dipole fit when DIPOLE_REFINE is enabled.
//
// in_range is the only output that matters to the caller: false means "do not
// move the cursor", covering pen absent, pen too high, pen off the edge of the
// active area, and pen present but not localisable.
void solve_position(const sensor_frame_t *f, pen_pos_t *out);

// Linear grid index of the cell the solver is currently tracking, or -1 if it is
// not tracking anything. Handed to sensors_set_pen_cell() so the scanner knows
// which neighbourhood to protect from baseline drift tracking.
int solve_pen_cell(void);
