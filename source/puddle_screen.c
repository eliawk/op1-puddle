/* puddle's screen on the OP-1 (GUI thread): reads the controls and the DSP's state, draws the scene of
 * puddle_scene.c with raster lines and native text. Only reads puddle.c's state; the audio never reads anything back.
 * The stock FX overlay draws the bypass dimming; when the effect stops, the scene stops (it runs on the block count).
 * GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). */
#include "op1_synth.h"
#include "puddle_dsp.h"
#include "puddle_scene.h"

#ifndef PUDDLE_ART_ONLY
#define PUDDLE_ART_ONLY 0      /* 1: the scene alone, no labels or readouts (plugin.json cflags) */
#endif

extern pd_state puddle_state[3];
extern volatile uint32_t puddle_blocks[3];

static ps_state scene;
static uint8_t texts[PS_TEXTS][OP1_TEXT_SIZE] __attribute__((aligned(4)));
static uint8_t made[PS_TEXTS];
static char cached[PS_TEXTS][8];

static void out_line(void *r, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t w8, const uint8_t *c)
{
    const op1_pt a = { x0 * 4, y0 * 4 }, b = { x1 * 4, y1 * 4 };   /* Q8 -> Q10 */
    op1_raster_line(r, &a, &b, w8 * 4, c);
}

/* a slot is built once at its place and colour; the string is set again only when it changes */
static void out_text(void *r, unsigned n, int x, int y, int size, const char *s, const uint8_t *c)
{
    static const int32_t ident[9] = { 1024, 0, 0, 0, 1024, 0, 0, 0, 1024 };
    if (n >= PS_TEXTS) return;
    if (!made[n]) { op1_text_ctor(texts[n], OP1_Q10(x), y, size, c); made[n] = 1; }
    unsigned j = 0;
    while (j < 7 && s[j] && cached[n][j] == s[j]) j++;
    if (cached[n][j] != s[j]) {
        for (j = 0; j < 7 && s[j]; j++) cached[n][j] = s[j];
        cached[n][j] = 0;
        op1_text_set(texts[n], cached[n]);
    }
    op1_text_draw(texts[n], r, ident);
}

static void init(void *scr) { (void)scr; ps_init(&scene); }

static void update(void *scr, int dt)
{
    int16_t q[4];
    op1_fx_knobs_live(q);
    const unsigned pos = (unsigned)op1_fx_screen_pos() % 3;
    const int16_t *w = op1_fx_knobs((int)pos);
    const volatile pd_state *st = &puddle_state[pos];

    /* word reads of the DSP's state (written once per sample or block on the audio side) */
    const int32_t env = st->env >> 8, ref = st->ref >> 8;
    int32_t level = ref > 0 ? env * 32767 / ref : 0;
    if (level > 32767) level = 32767;

    const ps_input in = {
        .rate = q[0] < 0 ? 0 : q[0], .depth = q[1] < 0 ? 0 : q[1],
        .filter = q[2] < 0 ? 0 : q[2], .mix = q[3] < 0 ? 0 : q[3],
        .damp = w[5] < 0 ? 0 : w[5], .res = w[7] < 0 ? 0 : w[7],
        .blue_page = op1_fx_page[pos][0] != 0, .white_page = op1_fx_page[pos][2] != 0,
        .art_only = PUDDLE_ART_ONLY,
        .hold = st->hold, .s2 = st->s2 >> 8,
        .events = st->events, .blocks = puddle_blocks[pos],
        .level = level < 0 ? 0 : level,
        .dt = dt,
    };
    op1_screen_draw(scr, dt);
    void *r = op1_raster();
    const ps_out o = { r, out_line, out_text };
    ps_frame(&scene, &in, OP1_SINE_TABLE, &o);
}

const op1_screen_t SCREEN_puddle = { init, update };
