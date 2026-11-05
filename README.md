# puddle

An FX plugin for the Teenage Engineering OP-1 running a kennel core (Hundekraft workshop firmware 3.x).

The effect is a vibrato/chorus. Its short delay wanders to random times at random intervals instead of following an
LFO. The delayed signal then goes through a resonant low-pass with drive. The plugin has its own screen: a puddle
under the moon that reacts to the knobs and the sound.

| knob | name | press |
|---|---|---|
| blue | RATE | DAMP |
| green | DEPTH | |
| white | FILTER (100 open and clean, 0 closed and driven) | RES |
| orange | MIX | |

## Contents

- `puddle.knl`: the plugin (0.9.2).
- `art-only/puddle.knl`: the same plugin, but the screen shows the scene without labels or numbers. It has the same
  plugin id, so install only one of the two.
- `source/`: the C source, the host tests and the full documentation (`source/README.md`).
- `preview/screen.html`: the screen scene in a browser, with sliders for every control.
- `renders/`: WAV renders of the effect from the host test (22.05 kHz mono).
- `toolchain/Dockerfile`: the Blackfin GCC 13.3.0 cross compiler used for the build.

## Status

The DSP and the screen pass their host tests, which run with the address and undefined-behaviour sanitizers. Not
checked on hardware yet:

- how it sounds;
- the CPU load;
- how the screen renders, including the font widths behind the right-aligned numbers.

## Licence

GPL-3.0-or-later, with the additional permission to combine the plugin with the OP-1 firmware (see `LICENSE` and
`NOTICE`). The OP-1 firmware is not part of this repository.
