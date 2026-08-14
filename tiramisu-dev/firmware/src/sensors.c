// Hall-grid acquisition for the aim1k tablet — DMA round-robin on core 1.
//
// Architecture (see ../docs/CALCULATIONS.md §2):
//   * S0..S3 form a shared 4-bit address bus to all eight muxes.
//   * All eight mux enables are tied low in hardware -> always active.
//   * The eight mux COM outputs land on GPIO40..47 (ADC inputs 0..7). The
//     schematic net names are reversed (net "ADC0" is on GPIO47 = ADC input 7),
//     but SENSOR_GRID is generated indexed BY ADC INPUT, so that is already
//     accounted for and the scan needs no reversal step of its own.
//   * The RP2350 has ONE ADC behind an 8-input front mux. In round-robin mode it
//     cycles inputs 0..7 back-to-back; a DMA channel drains those samples per
//     address straight into a buffer, so the CPU never polls the ADC.
//
// Written against the Raspberry Pi Pico SDK (RP2350).
#include "sensors.h"
#include "config.h"
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/adc.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/sync.h"
#include <math.h>

#if ADC_WARMUP_PASS
  #define PASSES_PER_ADDR 2
#else
  #define PASSES_PER_ADDR 1
#endif
#define SAMPLES_PER_ADDR (MUX_COUNT * PASSES_PER_ADDR)

static int      s_dma = -1;
static uint16_t s_raw[SAMPLES_PER_ADDR];         // DMA target, one address
static float    s_baseline[SENSOR_COUNT];
static float    s_sigma[SENSOR_COUNT];
static bool     s_calibrated = false;
static volatile bool s_recal_req = false;
// Linear grid index of the pen, or -1 for none. Written by core 0, read by core
// 1; a single aligned 32-bit word, so it cannot tear and needs no lock.
static volatile int32_t s_pen_cell = -1;
static volatile float s_frame_hz = 0.0f;

#if FRAME_AVG > 1
// Sliding boxcar over the last FRAME_AVG raw frames. Held as a running sum plus
// the ring of terms in it, so each new frame costs one add and one subtract per
// sensor instead of re-summing the window.
static float    s_hist[FRAME_AVG][SENSOR_COUNT];
static float    s_hsum[SENSOR_COUNT];
static uint32_t s_hcount = 0;                    // frames pushed, saturating
static uint32_t s_hhead  = 0;
#endif

// Lock-free single-producer (core 1) / single-consumer (core 0) publish.
static volatile uint32_t g_seq = 0;              // odd = write in progress
static sensor_frame_t    g_shared;

static inline void set_address(uint8_t addr) {
    gpio_put(PIN_SEL_S0, (addr >> 0) & 1);
    gpio_put(PIN_SEL_S1, (addr >> 1) & 1);
    gpio_put(PIN_SEL_S2, (addr >> 2) & 1);
    gpio_put(PIN_SEL_S3, (addr >> 3) & 1);
    // Analog line settle after switching all eight muxes.
    busy_wait_us(ADC_SETTLE_US);
}

// Capture the eight ADC inputs at the currently-selected address via DMA.
// With ADC_WARMUP_PASS the round-robin runs twice and only the second pass is
// kept, so each input's sample-and-hold starts from (almost) the right level
// instead of from whatever the previous channel left behind.
static void capture_address(uint8_t addr, uint16_t *dst8) {
    set_address(addr);

    adc_run(false);
    adc_fifo_drain();
    adc_select_input(0);       // round-robin proceeds 0,1,...,7 from here

    dma_channel_config c = dma_channel_get_default_config(s_dma);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_16);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, true);
    channel_config_set_dreq(&c, DREQ_ADC);
    dma_channel_configure(s_dma, &c, s_raw, &adc_hw->fifo,
                          SAMPLES_PER_ADDR, true);

    adc_run(true);
    dma_channel_wait_for_finish_blocking(s_dma);
    adc_run(false);

    // Keep the last pass.
    const uint16_t *keep = &s_raw[SAMPLES_PER_ADDR - MUX_COUNT];
    for (int i = 0; i < MUX_COUNT; i++) dst8[i] = keep[i];
}

#if FRAME_AVG > 1
static void frame_avg_reset(void) {
    s_hcount = 0;
    s_hhead  = 0;
    for (int i = 0; i < SENSOR_COUNT; i++) s_hsum[i] = 0.0f;
}

// Push one raw frame into the sliding window and replace it with the window
// mean. Running sum, so this is one add and one subtract per sensor rather than
// a re-sum of the whole window.
static void frame_avg_push(float f[SENSOR_COUNT]) {
    const bool full = (s_hcount >= FRAME_AVG);
    for (int i = 0; i < SENSOR_COUNT; i++) {
        if (full) s_hsum[i] -= s_hist[s_hhead][i];
        s_hist[s_hhead][i] = f[i];
        s_hsum[i] += f[i];
    }
    s_hhead = (s_hhead + 1u) % FRAME_AVG;
    if (!full) s_hcount++;
    const float inv = 1.0f / (float)(full ? (uint32_t)FRAME_AVG : s_hcount);
    for (int i = 0; i < SENSOR_COUNT; i++) f[i] = s_hsum[i] * inv;
}
#endif

// One full 120-sensor frame in raw ADC counts, de-interleaved into grid order.
static void raw_frame(float counts[SENSOR_COUNT]) {
    uint16_t addr_samples[MUX_COUNT];
    for (uint8_t addr = 0; addr < MUX_USED_CH; addr++) {
        capture_address(addr, addr_samples);
        for (int in = 0; in < MUX_COUNT; in++) {
            uint16_t idx = SENSOR_GRID[in][addr];
            if (idx != 0xFFFF)
                counts[idx] = (float)(addr_samples[in] & 0x0FFF);
        }
    }
}

void sensors_init(void) {
    const uint sel[4] = { PIN_SEL_S0, PIN_SEL_S1, PIN_SEL_S2, PIN_SEL_S3 };
    for (int i = 0; i < 4; i++) {
        gpio_init(sel[i]);
        gpio_set_dir(sel[i], GPIO_OUT);
        gpio_put(sel[i], 0);
    }

    adc_init();
    for (int i = 0; i < MUX_COUNT; i++)
        adc_gpio_init(ADC_GPIO_BASE + i);          // GPIO40..47
    adc_set_round_robin((1u << MUX_COUNT) - 1);    // inputs 0..7
    adc_fifo_setup(true,  // enable FIFO
                   true,  // DREQ enable (paces the DMA)
                   1,     // DREQ threshold: 1 sample
                   false, // no error bit in FIFO
                   false);// 16-bit samples (no byte shift)
    adc_set_clkdiv(0);                             // back-to-back, ~2 us/sample

    s_dma = dma_claim_unused_channel(true);

    for (int i = 0; i < SENSOR_COUNT; i++) s_sigma[i] = SIGMA_FLOOR;
}

// Welford: one pass gives both the mean (baseline) and the variance (noise sigma
// that every threshold downstream is expressed in).
void sensors_calibrate(uint32_t frames) {
    if (frames < 2) frames = 2;

    // Static, not stack: core 1's default stack is 2 KB and this runs there on a
    // recalibrate request, nested inside scan_core1's own scratch buffers.
    static float mean[SENSOR_COUNT];
    static float m2[SENSOR_COUNT];
    static float f[SENSOR_COUNT];

    for (int i = 0; i < SENSOR_COUNT; i++) { mean[i] = 0.0f; m2[i] = 0.0f; }

#if FRAME_AVG > 1
    // Measure sigma through the SAME averaging the live path publishes through,
    // otherwise every threshold expressed in sigma is silently wrong by
    // sqrt(FRAME_AVG). Prime the window first so no partial-window frame (which
    // is noisier) lands in the statistics.
    frame_avg_reset();
    for (int s = 0; s < FRAME_AVG; s++) { raw_frame(f); frame_avg_push(f); }
#endif

    for (uint32_t n = 1; n <= frames; n++) {
        raw_frame(f);
#if FRAME_AVG > 1
        frame_avg_push(f);
#endif
        for (int i = 0; i < SENSOR_COUNT; i++) {
            const float d = f[i] - mean[i];
            mean[i] += d / (float)n;
            m2[i]   += d * (f[i] - mean[i]);
        }
    }

    for (int i = 0; i < SENSOR_COUNT; i++) {
        s_baseline[i] = mean[i];
        float var = m2[i] / (float)(frames - 1);
        float sd  = (var > 0.0f) ? sqrtf(var) : 0.0f;
        s_sigma[i] = (sd < SIGMA_FLOOR) ? SIGMA_FLOOR : sd;
    }
    s_calibrated = true;
}

// Core-1 entry point: scan forever, publishing baseline-subtracted frames.
static void scan_core1(void) {
    if (!s_calibrated) sensors_calibrate(NOISE_CAL_FRAMES);

    absolute_time_t t_mark = get_absolute_time();
    uint32_t frames_since_mark = 0;

    for (;;) {
        if (s_recal_req) {
            s_recal_req = false;
            sensors_calibrate(NOISE_CAL_FRAMES);
        }

        static float acc[SENSOR_COUNT];
        static float f[SENSOR_COUNT];
        for (int i = 0; i < SENSOR_COUNT; i++) acc[i] = 0.0f;
        for (int s = 0; s < OVERSAMPLE; s++) {
            raw_frame(f);
            for (int i = 0; i < SENSOR_COUNT; i++) acc[i] += f[i];
        }
        const float inv = 1.0f / (float)OVERSAMPLE;
        for (int i = 0; i < SENSOR_COUNT; i++) acc[i] *= inv;
#if FRAME_AVG > 1
        frame_avg_push(acc);
#endif

        // Drift tracking, three protections deep:
        //   * cells near the tracked pen are frozen, so a resting magnet is
        //     never absorbed;
        //   * everywhere else, only deflections INSIDE the per-cell gate may be
        //     tracked. Drift creeps, so a tracking baseline never legitimately
        //     sees more than a few sigma; anything bigger is a FIELD — and it
        //     may be a field presence cannot see, because presence only looks
        //     for positive peaks. An ungated idle rate here once absorbed the
        //     NEGATIVE field of a pen resting flipped on the tablet, and the
        //     moment the pen was picked up its positive mirror image became a
        //     full-amplitude phantom that presence happily latched onto;
        //   * the one exception is the post-release re-zero pass, whose whole
        //     job is erasing large leftovers — it runs only inside the solver's
        //     blind window, when nothing can be acquired and anything static on
        //     the grid is by definition garbage.
        const int32_t pen    = s_pen_cell;
        const bool    rezero = (pen == SENSORS_PEN_REZERO);
        const bool    onpen  = (pen >= 0);
        const float   alpha  = rezero ? BASELINE_ALPHA_REZERO
                             : onpen  ? BASELINE_ALPHA
                                      : BASELINE_ALPHA_IDLE;
        const int     pcol   = onpen ? (int)(pen % SENSOR_COLS) : 0;
        const int     prow   = onpen ? (int)(pen / SENSOR_COLS) : 0;

        static sensor_frame_t out;
        for (int r = 0; r < SENSOR_ROWS; r++) {
            const int dr = (r > prow) ? (r - prow) : (prow - r);
            for (int c = 0; c < SENSOR_COLS; c++) {
                const int i = r * SENSOR_COLS + c;
                const float d = acc[i] - s_baseline[i];
                out.value[i] = d;
                if (onpen) {
                    const int dc = (c > pcol) ? (c - pcol) : (pcol - c);
                    if (dc <= BASELINE_FREEZE_RADIUS &&
                        dr <= BASELINE_FREEZE_RADIUS) continue;   // under the pen
                }
                const float gate = BASELINE_GATE_SNR * s_sigma[i];
                if (rezero) {
                    // The re-zero pass is ungated for POSITIVE deflections only.
                    // Its targets — leftover images, drift built up under a
                    // parked pen — are positive. A large NEGATIVE deflection is
                    // a flipped magnet resting on the tablet, and absorbing one
                    // is precisely the act of minting a positive phantom: the
                    // baseline dips under it, and whenever it leaves, a
                    // full-amplitude mirror image stands up in its place.
                    if (d < -gate) continue;
                } else {
                    if (d > gate || d < -gate) continue;    // a field, not drift
                }
                s_baseline[i] += alpha * d;
            }
        }

        // Publish (seqlock): bump to odd, copy, bump to even.
        __dmb(); g_seq++; __dmb();
        g_shared = out;
        __dmb(); g_seq++; __dmb();

        if (++frames_since_mark >= 512) {
            const absolute_time_t now = get_absolute_time();
            const int64_t us = absolute_time_diff_us(t_mark, now);
            if (us > 0) s_frame_hz = (float)frames_since_mark * 1e6f / (float)us;
            t_mark = now;
            frames_since_mark = 0;
        }
    }
}

void sensors_start(void) {
    multicore_launch_core1(scan_core1);
}

bool sensors_get_latest(sensor_frame_t *out) {
    for (int tries = 0; tries < 8; tries++) {
        uint32_t s1 = g_seq;
        if (s1 & 1u) continue;                     // write in progress
        __dmb();
        *out = g_shared;
        __dmb();
        if (g_seq == s1) return true;              // consistent snapshot
    }
    return false;
}

const float *sensors_sigma(void)    { return s_sigma; }
const float *sensors_baseline(void) { return s_baseline; }
float sensors_frame_rate(void)      { return s_frame_hz; }
void sensors_request_recalibrate(void) { s_recal_req = true; }
void sensors_set_pen_cell(int cell) {
    if (cell >= 0 && cell < SENSOR_COUNT) s_pen_cell = (int32_t)cell;
    else if (cell == SENSORS_PEN_REZERO)  s_pen_cell = SENSORS_PEN_REZERO;
    else                                  s_pen_cell = SENSORS_PEN_NONE;
}
