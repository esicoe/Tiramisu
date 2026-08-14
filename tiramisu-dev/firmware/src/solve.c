// Pen localisation from a hall-grid frame.
//
// Stages:
//   1. Peak cell, with hysteresis. The analysis window hangs off this cell, so
//      letting it be re-decided from noise every frame injects pitch-sized steps
//      into everything downstream. It is held until a rival wins decisively.
//   2. Presence. The peak is the maximum over 120 channels, so a fixed count
//      threshold is unusable — the expected max of 120 zero-mean Gaussians is
//      already ~2.9 sigma. Presence is judged in units of each sensor's own
//      measured sigma AND by spatial coherence (a magnet lights up a smooth
//      blob; noise does not), with amplitude hysteresis and a frame debounce.
//   3. Centroid. Background-subtracted, positive-weighted, over a window around
//      the peak. Cheap, unbiased enough to seed the fit.
//   4. Dipole refine. Warm-started, Huber-robust Levenberg-Marquardt fit of the
//      on-axis dipole model. This is what buys sub-0.1 mm accuracy: the centroid
//      is biased by the field's negative skirt and by window truncation at the
//      edges of the grid, and the fit is immune to both because it models them.
//   5. Trust. The fit's residual says whether the blob really is a magnet, its
//      z says how high, and its x/y — which, unlike a centroid, are free to land
//      outside the grid — say whether the pen is still over the tablet at all.
//      Anything that fails becomes a bad frame, and a run of bad frames drops
//      the pen out of range rather than steering the cursor with a guess.
#include "solve.h"
#include "config.h"
#include <math.h>

#define FIT_MAX_SAMPLES ((2 * NEIGHBOURHOOD + 1) * (2 * NEIGHBOURHOOD + 1))

// ---- Tracking state ---------------------------------------------------------
// All of this exists to make the estimate CONTINUOUS from frame to frame. A
// stationary magnet produces a stationary field; every discrete decision the
// solver makes (which cell is the peak, which window, which local minimum the
// fit lands in) is a chance to convert that into movement, and each one has to
// be given memory so it stops re-deciding on noise.
static struct {
    bool  active;             // a blob is being tracked (peak hold + warm start)
    int   pc, pr;             // held peak cell
    bool  have_fit;           // A/x0/y0/z below came from a fit that passed
    float A, x0, y0, z;       // warm-start parameters
    float last_x, last_y;     // last position that passed every trust test
    float peak_ref;           // what this contact has been reading, slow-decaying
    bool  blob;               // last frame's "a magnet is affecting the grid"
    int   block;              // frames left refusing to acquire, after a release
    int   settle;             // frames since this contact began (prior ramp)
    int   good, bad;          // consecutive-frame debounce counters
    bool  in_range;           // debounced output state
} s_trk;

static inline float at(const sensor_frame_t *f, int c, int r) {
    return f->value[r * SENSOR_COLS + c];
}

void solve_reset(void) {
    s_trk.active   = false;
    s_trk.have_fit = false;
    s_trk.in_range = false;
    s_trk.good = 0;
    s_trk.bad  = 0;
    s_trk.pc = 0; s_trk.pr = 0;
    s_trk.settle = 0;
    s_trk.A = 0.0f; s_trk.x0 = 0.0f; s_trk.y0 = 0.0f; s_trk.z = 0.0f;
    s_trk.last_x = 0.0f; s_trk.last_y = 0.0f;
    s_trk.peak_ref = 0.0f;
    s_trk.block = 0;
    s_trk.blob = false;
}

// What the scanner should do with its baseline this frame, as a cell index or
// one of the SENSORS_PEN_* sentinels. The solver is the only side that knows
// whether it is holding on to something, or has just let go of something it did
// not trust — which is exactly the difference between "protect this" and
// "erase this".
int solve_pen_cell(void) {
    if (s_trk.block > 0) return SENSORS_PEN_REZERO;
    if (s_trk.blob)      return s_trk.pr * SENSOR_COLS + s_trk.pc;
    return SENSORS_PEN_NONE;
}

// ---- 4x4 symmetric positive-definite solve (Cholesky) -----------------------
static bool solve4(const float A[4][4], const float b[4], float x[4]) {
    float L[4][4] = {{0}};
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j <= i; j++) {
            float s = A[i][j];
            for (int k = 0; k < j; k++) s -= L[i][k] * L[j][k];
            if (i == j) {
                if (s <= 1e-9f) return false;      // not PD — caller keeps best
                L[i][i] = sqrtf(s);
            } else {
                L[i][j] = s / L[j][j];
            }
        }
    }
    float y[4];
    for (int i = 0; i < 4; i++) {
        float s = b[i];
        for (int k = 0; k < i; k++) s -= L[i][k] * y[k];
        y[i] = s / L[i][i];
    }
    for (int i = 3; i >= 0; i--) {
        float s = y[i];
        for (int k = i + 1; k < 4; k++) s -= L[k][i] * x[k];
        x[i] = s / L[i][i];
    }
    return true;
}

// ---- Dipole model -----------------------------------------------------------
// A magnet on the pen axis is, at these distances, well approximated by a point
// dipole with its moment along z. The z-component it produces at a sensor offset
// (dx,dy) in the sensor plane, with the magnet at height z:
//
//     B_z = A * (2z^2 - rho^2) / (rho^2 + z^2)^(5/2),   rho^2 = dx^2 + dy^2
//
// A folds in mu0*m/4pi and the sensor's counts-per-tesla, so it is fitted, not
// calibrated. Note B_z goes NEGATIVE beyond rho = z*sqrt(2) — that skirt is
// exactly what biases a naive centroid, and exactly what this model captures.
typedef struct { float dx, dy, v; } sample_t;
typedef struct { float A, x0, y0, z; } dipole_t;

static inline float dipole_eval(float A, float dx, float dy, float z,
                                float *d_dA, float *d_dx0,
                                float *d_dy0, float *d_dz) {
    const float rho2 = dx * dx + dy * dy;
    const float u    = rho2 + z * z;
    const float n    = 2.0f * z * z - rho2;
    const float u_hf = sqrtf(u);                   // u^(1/2)
    const float u2   = u * u;
    const float inv52 = 1.0f / (u2 * u_hf);        // u^(-5/2)
    const float inv72 = inv52 / u;                 // u^(-7/2)

    if (d_dA) {
        *d_dA  = n * inv52;
        *d_dx0 = A * dx * inv72 * (2.0f * u + 5.0f * n);
        *d_dy0 = A * dy * inv72 * (2.0f * u + 5.0f * n);
        *d_dz  = A * z  * inv72 * (4.0f * u - 5.0f * n);
    }
    return A * n * inv52;
}

// ---- Robust loss ------------------------------------------------------------
// Huber: quadratic inside `scale`, linear outside, so a cell that disagrees
// wildly contributes a bounded pull instead of dominating the normal equations.
// rho'(r) = 2*w*r, which is what makes `huber_w` the matching IRLS weight.
static inline float huber_rho(float r, float scale) {
    const float a = fabsf(r);
    return (a <= scale) ? (r * r) : (scale * (2.0f * a - scale));
}

static inline float huber_w(float r, float scale) {
    const float a = fabsf(r);
    return (a <= scale) ? 1.0f : (scale / a);
}

// The scale tracks the fit's OWN residual level rather than the sensor noise.
// It has to: near the peak the residual is dominated by the point-dipole model's
// mismatch with a real magnet, not by noise, so a noise-scaled threshold would
// down-weight precisely the cells carrying the position information.
static inline float huber_scale(float sse, int n, float floor_) {
#if FIT_ROBUST
    float s = FIT_HUBER_K * sqrtf(sse / (float)n);
    return (s < floor_) ? floor_ : s;
#else
    (void)sse; (void)n; (void)floor_;
    return 1e30f;                                  // never clips => plain L2
#endif
}

static float dipole_resid(const sample_t *s, int n, const dipole_t *p, float *r) {
    float sse = 0.0f;
    for (int i = 0; i < n; i++) {
        const float d = dipole_eval(p->A, s[i].dx - p->x0, s[i].dy - p->y0, p->z,
                                    NULL, NULL, NULL, NULL) - s[i].v;
        r[i] = d;
        sse += d * d;
    }
    return sse;
}

static float huber_cost(const float *r, int n, float scale) {
    float c = 0.0f;
    for (int i = 0; i < n; i++) c += huber_rho(r[i], scale);
    return c;
}

// ---- Priors on the nuisance parameters --------------------------------------
// A (magnet moment x sensor gain) and z (hover height) are physically almost
// constant from one millisecond to the next, but the fit re-derives them from
// noisy data every frame — and their noise does not stay put. The model's
// parameters are correlated: across a 10 mm pitch, a slightly higher magnet
// whose centre has shifted slightly produces very nearly the same profile, so
// error in z comes back out as error in y (or x). Position ends up paying for
// the uncertainty in a quantity nobody asked about.
//
// Tying A and z to the previous frame's converged values with a Gaussian prior
// spends that correlation the other way. x0 and y0 stay completely free, so
// there is no lag on real movement; only the nuisance parameters are slowed,
// and they are allowed to drift on a ~FIT_NUISANCE_PRIOR-frame time constant,
// which is far faster than a hand changes hover height.
//
// Measured on synthetic frames (10 mm pitch, peak 200 counts, sigma 2, magnet
// at 8 mm): stationary position noise 0.131 mm rms free, 0.058 mm rms tied.
typedef struct {
    float A, z;        // prior means: last frame's converged values
    float w;           // strength relative to the data's own curvature; 0 = off
    float wA, wz;      // resolved weights, computed on the first iteration
} prior_t;

static inline float prior_penalty(const prior_t *pr, const dipole_t *p) {
    if (pr->w <= 0.0f) return 0.0f;
    const float da = p->A - pr->A, dz = p->z - pr->z;
    return pr->wA * da * da + pr->wz * dz * dz;
}

// Levenberg-Marquardt on (A, x0, y0, z), seeded from *p in place.
// Returns false only if the result is numerically unusable; the caller decides
// whether a usable result is also a TRUSTWORTHY one, from *rms_out.
static bool dipole_fit(const sample_t *s, int n, dipole_t *p, int iters,
                       float resid_floor, prior_t *pr, float *rms_out) {
    float r[FIT_MAX_SAMPLES], rt[FIT_MAX_SAMPLES];

    float sse    = dipole_resid(s, n, p, r);
    float scale  = huber_scale(sse, n, resid_floor);
    float cost   = huber_cost(r, n, scale);
    float lambda = 1e-3f;

    for (int it = 0; it < iters; it++) {
        float JtJ[4][4] = {{0}};
        float Jtr[4] = {0};

        for (int i = 0; i < n; i++) {
            float g[4];
            const float m = dipole_eval(p->A, s[i].dx - p->x0, s[i].dy - p->y0,
                                        p->z, &g[0], &g[1], &g[2], &g[3]);
            const float ri = m - s[i].v;
            const float w  = huber_w(ri, scale);
            for (int a = 0; a < 4; a++) {
                Jtr[a] += w * g[a] * ri;
                for (int b = 0; b <= a; b++) JtJ[a][b] += w * g[a] * g[b];
            }
        }
        for (int a = 0; a < 4; a++)
            for (int b = a + 1; b < 4; b++) JtJ[a][b] = JtJ[b][a];

        // Fold in the priors on A and z. The weights are resolved once, against
        // the data's own curvature, so the strength is dimensionless and does
        // not depend on signal level; after that they are constant for this fit,
        // which is what keeps the accept/reject test below comparing like with
        // like. The seed's cost is topped up here for the same reason.
        if (pr->w > 0.0f) {
            if (it == 0) {
                pr->wA = pr->w * JtJ[0][0];
                pr->wz = pr->w * JtJ[3][3];
                cost += prior_penalty(pr, p);
            }
            Jtr[0]    += pr->wA * (p->A - pr->A);
            Jtr[3]    += pr->wz * (p->z - pr->z);
            JtJ[0][0] += pr->wA;
            JtJ[3][3] += pr->wz;
        }

        const float trace = JtJ[0][0] + JtJ[1][1] + JtJ[2][2] + JtJ[3][3];
        if (!(trace > 0.0f)) break;                // no information at all

        // Levenberg damping, scaled per-parameter so A (counts) and the geometry
        // (mm) stay comparably conditioned. The diagonal floor matters: a
        // parameter the data cannot see at all has JtJ[a][a] == 0, and damping
        // it by a multiple of zero leaves the row undamped and the whole matrix
        // singular — which used to abandon the fit and fall back to the
        // centroid, i.e. swap estimators mid-stroke.
        const float dfloor = 1e-6f * trace;
        float Adamped[4][4];
        for (int a = 0; a < 4; a++) {
            for (int b = 0; b < 4; b++) Adamped[a][b] = JtJ[a][b];
            const float d = (JtJ[a][a] > dfloor) ? JtJ[a][a] : dfloor;
            Adamped[a][a] += lambda * d;
        }

        const float rhs[4] = { -Jtr[0], -Jtr[1], -Jtr[2], -Jtr[3] };
        float step[4];
        if (!solve4(Adamped, rhs, step)) break;    // keep the best iterate

        // Trust region. One frame cannot move the magnet a whole pitch, so a
        // step that claims otherwise is the solver leaving the basin, not new
        // information — and it is what turns a marginal frame into a cursor
        // jump. Bounding it costs nothing when the fit is behaving.
        const float lim = SENSOR_PITCH_MM;
        for (int a = 1; a < 4; a++) {
            if (step[a] >  lim) step[a] =  lim;
            if (step[a] < -lim) step[a] = -lim;
        }
        const float alim = 0.5f * fabsf(p->A) + 1.0f;
        if (step[0] >  alim) step[0] =  alim;
        if (step[0] < -alim) step[0] = -alim;

        dipole_t trial = { p->A  + step[0], p->x0 + step[1],
                           p->y0 + step[2], p->z  + step[3] };
        if (trial.z < DIPOLE_Z_MIN_MM) trial.z = DIPOLE_Z_MIN_MM;
        if (trial.z > DIPOLE_Z_MAX_MM) trial.z = DIPOLE_Z_MAX_MM;

        const float tsse  = dipole_resid(s, n, &trial, rt);
        const float tcost = huber_cost(rt, n, scale)    // same scale => same cost
                          + prior_penalty(pr, &trial);

        if (tcost < cost) {                        // accept, trust the model more
            *p = trial;
            for (int i = 0; i < n; i++) r[i] = rt[i];
            sse   = tsse;
            scale = huber_scale(sse, n, resid_floor);   // reweight around the new fit
            cost  = huber_cost(r, n, scale) + prior_penalty(pr, p);
            lambda *= 0.4f;
            if (lambda < 1e-6f) lambda = 1e-6f;
        } else {                                   // reject, step more cautiously
            lambda *= 4.0f;
            if (lambda > 1e3f) break;              // wedged; keep what we have
        }
    }

    if (!isfinite(p->A) || !isfinite(p->x0) ||
        !isfinite(p->y0) || !isfinite(p->z)) return false;
    if (p->A <= 0.0f) return false;                // a magnet pushes the peak up

    *rms_out = sqrtf(sse / (float)n);
    return true;
}

// ---- Main entry -------------------------------------------------------------
void solve_position(const sensor_frame_t *f, pen_pos_t *out) {
    const float *sigma = sensors_sigma();

    // 1. Strongest cell this frame.
    int ac = 0, ar = 0;
    float amax = -1e30f;
    for (int r = 0; r < SENSOR_ROWS; r++)
        for (int c = 0; c < SENSOR_COLS; c++) {
            const float v = at(f, c, r);
            if (v > amax) { amax = v; ac = c; ar = r; }
        }

    // 2. Analysis centre, with hysteresis. While tracking, last frame's cell is
    //    kept unless this frame's argmax beats it by a margin that noise cannot
    //    manufacture. Real movement clears the margin on the first frame it
    //    matters; a magnet parked on the line between two sensors never does,
    //    which is precisely the case that used to oscillate.
    int pc, pr;
    if (!s_trk.active) {
        pc = ac; pr = ar;
    } else {
        const float held   = at(f, s_trk.pc, s_trk.pr);
        const float margin = PEAK_HOLD_MARGIN_SIGMA *
                             sigma[s_trk.pr * SENSOR_COLS + s_trk.pc];
        bool sw = (amax > held + margin);
        if (sw) {
            // A rival FAR from a held cell whose own signal has COLLAPSED is
            // not the pen moving — one magnet cannot jump several pitches in a
            // millisecond, so it is the pen LEAVING while a second source (a
            // leftover baseline image, another magnet) sits elsewhere. Never
            // walk over to it: hold the dying cell so the release fires, the
            // blind window opens, and the re-zero pass erases whatever the
            // rival actually is. Walking over is how a leftover image kept
            // presence alive through every release that should have killed it —
            // and, mid-game, how the cursor teleported to the image whenever
            // the real pen lifted between objects.
            const int dcol = (ac > s_trk.pc) ? ac - s_trk.pc : s_trk.pc - ac;
            const int drow = (ar > s_trk.pr) ? ar - s_trk.pr : s_trk.pr - ar;
            if ((dcol > 2 || drow > 2) && s_trk.in_range &&
                held < PRESENCE_RELEASE_FRAC * s_trk.peak_ref)
                sw = false;
        }
        if (sw) { pc = ac;        pr = ar;        }
        else    { pc = s_trk.pc;  pr = s_trk.pr;  }
    }
    s_trk.pc = pc;
    s_trk.pr = pr;

    const int   pidx   = pr * SENSOR_COLS + pc;
    const float peak   = at(f, pc, pr);
    const float sig_pk = sigma[pidx];

    out->peak    = peak;
    out->snr     = peak / sig_pk;
    out->refined = false;
    out->outside = false;
    out->z_mm    = 0.0f;
    out->fit_rms = 0.0f;
    out->x_mm    = pc * SENSOR_PITCH_MM;
    out->y_mm    = pr * SENSOR_PITCH_MM;

    // 3. Spatial coherence: mean of the 4-neighbourhood over the peak. Noise is
    //    spatially uncorrelated, so this is near zero for a false peak — and for
    //    a single drifted sensor, which is why it is also tested on the way out.
    float nsum = 0.0f;
    int   ncnt = 0;
    if (pc > 0)               { nsum += at(f, pc - 1, pr); ncnt++; }
    if (pc < SENSOR_COLS - 1) { nsum += at(f, pc + 1, pr); ncnt++; }
    if (pr > 0)               { nsum += at(f, pc, pr - 1); ncnt++; }
    if (pr < SENSOR_ROWS - 1) { nsum += at(f, pc, pr + 1); ncnt++; }
    const float coherence = (ncnt && peak > 0.0f) ? (nsum / ncnt) / peak : 0.0f;
    out->coherence = coherence;

    // 4. Amplitude + coherence, hysteretic on the debounced state.
    //
    // The exit test also asks whether the peak has collapsed relative to what
    // THIS contact has been reading. An absolute sigma threshold cannot answer
    // "has the pen gone" — it has to sit low enough for the weakest pen worth
    // seeing, which is far below anything a strong magnet decays to, and it lets
    // a drifted patch of grid hold presence forever. Comparing the pen against
    // itself needs no knowledge of how much the grid has drifted.
    bool good;
    if (s_trk.in_range)
        good = (peak > PRESENCE_EXIT_SNR * sig_pk) &&
               (peak > PRESENCE_MIN_COUNTS * 0.5f) &&
               (peak > PRESENCE_RELEASE_FRAC * s_trk.peak_ref) &&
               (coherence > COHERENCE_EXIT_MIN);
    else
        good = (peak > PRESENCE_ENTER_SNR * sig_pk) &&
               (peak > PRESENCE_MIN_COUNTS) &&
               (coherence > COHERENCE_MIN);

    // Blind window after a release, while the scanner's ungated re-zero pass
    // erases whatever the grid still carries. Flat refusal, no exceptions: a
    // baseline image of the pen is at the pen's FULL amplitude, so any test
    // lenient enough to admit a returning pen admits the image too — which then
    // freezes its own neighbourhood and cancels the re-zero that would have
    // erased it (see PRESENCE_REACQUIRE_BLOCK_MS in config.h).
    if (s_trk.block > 0) {
        s_trk.block--;
        good = false;
    }

    // Reported before the fit and area tests narrow it further: this is "a
    // magnet is affecting the grid", which is the question baseline drift
    // tracking needs answered. A pen hovering just off the edge is not in range
    // but must still freeze the baseline, or the cells nearest it quietly
    // absorb its field while it sits there.
    out->blob = good;
    s_trk.blob = good;

    float x_mm = out->x_mm, y_mm = out->y_mm;

    if (good) {
        // 5. Window around the peak, clamped to the grid.
        int c0 = pc - NEIGHBOURHOOD, c1 = pc + NEIGHBOURHOOD;
        int r0 = pr - NEIGHBOURHOOD, r1 = pr + NEIGHBOURHOOD;
        if (c0 < 0) c0 = 0;
        if (r0 < 0) r0 = 0;
        if (c1 >= SENSOR_COLS) c1 = SENSOR_COLS - 1;
        if (r1 >= SENSOR_ROWS) r1 = SENSOR_ROWS - 1;

        // 6. Centroid seed. Weight only what is clearly above noise, so the
        //    negative skirt and near-zero cells cannot drag the estimate. The
        //    floor is the larger of a fraction of the peak and a noise multiple:
        //    a purely peak-relative floor lets a noisy peak decide which cells
        //    count, which is one more way for a still pen to move.
        float wfloor = 0.25f * peak;
        if (wfloor < 3.0f * sig_pk) wfloor = 3.0f * sig_pk;

        float sw = 0.0f, sx = 0.0f, sy = 0.0f;
        for (int r = r0; r <= r1; r++)
            for (int c = c0; c <= c1; c++) {
                const float w = at(f, c, r) - wfloor;
                if (w <= 0.0f) continue;
                sw += w;
                sx += w * c;
                sy += w * r;
            }

        if (sw > 0.0f) {
            x_mm = (sx / sw) * SENSOR_PITCH_MM;
            y_mm = (sy / sw) * SENSOR_PITCH_MM;
        } else {
            x_mm = pc * SENSOR_PITCH_MM;
            y_mm = pr * SENSOR_PITCH_MM;
        }

#if DIPOLE_REFINE
        // 7. Refine against the dipole model over the same window.
        sample_t s[FIT_MAX_SAMPLES];
        int ns = 0;
        for (int r = r0; r <= r1; r++)
            for (int c = c0; c <= c1; c++) {
                s[ns].dx = c * SENSOR_PITCH_MM;
                s[ns].dy = r * SENSOR_PITCH_MM;
                s[ns].v  = at(f, c, r);
                ns++;
            }

        if (ns >= 8) {
            // Warm start from last frame's solution when it is still nearby, so
            // the fit continues one trajectory instead of re-deriving a slightly
            // different answer every millisecond. Cold start (from the centroid,
            // with a bigger iteration budget) only when the pen has evidently
            // been picked up and put down somewhere else.
            dipole_t p;
            prior_t  pr = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
            int iters;
            if (s_trk.have_fit &&
                fabsf(x_mm - s_trk.x0) <= WARM_START_MAX_MM &&
                fabsf(y_mm - s_trk.y0) <= WARM_START_MAX_MM) {
                p.A = s_trk.A; p.x0 = s_trk.x0; p.y0 = s_trk.y0; p.z = s_trk.z;
                iters = DIPOLE_ITERS;
                // Tie A and z to where they were; leave x0/y0 free — but only
                // once they have had a free window to converge in. A and z are
                // strongly correlated, so the fit descends a narrow valley and
                // needs a few frames' worth of iterations (the warm start
                // carries the progress forward) to reach the floor of it. Any
                // prior applied before then, even a weak ramped one, holds the
                // fit up on the valley wall and the position error it implies
                // bleeds off over a visible fraction of a second. So: off, then
                // on, at a settle time chosen to be far longer than convergence
                // takes. Switching it on is not a step in the output, because by
                // then there is nothing left for it to pull against.
                pr.A = s_trk.A; pr.z = s_trk.z;
                pr.w = (s_trk.settle >= FIT_PRIOR_SETTLE_FRAMES)
                           ? FIT_NUISANCE_PRIOR : 0.0f;
            } else {
                p.z  = SENSOR_PITCH_MM;
                p.A  = peak * p.z * p.z * p.z * 0.5f;   // model is 2A/z^3 at rho=0
                p.x0 = x_mm; p.y0 = y_mm;
                iters = DIPOLE_ITERS_COLD;
                // No prior on a cold start: there is nothing yet to tie A and z
                // to, and the fit needs them free to find the magnet's height.
            }

            float rms = 0.0f;
            if (dipole_fit(s, ns, &p, iters, 0.5f * sig_pk, &pr, &rms)) {
                // Once the fit is numerically valid it IS the estimator; the
                // centroid stays a seed. Picking between the two per frame on a
                // quality score would feed their difference — the skirt and
                // truncation bias the fit exists to remove — straight into the
                // output as movement, which is the same class of bug as letting
                // the peak cell be re-decided every frame.
                x_mm = p.x0;
                y_mm = p.y0;
                out->fit_rms = rms;
                out->z_mm    = p.z;
                out->refined = true;

                // Does the dipole model actually describe this blob? Noise term
                // for a weak pen, model-mismatch term for a strong one. Failing
                // this does not change WHICH estimate is used, only whether the
                // frame is trusted at all.
                const float gate = FIT_MAX_RESID_SIGMA * sig_pk +
                                   FIT_MAX_RESID_FRAC * peak;
                if (rms <= gate) {
                    s_trk.A = p.A; s_trk.x0 = p.x0;
                    s_trk.y0 = p.y0; s_trk.z = p.z;
                    s_trk.have_fit = true;
                } else {
                    good = false;
                    s_trk.have_fit = false;   // do not warm-start off a bad fit
                }

                // Too high to localise usefully. Amplitude falls as 1/z^3 so
                // PRESENCE_EXIT_SNR normally trips first; this catches a strong
                // magnet held well above the surface, which clears the amplitude
                // bar with a blob too broad to place.
                if (p.z > HOVER_MAX_Z_MM) good = false;
            } else {
                good = false;                 // fit numerically unusable
                s_trk.have_fit = false;
            }
        }
#endif

        // 8. Off the tablet? Only the fit can tell us: it is free to place the
        //    magnet outside the grid, whereas a centroid is trapped inside it
        //    and would instead smear along the border while the pen wanders
        //    around off-surface — the cursor "glitching around" at the edge.
        const float xmax = (SENSOR_COLS - 1) * SENSOR_PITCH_MM;
        const float ymax = (SENSOR_ROWS - 1) * SENSOR_PITCH_MM;
        if (x_mm < -AREA_MARGIN_MM || x_mm > xmax + AREA_MARGIN_MM ||
            y_mm < -AREA_MARGIN_MM || y_mm > ymax + AREA_MARGIN_MM) {
            out->outside = true;
            good = false;
        }

        // 9. Clamp into the reportable area.
        if (x_mm < 0.0f) x_mm = 0.0f;
        if (y_mm < 0.0f) y_mm = 0.0f;
        if (x_mm > xmax) x_mm = xmax;
        if (y_mm > ymax) y_mm = ymax;
    }

    // 10. Debounce. A single bad frame inside a contact must not move the
    //     cursor and must not drop the pen; it reports the last trusted
    //     position and waits. Only a sustained run of bad frames ends the
    //     contact, and only a sustained run of good ones starts it.
    if (good) {
        // Reference level for the relative release test: a symmetric average of
        // what this contact has been reading, over PEAK_REF_TAU_S.
        //
        // Symmetric, not a running maximum. The peak legitimately swings by 2-3x
        // as the magnet crosses between sensors — it is largest directly over
        // one and smallest halfway between, and the swing grows the closer the
        // pen is — so a reference pinned to the maximum would sit near the top
        // of that swing and the bottom of it would look like a departure. An
        // average sits in the middle of the swing instead, which leaves the
        // release threshold a wide margin below anything a present pen does,
        // while a pen that has actually gone still collapses through it in
        // milliseconds.
        if (!s_trk.active) {
            s_trk.peak_ref = peak;
        } else {
            const float a = 1.0f / (PEAK_REF_TAU_S * (float)REPORT_HZ);
            s_trk.peak_ref += a * (peak - s_trk.peak_ref);
        }
        s_trk.good++;
        s_trk.bad = 0;
        s_trk.active = true;
        s_trk.last_x = x_mm;
        s_trk.last_y = y_mm;
        if (s_trk.settle < FIT_PRIOR_SETTLE_FRAMES) s_trk.settle++;
    } else {
        s_trk.bad++;
        s_trk.good = 0;
        x_mm = s_trk.last_x;
        y_mm = s_trk.last_y;
        if (s_trk.bad >= PRESENCE_EXIT_FRAMES) {
            s_trk.active   = false;
            s_trk.have_fit = false;
            s_trk.settle   = 0;
        }
    }

    if (!s_trk.in_range) {
        if (s_trk.good >= PRESENCE_ENTER_FRAMES) s_trk.in_range = true;
    } else {
        if (s_trk.bad >= PRESENCE_EXIT_FRAMES) {
            s_trk.in_range = false;
            s_trk.block = (PRESENCE_REACQUIRE_BLOCK_MS * REPORT_HZ) / 1000;
        }
    }

    out->in_range = s_trk.in_range;
    out->x_mm = x_mm;
    out->y_mm = y_mm;
}
