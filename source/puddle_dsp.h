/* puddle: random-modulated short delay (vibrato/chorus) into a resonant low-pass with drive.
 * Pure C (stdint only), so the host test builds it without the firmware headers. See README.md.
 * GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). */
#ifndef PUDDLE_DSP_H
#define PUDDLE_DSP_H
#include <stdint.h>

#define PD_RING   512          /* samples per channel ring: 23 ms at 22.05 kHz, longest delay 166 + 2 */
#define PD_BLOCK  64

/* all controls 0..32767 */
typedef struct {
    int32_t rate;              /* how often a new random target is drawn: 2 s .. 20 ms on average */
    int32_t depth;             /* how far the delay moves: 0 .. 6 ms on top of the 1.5 ms base */
    int32_t damp;              /* how slowly the delay glides to each target: 40 Hz .. 0.4 Hz smoothing */
    int32_t filter;            /* low-pass on the wet: 0 = closed (120 Hz, driven) .. 32767 = open (9 kHz, clean) */
    int32_t mix;               /* 0 = dry, 32767 = wet only */
    int32_t res;               /* the low-pass's resonance: Q 0.7 .. 10 */
} pd_params;

typedef struct {
    int16_t ring[2][PD_RING];
    uint32_t w;                /* write index (wraps by mask) */
    uint32_t rng;              /* xorshift32, never 0 */
    int32_t hold;              /* current random target, Q15 signed */
    int32_t countdown;         /* samples until the next target */
    int32_t s1, s2;            /* two smoothing poles on the target, Q23 */
    int32_t sweep;             /* delay sweep of the previous block, Q8 samples */
    int32_t mix;               /* mix of the previous block, Q15 */
    int32_t closed, res;       /* how closed the filter is (32767 - FILTER) and RES, eased per block (no zipper), Q15 */
    int32_t tl[2][2];          /* fixed BBD low-pass poles per channel, Q8 */
    int32_t ic[2][2];          /* the low-pass's two integrator states per channel, Q4 */
    int32_t env;               /* block peak follower, Q8 (also read by the screen) */
    int32_t ref;               /* slow peak reference (what "loud" means right now), Q8: the drive's scale */
    uint32_t events;           /* number of random targets drawn (read by the screen) */
    int32_t mod;               /* last modulation value, Q15 signed (for tests) */
} pd_state;

void pd_init(pd_state *s, uint32_t seed);
/* processes PD_BLOCK samples in place; right may be 0 for mono. Both channels share one modulation and one filter. */
void pd_process(pd_state *s, int16_t *left, int16_t *right, const pd_params *p);

/* exposed for the tests */
int32_t pd_exp2_q16(int32_t x_q12);
int32_t pd_mean_interval(int32_t rate);
int32_t pd_damp_coef(int32_t damp);
int32_t pd_cutoff_g(int32_t closed);       /* how closed (32767 - FILTER) -> tan(pi fc / fs), Q16 */
int32_t pd_drive(int32_t closed);          /* how closed -> the drive, Q12: 0.15 .. 1.2 */
/* the low-pass's coefficients a1, a2, a3 (Q15) for g (Q16) and RES */
void pd_svf_coefs(int32_t g, int32_t res, int32_t *a1, int32_t *a2, int32_t *a3);
/* the soft clip: 1.5 u - 0.5 u^3 for |u| < 1, else +-1 (Q15) */
int32_t pd_softclip(int32_t u);

#endif
