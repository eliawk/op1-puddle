/* puddle: a random-modulation vibrato/chorus into a resonant low-pass with drive (see README.md).
 *   blue   : RATE   how often the delay jumps to a new random time (2 s .. 20 ms on average)
 *   green  : DEPTH  how far it moves (0 .. 6 ms over a 1.5 ms base)
 *   white  : FILTER low-pass on the wet, closed at 0, open at 100; the drive grows as it closes
 *   orange : MIX    0 = dry, 100 = wet only (default)
 * Encoder press (page 2, saved with the sound; the SDK gives a page to the first three encoders only):
 *   blue   : DAMP   how slowly the delay glides to each new time (steps .. slow drift)
 *   white  : RES    the low-pass's resonance (Q 0.7 .. 10)
 * State per FX position (0 synth, 1 drum, 2 master) in .bss: about 2.2 KB each, the scratch buffer is not used.
 * GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). */
#include "op1_plugin.h"
#include "puddle_dsp.h"

/* read by puddle_screen.c (GUI thread): word reads only, never written there */
pd_state puddle_state[3];
volatile uint32_t puddle_blocks[3];

static inline unsigned slot(int pos) { return (unsigned)pos > 2u ? 0u : (unsigned)pos; }

static inline int32_t word(const int16_t *p, int i) { return p[i] < 0 ? 0 : p[i]; }

static void init(int pos) { pd_init(&puddle_state[slot(pos)], 0x2545F491u + 0x9E3779B9u * slot(pos)); }

static void render_stereo(int pos, int16_t *l, int16_t *r, const int16_t *p)
{
    const pd_params q = {
        .rate = op1_fx_param_q15(p, 0), .depth = op1_fx_param_q15(p, 1),
        .filter = op1_fx_param_q15(p, 2), .mix = op1_fx_param_q15(p, 3),
        .damp = word(p, 5), .res = word(p, 7),
    };
    pd_process(&puddle_state[slot(pos)], l, r, &q);
    puddle_blocks[slot(pos)]++;
}

static void render(int pos, int16_t *buf, const int16_t *p) { render_stereo(pos, buf, 0, p); }

const op1_fx_stereo_t FX_puddle = {
    { init, render, { OP1_PRESS_PAGE, OP1_PRESS_NONE, OP1_PRESS_PAGE, OP1_PRESS_NONE } }, render_stereo };
