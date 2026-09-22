#pragma once
// aim1k tablet firmware — build-time configuration.

// ---- USB identity -----------------------------------------------------------
// 0x1209/0x0001 is the pid.codes prototype VID/PID. Register a real PID before
// distributing. The OpenTabletDriver config (opentabletdriver/aim1k.json) MUST
// match these two values.
#define USB_VID           0x1209
#define USB_PID           0x0001
#define USB_MANUFACTURER  "aim1k"
#define USB_PRODUCT       "aim1k tablet"

// ---- Active area ------------------------------------------------------------
// Sensor-centre span is 110 x 90 mm. We report absolute coordinates in units of
// 0.01 mm so OpenTabletDriver sees Width=110 mm, Height=90 mm cleanly.
#define AREA_W_MM         110.0f
#define AREA_H_MM         90.0f
#define LOGICAL_MAX_X     11000   // 110.00 mm
#define LOGICAL_MAX_Y     9000    // 90.00 mm
#define LOGICAL_MAX_P     4095    // pressure (stubbed; 12-bit)

// ---- Report cadence ---------------------------------------------------------
// 1000 Hz is the CEILING here, not a choice. USB Full-Speed has a 1 ms frame and
// bInterval=1 is the minimum, so one interrupt-IN transfer per millisecond is all
// the bus allows. Beating it needs High-Speed microframes (125 us), and the
// RP2350/RP2354B USB controller is Full-Speed only. Reports are driven from
// tud_hid_report_complete_cb (see main.c) so the cadence is locked to the host's
// actual polling instead of a free-running timer that beats against it.
#define REPORT_HZ         1000    // USB Full-Speed 1 ms interval
#define HID_POLL_MS       1

// ---- HID personality --------------------------------------------------------
// The device exposes TWO top-level collections on one interface:
//
//   REPORT_ID_MOUSE  absolute mouse  -> Windows moves the cursor with no driver,
//                                       mapped to the whole virtual desktop.
//   REPORT_ID_PEN    vendor-defined  -> OpenTabletDriver reads this one.
//
// Why two. A Digitizer/Pen descriptor drags in Windows Ink AND makes Windows
// open the collection EXCLUSIVELY, so OTD gets ACCESS_DENIED - that is a hard
// wall, not a config problem. A mouse collection gives driverless cursor control
// with no ink stack anywhere, but Windows claims it exclusively too. Windows
// claims collections individually, though, so the vendor collection next to it
// stays open for OTD. This is the same split commercial tablets ship.
//
// Only one may drive the cursor at a time or they fight, so mouse reports are
// switched off when OTD takes over, via a feature report on REPORT_ID_MODE
// (byte 0: 1 = mouse on, 0 = mouse off). OTD sends it through FeatureInitReport
// in aim1k.json; the CDC console 'm' command toggles it as a fallback.
#define MOUSE_DEFAULT_ON  1     // plug-and-play out of the box

// Set to 1 to drop the mouse collection entirely (vendor-only build).
#define VENDOR_ONLY       0

// ---- Acquisition ------------------------------------------------------------
// After changing the shared address bus, every mux COM line has to settle before
// the ADC front-end sees a valid level. Tune on a scope (bring-up step 3).
#define ADC_SETTLE_US     5

// The RP2350 has ONE ADC behind an 8-input front mux. Switching inputs at
// 500 kSa/s with the CD74HC4067's Ron (~200 ohm at 3V3) in series does not give
// the sample-and-hold cap time to fully charge, so each input is contaminated by
// the previous one — classic round-robin crosstalk, and a prime suspect for
// "the grid reads wrong". Run the round-robin TWICE per address and keep only
// the second pass: the first pass leaves each S&H already near its final value.
// Costs 16 us/address; a full frame is still ~555 us => ~1.8 kHz.
#define ADC_WARMUP_PASS   1

// Frames averaged per published frame. 1 is usually right now that the warm-up
// pass does the heavy lifting; raise it only if noise is still the limit.
#define OVERSAMPLE        1

// Sliding boxcar over the last N PUBLISHED frames. This is nearly free noise
// reduction: core 1 scans at ~1.8 kHz but core 0 only consumes at 1 kHz, so
// ~45% of frames were being computed and then discarded. Averaging N of them
// divides the noise by sqrt(N) and costs only (N-1)/2 frame times of latency
// (~0.28 ms at N=2), because the window SLIDES — every scan still publishes.
// Raising OVERSAMPLE instead would buy the same sqrt(N) but at N times the
// frame period, which is real latency. Prefer this. 1 disables it.
#define FRAME_AVG         2

// ---- Noise model ------------------------------------------------------------
// Thresholds are expressed in units of each sensor's OWN measured noise sigma,
// not in raw counts. A fixed count threshold cannot work here: presence tests the
// MAXIMUM over 120 channels, and the expected max of 120 zero-mean Gaussians is
// already ~2.9 sigma, so any fixed threshold near the single-channel noise floor
// latches on permanently (which is exactly what the first build did).
#define NOISE_CAL_FRAMES  256     // frames used to measure per-sensor sigma
#define SIGMA_FLOOR       0.75f   // counts; guards against a sensor reading 0 noise

// ---- Presence detection -----------------------------------------------------
// Hysteresis: acquire on a strong peak, hold until it decays well below that, so
// the pen does not flicker in and out at the edge of range.
#define PRESENCE_ENTER_SNR 8.0f   // peak must exceed this * sigma to acquire
#define PRESENCE_EXIT_SNR  4.0f   // ...and fall below this to release
#define PRESENCE_MIN_COUNTS 6.0f  // absolute floor regardless of sigma

// Spatial coherence: a real magnet lights up its neighbours, uncorrelated noise
// (and a single drifted or stuck sensor) does not. The 4-neighbour mean must be
// at least this fraction of the peak.
//
// 0.18 was far too high for a 10 mm pitch and is the reason for a whole class of
// dropout. The premise behind it — that a magnet "spans 2-3 pitches" — is false
// here: the blob's width is set by the hover height, and the neighbours at 10 mm
// are out in the dipole's NEGATIVE skirt (which begins at rho = z*sqrt(2))
// whenever the pen is low. Coherence for a magnet parked directly over a sensor,
// against hover height:
//
//     z (mm)      4   6   8  10  12  14  16
//     point dip  -.02 -.01 .02 .09 .18 .27 .35
//     6mm cyl    -.01  .03 .10 .19 .28 .37 .45
//
// So at 0.18 the pen simply does not exist below ~10-12 mm — and, worse, as it
// slides across the tablet at a fixed height the coherence swings between the
// over-a-sensor value and the roughly 0.2-higher between-sensors value, crossing
// the threshold once per 10 mm of travel. That is presence flickering on and off
// with a period equal to the sensor pitch, each flicker costing a filter reset.
//
// Coherence is kept only as a cheap pre-filter against a lone hot cell. The real
// "is this a magnet" test is now the dipole fit's residual (FIT_MAX_RESID_*),
// which is a statement about the whole blob's shape and does not care how wide
// it is.
//
// NEGATIVE, and it has to be. Coherence is a monotonically DECREASING function
// of how close the pen is, so any positive threshold is really a minimum hover
// height in disguise — press the pen down harder and the tablet cuts out, which
// is the exact opposite of what a tablet should do. Measured on a left-edge drag
// with this fixed at 0.05 / 0.02: 100% usable at 7 mm, 99.8% at 6 mm, and 79%
// at 5 mm with presence chattering 213 times in four seconds. Every one of those
// failures was the coherence test; amplitude, residual, height and area never
// fired. The floor now sits below what a magnet produces at ~3 mm, i.e. below
// anything the 10 mm pitch can localise anyway, so it can no longer be the
// binding constraint at any usable height.
#define COHERENCE_MIN     -0.10f

// Coherence is also tested on the way OUT, at a relaxed bar. Testing it only on
// entry (as this originally did) leaves a trap: one sensor that has drifted past
// the baseline tracker's reach looks like a permanent strong peak, so once the
// tablet is tracking it never drops below the exit threshold, presence latches
// on forever, and — because presence freezes baseline tracking (see below) — the
// drift can never be corrected either. A lone drifted cell has no coherent
// neighbours, so this test breaks that deadlock.
#define COHERENCE_EXIT_MIN -0.25f

// Temporal debounce, in report iterations (~1 kHz), on top of the amplitude
// hysteresis. Amplitude hysteresis alone still chatters when the pen hovers
// right at the edge of range, and every chatter costs a filter reset and a
// cursor jump. Requiring N consecutive agreeing frames turns that into one
// clean transition. Exit is deliberately much longer than enter: a few ms of
// "still here" after the pen has really gone is invisible, whereas dropping out
// for one frame mid-stroke is not.
#define PRESENCE_ENTER_FRAMES 3
#define PRESENCE_EXIT_FRAMES  12

// Release when the peak collapses to this fraction of what THIS CONTACT has
// been reading, independent of any absolute threshold.
//
// An absolute threshold cannot answer "has the pen gone", because it has to be
// set low enough for the weakest pen the tablet should still see, which leaves
// it far below anything a strong magnet decays to. Measured here: a pen reading
// 865 counts against PRESENCE_EXIT_SNR * sigma = 16 counts would have to move
// nearly 4x further away before the absolute test let go — and a drifted patch
// of 45 counts held presence indefinitely.
//
// The pen's own signal is the right reference. 45 counts is 5% of 865: not this
// pen, at any drift level, without needing to know anything about drift. The
// reference decays slowly (PEAK_REF_TAU_S) so a genuine slow lift is tracked
// rather than treated as a departure — for that case HOVER_MAX_Z_MM is the gate
// that fires, which is the correct one, since it is a statement about height.
// 0.25 rather than something tighter because the reference decays (below): any
// hover height held for longer than PEAK_REF_TAU_S simply becomes the new
// reference, so this only ever fires on a change faster than that — which is
// what lifting a pen off a tablet is, and what sensor drift never is.
#define PRESENCE_RELEASE_FRAC 0.25f
#define PEAK_REF_TAU_S        2.0f

// After a release, refuse to acquire ANYTHING for this long while the ungated
// re-zero pass erases whatever the grid still carries.
//
// Flat and unconditional, deliberately. An earlier revision let strong peaks
// through the window on the reasoning that a leftover is "a fraction of what
// was just released" — but a baseline IMAGE of the pen is at the pen's FULL
// amplitude, indistinguishable from the pen coming back down by amplitude or by
// position. Any exception wide enough to admit a returning pen admits the
// image, which then freezes its own neighbourhood and cancels the very re-zero
// that would have erased it. So: nothing acquires, for a window long enough
// that nothing static survives it.
//
// This is also the one test that separates a pen from every artifact in
// principle: a pen ARRIVES, an artifact is already there. The cost is a pen
// put back down within a quarter second of a full lift-away waits out the
// window — on a hover-only tablet, where release means the pen went well
// beyond tracking height, that is a rare and deliberate motion.
#define PRESENCE_REACQUIRE_BLOCK_MS 250

// ---- Baseline tracking ------------------------------------------------------
// Slow IIR that follows thermal drift.
//
// The time constant is tau = 1 / (alpha * scan_rate). At ~1.8 kHz, the old
// 0.0005 gave tau = 1.1 SECONDS — not a drift tracker at all but a 0.14 Hz
// high-pass sitting directly under the pen. Rest the magnet in one place and
// within about a second the baseline crawls up to meet the field, the blob's
// skirt gets eaten first (it is the shallowest part), the blob changes shape
// underneath the solver, and the reported position creeps and jumps. 3.0e-5
// gives tau ~ 18 s, which is far slower than any pen movement and still fast
// enough for thermal drift.
#define BASELINE_ALPHA    3.0e-5f

// Freeze baseline tracking only for cells NEAR THE PEN, in cells of radius.
//
// Freezing the whole grid instead — which is what this did first — is what
// produced the phantom-pen bug: with a magnet resting on the tablet the entire
// baseline stopped updating, so thermal drift accumulated uncorrected
// everywhere, and after a while a corner of the grid had drifted into a smooth
// 45-count bump. That bump passes every test there is. It is 11 sigma, its
// neighbours drift with it so it is spatially coherent, and being smooth it fits
// the dipole model with a small residual — so lifting the real magnet away just
// moved the argmax onto it and the tablet reported a pen there forever.
//
// A magnet only influences its own neighbourhood, so only that neighbourhood
// needs protecting. The radius must exceed NEIGHBOURHOOD, so that every cell the
// fit actually looks at is frozen; cells beyond it may absorb the far skirt, but
// the fit never sees them and a cell drifting toward zero cannot invent a peak.
// Everything else on the grid keeps tracking drift the whole time the pen is
// down, which is what stops the bump forming in the first place.
#define BASELINE_FREEZE_RADIUS (NEIGHBOURHOOD + 1)

// Baseline rate while NO pen is being tracked. ~0.5 s time constant, so drift
// and small residues are erased quickly between contacts.
#define BASELINE_ALPHA_IDLE 1.3e-3f

// Per-cell gate, in units of each sensor's sigma: deflections INSIDE the gate
// are drift and may be tracked; deflections BEYOND it are a FIELD and must
// never be absorbed, at any rate, in any state (the post-release re-zero pass
// below is the single deliberate exception).
//
// This gate existed in the original firmware and was removed in the jitter
// rework on the argument that "no pen detected means nothing to protect". That
// argument is false, and the failure it permits was captured live: no pen
// detected does NOT mean no field present, because the solver only looks for
// POSITIVE peaks. Rest the pen on the tablet flipped, or on its side — the
// natural way to put a pen down — and its NEGATIVE field is invisible to
// presence, so the ungated idle rate absorbed all -900 counts of it into the
// baseline within a couple of seconds. Pick the pen up and a perfect POSITIVE
// 900-count dipole image appears out of the corrected-for field, gets acquired
// as a pen, freezes its own neighbourhood, and sits there forever: a phantom at
// full pen amplitude, logged holding peak ~900 / z 12.0 for five unbroken
// minutes with no magnet anywhere near the tablet.
//
// Drift creeps continuously, so a tracking baseline never legitimately sees a
// deflection outside a few sigma; 4 sigma (~16 counts here) passes drift and
// blocks every magnet orientation at every height worth worrying about.
#define BASELINE_GATE_SNR 4.0f

// Baseline rate during the blind window after a release (see
// PRESENCE_REACQUIRE_BLOCK_MS). This pass is UNGATED — it is the one designated
// eraser of large leftovers (a stale image, the hole left by a boot that
// calibrated with the magnet on, drift accumulated under a long-parked pen) —
// which is also what makes every corruption self-healing: whatever garbage is
// on the grid, one ordinary pen contact and lift wipes it. ~30 ms time
// constant, so even a full-amplitude 900-count image is below the gate within
// the window with two orders of magnitude to spare.
#define BASELINE_ALPHA_REZERO 2.0e-2f

// ---- Localisation -----------------------------------------------------------
#define NEIGHBOURHOOD     2       // half-width of the fit window (2 => 5x5)

// Hysteresis on the PEAK CELL, in sigma. This is the single most important knob
// for a stationary pen.
//
// Everything downstream — the analysis window, the centroid, the samples handed
// to the fit — hangs off which cell is the argmax, and the argmax is a discrete
// choice made from noisy data. Park the magnet halfway between two sensors and
// the two cells read within noise of each other, so the argmax flips back and
// forth at random. Each flip slides the 5x5 window a full 10 mm pitch, swapping
// which skirt cells are inside it, which moves the centroid and hands the fit a
// different sample set. The result is a cursor that snaps between two spots
// along one axis while the magnet has not moved at all — exactly the "glitches
// side to side or up and down" symptom, and why the glitch is axis-aligned.
//
// So: keep last frame's cell until a rival beats it by this many sigma. Genuine
// motion clears that bar instantly; noise never does. A late switch costs
// nothing because the dipole fit models window truncation anyway.
#define PEAK_HOLD_MARGIN_SIGMA 2.5f

// Refine the centroid with a Levenberg-Marquardt fit of the on-axis dipole model
// (see solve.c). The centroid alone is biased near the edges and by the field's
// negative skirt; the fit removes both. Falls back to the centroid whenever the
// fit fails to converge or lands outside the window.
#define DIPOLE_REFINE     1
#define DIPOLE_ITERS      8       // warm-started (tracking) iteration budget
#define DIPOLE_ITERS_COLD 16      // first frame of a contact, from the centroid
#define DIPOLE_Z_MIN_MM   3.0f
#define DIPOLE_Z_MAX_MM   30.0f

// Warm start: seed each fit from the previous frame's converged parameters
// rather than from the centroid every time. A 4-parameter fit re-seeded from
// scratch can settle into a slightly different local minimum from one frame to
// the next, and the difference shows up as position noise that no amount of
// output filtering can tell from real movement. Continuing the same solution
// makes the estimate temporally coherent and converges in fewer iterations.
// Dropped (cold restart) if the centroid has moved further than this from the
// previous solution, i.e. the pen was picked up and put down somewhere else.
#define WARM_START_MAX_MM 12.0f

// Strength of the Gaussian priors tying the fit's two NUISANCE parameters —
// magnet strength A and hover height z — to their previous values, expressed as
// a multiple of the data's own curvature. The pen's position stays completely
// free, so this adds no lag to movement whatsoever.
//
// This is the largest single win for a stationary pen. A 4-parameter fit re-run
// from scratch every millisecond spends real information estimating two
// quantities that physically cannot change at 1 kHz, and because the parameters
// are correlated (over a 10 mm pitch a slightly higher magnet looks much like a
// slightly displaced one) their noise reappears as position noise. Measured on
// synthetic frames: 0.131 mm rms free, 0.058 mm rms tied — a 2.3x reduction, at
// zero latency cost.
//
// The value is roughly the time constant in frames over which A and z may still
// drift: 30 at 1 kHz lets hover height follow a hand over ~30 ms. Raising it
// steadies a stationary pen further but makes the fit slower to notice a genuine
// change in height; 0 disables the priors and restores the free 4-parameter fit.
#define FIT_NUISANCE_PRIOR 30.0f

// Frames over which that prior is ramped in at the start of each contact. A and
// z are correlated enough that the fit descends a narrow valley and needs a few
// frames' worth of iterations to reach the bottom; clamping them from frame two
// instead freezes a still-converging answer, and the resulting position error
// then bleeds off over a visible fraction of a second as the prior slowly gives
// way. Ramping costs nothing — the pen is barely down yet.
#define FIT_PRIOR_SETTLE_FRAMES 40

// Robust (Huber) reweighting inside the fit. One misbehaving sensor in the
// window — mux crosstalk, a stuck channel, a cell whose baseline is stale —
// otherwise drags a plain least-squares fit toward itself. The scale adapts to
// the fit's own residual level, so it down-weights genuine outliers without
// punishing the peak cells, where the residual is dominated by the point-dipole
// model's mismatch with a real magnet rather than by noise.
#define FIT_ROBUST        1
#define FIT_HUBER_K       2.5f

// Fit-quality gate. The RMS residual has to be explainable as noise plus a
// bounded amount of model mismatch, or the blob is not a magnet and the
// position is meaningless. Two terms because the two regimes are different: for
// a distant/weak pen the residual is all noise (sigma term), for a close strong
// one it is all model mismatch (fraction-of-peak term). This is the check that
// catches "something is lighting up the grid but it is not a pen on it".
//
// IF THE TABLET STOPS GOING IN RANGE, THIS IS THE FIRST KNOB TO LOOSEN. The
// fraction term has to absorb everything the point-dipole model does not
// describe — a magnet that is not point-like at this height, tilt, and above
// all the HAL403's part-to-part sensitivity spread, which shows up as a fixed
// per-cell gain error the fit cannot represent. The console's 'i' prints the
// live `rms` next to `peak`; if rms/peak sits near this number with a pen
// plainly on the tablet, raise it.
#define FIT_MAX_RESID_SIGMA 5.0f
#define FIT_MAX_RESID_FRAC  0.25f

// Hover ceiling, in mm, applied to the fitted magnet height. Peak amplitude
// falls as 1/z^3 so PRESENCE_EXIT_SNR usually trips first; this is the direct
// geometric statement of the same limit and it catches the case where a strong
// magnet held high still clears the amplitude bar while its position estimate
// has gone soft. Raise it if a genuinely high hover drops out.
#define HOVER_MAX_Z_MM    25.0f

// How far outside the sensor-centre span the fit is allowed to place the pen
// before it is treated as OFF THE TABLET rather than at its edge.
//
// The fit is unconstrained in x/y, which is what makes this test possible: move
// the magnet past the last column and the peak stays pinned to that column, but
// the fit — which knows what the field's shape should look like — reports a
// position out beyond the edge. A centroid can never do this; it is trapped
// inside the grid, which is why a centroid-only tablet smears and jitters along
// its border instead of admitting the pen has left. Half a pitch of margin
// keeps the real edge fully usable.
#define AREA_MARGIN_MM    5.0f

// ---- Output conditioning ----------------------------------------------------
// Median filter over the solver's output, in taps, applied per axis before the
// 1-Euro filter. Odd: 1 (off), 3 or 5.
//
// OFF BY DEFAULT, on measurement rather than principle. A median is the right
// tool for ISOLATED single-frame outliers, which a low-pass cannot remove — a
// low-pass smears a spike across its whole time constant, and a 1-Euro filter
// does worse than that, reading the spike as speed and opening its cutoff so
// the next one passes cleanly. But once the peak-cell hysteresis and the
// nuisance priors took the discrete jumps out at the source, there were no
// isolated spikes left to delete: what remains is white noise, which a median
// does nothing for. Measured end-to-end on synthetic frames:
//
//     taps   cursor p2p (worst)   cursor sd    lag at 400 mm/s
//       1        0.25 mm           0.0136 mm       0.88 ms
//       3        0.25 mm           0.0134 mm       1.80 ms
//
// i.e. all cost, no benefit. Turn it on (3) if real hardware shows impulsive
// glitches this synthetic model has no way to produce — an intermittent mux
// channel, a marginal solder joint on one sensor — which look like a single
// frame flung a long way and back. The console's 'j' will show it as a large
// p2p next to a small sd.
#define MEDIAN_TAPS       1

// ---- Output filter (1-Euro) -------------------------------------------------
// Adaptive low-pass: heavy smoothing when the pen is nearly still (kills the
// sub-pixel jitter you see as a shaking cursor), almost none when it moves fast
// (so flicks keep full 1 kHz responsiveness). Latency at speed is what matters
// for osu!, which is why this is preferred over any fixed-cutoff filter.
#define FILTER_ENABLE     1
#define FILTER_MIN_CUTOFF 0.8f    // Hz — lower = steadier when still
#define FILTER_BETA       0.45f   // higher = less lag when moving fast
#define FILTER_DCUTOFF    1.0f    // Hz — cutoff for the speed estimate itself

// ---- Debug console ----------------------------------------------------------
// A USB CDC serial port carrying the raw grid. You cannot polish what you cannot
// see; this is how the sensor map gets verified empirically. Set to 0 for a
// release build (drops the CDC interface entirely).
#define DEBUG_CONSOLE     1
#define DEBUG_STREAM_HZ   20

// Reports captured by the console's 'j' (jitter) command — ~2 s at 1 kHz.
#define JITTER_FRAMES     2000
