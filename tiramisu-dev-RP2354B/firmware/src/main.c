// aim1k tablet — top level.
//
// core 1: free-running DMA scan of the 120-sensor hall grid (sensors.c).
// core 0: USB, localisation (solve.c), output filtering (filter.c), 1 kHz HID.
//
// The scan and the report loop are deliberately decoupled: core 1 publishes
// frames as fast as it can (~1.8 kHz) through a seqlock and core 0 always solves
// the freshest one, so report jitter never depends on scan timing.
#include <math.h>
#include <stdio.h>
#include "pico/stdlib.h"
#include "tusb.h"

#include "config.h"
#include "hid_report.h"
#include "sensors.h"
#include "solve.h"
#include "filter.h"

static one_euro_t s_filter;
static sensor_frame_t s_frame;
static pen_pos_t s_pos;
static uint16_t s_last_x = LOGICAL_MAX_X / 2, s_last_y = LOGICAL_MAX_Y / 2;
static bool s_was_in_range = false;
static absolute_time_t s_last_report;
static absolute_time_t s_last_solve;
static volatile bool s_mouse_on = (MOUSE_DEFAULT_ON != 0);
static bool s_send_mouse_next = false;
static bool s_idle_sent = false;

static inline uint16_t clamp_u16(float v, uint16_t hi) {
    if (v < 0) return 0;
    if (v > hi) return hi;
    return (uint16_t)(v + 0.5f);
}

#if DEBUG_CONSOLE
// Defined with the rest of the console, below; called from the report loop
// because that is the only place that sees every reported position.
static void dbg_jitter_sample(float raw_x, float raw_y, float flt_x, float flt_y);

// Solve time, so the 1 ms report budget can be checked on real hardware rather
// than estimated. The dipole fit is the only thing here that could plausibly
// overrun it, and the answer depends on the flash/XIP cache, not on arithmetic
// anyone can count. Reported by 'i'.
static uint32_t s_solve_us_max = 0, s_solve_us_acc = 0, s_solve_n = 0;
#endif

// Solve the freshest frame and queue exactly one report.
//
// This is called from tud_hid_report_complete_cb, i.e. once per report the host
// actually collected, so the cadence is phase-locked to the host's polling.
// The previous design ran a free-running 1 ms timer and skipped the report
// whenever tud_hid_ready() happened to be false at the deadline; because that
// timer and the USB SOF drift against each other, the skips came in a beat
// pattern and cost ~6% of frames (measured 945 Hz with 6% of intervals ~2 ms).
static void report_once(void) {
    if (!tud_hid_ready()) return;

    // Measured, not assumed. The chain is driven by the host's polling and by a
    // 2 ms watchdog, so the spacing is only nominally 1 ms, and every cutoff in
    // the output filter is derived from it.
    const absolute_time_t now = get_absolute_time();
    const float dt = (float)absolute_time_diff_us(s_last_solve, now) * 1e-6f;
    s_last_solve = now;

    if (sensors_get_latest(&s_frame)) {
#if DEBUG_CONSOLE
        const absolute_time_t t0 = get_absolute_time();
#endif
        solve_position(&s_frame, &s_pos);
#if DEBUG_CONSOLE
        const uint32_t us =
            (uint32_t)absolute_time_diff_us(t0, get_absolute_time());
        if (us > s_solve_us_max) s_solve_us_max = us;
        s_solve_us_acc += us;
        s_solve_n++;
#endif
        // Hand the pen's location back to the scanner. Baseline drift tracking
        // has to stop under a resting magnet, or it absorbs the very signal it
        // is the reference for — but only under it. Everywhere else has to keep
        // tracking, or drift accumulates there instead and eventually looks more
        // like a pen than the pen does.
        sensors_set_pen_cell(solve_pen_cell());
        if (s_pos.in_range != s_was_in_range) one_euro_reset(&s_filter);
        s_was_in_range = s_pos.in_range;
    }

    pen_report_t r = {0};
    if (s_pos.in_range) {
        // Hover-only by design: the tablet reports position, clicks come from the
        // keyboard (osu! K1/K2). The button bits are intentionally never set.
        float fx = s_pos.x_mm, fy = s_pos.y_mm;
#if FILTER_ENABLE
        one_euro_apply(&s_filter, s_pos.x_mm, s_pos.y_mm, dt, &fx, &fy);
#endif
#if DEBUG_CONSOLE
        dbg_jitter_sample(s_pos.x_mm, s_pos.y_mm, fx, fy);
#endif
        r.buttons  = PEN_IN_RANGE;
        r.x        = clamp_u16(fx / AREA_W_MM * LOGICAL_MAX_X, LOGICAL_MAX_X);
        r.y        = clamp_u16(fy / AREA_H_MM * LOGICAL_MAX_Y, LOGICAL_MAX_Y);
        r.pressure = 0;
        s_last_x = r.x;
        s_last_y = r.y;
    } else {
        r.buttons = 0;                            // pen lifted
        r.x = s_last_x;
        r.y = s_last_y;
    }

    // No pen: send one final "lifted" report, then go quiet until it comes back.
    //
    // A tablet that keeps streaming its last position pins the cursor there and
    // makes the regular mouse unusable - through the mouse collection directly,
    // and through OTD too, since its parser applies a position for every report
    // it receives. Real tablets stop reporting once the pen leaves proximity, so
    // this one does the same and the cursor is free for anything else to move.
    // s_last_report is still stamped so the main-loop watchdog keeps ticking at
    // its 2 ms pace instead of spinning.
    if (!s_pos.in_range) {
        s_last_report = get_absolute_time();
        if (s_idle_sent) return;
        s_idle_sent = true;
        tud_hid_report(REPORT_ID_PEN, &r, sizeof(r));
        return;
    }
    s_idle_sent = false;

    bool queued;
#if !VENDOR_ONLY
    // Alternate: the vendor report always goes out (OTD needs every sample), and
    // the mouse report only while the mouse collection is enabled. They alternate
    // rather than both going out per poll because one interrupt-IN transfer per
    // 1 ms frame is all USB Full-Speed allows - sending both would halve the rate
    // OTD sees. Standalone (no OTD) the mouse still gets a fresh 500 Hz, and with
    // OTD driving, the mouse collection is off and the pen gets the full 1 kHz.
    if (s_mouse_on && s_send_mouse_next) {
        mouse_report_t m = {0};
        m.buttons = 0;
        m.x = (uint16_t)((uint32_t)r.x * MOUSE_LOGICAL_MAX / LOGICAL_MAX_X);
        m.y = (uint16_t)((uint32_t)r.y * MOUSE_LOGICAL_MAX / LOGICAL_MAX_Y);
        queued = tud_hid_report(REPORT_ID_MOUSE, &m, sizeof(m));
    } else {
        queued = tud_hid_report(REPORT_ID_PEN, &r, sizeof(r));
    }
    s_send_mouse_next = !s_send_mouse_next;
#else
    queued = tud_hid_report(REPORT_ID_PEN, &r, sizeof(r));
#endif

    if (queued) s_last_report = get_absolute_time();
}

// Host-side mode switch. OTD sends this via FeatureInitReport in aim1k.json so
// Windows' own cursor motion stops the moment OTD takes over; without it the two
// would fight over the pointer.
uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                               hid_report_type_t report_type,
                               uint8_t *buffer, uint16_t reqlen) {
    (void)instance;
    if (report_type == HID_REPORT_TYPE_FEATURE &&
        report_id == REPORT_ID_MODE && reqlen >= 1) {
        buffer[0] = s_mouse_on ? 1 : 0;
        return 1;
    }
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                           hid_report_type_t report_type,
                           uint8_t const *buffer, uint16_t bufsize) {
    (void)instance;
    if (report_type != HID_REPORT_TYPE_FEATURE || bufsize < 1) return;

    // Depending on the stack, the report ID may arrive in report_id or as the
    // first payload byte. Accept both rather than depend on which.
    uint8_t id = report_id;
    const uint8_t *p = buffer;
    uint16_t n = bufsize;
    if (id == 0) {
        id = buffer[0];
        p++;
        n--;
    }
    if (id == REPORT_ID_MODE && n >= 1) s_mouse_on = (p[0] != 0);
}

// Chain: every completed report immediately queues the next one.
void tud_hid_report_complete_cb(uint8_t instance, uint8_t const *report,
                                uint16_t len) {
    (void)instance; (void)report; (void)len;
    report_once();
}

// ---- Debug console ----------------------------------------------------------
// A CDC serial port that dumps the raw grid. This is the instrument the sensor
// map and the noise thresholds are verified against — see scripts/grid_view.py.
#if DEBUG_CONSOLE
static bool s_stream = false;
static char s_buf[1200];

static void dbg_banner(void) {
    int n = snprintf(s_buf, sizeof(s_buf),
        "\r\naim1k debug console\r\n"
        "  d = one deflection frame     r = one raw-count frame\r\n"
        "  n = per-sensor noise sigma   b = baseline\r\n"
        "  s = stream deflections       x = stop streaming\r\n"
        "  c = recalibrate (pen away!)  i = info\r\n"
        "  j = measure jitter (park the pen and keep still)\r\n"
        "  m = toggle mouse collection (off = let OTD drive)\r\n"
        "grid is %d cols x %d rows, row-major\r\n",
        SENSOR_COLS, SENSOR_ROWS);
    tud_cdc_write(s_buf, (uint32_t)n);
    tud_cdc_write_flush();
}

// Emit one tagged row-major array of 120 values.
static void dbg_dump(char tag, const float *v, bool as_int) {
    int n = snprintf(s_buf, sizeof(s_buf), "%c", tag);
    for (int i = 0; i < SENSOR_COUNT && n < (int)sizeof(s_buf) - 12; i++) {
        if (as_int)
            n += snprintf(s_buf + n, sizeof(s_buf) - n, " %d", (int)lrintf(v[i]));
        else
            n += snprintf(s_buf + n, sizeof(s_buf) - n, " %.2f", (double)v[i]);
    }
    n += snprintf(s_buf + n, sizeof(s_buf) - n, "\r\n");
    if (tud_cdc_write_available() >= (uint32_t)n) {
        tud_cdc_write(s_buf, (uint32_t)n);
        tud_cdc_write_flush();
    }
}

static void dbg_info(const pen_pos_t *p) {
    const float *sg = sensors_sigma();
    float smin = 1e30f, smax = 0.0f, ssum = 0.0f;
    for (int i = 0; i < SENSOR_COUNT; i++) {
        if (sg[i] < smin) smin = sg[i];
        if (sg[i] > smax) smax = sg[i];
        ssum += sg[i];
    }
    const double solve_avg = s_solve_n ? (double)s_solve_us_acc / s_solve_n : 0.0;
    const unsigned long solve_max = (unsigned long)s_solve_us_max;
    s_solve_us_max = 0; s_solve_us_acc = 0; s_solve_n = 0;   // max is since last 'i'

    int n = snprintf(s_buf, sizeof(s_buf),
        "scan %.0f Hz | sigma min %.2f mean %.2f max %.2f counts"
        " | solve %.0f us avg %lu us max (budget 1000)\r\n"
        "pen %s peak %.1f snr %.1f coh %.2f  x %.2f y %.2f z %.1f %s"
        " rms %.1f%s%s\r\n",
        (double)sensors_frame_rate(), (double)smin,
        (double)(ssum / SENSOR_COUNT), (double)smax, solve_avg, solve_max,
        p->in_range ? "IN " : "out", (double)p->peak, (double)p->snr,
        (double)p->coherence, (double)p->x_mm, (double)p->y_mm,
        (double)p->z_mm, p->refined ? "(dipole)" : "(centroid)",
        (double)p->fit_rms,
        p->outside ? " OFF-AREA" : "",
        (p->blob && !p->in_range) ? " (blob, untrusted)" : "");
    tud_cdc_write(s_buf, (uint32_t)n);
    tud_cdc_write_flush();
}

// ---- Jitter measurement -----------------------------------------------------
// Park the magnet, press 'j', don't touch anything. Reports the standard
// deviation and peak-to-peak of the reported position over the capture, both
// before and after the output filter, so "is it still glitching" has a number
// instead of an opinion. Peak-to-peak is the one that matters for the symptom:
// a discrete jump shows up there long before it moves the standard deviation.
typedef struct {
    uint32_t n;
    float mx, my, m2x, m2y;      // Welford
    float lo_x, hi_x, lo_y, hi_y;
} jitter_t;

static jitter_t s_jit_raw, s_jit_flt;
static uint32_t s_jit_left = 0;          // frames still to capture
static bool     s_jit_ready = false;

static void jit_reset(jitter_t *j) {
    j->n = 0;
    j->mx = j->my = j->m2x = j->m2y = 0.0f;
    j->lo_x = j->lo_y =  1e30f;
    j->hi_x = j->hi_y = -1e30f;
}

static void jit_push(jitter_t *j, float x, float y) {
    j->n++;
    const float dx = x - j->mx, dy = y - j->my;
    j->mx += dx / (float)j->n;
    j->my += dy / (float)j->n;
    j->m2x += dx * (x - j->mx);
    j->m2y += dy * (y - j->my);
    if (x < j->lo_x) j->lo_x = x;
    if (x > j->hi_x) j->hi_x = x;
    if (y < j->lo_y) j->lo_y = y;
    if (y > j->hi_y) j->hi_y = y;
}

static inline float jit_sd(float m2, uint32_t n) {
    return (n > 1) ? sqrtf(m2 / (float)(n - 1)) : 0.0f;
}

static void dbg_jitter_sample(float raw_x, float raw_y, float flt_x, float flt_y) {
    if (!s_jit_left) return;
    jit_push(&s_jit_raw, raw_x, raw_y);
    jit_push(&s_jit_flt, flt_x, flt_y);
    if (--s_jit_left == 0) s_jit_ready = true;
}

static void dbg_jitter_report(void) {
    if (s_jit_raw.n < 2) {
        tud_cdc_write_str("jitter: no in-range frames captured\r\n");
        tud_cdc_write_flush();
        return;
    }
    int n = snprintf(s_buf, sizeof(s_buf),
        "jitter over %lu frames (mm)\r\n"
        "  solver   sd %.4f %.4f  p2p %.3f %.3f\r\n"
        "  filtered sd %.4f %.4f  p2p %.3f %.3f\r\n",
        (unsigned long)s_jit_raw.n,
        (double)jit_sd(s_jit_raw.m2x, s_jit_raw.n),
        (double)jit_sd(s_jit_raw.m2y, s_jit_raw.n),
        (double)(s_jit_raw.hi_x - s_jit_raw.lo_x),
        (double)(s_jit_raw.hi_y - s_jit_raw.lo_y),
        (double)jit_sd(s_jit_flt.m2x, s_jit_flt.n),
        (double)jit_sd(s_jit_flt.m2y, s_jit_flt.n),
        (double)(s_jit_flt.hi_x - s_jit_flt.lo_x),
        (double)(s_jit_flt.hi_y - s_jit_flt.lo_y));
    tud_cdc_write(s_buf, (uint32_t)n);
    tud_cdc_write_flush();
}

static void dbg_poll(const sensor_frame_t *f, const pen_pos_t *p) {
    static bool was_connected = false;
    const bool now_connected = tud_cdc_connected();
    if (now_connected && !was_connected) dbg_banner();
    was_connected = now_connected;
    if (!now_connected) { s_stream = false; return; }

    while (tud_cdc_available()) {
        int ch = tud_cdc_read_char();
        switch (ch) {
        case 'd': dbg_dump('D', f->value, true); break;
        case 'n': dbg_dump('N', sensors_sigma(), false); break;
        case 'b': dbg_dump('B', sensors_baseline(), true); break;
        case 'r': {
            static float raw[SENSOR_COUNT];
            const float *bl = sensors_baseline();
            for (int i = 0; i < SENSOR_COUNT; i++) raw[i] = f->value[i] + bl[i];
            dbg_dump('R', raw, true);
            break;
        }
        case 's': s_stream = true;  break;
        case 'x': s_stream = false; break;
        case 'i': dbg_info(p); break;
        case 'j':
            jit_reset(&s_jit_raw);
            jit_reset(&s_jit_flt);
            s_jit_ready = false;
            s_jit_left  = JITTER_FRAMES;
            tud_cdc_write_str("measuring jitter, hold still...\r\n");
            tud_cdc_write_flush();
            break;
        case 'm':
            s_mouse_on = !s_mouse_on;
            tud_cdc_write_str(s_mouse_on
                ? "mouse collection ON (cursor moves with no driver)\r\n"
                : "mouse collection OFF (OpenTabletDriver drives the cursor)\r\n");
            tud_cdc_write_flush();
            break;
        case 'c':
            tud_cdc_write_str("recalibrating, keep the pen away...\r\n");
            tud_cdc_write_flush();
            sensors_request_recalibrate();
            solve_reset();
            one_euro_reset(&s_filter);
            break;
        default: break;
        }
    }

    if (s_jit_ready) {
        s_jit_ready = false;
        dbg_jitter_report();
    }

    if (s_stream) {
        static absolute_time_t next = {0};
        if (absolute_time_diff_us(get_absolute_time(), next) <= 0) {
            next = make_timeout_time_ms(1000 / DEBUG_STREAM_HZ);
            dbg_dump('D', f->value, true);
        }
    }
}
#endif // DEBUG_CONSOLE

int main(void) {
    sensors_init();
    tusb_init();
    one_euro_init(&s_filter, (float)REPORT_HZ,
                  FILTER_MIN_CUTOFF, FILTER_BETA, FILTER_DCUTOFF);

    // Let enumeration settle, then learn the no-magnet baseline AND the per-sensor
    // noise sigma. Keep the pen off the tablet for the first ~half second.
    absolute_time_t warmup = make_timeout_time_ms(500);
    while (!time_reached(warmup)) tud_task();
    sensors_calibrate(NOISE_CAL_FRAMES);

    // Hand the grid scan to core 1; core 0 now only does USB + solve + filter.
    sensors_start();

    s_last_report = get_absolute_time();
    s_last_solve  = s_last_report;

    for (;;) {
        tud_task();

        // The report chain is self-sustaining, but it has to be kick-started
        // after enumeration and restarted if it ever stalls (suspend/resume, a
        // refused queue). Anything past ~2 ms means the chain is not running.
        if (tud_hid_ready() &&
            absolute_time_diff_us(s_last_report, get_absolute_time()) > 2000)
            report_once();

#if DEBUG_CONSOLE
        dbg_poll(&s_frame, &s_pos);
#endif
    }
}
