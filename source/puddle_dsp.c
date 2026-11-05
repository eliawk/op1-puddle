/* puddle DSP. Integers only, no per-sample division, no 64-bit maths, no libc.
 * The FX runs at 22.05 kHz in blocks of 64 samples.
 *
 * Signal path per sample (both channels share the modulation and the filter's settings):
 *   random target (sample and hold, random intervals) -> two smoothing poles (DAMP) -> delay time
 *   in -> ring -> interpolated tap -> two fixed BBD-like poles -> resonant low-pass (FILTER, RES) -> drive -> mix (MIX)
 * FILTER 100 is open and clean, 0 closed; the drive grows as it closes. It is scaled by the input's recent peak, so it sounds the same at any playing
 * level, and its output is brought back to that peak, so closing the filter does not make the effect louder.
 * GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). */
#include "puddle_dsp.h"

#define BASE_Q8     (33 << 8)       /* 1.5 ms minimum delay */
#define SWEEP_MAX   (132 << 8)      /* + 6 ms at full depth */
#define BBD_A       26150           /* fixed wet darkening, one-pole a in Q15 twice: about 3.6 kHz at -3 dB */
#define ENV_REL     1003            /* peak follower release, Q15 per block: about 90 ms */
#define REF_REL     32              /* reference release, Q15 per block: about 3 s */
#define REF_FLOOR   (64 << 8)       /* below this level the input counts as silence, not as quiet playing */
#define K_OPEN      5793            /* the low-pass's damping 1/Q at RES 0, Q12: 1.414 (Q 0.7, no peak) */
#define K_SPAN      5383            /* down to 0.1 at RES 100 (Q 10) */
#define DRIVE_MIN   614             /* drive with the filter open, Q12: 0.15 (clean, under 1 % at the peak) */
#define DRIVE_OCT   12288           /* 3 octaves more closed (1.2: the peaks clip, quiet parts come up) */

/* (d * a) >> 15 for |d| < 2^25 and 0 <= a <= 32767 without overflowing 32 bits (floor rounding, as >> would) */
static inline int32_t mul_q15(int32_t d, int32_t a)
{
    return (d >> 15) * a + (((d & 32767) * a) >> 15);
}

static inline int16_t sat16(int32_t x)
{
    return (int16_t)(x > 32767 ? 32767 : (x < -32768 ? -32768 : x));
}

static inline int32_t clamp_q15(int32_t x)
{
    return x < 0 ? 0 : (x > 32767 ? 32767 : x);
}

static inline uint32_t xorshift(uint32_t *r)
{
    uint32_t x = *r;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return *r = x;
}

/* 2^(x / 4096) in Q16 for x in [-16 * 4096, 14 * 4096]; second-order fit of 2^f on [0, 1), within 0.3 % */
int32_t pd_exp2_q16(int32_t x_q12)
{
    if (x_q12 < -16 * 4096) x_q12 = -16 * 4096;
    if (x_q12 > 14 * 4096) x_q12 = 14 * 4096;
    const int32_t i = x_q12 >> 12, f = x_q12 & 4095;
    const int32_t inner = 2689 + ((1407 * f) >> 12);          /* Q12: 0.6565 + 0.3435 f */
    const int32_t m = 65536 + ((f * inner) >> 8);              /* Q16: 1 .. 2 */
    return i >= 0 ? m << i : m >> -i;
}

/* RATE -> mean samples between targets: 441 x 2^(6.64 (1 - rate)), 20 ms .. 2 s */
int32_t pd_mean_interval(int32_t rate)
{
    const int32_t l = ((32767 - clamp_q15(rate)) * 27197) >> 15;   /* 6.64 in Q12 */
    return ((pd_exp2_q16(l) >> 4) * 441) >> 12;
}

/* DAMP -> smoothing pole coefficient, Q15: 371 (about 40 Hz) x 2^(-7.3 damp), down to 4 (about 0.4 Hz) */
int32_t pd_damp_coef(int32_t damp)
{
    const int32_t l = -((clamp_q15(damp) * 29901) >> 15);         /* 7.3 in Q12 */
    const int32_t a = (371 * (pd_exp2_q16(l) >> 1)) >> 15;
    return a < 2 ? 2 : a;
}

/* how closed (0 .. 32767, that is 32767 - FILTER) -> tan(pi fc / fs) in Q16, fc = 9 kHz x 2^(-6.2 closed): 9 kHz open
 * .. 122 Hz closed. Table every 1/32 of the knob (python: round(65536 * tan(pi * 9000 * 2 ** (-6.2 * i / 32) / 22050))), linear in between */
static const int32_t cutoff_g[33] = {
    220813, 135788, 97763, 75672, 60974, 50359, 42270, 35875, 30686, 26395, 22797, 19749, 17147, 14913, 12987, 11320,
    9875, 8618, 7525, 6573, 5742, 5018, 4385, 3833, 3350, 2928, 2560, 2238, 1957, 1711, 1496, 1308, 1143,
};
int32_t pd_cutoff_g(int32_t closed)
{
    const int32_t f = clamp_q15(closed), i = f >> 10, fr = f & 1023;
    if (i >= 32) return cutoff_g[32];
    return cutoff_g[i] + (((cutoff_g[i + 1] - cutoff_g[i]) * fr) >> 10);
}

/* how closed -> drive, Q12: 0.15 x 2^(3 closed), 0.15 .. 1.2 */
int32_t pd_drive(int32_t closed)
{
    return (DRIVE_MIN * (pd_exp2_q16((clamp_q15(closed) * (DRIVE_OCT >> 3)) >> 12) >> 4)) >> 12;
}

/* the zero-delay-feedback state-variable low-pass (trapezoidal integrators), stable at any cutoff and resonance:
 * a1 = 1 / (1 + g (g + k)), a2 = g a1, a3 = g a2, all below 1 (Q15); g in Q13 here (0.7 % at the lowest cutoff) */
void pd_svf_coefs(int32_t g, int32_t res, int32_t *a1, int32_t *a2, int32_t *a3)
{
    const int32_t g13 = g >> 3, k13 = (K_OPEN - ((clamp_q15(res) * K_SPAN) >> 15)) << 1;
    const int32_t den = 8192 + ((g13 * (g13 + k13)) >> 13);
    int32_t b1 = (int32_t)(268435456 / den);                   /* 2^28 / den: Q15 */
    if (b1 > 32767) b1 = 32767;
    const int32_t b2 = (g13 * b1) >> 13;
    *a1 = b1;
    *a2 = b2;
    *a3 = (g13 * b2) >> 13;
}

int32_t pd_softclip(int32_t u)
{
    const int32_t a = u < 0 ? -u : u;                          /* on the magnitude: exactly odd */
    if (a >= 32767) return u < 0 ? -32767 : 32767;
    const int32_t a2 = (a * a) >> 15, a3 = (a2 * a) >> 15;
    int32_t y = (3 * a - a3) >> 1;
    if (y > 32767) y = 32767;                                  /* rounding just below 1 */
    return u < 0 ? -y : y;
}

void pd_init(pd_state *s, uint32_t seed)
{
    for (int c = 0; c < 2; c++) {
        for (int i = 0; i < PD_RING; i++) s->ring[c][i] = 0;
        s->tl[c][0] = s->tl[c][1] = 0;
        s->ic[c][0] = s->ic[c][1] = 0;
    }
    s->w = 0;
    s->rng = seed ? seed : 0x9E3779B9u;
    s->hold = 0;
    s->countdown = 0;
    s->s1 = s->s2 = 0;
    s->sweep = -1;                        /* -1: take the first block's values without a ramp */
    s->mix = -1;
    s->closed = s->res = -1;
    s->env = 0;
    s->ref = REF_FLOOR;
    s->events = 0;
    s->mod = 0;
}

/* the per-block settings of the low-pass and the drive */
typedef struct {
    int32_t a1, a2, a3;        /* the low-pass, Q15 */
    int32_t ylim;              /* the filter output (Q4) at which the drive's input reaches 1 */
    int32_t kin;               /* filter output (Q4) -> the drive's input u (Q15): u = y kin >> 15 */
    int32_t og;                /* the drive's output (Q15) -> samples, brought back to the recent peak */
} wet_set;

/* one channel's wet path after the tap: the fixed BBD poles, the low-pass, the drive, then the mix with the dry */
static inline int16_t wet_path(pd_state *s, int ch, int32_t tap, int32_t dry, const wet_set *ws, int32_t mix)
{
    int32_t *t = s->tl[ch];
    t[0] += mul_q15(tap * 256 - t[0], BBD_A);
    t[1] += mul_q15(t[0] - t[1], BBD_A);
    int32_t *ic = s->ic[ch];
    const int32_t v0 = t[1] >> 4;                                     /* Q4 */
    const int32_t v3 = v0 - ic[1];
    const int32_t v1 = mul_q15(ic[0], ws->a1) + mul_q15(v3, ws->a2);
    const int32_t v2 = ic[1] + mul_q15(ic[0], ws->a2) + mul_q15(v3, ws->a3);
    ic[0] = 2 * v1 - ic[0];
    ic[1] = 2 * v2 - ic[1];
    const int32_t ay = v2 < 0 ? -v2 : v2;
    const int32_t sc = ay >= ws->ylim ? 32767 : pd_softclip((ay * ws->kin) >> 15);
    const int32_t m = mul_q15(ws->og, sc);
    const int32_t wet = v2 < 0 ? -m : m;
    return sat16(dry + mul_q15(wet - dry, mix));
}

void pd_process(pd_state *s, int16_t *left, int16_t *right, const pd_params *p)
{
    /* ---- per block: controls ---- */
    const int32_t mean = pd_mean_interval(p->rate);
    const int32_t damp_a = pd_damp_coef(p->damp);
    const int32_t d = clamp_q15(p->depth), d2 = (d * d) >> 15;
    const int32_t sweep = (d2 * (SWEEP_MAX >> 8)) >> 7;          /* Q8 samples, 0 .. 33792 */
    const int32_t mix = clamp_q15(p->mix);
    if (s->sweep < 0) s->sweep = sweep;
    if (s->mix < 0) s->mix = mix;
    const int32_t closed = 32767 - clamp_q15(p->filter);
    if (s->closed < 0) s->closed = closed;
    if (s->res < 0) s->res = clamp_q15(p->res);
    const int32_t sweep0 = s->sweep, mix0 = s->mix;
    s->closed += (closed - s->closed) >> 2;                     /* about 10 ms: no zipper on the knob */
    s->res += (clamp_q15(p->res) - s->res) >> 2;

    /* ---- per block: the input's level and its recent peak (the drive's scale, the screen's sun) ---- */
    int32_t pk = 0;
    for (int i = 0; i < PD_BLOCK; i++) {
        int32_t v = left[i] < 0 ? -left[i] : left[i];
        if (v > pk) pk = v;
        if (right) {
            v = right[i] < 0 ? -right[i] : right[i];
            if (v > pk) pk = v;
        }
    }
    const int32_t pk8 = pk << 8;
    if (pk8 > s->env) s->env = pk8;
    else s->env -= mul_q15(s->env - pk8, ENV_REL);
    if (s->env > s->ref) s->ref = s->env;
    else s->ref -= mul_q15(s->ref, REF_REL);
    if (s->ref < REF_FLOOR) s->ref = REF_FLOOR;

    /* ---- per block: the low-pass and the drive (three divisions) ---- */
    wet_set ws;
    pd_svf_coefs(pd_cutoff_g(s->closed), s->res, &ws.a1, &ws.a2, &ws.a3);
    {
        const int32_t dr = pd_drive(s->closed), r8 = s->ref >> 8, ref = r8 > 32767 ? 32767 : r8;   /* Q12; samples, 64 .. 32767 */
        ws.ylim = (ref << 16) / dr;                                 /* ref / drive, Q4 */
        ws.kin = (dr << 14) / ref;                                  /* drive / ref, scaled for Q4 in, Q15 out */
        const int32_t d15 = dr << 3;
        int32_t ymax = 32767;                                       /* the clip's output at the peak */
        if (d15 < 32767) {
            const int32_t q2 = (d15 * d15) >> 15, q3 = (q2 * d15) >> 15;
            ymax = (3 * d15 - q3) >> 1;
        }
        ws.og = (ref << 15) / ymax;                                 /* back to the peak: samples per Q15 */
    }

    /* ---- per sample ---- */
    const uint32_t mask = PD_RING - 1;
    for (int i = 0; i < PD_BLOCK; i++) {
        /* random target at random intervals: mean/2 .. 3 mean/2 */
        if (--s->countdown <= 0) {
            s->hold = (int32_t)(int16_t)(xorshift(&s->rng) >> 16);
            s->countdown = (mean >> 1) + (int32_t)(xorshift(&s->rng) % (uint32_t)(mean > 1 ? mean : 1));
            s->events++;
        }
        s->s1 += mul_q15(s->hold * 256 - s->s1, damp_a);
        s->s2 += mul_q15(s->s1 - s->s2, damp_a);
        const int32_t m = s->s2 >> 8;                               /* Q15 signed */

        /* delay time: the sweep ramps over the block, the target moves it inside 0 .. sweep */
        const int32_t sw = sweep0 + (((sweep - sweep0) * i) >> 6);
        const int32_t u = m + 32768;                                /* 0 .. 65535 */
        const uint32_t delay = (uint32_t)(BASE_Q8 + (((sw >> 1) * u) >> 15));
        const int32_t mx = mix0 + (((mix - mix0) * i) >> 6);

        const uint32_t w = s->w;
        const uint32_t rp = (w << 8) - delay;                       /* read position, Q8 */
        const uint32_t k = (rp >> 8) & mask, k1 = (k + 1) & mask;
        const int32_t f = (int32_t)(rp & 255);

        int32_t x = left[i];
        s->ring[0][w & mask] = (int16_t)x;
        int32_t a0 = s->ring[0][k], a1 = s->ring[0][k1];
        left[i] = wet_path(s, 0, a0 + (((a1 - a0) * f) >> 8), x, &ws, mx);
        if (right) {
            x = right[i];
            s->ring[1][w & mask] = (int16_t)x;
            a0 = s->ring[1][k];
            a1 = s->ring[1][k1];
            right[i] = wet_path(s, 1, a0 + (((a1 - a0) * f) >> 8), x, &ws, mx);
        }
        s->w = w + 1;
        s->mod = m;
    }
    s->sweep = sweep;
    s->mix = mix;
}
