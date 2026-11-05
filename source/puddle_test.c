/* Host test and renderer for puddle's DSP (float only in this test, never in the plugin).
 *   cc -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined puddle_test.c puddle_dsp.c -lm -o puddle_test
 *   ./puddle_test check          checks only (exit 1 on failure)
 *   ./puddle_test render <dir>   22.05 kHz mono WAVs: dry and a few settings
 * GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). */
#include "puddle_dsp.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FS 22050
static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static pd_params P(int rate, int depth, int damp, int lpg, int mix, int response)
{
    pd_params p = { rate, depth, damp, lpg, mix, response };
    return p;
}

/* runs n samples (multiple of 64) through a fresh state; right may be 0 */
static void run(const pd_params *p, int16_t *l, int16_t *r, int n, uint32_t seed)
{
    static pd_state s;
    pd_init(&s, seed);
    for (int b = 0; b + PD_BLOCK <= n; b += PD_BLOCK) pd_process(&s, l + b, r ? r + b : 0, p);
}

static void sine(int16_t *x, int n, float f, float amp)
{
    for (int i = 0; i < n; i++) x[i] = (int16_t)lrintf(amp * sinf(6.2831853f * f * (float)i / FS));
}

/* plucked saw notes: a new note every `step` samples, exponential decay */
static void plucks(int16_t *x, int n, int step, float amp)
{
    static const float notes[] = { 196.0f, 246.94f, 293.66f, 392.0f, 329.63f, 246.94f, 220.0f, 293.66f };
    float ph = 0;
    for (int i = 0; i < n; i++) {
        const int k = i / step, t = i % step;
        const float f = notes[k % 8];
        ph += f / FS;
        if (ph >= 1) ph -= 1;
        float v = 0;
        for (int h = 1; h * f < 8000; h++) v += sinf(6.2831853f * ph * (float)h) / (float)h;
        x[i] = (int16_t)lrintf(amp * 0.5f * v * expf(-(float)t / (0.35f * FS)));
    }
}

/* the input's instantaneous frequency from zero crossings: RMS deviation from the mean, in cents */
static float vibrato_cents(const int16_t *x, int n)
{
    static float per[20000];
    int cnt = 0, last = -1;
    float lastf = 0;
    for (int i = 1; i < n && cnt < 20000; i++) {
        if (x[i - 1] < 0 && x[i] >= 0) {
            const float frac = (float)-x[i - 1] / (float)(x[i] - x[i - 1]);
            const float t = (float)(i - 1) + frac;
            if (last >= 0) per[cnt++] = t - lastf;
            last = i;
            lastf = t;
        }
    }
    if (cnt < 4) return -1;
    double mean = 0, var = 0;
    for (int i = 0; i < cnt; i++) mean += per[i];
    mean /= cnt;
    for (int i = 0; i < cnt; i++) {
        const double c = 1200.0 * log2(per[i] / mean);
        var += c * c;
    }
    return (float)sqrt(var / cnt);
}

static double rms(const int16_t *x, int n)
{
    double a = 0;
    for (int i = 0; i < n; i++) a += (double)x[i] * x[i];
    return sqrt(a / (n > 0 ? n : 1));
}

/* RMS of the first difference over RMS: a rough brightness */
static double brightness(const int16_t *x, int n)
{
    double a = 0;
    for (int i = 1; i < n; i++) { const double d = x[i] - x[i - 1]; a += d * d; }
    return sqrt(a / (n - 1)) / (rms(x, n) + 1e-9);
}

static void check_helpers(void)
{
    /* exp2 within 0.5 % across its range */
    float worst = 0;
    for (int x = -16 * 4096; x <= 14 * 4096; x += 37) {
        const float got = (float)pd_exp2_q16(x) / 65536.0f, want = exp2f((float)x / 4096.0f);
        if (want > 0.01f) {
            const float e = fabsf(got / want - 1);
            if (e > worst) worst = e;
        }
    }
    CHECK(worst < 0.005f, "exp2 worst error %.4f", worst);
    const int32_t slow = pd_mean_interval(0), fast = pd_mean_interval(32767), mid = pd_mean_interval(16384);
    CHECK(slow > 42000 && slow < 46000, "rate 0 mean %d samples, want about 2 s", slow);
    CHECK(fast > 430 && fast < 450, "rate max mean %d samples, want about 20 ms", fast);
    CHECK(mid > fast && mid < slow, "rate is monotonic");
    CHECK(pd_damp_coef(0) == 371, "damp 0 coef %d", pd_damp_coef(0));
    CHECK(pd_damp_coef(32767) >= 2 && pd_damp_coef(32767) <= 6, "damp max coef %d", pd_damp_coef(32767));
    for (int v = 0, prev = 1 << 30; v <= 32767; v += 1024) {
        const int a = pd_damp_coef(v);
        CHECK(a <= prev, "damp monotonic at %d", v);
        prev = a;
    }
    /* the cutoff law falls with FILTER, the drive rises, the clip is odd and saturates */
    for (int v = 0, prev = 1 << 30; v <= 32767; v += 256) {
        const int32_t g = pd_cutoff_g(v);
        CHECK(g < prev, "cutoff not falling at %d", v);
        prev = g;
    }
    {
        const double fc0 = atan(pd_cutoff_g(0) / 65536.0) * FS / M_PI, fc1 = atan(pd_cutoff_g(32767) / 65536.0) * FS / M_PI;
        printf("FILTER cutoff: %.0f Hz open, %.0f Hz closed; drive %.2f .. %.2f\n", fc0, fc1,
               pd_drive(0) / 4096.0, pd_drive(32767) / 4096.0);
        CHECK(fc0 > 8500 && fc0 < 9500 && fc1 > 110 && fc1 < 135, "cutoff range");
        CHECK(pd_drive(0) > 580 && pd_drive(0) < 650 && pd_drive(32767) > 4700 && pd_drive(32767) < 5100, "drive range");
    }
    for (int u = -40000; u <= 40000; u += 97) {
        const int32_t y = pd_softclip(u), z = pd_softclip(-u);
        CHECK(y == -z && y >= -32767 && y <= 32767, "softclip at %d", u);
    }
    CHECK(pd_softclip(32767) == 32767 && pd_softclip(16384) > 22000, "softclip shape");
    /* the low-pass's coefficients stay below 1 at every corner of FILTER and RES */
    for (int fv = 0; fv <= 32767; fv += 4096) for (int r = 0; r <= 32767; r += 8191) {
        int32_t a1, a2, a3;
        pd_svf_coefs(pd_cutoff_g(fv), r, &a1, &a2, &a3);
        CHECK(a1 > 0 && a1 <= 32767 && a2 > 0 && a2 < 32767 && a3 > 0 && a3 < 32767, "svf coefs at %d %d", fv, r);
    }
}

/* every corner and middle of the six controls, full-scale noise and a square, mono and stereo: no UB, no crash */
static void check_corners(void)
{
    static int16_t l[4096], r[4096];
    static const int v[3] = { 0, 16384, 32767 };
    int n = 0;
    for (int a = 0; a < 3; a++) for (int b = 0; b < 3; b++) for (int c = 0; c < 3; c++)
    for (int d = 0; d < 3; d++) for (int e = 0; e < 3; e++) for (int f = 0; f < 3; f++) {
        const pd_params p = P(v[a], v[b], v[c], v[d], v[e], v[f]);
        uint32_t rnd = 12345u + (uint32_t)n;
        for (int i = 0; i < 4096; i++) {
            rnd = rnd * 1664525u + 1013904223u;
            l[i] = (int16_t)(rnd >> 16);
            r[i] = (i & 32) ? 32767 : -32768;
        }
        run(&p, l, r, 4096, 7);
        run(&p, l, 0, 4096, 7);
        n++;
    }
    CHECK(n == 729, "corners run %d", n);
    /* out-of-range controls are clamped, not undefined */
    const pd_params bad = P(-5, 99999, -1, 40000, -3, 70000);
    sine(l, 4096, 441, 20000);
    run(&bad, l, 0, 4096, 1);
}

/* MIX 0 gives the dry signal bit for bit, whatever the other controls */
static void check_mix0(void)
{
    static int16_t x[22050], y[22050];
    plucks(x, 22016, 5000, 20000);
    memcpy(y, x, sizeof x);
    const pd_params p = P(32767, 32767, 0, 32767, 0, 32767);
    run(&p, y, 0, 22016, 3);
    CHECK(!memcmp(x, y, 22016 * 2), "mix 0 is not the dry signal");
}

/* silence after a loud burst ends in exact zeros, and silence in gives silence out */
static void check_silence(void)
{
    static int16_t x[5 * FS];
    const int n = 5 * FS / 64 * 64;
    memset(x, 0, sizeof x);
    plucks(x, FS / 2, 3000, 30000);
    const pd_params p = P(16384, 32767, 8000, 16384, 32767, 16384);
    run(&p, x, 0, n, 9);
    int nz = 0;
    for (int i = n - FS; i < n; i++) nz += x[i] != 0;
    CHECK(nz == 0, "tail not silent: %d non-zero samples in the last second", nz);
    memset(x, 0, sizeof x);
    run(&p, x, 0, n, 9);
    nz = 0;
    for (int i = 0; i < n; i++) nz += x[i] != 0;
    CHECK(nz == 0, "silence in gave %d non-zero samples", nz);
}

/* stereo with L == R gives the mono result on both sides; the two sides share modulation and gate */
static void check_stereo(void)
{
    static int16_t m[22016], l[22016], r[22016];
    plucks(m, 22016, 4000, 22000);
    memcpy(l, m, sizeof m);
    memcpy(r, m, sizeof m);
    const pd_params p = P(20000, 25000, 10000, 20000, 32767, 8000);
    run(&p, m, 0, 22016, 5);
    run(&p, l, r, 22016, 5);
    CHECK(!memcmp(m, l, sizeof m) && !memcmp(m, r, sizeof m), "stereo L=R differs from mono");
}

/* DEPTH 0 leaves the pitch alone; more depth gives more vibrato; DAMP at max calms it */
static void check_depth_damp(void)
{
    static int16_t x[10 * FS];
    const int n = 10 * FS / 64 * 64;
    float c[4];
    const int depths[4] = { 0, 10000, 20000, 32767 };
    for (int k = 0; k < 4; k++) {
        sine(x, n, 441, 16000);
        const pd_params p = P(20000, depths[k], 12000, 0, 32767, 0);
        run(&p, x, 0, n, 11);
        c[k] = vibrato_cents(x + FS, n - FS);
    }
    printf("vibrato RMS cents at depth 0/10k/20k/max (rate 61%%, damp 37%%): %.2f %.2f %.2f %.2f\n", c[0], c[1], c[2], c[3]);
    CHECK(c[0] >= 0 && c[0] < 0.5f, "depth 0 still bends the pitch: %.2f cents", c[0]);
    CHECK(c[1] > c[0] && c[2] > c[1] && c[3] > c[2], "vibrato not increasing with depth");
    CHECK(c[3] > 10, "full depth too weak: %.2f cents", c[3]);
    sine(x, n, 441, 16000);
    const pd_params calm = P(20000, 32767, 32767, 0, 32767, 0);
    run(&calm, x, 0, n, 11);
    const float cd = vibrato_cents(x + FS, n - FS);
    printf("vibrato RMS cents at full depth, damp max: %.2f\n", cd);
    CHECK(cd < c[3], "damp max does not calm the modulation (%.2f vs %.2f)", cd, c[3]);
}

/* the random targets arrive at the mean interval RATE asks for */
static void check_rate(void)
{
    static pd_state s;
    static int16_t x[PD_BLOCK];
    const int rates[3] = { 0, 16384, 32767 };
    for (int k = 0; k < 3; k++) {
        const pd_params p = P(rates[k], 16384, 16384, 0, 32767, 0);
        pd_init(&s, 77);
        const int blocks = 200 * FS / PD_BLOCK;
        for (int b = 0; b < blocks; b++) {
            memset(x, 0, sizeof x);
            pd_process(&s, x, 0, &p);
        }
        const double got = (double)blocks * PD_BLOCK / s.events, want = pd_mean_interval(rates[k]);
        printf("rate %5d: mean interval %.0f samples (want %.0f), %u targets\n", rates[k], got, want, s.events);
        CHECK(fabs(got / want - 1) < 0.15, "rate %d: interval %.0f, want %.0f", rates[k], got, want);
    }
}

/* the level of frequency f in x (Goertzel), as an amplitude */
static double tone(const int16_t *x, int n, double f)
{
    const double w = 2 * M_PI * f / FS, c = 2 * cos(w);
    double s1 = 0, s2 = 0;
    for (int i = 0; i < n; i++) { const double s0 = x[i] + c * s1 - s2; s2 = s1; s1 = s0; }
    return 2 * sqrt(s1 * s1 + s2 * s2 - c * s1 * s2) / n;
}

/* FILTER 0 is transparent; closing it darkens a steady saw without making it louder; RES lifts the cutoff;
 * the drive's distortion grows as FILTER closes, at a quiet and at a loud playing level alike */
static void check_filter(void)
{
    static int16_t a[4 * FS], d[4 * FS];
    const int n = 4 * FS / 64 * 64, w0 = FS, wn = 2 * FS;
    sine(a, n, 300, 12000);
    memcpy(d, a, sizeof a);
    const pd_params open = P(16384, 0, 16384, 32767, 32767, 0);    /* FILTER 100: open */
    run(&open, a, 0, n, 1);
    const double ratio = rms(a + w0, wn) / rms(d + w0, wn);
    printf("FILTER 100 (open), steady 300 Hz: wet/dry level %.3f\n", ratio);
    CHECK(ratio > 0.93 && ratio < 1.03, "FILTER 100 changes a steady tone's level: %.3f", ratio);

    /* a steady 110 Hz saw */
    for (int i = 0; i < n; i++) {
        float v = 0;
        for (int h = 1; h * 110 < 8000; h++) v += sinf(6.2831853f * 110.0f * (float)h * (float)i / FS) / (float)h;
        d[i] = (int16_t)lrintf(6000.0f * v);
    }
    const int32_t fl[3] = { 32767, 16384, 0 };                     /* open, half, closed */
    double br[3], lv[3], pk[3];
    for (int k = 0; k < 3; k++) {
        memcpy(a, d, sizeof d);
        const pd_params p = P(9830, 0, 13107, fl[k], 32767, 0);
        run(&p, a, 0, n, 1);
        br[k] = brightness(a + w0, wn);
        lv[k] = rms(a + w0, wn) / rms(d + w0, wn);
        int m = 0; for (int i = w0; i < w0 + wn; i++) if (abs(a[i]) > m) m = abs(a[i]);
        int md = 0; for (int i = w0; i < w0 + wn; i++) if (abs(d[i]) > md) md = abs(d[i]);
        pk[k] = (double)m / md;
    }
    printf("steady saw, FILTER 100 / 50 / 0 %%: brightness %.3f / %.3f / %.3f, level %.2f / %.2f / %.2f, peak %.2f / %.2f / %.2f\n",
           br[0], br[1], br[2], lv[0], lv[1], lv[2], pk[0], pk[1], pk[2]);
    CHECK(br[1] < br[0] * 0.7 && br[2] < br[1] * 0.7, "FILTER does not darken (%.3f %.3f %.3f)", br[0], br[1], br[2]);
    CHECK(pk[1] < 1.3 && pk[2] < 1.3, "closing the filter makes it louder (%.2f %.2f)", pk[1], pk[2]);

    /* RES: a sine at the cutoff (FILTER 50 %, about 1 kHz) comes out louder with RES up */
    const double fc = atan(pd_cutoff_g(16384) / 65536.0) * FS / M_PI;
    double at[2];
    for (int k = 0; k < 2; k++) {
        sine(a, n, (float)fc, 4000);
        const pd_params p = P(9830, 0, 13107, 16384, 32767, k ? 32767 : 0);
        run(&p, a, 0, n, 1);
        at[k] = tone(a + w0, wn, fc);
    }
    printf("RES, a sine at the cutoff (%.0f Hz, amplitude 4000): %.0f at RES 0, %.0f at RES 100\n", fc, at[0], at[1]);
    CHECK(at[1] > at[0] * 1.8, "RES does not lift the cutoff (%.0f vs %.0f)", at[1], at[0]);

    /* the drive: third harmonic of an 80 Hz sine, at FILTER 0 and 100, quiet (600) and loud (20000) */
    const float amps[2] = { 600, 20000 };
    for (int q = 0; q < 2; q++) {
        double h3[2];
        for (int k = 0; k < 2; k++) {
            sine(a, n, 80, amps[q]);
            const pd_params p = P(9830, 0, 13107, k ? 0 : 32767, 32767, 0);
            run(&p, a, 0, n, 1);
            h3[k] = tone(a + w0, wn, 240) / tone(a + w0, wn, 80);
        }
        printf("drive, 80 Hz at %5.0f: third harmonic %.2f %% at FILTER 100 (open), %.2f %% at FILTER 0 (closed)\n", amps[q], 100 * h3[0], 100 * h3[1]);
        CHECK(h3[0] < 0.01 && h3[1] > 0.03 && h3[1] > 8 * h3[0], "drive does not grow with FILTER at %.0f", amps[q]);
    }
}

/* turning every knob across its range while a sine plays: no step larger than the sine's own slope allows plus margin */
static void check_clicks(void)
{
    static pd_state s;
    static int16_t x[PD_BLOCK];
    pd_init(&s, 3);
    int prev = 0, worst = 0;
    const int blocks = 6 * FS / PD_BLOCK;
    for (int b = 0; b < blocks; b++) {
        for (int i = 0; i < PD_BLOCK; i++) x[i] = (int16_t)lrintf(12000.0f * sinf(6.2831853f * 200.0f * (float)(b * PD_BLOCK + i) / FS));
        const int k = (b * 32767) / blocks;
        /* RES stays 0 here: a resonant, driven filter swept across the sine makes steep (clipped) waves on purpose;
         * RES is swept on its own below, with the cutoff well above the sine */
        const pd_params p = P(k, k, 32767 - k, k, k, 0);
        pd_process(&s, x, 0, &p);
        for (int i = 0; i < PD_BLOCK; i++) {
            const int dlt = abs(x[i] - prev);
            if (b > 2 && dlt > worst) worst = dlt;
            prev = x[i];
        }
    }
    /* the sine's own largest step is 12000 x 2 pi 200 / 22050 = 684 */
    printf("knob sweep: largest sample step %d (the dry sine's is 684)\n", worst);
    CHECK(worst < 1600, "knob sweep clicks: step %d", worst);
    pd_init(&s, 3);
    prev = 0;
    int worst_res = 0;
    for (int b = 0; b < blocks; b++) {
        for (int i = 0; i < PD_BLOCK; i++) x[i] = (int16_t)lrintf(12000.0f * sinf(6.2831853f * 200.0f * (float)(b * PD_BLOCK + i) / FS));
        const pd_params p = P(9830, 0, 13107, 24575, 32767, (b * 32767) / blocks);
        pd_process(&s, x, 0, &p);
        for (int i = 0; i < PD_BLOCK; i++) {
            const int dlt = abs(x[i] - prev);
            if (b > 2 && dlt > worst_res) worst_res = dlt;
            prev = x[i];
        }
    }
    printf("RES sweep (cutoff about 3.1 kHz): largest sample step %d\n", worst_res);
    CHECK(worst_res < 1600, "RES sweep clicks: step %d", worst_res);
}

static void wav(const char *path, const int16_t *x, int n)
{
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }
    uint32_t d = (uint32_t)n * 2, v;
    uint16_t h;
    fwrite("RIFF", 1, 4, f); v = 36 + d; fwrite(&v, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
    v = 16; fwrite(&v, 4, 1, f); h = 1; fwrite(&h, 2, 1, f); h = 1; fwrite(&h, 2, 1, f);
    v = FS; fwrite(&v, 4, 1, f); v = FS * 2; fwrite(&v, 4, 1, f); h = 2; fwrite(&h, 2, 1, f); h = 16; fwrite(&h, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&d, 4, 1, f); fwrite(x, 2, (size_t)n, f); fclose(f);
}

static void render(const char *dir)
{
    static int16_t src[12 * FS], y[12 * FS];
    const int n = 12 * FS / 64 * 64;
    plucks(src, n, FS * 3 / 4, 22000);
    struct { const char *name; pd_params p; } sets[] = {
        { "default", { 9830, 14746, 13107, 26214, 32767, 6554 } },
        { "seasick", { 6000, 32767, 26000, 0, 32767, 0 } },
        { "jittery", { 30000, 12000, 0, 0, 32767, 0 } },
        { "chorus_half_mix", { 18000, 9000, 16000, 0, 16384, 0 } },
        { "filter_half", { 12000, 20000, 14000, 16384, 32767, 0 } },
        { "filter_closed_resonant", { 12000, 20000, 14000, 3767, 32767, 26000 } },
    };
    char path[512];
    snprintf(path, sizeof path, "%s/dry.wav", dir);
    wav(path, src, n);
    for (size_t k = 0; k < sizeof sets / sizeof sets[0]; k++) {
        memcpy(y, src, sizeof src);
        run(&sets[k].p, y, 0, n, 21);
        snprintf(path, sizeof path, "%s/%s.wav", dir, sets[k].name);
        wav(path, y, n);
        printf("wrote %s\n", path);
    }
}

int main(int argc, char **argv)
{
    if (argc >= 3 && !strcmp(argv[1], "render")) { render(argv[2]); return 0; }
    check_helpers();
    check_corners();
    check_mix0();
    check_silence();
    check_stereo();
    check_depth_damp();
    check_rate();
    check_filter();
    check_clicks();
    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
