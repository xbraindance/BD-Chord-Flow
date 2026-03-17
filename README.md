# Chord Flow (Move Everything)

Chord Flow is a chainable MIDI FX module for Move Everything that lets you browse, play, and edit 32-pad chord collections.

## Current behavior

- Starts in a preset browser.
- Click (jog press) loads the selected preset and enters edit mode.
- Pad presses select the active pad and refresh the edit values.
- `Global Oct` (default `+2`) transposes all pads in the current preset.
- `Global Trans` adds semitone transpose (`-12..+12`) for the whole preset.
- `Pad Oct` adds per-pad octave transpose on top of `Global Oct`.
- Chord type and other edit parameters are changed with the jog encoder.
- Save row writes a new preset snapshot of all 32 pad slots.

## Repository layout

```text
src/
  module.json
  help.json
  ui.js
  presets/
    default.json
  dsp/
    chord_flow_plugin.c
scripts/
  build.sh
  build-module.sh
  install.sh
  test.sh
tests/
  test_chordflow_pad_switch.c
  test_chordflow_pad_switch.sh
  test_chordflow_save_behavior.c
  test_chordflow_save_behavior.sh
  chordflow_ui_behavior_test.sh
```

This layout matches the same structure used by modules like `superarp` and `eucalypso`.

## Build

```bash
./scripts/build.sh
```

Output:

- `dist/chord-flow/`
- `dist/chord-flow-module.tar.gz`

## Test

```bash
./scripts/test.sh
```

## Install to Move

```bash
./scripts/install.sh
```

The module is installed to:

`/data/UserData/move-anything/modules/midi_fx/chord-flow/`

Preset files are stored under:

`/data/UserData/move-anything/modules/midi_fx/chord-flow/presets/`

- `default.json` (shipped defaults)
- `user.json` (saved user presets)

## References

- Move Everything module docs: [MODULES.md](https://github.com/handcraftedcc/move-everything/blob/main/docs/MODULES.md)
