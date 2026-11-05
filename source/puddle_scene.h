/* puddle's screen as plain C (stdint only): the scene from the controls and the DSP's state, drawn through two
 * callbacks, so the host test runs it without the firmware. puddle_screen.c connects it to the OP-1.
 * GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). */
#ifndef PUDDLE_SCENE_H
#define PUDDLE_SCENE_H
#include <stdint.h>

#define PS_RIPPLES 6           /* ripples on the water at most */
#define PS_TEXTS   10          /* native text slots; each slot always has the same place, size and colour */
#define PS_LIFT    12          /* the effect-screen lift, applied here to every line and text */

typedef struct {
    int32_t hold;              /* the random target that made it, Q15 signed: where it lands */
    uint32_t born;             /* the DSP block count when it was drawn */
} ps_ripple;

typedef struct {
    ps_ripple rip[PS_RIPPLES];
    unsigned count, next;      /* ripples in use, the next one to replace */
    uint32_t last_events, last_blocks;
    unsigned idle;             /* frames without a new DSP block */
    uint32_t wob, wob2;        /* the rim's two wobble phases */
    int32_t prev_s2, vel;      /* the modulation's speed, Q15 per second, smoothed */
    int started;
} ps_state;

typedef struct {
    int32_t rate, depth, mix, filter, damp, res;   /* 0 .. 32767 */
    int blue_page;             /* the blue encoder's press page is shown (DAMP instead of RATE) */
    int white_page;            /* the white encoder's press page is shown (RES instead of MIX) */
    int art_only;              /* no labels or readouts: the scene alone */
    int32_t hold, s2;          /* the DSP's current random target and smoothed modulation, Q15 signed */
    uint32_t events, blocks;   /* the DSP's target count and block count */
    int32_t level;             /* the input level against its recent peak, 0 .. 32767 */
    int dt;                    /* ms since the last frame */
} ps_input;

typedef struct {
    void *ctx;
    /* coordinates in Q8 screen pixels (lift applied), width in Q8 pixels, colour RGBA */
    void (*line)(void *ctx, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t w8, const uint8_t *rgba);
    /* native text: x, y in screen pixels (lift applied); slot n always comes with the same x, y, size, colour */
    void (*text)(void *ctx, unsigned slot, int x, int y, int size, const char *s, const uint8_t *rgba);
} ps_out;

void ps_init(ps_state *s);
/* one frame; sine is the OP-1's 8192-entry Q15 sine table (OP1_SINE_TABLE) */
void ps_frame(ps_state *s, const ps_input *in, const int16_t *sine, const ps_out *o);

/* exposed for the tests */
int ps_number_width(int value);            /* estimated width of a readout (size 40), px */

#endif
