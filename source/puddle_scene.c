/* puddle's screen: a puddle seen in perspective, a white moon and rain, after the owner's approved mock-up
 * (preview/screen.html). Integer only; geometry in Q8 pixels in the art's space, lifted by PS_LIFT on the way out.
 *   RATE  : each new random target is a drop that falls and opens a ripple where it lands
 *   DEPTH : the puddle's size, nothing else
 *   MIX   : the rim's light: dashed at 0, then solid, dim at low MIX and full at 100
 *   FILTER: the moon's size and place: small, high and left when open (100), big, low and right when closed (0);
 *           as the drive grows (closing) it trembles, more while sound comes in; its reflection lies in the water
 *   DAMP  : the rim's wobble (many quick lobes when low, a slow swell when high) and how long ripples last
 * The scene runs on the DSP's block count, so it is the same at any frame rate and stops when the effect stops.
 * GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). */
#include "puddle_scene.h"

#define Q8(v)       ((int32_t)((v) * 256))
#define CX          Q8(160)
#define CY          Q8(106)
#define RIGHT       306         /* right edge of the right column */
#define FALL        36          /* blocks a drop takes to fall (about 0.1 s) */
#define DROP_TOP    Q8(20)
#define RIM_N       48          /* rim segments */
#define RIP_N       16          /* ripple ring segments */
#define RIP_IN_N    10          /* inner ring segments */
#define MOON_N      24
#define MOON_W      320         /* 1.25 px: a 1 px white reads grey on the panel */
#define TERM_N      10          /* the moon's shadow curve */
#define TWO_PI_PH   683565276u  /* 1 radian as a phase (2^32 / 2 pi) */

/* Estimated advances of the stock font (the native text has no measuring call): readouts at size 40 about 23 px a
 * digit plus 5 px between digits; labels at size 14 from the other screens' right-aligned size-12 labels, x 14/12.
 * To be checked on the panel. */
#define DIGIT_W     23
#define DIGIT_GAP   5
#define RCOL        (RIGHT - 2 * DIGIT_W - DIGIT_GAP)   /* the right column's left edge: labels start here */

static const uint8_t blue[4] = { 107, 142, 255, 255 }, green[4] = { 0, 239, 148, 255 };
static const uint8_t white[4] = { 255, 255, 255, 255 }, red[4] = { 255, 65, 57, 255 };


typedef struct {
    const ps_out *o;
    const int16_t *sine;
} ctx;

static inline int32_t sn(const ctx *c, uint32_t ph) { return c->sine[(ph >> 19) & 8191]; }
static inline int32_t cs(const ctx *c, uint32_t ph) { return c->sine[((ph + 0x40000000u) >> 19) & 8191]; }

static inline int32_t clampi(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* colour at q15 of its light (the panel is black, so this is the same as alpha over the background) */
static void shade(uint8_t *out, const uint8_t *c, int32_t q15)
{
    q15 = clampi(q15, 0, 32767);
    for (int i = 0; i < 3; i++) out[i] = (uint8_t)(1 + (((int32_t)c[i] - 1) * q15 >> 15));
    out[3] = 255;
}

/* art space in, lifted and kept on the panel out */
static void ln(const ctx *c, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t w8, const uint8_t *col)
{
    const int32_t lift = Q8(PS_LIFT);
    x0 = clampi(x0, 0, Q8(320));
    x1 = clampi(x1, 0, Q8(320));
    y0 = clampi(y0 - lift, 0, Q8(160));
    y1 = clampi(y1 - lift, 0, Q8(160));
    c->o->line(c->o->ctx, x0, y0, x1, y1, w8, col);
}

static void ellipse(const ctx *c, int32_t cx, int32_t cy, int32_t rx, int32_t ry, unsigned n, int32_t w8,
                    const uint8_t *col)
{
    const uint32_t step = 0xFFFFFFFFu / n + 1u;
    int32_t px = cx + rx, py = cy;
    for (unsigned i = 1; i <= n; i++) {
        const uint32_t ph = i * step;
        const int32_t x = cx + (rx * cs(c, ph) >> 15), y = cy + (ry * sn(c, ph) >> 15);
        ln(c, px, py, x, y, w8, col);
        px = x;
        py = y;
    }
}

int ps_number_width(int value) { return value >= 10 ? 2 * DIGIT_W + DIGIT_GAP : DIGIT_W; }

static int pct(int32_t q) { return (int)((clampi(q, 0, 32767) * 99 + 16383) >> 15); }

static void itoa2(int v, char *b)
{
    if (v >= 10) { b[0] = (char)('0' + v / 10); b[1] = (char)('0' + v % 10); b[2] = 0; }
    else { b[0] = (char)('0' + v); b[1] = 0; }
}

/* a ripple's seeded offset on the surface, -500 .. 499 (the mock-up's hash) */
static int32_t seed_offset(uint32_t n)
{
    uint32_t x = n * 2654435761u;
    x ^= x >> 15;
    return (int32_t)(x % 1000u) - 500;
}

void ps_init(ps_state *s)
{
    for (unsigned i = 0; i < PS_RIPPLES; i++) { s->rip[i].hold = 0; s->rip[i].born = 0; }
    s->count = s->next = 0;
    s->last_events = s->last_blocks = 0;
    s->idle = 99;
    s->wob = s->wob2 = 0;
    s->prev_s2 = s->vel = 0;
    s->started = 0;
}

void ps_frame(ps_state *s, const ps_input *in, const int16_t *sine, const ps_out *o)
{
    const ctx c = { o, sine };
    const int32_t depth = clampi(in->depth, 0, 32767), mix = clampi(in->mix, 0, 32767);
    const int32_t flt = clampi(in->filter, 0, 32767), damp = clampi(in->damp, 0, 32767);
    const int32_t closed = 32767 - flt;                                        /* the art follows how closed it is */
    const int32_t level = clampi(in->level, 0, 32767);
    const int dt = in->dt > 0 && in->dt <= 200 ? in->dt : 33;

    /* ---- the effect runs while its block count moves; when it stops the scene stops too ---- */
    if (!s->started) {
        s->started = 1;
        s->last_events = in->events;
        s->last_blocks = in->blocks;
        s->prev_s2 = in->s2;
    }
    s->idle = in->blocks != s->last_blocks ? 0 : (s->idle < 99 ? s->idle + 1 : s->idle);
    s->last_blocks = in->blocks;
    const int running = s->idle < 4;
    if (running) {
        if (in->events != s->last_events) {               /* a new target: one drop for this frame */
            s->rip[s->next].hold = in->hold;
            s->rip[s->next].born = in->blocks;
            s->next = (s->next + 1) % PS_RIPPLES;
            if (s->count < PS_RIPPLES) s->count++;
            s->last_events = in->events;
        }
        /* the wobble: 0.02 .. 0.14 radians per 1/60 s as DAMP goes down, the second wave 1.7 times as fast */
        const uint32_t inc = 820278u + (uint32_t)(32767 - damp) * 150u;      /* phase per ms */
        s->wob += inc * (uint32_t)dt;
        s->wob2 += (inc + inc / 10u * 7u) * (uint32_t)dt;
        /* the modulation's speed, Q15 per second, eased over about 40 ms */
        int32_t d = in->s2 - s->prev_s2;
        if (d < 0) d = -d;
        const int32_t target = clampi(d * 1000 / dt, 0, 32768);
        s->vel += (target - s->vel) * dt / (dt + 39);
        s->prev_s2 = in->s2;
    }

    /* ---- DEPTH: the puddle's size; DAMP: the rim's lobes and their size with the modulation's speed ---- */
    const int32_t rx = 10010 + (depth * 8243 >> 15), ry = 2650 + (depth * 2944 >> 15);   /* 39.1 .. 71.3, 10.35 .. 21.85 px */
    const int32_t lobes = (2 * 32768 + 6 * (32767 - damp) + 16384) >> 15;
    int32_t wamp = 64 + (((s->vel >> 2) * ((98304 - 2 * damp) >> 2)) >> 18);  /* 0.25 + vel (3 - 2 damp) px */
    if (wamp > 563) wamp = 563;                                                /* 2.2 px */
    const int dry = mix < 655;                                                 /* MIX 0 */

    /* ---- MIX: the rim, dashed when dry, else solid and brightening with MIX ---- */
    {
        uint8_t col[4];
        shade(col, blue, dry ? 32767 : 6554 + (mix * 26213 >> 15));
        const uint32_t step = 0xFFFFFFFFu / RIM_N + 1u;
        int32_t px = 0, py = 0;
        for (unsigned i = 0; i <= RIM_N; i++) {
            const uint32_t ph = i * step;
            const int32_t a = sn(&c, (uint32_t)lobes * ph + s->wob), b = sn(&c, (uint32_t)(lobes + 3) * ph - s->wob2);
            const int32_t rr = (wamp * a >> 15) + (wamp * b >> 16);           /* Q8 px off the ellipse */
            const int32_t x = CX + ((rx + rr) * cs(&c, ph) >> 15);
            const int32_t y = CY + ((ry + rr * ry / rx) * sn(&c, ph) >> 15);
            if (i && (!dry || (i & 1))) ln(&c, px, py, x, y, Q8(1), col);
            px = x;
            py = y;
        }
    }

    /* ---- FILTER: the moon, a ring and the curve of its shadow (a crescent lit on the left), full white; it trembles
       with the drive (up to 1.5 px fully closed, from 30 % to all of it as sound comes in), at about 9 and 13 Hz ---- */
    const int32_t sx = Q8(176) + (closed >> 3), sy = Q8(32) + (closed * 15 >> 6);
    const int32_t sr = 2061 + (closed * 2355 >> 15);                            /* 8.05 .. 17.25 px */
    const int32_t c2 = (closed * closed) >> 15;
    const int32_t shake = (((384 * c2) >> 15) * (9830 + ((level * 22937) >> 15))) >> 15;   /* Q8 px */
    const int32_t mx = sx + ((shake * sn(&c, in->blocks * 112197000u)) >> 15);
    const int32_t my = sy + ((shake * sn(&c, in->blocks * 162063000u + 0x40000000u)) >> 15) * 3 / 5;
    ellipse(&c, mx, my, sr, sr, MOON_N, MOON_W, white);
    {
        /* the terminator: half an ellipse 0.45 of the radius wide, from the top through the left to the bottom */
        const int32_t tx = sr * 115 >> 8;
        int32_t px = mx, py = my - sr;
        for (unsigned i = 1; i <= TERM_N; i++) {
            const uint32_t ph = 0xC0000000u - i * (0x80000000u / TERM_N);     /* 270 .. 90 degrees, through 180 */
            const int32_t x = mx + (tx * cs(&c, ph) >> 15), y = my + (sr * sn(&c, ph) >> 15);
            ln(&c, px, py, x, y, MOON_W, white);
            px = x;
            py = y;
        }
    }
    if (!dry)
        for (int j = 0; j < 3; j++) {
            const int32_t yy = CY - (ry * 141 >> 8) + j * 819;                 /* 0.55 ry above the centre, 3.2 px apart */
            const int32_t hw = sr * (10 - 3 * j) / 10;
            const int32_t xx = sx + (wamp * sn(&c, s->wob + (uint32_t)j * 888632843u) >> 15);   /* 1.3 rad apart */
            ln(&c, xx - hw, yy, xx + hw, yy, MOON_W, white);
        }

    /* ---- RATE: rain; DAMP sets how long a ripple lasts ---- */
    {
        const int32_t spread = rx * 159 >> 8, life = 140 + (damp * 620 >> 15);   /* 0.62 rx; blocks */
        uint8_t col[4];
        for (unsigned i = 0; i < s->count; i++) {
            const ps_ripple *r = &s->rip[i];
            const uint32_t age = in->blocks - r->born;
            const int32_t x = CX + (r->hold * spread >> 15);
            const int32_t y = CY + seed_offset(r->born) * ry / 2000;
            if (age < FALL) {
                const int32_t y0 = DROP_TOP + (int32_t)age * (y - DROP_TOP) / FALL;
                shade(col, blue, 29491);
                ln(&c, x, y0 - Q8(6), x, y0, Q8(1), col);
                continue;
            }
            if (dry || age - FALL >= (uint32_t)life) continue;
            const int32_t f = (int32_t)(age - FALL) * 32767 / life;            /* 0 .. 1 over its life */
            const int32_t rad = Q8(2) + (f * (rx * 430 >> 10) >> 15);          /* + 0.42 rx */
            shade(col, blue, 32767 - f);
            ellipse(&c, x, y, rad, rad * ry / rx, RIP_N, Q8(1), col);
            if (rad > Q8(6)) {
                const int32_t ri = rad * 141 >> 8;                             /* 0.55 */
                shade(col, blue, (32767 - f) * 3 / 5);
                ellipse(&c, x, y, ri, ri * ry / rx, RIP_IN_N, Q8(1), col);
            }
        }
    }

    /* ---- the corners, as the stock FX screens: labels in the encoder colours, readouts white ---- */
    if (in->art_only) return;
    char b[4];
    const int page = in->blue_page, wpage = in->white_page;
    const int v0 = pct(page ? in->damp : in->rate), v1 = pct(depth), v2 = pct(wpage ? in->res : flt), v3 = pct(mix);
    o->text(o->ctx, 0, 14, 16 - PS_LIFT, 14, page ? "damp" : "rate", blue);
    itoa2(v0, b);
    o->text(o->ctx, 1, 14, 34 - PS_LIFT, 40, b, white);
    o->text(o->ctx, 2, 14, 92 - PS_LIFT, 14, "depth", green);
    itoa2(v1, b);
    o->text(o->ctx, 3, 14, 110 - PS_LIFT, 40, b, white);
    o->text(o->ctx, 4, RCOL, 16 - PS_LIFT, 14, wpage ? "res" : "lpf", white);   /* labels left-aligned */
    itoa2(v2, b);                                       /* readouts right-aligned: a slot per width, a slot keeps its x */
    if (v2 >= 10) o->text(o->ctx, 5, RIGHT - ps_number_width(10), 34 - PS_LIFT, 40, b, white);
    else o->text(o->ctx, 6, RIGHT - ps_number_width(0), 34 - PS_LIFT, 40, b, white);
    o->text(o->ctx, 7, RCOL, 92 - PS_LIFT, 14, "mix", red);
    itoa2(v3, b);
    if (v3 >= 10) o->text(o->ctx, 8, RIGHT - ps_number_width(10), 110 - PS_LIFT, 40, b, white);
    else o->text(o->ctx, 9, RIGHT - ps_number_width(0), 110 - PS_LIFT, 40, b, white);
}
