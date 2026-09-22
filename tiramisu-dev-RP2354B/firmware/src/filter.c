#include "filter.h"
#include <math.h>

// ---- Median stage -----------------------------------------------------------
// Per axis, over the last MEDIAN_TAPS samples. A lone outlier can never be the
// middle value of three, so it is deleted outright rather than smeared over a
// time constant the way a low-pass would smear it. Steady motion is untouched:
// the median of a monotonic ramp is its middle sample, so the only cost is
// (MEDIAN_TAPS-1)/2 samples of delay.
#if MEDIAN_TAPS > 1
static float median_of(const float *src, int n) {
    float v[MEDIAN_TAPS];
    for (int i = 0; i < n; i++) v[i] = src[i];
    for (int i = 1; i < n; i++) {              // insertion sort; n is 3 or 5
        const float k = v[i];
        int j = i - 1;
        while (j >= 0 && v[j] > k) { v[j + 1] = v[j]; j--; }
        v[j + 1] = k;
    }
    return v[n / 2];
}
#endif

static void median_reset(median_t *m) {
    m->n = 0;
    m->head = 0;
}

static void median_push(median_t *m, float x, float y, float *xo, float *yo) {
#if MEDIAN_TAPS == 1
    (void)m;
    *xo = x; *yo = y;
#else
    m->hx[m->head] = x;
    m->hy[m->head] = y;
    m->head = (m->head + 1) % MEDIAN_TAPS;
    if (m->n < MEDIAN_TAPS) m->n++;
    // Until the window fills, the median of what we have is still the right
    // answer — it just has fewer taps to reject with.
    *xo = median_of(m->hx, m->n);
    *yo = median_of(m->hy, m->n);
#endif
}

// ---- 1-Euro stage -----------------------------------------------------------
// Smoothing factor for a first-order low-pass at cutoff fc, sampled dt apart.
static inline float alpha_for(float cutoff_hz, float dt_s) {
    const float tau = 1.0f / (2.0f * 3.14159265358979f * cutoff_hz);
    return 1.0f / (1.0f + tau / dt_s);
}

static inline float lowpass(float x, float prev, float a) {
    return a * x + (1.0f - a) * prev;
}

static float axis_apply(one_euro_axis_t *ax, float x, const one_euro_t *f,
                        float dt_s) {
    if (!ax->primed) {
        ax->primed = true;
        ax->hatx = x;
        ax->hatdx = 0.0f;
        return x;
    }

    // 1. Estimate speed, itself low-passed so a single noisy sample cannot spike
    //    the cutoff and let jitter straight through.
    const float dx = (x - ax->hatx) / dt_s;
    ax->hatdx = lowpass(dx, ax->hatdx, alpha_for(f->dcutoff, dt_s));

    // 2. Cutoff rises with speed: still pen => heavy smoothing, fast pen => none.
    const float cutoff = f->mincutoff + f->beta * fabsf(ax->hatdx);
    ax->hatx = lowpass(x, ax->hatx, alpha_for(cutoff, dt_s));
    return ax->hatx;
}

void one_euro_init(one_euro_t *f, float rate_hz,
                   float mincutoff, float beta, float dcutoff) {
    f->rate_hz   = rate_hz;
    f->mincutoff = mincutoff;
    f->beta      = beta;
    f->dcutoff   = dcutoff;
    one_euro_reset(f);
}

void one_euro_reset(one_euro_t *f) {
    median_reset(&f->med);
    f->x.primed = false;
    f->y.primed = false;
    f->x.hatx = f->x.hatdx = 0.0f;
    f->y.hatx = f->y.hatdx = 0.0f;
}

void one_euro_apply(one_euro_t *f, float x_in, float y_in, float dt_s,
                    float *x_out, float *y_out) {
    // A stalled or resumed report chain can hand us an absurd interval; clamp it
    // to a sane band rather than let it drive the cutoffs to nonsense.
    const float nominal = 1.0f / f->rate_hz;
    if (!(dt_s > 0.0f) || !isfinite(dt_s)) dt_s = nominal;
    if (dt_s < 0.1f * nominal) dt_s = 0.1f * nominal;
    if (dt_s > 50.0f * nominal) dt_s = 50.0f * nominal;

    float mx, my;
    median_push(&f->med, x_in, y_in, &mx, &my);

    *x_out = axis_apply(&f->x, mx, f, dt_s);
    *y_out = axis_apply(&f->y, my, f, dt_s);
}
