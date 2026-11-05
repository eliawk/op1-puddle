/* puddle screen host test: runs puddle_scene.c with a host sine table and recording callbacks.
 *   puddle_scene_test check          bounds, cost, text slots and the knob-to-scene behaviour
 *   puddle_scene_test svg <dir>      a few frames as SVG previews (native text drawn with a browser font)
 * GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). */
#include "puddle_scene.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int16_t sine[8192];
static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define MAXL 1024
typedef struct { int32_t x0, y0, x1, y1, w; uint8_t c[4]; } rline;
typedef struct { int used, x, y, size; uint8_t c[4]; char s[16]; } rtext;
typedef struct {
    rline l[MAXL];
    int n, overflow;
    rtext t[PS_TEXTS];
    int ntext, bad_slot;
} rec;
/* each slot's place, size and colour from its first use, for the whole run (as the firmware builds it once) */
static rtext slot_first[PS_TEXTS];
static int slot_mismatch;

static void rec_line(void *ctx, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t w8, const uint8_t *c)
{
    rec *r = ctx;
    if (r->n >= MAXL) { r->overflow = 1; return; }
    rline *l = &r->l[r->n++];
    l->x0 = x0; l->y0 = y0; l->x1 = x1; l->y1 = y1; l->w = w8;
    memcpy(l->c, c, 4);
}
static void rec_text(void *ctx, unsigned n, int x, int y, int size, const char *s, const uint8_t *c)
{
    rec *r = ctx;
    if (n >= PS_TEXTS) { r->bad_slot = 1; return; }
    rtext *t = &r->t[n];
    if (t->used) r->bad_slot = 1;                      /* a slot drawn twice in a frame */
    t->used = 1; t->x = x; t->y = y; t->size = size;
    memcpy(t->c, c, 4);
    snprintf(t->s, sizeof t->s, "%s", s);
    r->ntext++;
    rtext *f = &slot_first[n];
    if (!f->used) *f = *t;
    else if (f->x != x || f->y != y || f->size != size || memcmp(f->c, c, 4)) slot_mismatch = 1;
}

typedef struct { int32_t rate, depth, mix, filter, damp, level; int page; int32_t res; int wpage, art; } knobs;

/* a simulated DSP: blocks at 22.05 kHz / 64, targets at the RATE law's mean interval, s2 easing toward them */
typedef struct { uint32_t blocks, events, rng, until; int32_t hold, s2; } sim;
static uint32_t xs(uint32_t *r) { uint32_t x = *r; x ^= x << 13; x ^= x >> 17; x ^= x << 5; return *r = x; }
static void sim_step(sim *d, const knobs *k, int dt)
{
    const double mean = 441.0 * pow(2.0, 6.64 * (1.0 - k->rate / 32767.0)) / 64.0;   /* blocks */
    const int nb = dt * 22050 / 64 / 1000;
    for (int i = 0; i < nb; i++) {
        d->blocks++;
        if (d->blocks >= d->until) {
            d->hold = (int16_t)(xs(&d->rng) >> 16);
            d->events++;
            d->until = d->blocks + 1 + (uint32_t)(mean * (0.5 + (xs(&d->rng) % 1000) / 1000.0));
        }
        d->s2 += (d->hold - d->s2) / 8;
    }
}

static void frame(ps_state *s, sim *d, const knobs *k, rec *r, int run)
{
    memset(r, 0, sizeof *r);
    if (run) sim_step(d, k, 33);
    const ps_input in = { .rate = k->rate, .depth = k->depth, .mix = k->mix, .filter = k->filter, .damp = k->damp,
                          .res = k->res, .blue_page = k->page, .white_page = k->wpage, .art_only = k->art,
                          .hold = d->hold, .s2 = d->s2, .events = d->events, .blocks = d->blocks, .level = k->level, .dt = 33 };
    const ps_out o = { r, rec_line, rec_text };
    ps_frame(s, &in, sine, &o);
}

static int same_frame(const rec *a, const rec *b)
{
    if (a->n != b->n) return 0;
    return !memcmp(a->l, b->l, sizeof(rline) * (size_t)a->n);
}

/* the moon: white lines above y 72 (the reflection is white too, in the water below; the readouts are text) */
static int is_moon(const rline *l) { return l->c[0] == l->c[1] && l->c[1] == l->c[2] && l->c[0] > 100 && l->y0 < 72 * 256 && l->y1 < 72 * 256; }
static int is_refl(const rline *l) { return l->c[0] == l->c[1] && l->c[1] == l->c[2] && l->y0 >= 72 * 256 && l->y0 == l->y1; }
static int is_blue(const rline *l) { return l->c[2] > l->c[0] + 30 && l->c[2] > l->c[1]; }
static void moon_box(const rec *r, double *cx, double *cy, double *wd)
{
    double mn = 1e9, mx = -1e9, sx = 0, sy = 0; int n = 0;
    for (int i = 0; i < r->n; i++) {
        const rline *l = &r->l[i];
        if (!is_moon(l)) continue;
        sx += l->x0 + l->x1; sy += l->y0 + l->y1; n += 2;
        if (l->x0 < mn) mn = l->x0;
        if (l->x0 > mx) mx = l->x0;
    }
    *cx = n ? sx / n / 256 : 0; *cy = n ? sy / n / 256 : 0; *wd = (mx - mn) / 256;
}
static double blue_width(const rec *r)
{
    double mn = 1e9, mx = -1e9;
    for (int i = 0; i < r->n; i++) {
        const rline *l = &r->l[i];
        if (!is_blue(l) || l->y0 < 80 * 256) continue;   /* the rim and ripples, not falling drops */
        if (l->x0 < mn) mn = l->x0;
        if (l->x0 > mx) mx = l->x0;
    }
    return (mx - mn) / 256;
}

static void check_frame(const rec *r, const char *what, int texts)
{
    CHECK(!r->overflow, "%s: more than %d lines", what, MAXL);
    CHECK(r->n <= 400, "%s: %d lines in a frame (budget 400)", what, r->n);
    CHECK(!r->bad_slot, "%s: text slot out of range or drawn twice", what);
    CHECK(r->ntext == texts, "%s: %d texts, expected %d", what, r->ntext, texts);
    for (int i = 0; i < r->n; i++) {
        const rline *l = &r->l[i];
        const int in = l->x0 >= 256 && l->x0 <= 319 * 256 && l->x1 >= 256 && l->x1 <= 319 * 256 &&
                       l->y0 >= 256 && l->y0 <= 150 * 256 && l->y1 >= 256 && l->y1 <= 150 * 256;
        CHECK(in, "%s: line %d (%d,%d)-(%d,%d) leaves the safe area", what, i, l->x0, l->y0, l->x1, l->y1);
        CHECK(l->w >= 256, "%s: line %d is thinner than 1 px (%d)", what, i, l->w);
        if (!in || l->w < 256) return;
    }
    for (int n = 0; n < PS_TEXTS; n++) {
        const rtext *t = &r->t[n];
        if (!t->used) continue;
        for (const char *p = t->s; *p; p++)
            CHECK((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9'), "%s: slot %d text '%s' (lowercase and digits only)", what, n, t->s);
        CHECK(t->x >= 14 && t->x <= 306, "%s: slot %d at x %d", what, n, t->x);
    }
}

static void check(void)
{
    for (int i = 0; i < 8192; i++) sine[i] = (int16_t)lrint(32767.0 * sin(2.0 * M_PI * i / 8192.0));


    /* every corner of the controls, both pages, quiet and loud, 90 frames (3 s) each */
    static const int32_t v[3] = { 0, 16384, 32767 };
    static rec r;
    int maxl = 0, corners = 0;
    for (int a = 0; a < 3; a++) for (int b = 0; b < 3; b++) for (int c = 0; c < 3; c++)
    for (int d = 0; d < 3; d++) for (int e = 0; e < 3; e++) for (int p = 0; p < 4; p++) for (int lv = 0; lv < 2; lv++) {
        /* both press pages, RES at its corners too, and every other corner without text */
        const knobs k = { v[a], v[b], v[c], v[d], v[e], lv ? 32767 : 0, p & 1, v[(a + e) % 3], p >> 1, (a + b + c + d + e + p) & 1 };
        ps_state s; ps_init(&s);
        sim dsp = { 0, 0, 0x2545F491u, 0, 0, 0 };
        for (int f = 0; f < 90; f++) {
            frame(&s, &dsp, &k, &r, 1);
            char what[64];
            snprintf(what, sizeof what, "corner %d%d%d%d%d p%d l%d f%d", a, b, c, d, e, p, lv, f);
            const int before = fails;
            check_frame(&r, what, k.art ? 0 : 8);
            if (r.n > maxl) maxl = r.n;
            if (fails != before) { f = 90; a = b = c = d = e = 3; p = 4; lv = 2; }   /* one report is enough */
        }
        corners++;
    }
    printf("corners: %d, the most lines in a frame %d\n", corners, maxl);
    CHECK(!slot_mismatch, "a text slot changed its place, size or colour");

    /* the right column: labels left-aligned on its left edge, readouts right-aligned on x 306 (estimated widths) */
    {
        const knobs k = { 9830, 14746, 32767, 0, 13107, 0, 0, 0, 0, 0 };
        ps_state s; ps_init(&s);
        sim dsp = { 0, 0, 1u, 0, 0, 0 };
        frame(&s, &dsp, &k, &r, 1);
        CHECK(r.t[6].used && !r.t[5].used && r.t[6].x + ps_number_width(0) == 306, "FILTER 0 uses slot 6 at %d", r.t[6].x);
        CHECK(r.t[8].used && r.t[8].x + ps_number_width(99) == 306, "MIX 99 slot 8 at %d", r.t[8].x);
        CHECK(r.t[4].x == r.t[7].x && r.t[4].x == r.t[8].x, "right labels not on the column's left edge (%d %d %d)", r.t[4].x, r.t[7].x, r.t[8].x);
        CHECK(!strcmp(r.t[0].s, "rate") && !strcmp(r.t[4].s, "lpf") && !strcmp(r.t[7].s, "mix") && !strcmp(r.t[2].s, "depth"),
              "labels %s %s %s %s", r.t[0].s, r.t[2].s, r.t[4].s, r.t[7].s);
        const knobs kp = { 9830, 14746, 32767, 0, 13107, 0, 1, 0, 0, 0 };
        frame(&s, &dsp, &kp, &r, 1);
        CHECK(!strcmp(r.t[0].s, "damp") && !strcmp(r.t[1].s, "40"), "blue page shows DAMP 40: %s %s", r.t[0].s, r.t[1].s);
        knobs kw = k; kw.wpage = 1; kw.res = 6554;
        frame(&s, &dsp, &kw, &r, 1);
        CHECK(r.t[4].used && !strcmp(r.t[4].s, "res") && !strcmp(r.t[5].s, "20"), "white page shows RES 20: %s %s", r.t[4].s, r.t[5].s);
        CHECK(r.t[7].used && !strcmp(r.t[7].s, "mix") && r.t[7].c[0] == 255 && r.t[7].c[1] == 65, "the orange label is mix: %s", r.t[7].s);
    }
    /* art only: the same lines, no text */
    {
        knobs k = { 9830, 14746, 32767, 6554, 13107, 0, 0, 0, 0, 0 };
        ps_state s1, s2; ps_init(&s1); ps_init(&s2);
        sim d1 = { 0, 0, 5u, 0, 0, 0 }, d2 = d1;
        static rec ra, rb;
        knobs ka = k; ka.art = 1;
        for (int f = 0; f < 40; f++) { frame(&s1, &d1, &k, &ra, 1); frame(&s2, &d2, &ka, &rb, 1); }
        CHECK(rb.ntext == 0 && ra.ntext == 8 && same_frame(&ra, &rb), "art only: same lines, no text (%d texts)", rb.ntext);
    }
    /* FILTER: closing it grows the moon, sinks it and moves it right */
    {
        double x0, y0, w0, x1, y1, w1;
        knobs k = { 9830, 14746, 32767, 32767, 13107, 0, 0, 0, 0, 0 };
        ps_state s; ps_init(&s); sim dsp = { 0, 0, 1u, 0, 0, 0 };
        frame(&s, &dsp, &k, &r, 1); moon_box(&r, &x0, &y0, &w0);
        k.filter = 0;
        frame(&s, &dsp, &k, &r, 1); moon_box(&r, &x1, &y1, &w1);
        printf("moon: FILTER 100 (open) at (%.1f, %.1f) %.1f px wide, FILTER 0 (closed) at (%.1f, %.1f) %.1f px wide\n", x0, y0, w0, x1, y1, w1);
        CHECK(x1 > x0 + 10 && y1 > y0 + 20 && w1 > w0 + 10, "FILTER moves and grows the moon");
    }
    /* the moon is full white; it stands still while open and trembles with the drive, more with sound coming in */
    {
        double spread[3];
        const int32_t fl[3] = { 32767, 0, 0 }, lv[3] = { 32767, 0, 32767 };
        int white_ok = 1;
        for (int q = 0; q < 3; q++) {
            knobs k = { 9830, 14746, 32767, fl[q], 13107, lv[q], 0, 0, 0, 0 };
            ps_state s; ps_init(&s); sim dsp = { 0, 0, 1u, 0, 0, 0 };
            double mn = 1e9, mx = -1e9;
            for (int f = 0; f < 60; f++) {
                frame(&s, &dsp, &k, &r, 1);
                double x, y, w; moon_box(&r, &x, &y, &w);
                if (x < mn) mn = x;
                if (x > mx) mx = x;
                for (int i = 0; i < r.n; i++) if (is_moon(&r.l[i]) && (r.l[i].c[0] < 250 || r.l[i].w != 320)) white_ok = 0;
            }
            spread[q] = mx - mn;
        }
        printf("moon tremble (x range over 2 s): open %.2f px, closed and quiet %.2f px, closed and loud %.2f px\n", spread[0], spread[1], spread[2]);
        CHECK(spread[0] < 0.01 && spread[1] > 0.3 && spread[2] > spread[1] * 2 && spread[2] < 3.1, "the moon's tremble");
        CHECK(white_ok, "the moon is not full white at 1.25 px");
    }
    /* DEPTH: the puddle grows */
    {
        knobs k = { 9830, 0, 32767, 6554, 32767, 0, 0, 0, 0, 0 };
        ps_state s; ps_init(&s); sim dsp = { 0, 0, 1u, 0, 0, 0 };
        frame(&s, &dsp, &k, &r, 0); const double a = blue_width(&r);
        k.depth = 32767;
        frame(&s, &dsp, &k, &r, 0); const double b = blue_width(&r);
        printf("puddle: %.1f px wide at DEPTH 0, %.1f px at DEPTH 100\n", a, b);
        CHECK(a > 72 && a < 86 && b > 133 && b < 150, "DEPTH sets the puddle's size");
    }
    /* MIX: dashed at 0, dim and solid just above, full at 100; no ripples or reflection while dry */
    {
        knobs k = { 32767, 16384, 0, 16384, 32767, 0, 0, 0, 0, 0 };
        ps_state s; ps_init(&s); sim dsp = { 0, 0, 7u, 0, 0, 0 };
        for (int f = 0; f < 30; f++) frame(&s, &dsp, &k, &r, 1);
        int rim0 = 0, refl0 = 0, c0 = 0;
        for (int i = 0; i < r.n; i++) {
            if (is_blue(&r.l[i]) && r.l[i].y0 >= 70 * 256 && r.l[i].c[2] >= 250) { rim0++; c0 = r.l[i].c[2]; }
            if (is_refl(&r.l[i])) refl0++;
        }
        k.mix = 1300;
        frame(&s, &dsp, &k, &r, 1);
        int c1 = 255; for (int i = 0; i < r.n; i++) if (is_blue(&r.l[i]) && r.l[i].y0 >= 80 * 256) { if (r.l[i].c[2] < c1) c1 = r.l[i].c[2]; }
        int refl1 = 0; for (int i = 0; i < r.n; i++) if (is_refl(&r.l[i])) refl1++;
        printf("rim: MIX 0 %d full-light segments (dashed), MIX 4 dimmest blue %d of 255\n", rim0, c1);
        CHECK(rim0 == 24 && c0 >= 250 && refl0 == 0, "MIX 0: a dashed rim at full light and no reflection (%d, %d)", rim0, refl0);
        CHECK(c1 < 80 && refl1 == 3, "MIX just above 0: a dim solid rim and the reflection");
    }
    /* RATE: drops follow the targets; the scene stops when the DSP stops */
    {
        knobs k = { 32767, 16384, 32767, 6554, 0, 0, 0, 0, 0, 0 };
        ps_state s; ps_init(&s); sim dsp = { 0, 0, 3u, 0, 0, 0 };
        for (int f = 0; f < 20; f++) frame(&s, &dsp, &k, &r, 1);
        static rec a, b;
        for (int f = 0; f < 5; f++) frame(&s, &dsp, &k, &a, 0);
        frame(&s, &dsp, &k, &b, 0);
        CHECK(same_frame(&a, &b), "stopped effect: the scene stands still");
        knobs slow = k; slow.rate = 0;
        ps_init(&s); dsp = (sim){ 0, 0, 3u, 0, 0, 0 };
        int fast_lines = 0, slow_lines = 0;
        for (int f = 0; f < 60; f++) { frame(&s, &dsp, &k, &r, 1); fast_lines += r.n; }
        ps_init(&s); dsp = (sim){ 0, 0, 3u, 0, 0, 0 };
        for (int f = 0; f < 60; f++) { frame(&s, &dsp, &slow, &r, 1); slow_lines += r.n; }
        printf("rain: %d lines over 2 s at RATE 100, %d at RATE 0\n", fast_lines, slow_lines);
        CHECK(fast_lines > slow_lines + 1000, "RATE brings more rain");
    }
    printf("%d checks, %d failed\n", checks, fails);
}

static void svg(const char *dir)
{
    for (int i = 0; i < 8192; i++) sine[i] = (int16_t)lrint(32767.0 * sin(2.0 * M_PI * i / 8192.0));
    static const struct { const char *name; knobs k; } sets[] = {
        { "default", { 9830, 14746, 32767, 26214, 13107, 16000, 0, 0, 0, 0 } },
        { "deep_low_moon", { 26214, 32767, 32767, 327, 2621, 30000, 0, 0, 0, 0 } },
        { "dry_small", { 8192, 6554, 0, 32767, 27852, 0, 0, 0, 0, 0 } },
        { "damp_page_dim_mix", { 9830, 14746, 3932, 16384, 13107, 8000, 1, 0, 0, 0 } },
        { "art_only", { 9830, 22000, 32767, 19660, 13107, 16000, 0, 0, 0, 1 } },
        { "art_only_low_moon", { 26214, 32767, 32767, 2767, 6554, 30000, 0, 0, 0, 1 } },
    };
    static rec r;
    for (unsigned n = 0; n < sizeof sets / sizeof sets[0]; n++) {
        ps_state s; ps_init(&s); sim dsp = { 0, 0, 0x2545F491u, 0, 0, 0 };
        for (int f = 0; f < 70; f++) frame(&s, &dsp, &sets[n].k, &r, 1);
        char path[512];
        snprintf(path, sizeof path, "%s/%s.svg", dir, sets[n].name);
        FILE *fp = fopen(path, "w");
        if (!fp) { perror(path); exit(1); }
        fprintf(fp, "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"960\" height=\"480\" viewBox=\"0 0 320 160\">\n"
                    "<rect width=\"320\" height=\"160\" fill=\"#000\"/>\n");
        for (int i = 0; i < r.n; i++) {
            const rline *l = &r.l[i];
            fprintf(fp, "<line x1=\"%.3f\" y1=\"%.3f\" x2=\"%.3f\" y2=\"%.3f\" stroke=\"rgb(%u,%u,%u)\" stroke-width=\"%.3f\"/>\n",
                    l->x0 / 256.0, l->y0 / 256.0, l->x1 / 256.0, l->y1 / 256.0, l->c[0], l->c[1], l->c[2], l->w / 256.0);
        }
        for (int t = 0; t < PS_TEXTS; t++) {
            const rtext *x = &r.t[t];
            if (!x->used) continue;
            /* the native font's cap height is roughly 0.75 of its size; a browser font stands in */
            fprintf(fp, "<text x=\"%d\" y=\"%.1f\" font-family=\"Helvetica\" font-weight=\"200\" font-size=\"%.1f\" fill=\"rgb(%u,%u,%u)\">%s</text>\n",
                    x->x, x->y + x->size * 0.75, x->size * 0.95, x->c[0], x->c[1], x->c[2], x->s);
        }
        fprintf(fp, "</svg>\n");
        fclose(fp);
        printf("%s: %d lines\n", path, r.n);
    }
}

int main(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "check")) { check(); return fails ? 1 : 0; }
    if (argc >= 3 && !strcmp(argv[1], "svg")) { svg(argv[2]); return 0; }
    fprintf(stderr, "usage: %s check | svg <dir>\n", argv[0]);
    return 2;
}
