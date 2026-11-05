# puddle (FX)

A vibrato/chorus whose short delay wanders to **random** times at **random** intervals instead of following an
LFO, into a **resonant low-pass with drive**. It is inspired by the behaviour of random-modulation BBD pedals. It is
an original implementation from public descriptions of how such pedals behave, not a model or measurement of any
specific unit.

## Knobs

| knob | name | what it does |
|---|---|---|
| blue | RATE | How often a new random delay time is drawn: on average every 2 s (left) down to every 20 ms (right). Each interval is itself random, between half and 1.5 times the average. |
| green | DEPTH | How far the delay can move: 0 to 6 ms on top of a 1.5 ms base (square law, so the low half is fine-grained). 0 leaves the pitch alone. |
| white | FILTER | Low-pass on the wet signal: 100 is open (9 kHz) and clean, 0 is closed (about 120 Hz). The cutoff moves exponentially, so the knob's travel is even to the ear. As it closes the drive grows: under 0.2 % distortion at 100, about 1.5 % at 50, about 14 % (third harmonic) at 0. The drive is scaled by the input's recent peak, so it sounds the same at any playing level, and its output is brought back to that peak, so closing the filter does not make the effect louder. |
| orange | MIX | 0 is dry, 100 is wet only (default 100, a pure vibrato). Lower values give a chorus. |
| blue (press) | DAMP | How slowly the delay glides to each new time: two smoothing poles from about 40 Hz (audible jumps, glitchy) down to about 0.4 Hz (slow drift). At high DAMP and high RATE the targets average out and the movement becomes small. The three modulation controls interact. |
| white (press) | RES | The low-pass's resonance: Q 0.7 (no peak) at 0 to Q 10 at 100. The drive comes after the filter, so a strong resonance is clipped into a growl instead of running away. |

Defaults: RATE 30 %, DEPTH 45 %, FILTER 80 %, MIX 100 %, DAMP 40 %, RES 20 %.

The resonance is the filter knob's press. (The SDK gives a second page to the first three encoders only,
`OP1_PRESS_PAGE`: words 5..7; the orange press could only be a latch, which is not saved.)

The delay path also has a fixed, BBD-like darkening (two one-poles, about 3.6 kHz at -3 dB) before the filter.

## Design

- 22.05 kHz, blocks of 64 samples, integers only, no per-sample division, no 64-bit maths, no libc in the audio
  path. `puddle_dsp.c` depends only on `stdint.h`; `puddle.c` is the thin OP-1 wrapper.
  The screen is `puddle_scene.c` (plain C) and `puddle_screen.c` (see Screen).
- Modulation: an xorshift32 generator draws a signed target and the next interval; two one-pole smoothers (DAMP)
  turn the steps into glides; the result sets the delay inside `1.5 ms + [0, DEPTH]`.
- Delay: a 512-sample ring per channel, linear-interpolated tap.
- Filter: a zero-delay-feedback state-variable low-pass (trapezoidal integrators, stable at any cutoff and
  resonance), 12 dB per octave, states in Q4. The cutoff's tan(pi fc / fs) comes from a 33-point table, linear in
  between; the coefficients are set once per block (one division). FILTER and RES are eased per block (about 10 ms).
- Drive: after the filter, a soft clip 1.5 u - 0.5 u^3 (hard at |u| = 1) on u = filter out x drive / recent peak.
  The drive is 0.15 x 2^(3 FILTER), 0.15 .. 1.2. The output is scaled so the peak comes back at the recent peak
  (two divisions per block). The recent peak is the block peak against a slow reference (3 s release, floor 64).
- Stereo: on the master position both sides share one modulation and one filter setting, so the image stays put.
  With L = R the output equals the mono path bit for bit. The synth and drum positions run mono.
- State: about 2.2 KB per position in `.bss` (three positions). The FX scratch buffer is not used.
- Knob words go through `op1_fx_param_q15`; page-2 words 5 (DAMP, blue press) and 7 (RES, white press) are read
  directly (0..32767).

## Checks

Host, with address and undefined-behaviour sanitizers:

```sh
cc -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=undefined \
   puddle_test.c puddle_dsp.c -lm -o puddle_test
./puddle_test check
./puddle_test render <dir>     # 22.05 kHz mono WAVs: dry, default, seasick, jittery, chorus_half_mix, filter_half, filter_closed_resonant
```

`check` covers: the exp2 approximation, the RATE and DAMP laws, the cutoff law (9000 Hz open, 122 Hz closed, falling
all the way), the drive's range, the soft clip (odd, bounded), the filter's coefficients below 1 at every corner, all
3^6 control corners in mono and stereo with full-scale input, out-of-range controls, MIX 0 equal to the dry signal
bit for bit, silence after a burst ending in exact zeros, stereo L = R equal to mono, vibrato growing with DEPTH
(0.25 / 3.2 / 12.8 / 34.5 cents RMS on a 441 Hz sine at DEPTH 0 / 30 / 61 / 100 %) and DAMP calming it, the measured
mean interval at three RATE settings (within 2 %), FILTER 100 transparent on a steady tone (0.999), a steady saw
darker as FILTER closes (brightness 0.152 / 0.078 / 0.033 at 100 / 50 / 0 %) without getting louder (peak 0.90 /
0.86 / 0.81 of the input), RES lifting a sine at the cutoff (2860 -> 8462 for an amplitude of 4000), the drive's
third harmonic on an 80 Hz sine (0.2 % at FILTER 100, 14 % at FILTER 0, the same at input peaks of 600 and 20000),
and knob sweeps with no clicks (all knobs with RES 0; RES alone).

Screen, host (`puddle_scene_test.c`, same sanitizers):

```sh
cc -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=undefined \
   puddle_scene_test.c puddle_scene.c -lm -o puddle_scene_test
./puddle_scene_test check
./puddle_scene_test svg <dir>  # six frames as SVG previews (a browser font stands in for the native one)
```

`check` runs all 3^5 control corners on all four press-page combinations, quiet and loud, with and without text,
90 frames each, against a simulated DSP: every line inside the panel and at least 1 px wide, at most 400 lines a
frame (258 at most), eight texts a frame in fixed slots (none in the art-only build), and the behaviour: FILTER moves
and grows the moon as it closes, the moon stays full white and trembles only when closed (0.9 px quiet, 3 px
loud, still when open), the right labels share the column's left edge, DEPTH sets the puddle's size, MIX 0 dashes the rim at full light with
no reflection, MIX just above 0 a dim solid rim, RATE brings more rain, the white page shows RES, art only draws the
same lines without text, a stopped effect stands still.

Not verified: listening on an OP-1, CPU load on hardware (the filter and the drive add a few multiplies per sample),
the screen on the panel (the native renderer, its font, the estimated text widths behind the right-aligned column,
the frame cost), and the real input levels reaching the FX.

## Screen

Drawn with raster lines and native text; `preview/screen.html` in the release folder is the same scene in a browser
(same geometry, colours and laws), to try changes before building:

| control | on the screen |
|---|---|
| RATE | each new random target is a drop that falls and opens a ripple where it lands |
| DEPTH | the puddle's size |
| MIX | the rim's light: dashed at 0, then solid, dim at low MIX and full at 100 |
| FILTER | a white moon (a ring and the curve of its shadow, full white, 1.25 px): its size and place (small, high, left when open at 100; big, low, right when closed at 0); it trembles with the drive (up to about 1.5 px each way fully closed, more while sound comes in); its reflection lies in the water |
| DAMP | the rim's wobble (many quick lobes when low, a slow swell when high) and how long the ripples last |

Labels in the encoder colours (blue, green, white, orange) and left-aligned, readouts white and right-aligned: the
right column's labels start at its left edge (x 255), its readouts end on x 306. The native text has no measuring
call, so the readouts' widths are estimated (`DIGIT_W`, `DIGIT_GAP` in `puddle_scene.c`), to be checked on the panel. `puddle_scene.c` is plain C (stdint only);
`puddle_screen.c` reads the knobs, the page words and flags and the DSP's state (`puddle_state`, `puddle_blocks`,
word reads only) and draws it. The scene runs on the DSP's block count: the same at any frame rate, and still when
the effect stops (the stock overlay draws the bypass dimming). `puddle.svg` has no art, only one black pixel so the
document is not empty.

**Art only**: built with `"cflags": ["-DPUDDLE_ART_ONLY=1"]` in `plugin.json`, the screen draws the scene without
labels or readouts. Same plugin id, so install one build or the other.

## Build

Needs the matching kennel SDK (hundekraft 3.1.4 source bundle) and a Blackfin GCC 13.3.0 cross compiler.
`../toolchain/Dockerfile` builds one (binutils 2.42 + gcc 13.3.0, target bfin-elf):

```sh
docker build -t bfin-elf-13 toolchain
cp -r source <kennel>/synth_sdk/plugins/puddle
docker run --rm -v <kennel>:/k -w /k bfin-elf-13 \
  python3 tools/kennel/pack.py synth_sdk/plugins/puddle -o build/puddle.knl
```

Packer output for 0.9.2 (FILTER 100 open, 0 closed; the moon full white, trembling with the drive; right labels left-aligned):
`puddle.knl: 9128 bytes, sections .text 7400, .rodata 244, .data 0, .bss 8240; 78 load-time relocations; ELF relocations R_BFIN_LUIMM16 36, R_BFIN_HUIMM16 36, R_BFIN_PCREL24 37, R_BFIN_PCREL24_JUMP_L 3, R_BFIN_BYTE4_DATA 5; imports _read_fx_param_safe, _op1_fx_page` (the core's FX-screen helpers, as crush's screen). The art-only build has
the same sections.
Core requirement: a kennel core with stereo FX support (the 3.x workshop cores); the screen uses only the core's
FX-screen helpers (`read_fx_param_safe`, `op1_fx_page`), as crush does.
