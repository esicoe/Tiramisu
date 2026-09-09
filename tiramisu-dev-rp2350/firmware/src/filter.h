#pragma once
// Output conditioning for the reported pen position: a median stage to delete
// impulses, then a 1-Euro filter to smooth what is left.
//
// The order matters. A 1-Euro filter (Casiez, Roussel & Vogel, CHI 2012) adapts
// its cutoff to the measured speed — nearly still pen, low cutoff, jitter
// crushed; fast pen, high cutoff, lag approaches zero — which is exactly the
// trade osu! needs, since lag-at-speed is the part that must not be compromised.
// But that same adaptivity makes it the wrong tool for impulsive glitches: a
// one-frame jump reads as enormous speed, so the filter OPENS in response to a
// glitch and passes the next one straight through. Deleting impulses first, with
// a median, leaves the 1-Euro stage a clean signal to work on.
#include <stdbool.h>
#include "config.h"

#if (MEDIAN_TAPS != 1) && (MEDIAN_TAPS != 3) && (MEDIAN_TAPS != 5)
#error "MEDIAN_TAPS must be 1 (off), 3 or 5"
#endif

typedef struct {
    float hx[MEDIAN_TAPS];
    float hy[MEDIAN_TAPS];
    int   n;                 // samples held, saturating at MEDIAN_TAPS
    int   head;
} median_t;

typedef struct {
    float hatx;      // filtered value
    float hatdx;     // filtered derivative
    bool  primed;
} one_euro_axis_t;

typedef struct {
    median_t med;
    one_euro_axis_t x, y;
    float mincutoff, beta, dcutoff;
    float rate_hz;   // nominal rate; only a fallback when dt is not usable
} one_euro_t;

void one_euro_init(one_euro_t *f, float rate_hz,
                   float mincutoff, float beta, float dcutoff);

// Drop all history — call whenever the pen leaves range so the next contact does
// not get dragged toward the old position.
void one_euro_reset(one_euro_t *f);

// Median, then smooth. dt_s is the interval since the previous call; pass <= 0
// to fall back to the nominal rate. Using the measured interval matters because
// the report chain is driven by the host's polling, not by a local timer, so the
// spacing is only nominally 1 ms — and every cutoff in here is computed from it.
void one_euro_apply(one_euro_t *f, float x_in, float y_in, float dt_s,
                    float *x_out, float *y_out);
