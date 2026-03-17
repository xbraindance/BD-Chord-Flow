# Chord Flow Preset Format

Chord Flow reads presets from JSON arrays of preset objects.

Default load order:

1. `presets/default.json` -> `Factory` bank
2. Any additional `presets/*.json` (except `default.json` and `user.json`) -> one bank per file
3. `presets/user.json` -> `User` bank

Save behavior:

- Saving from UI always writes to the `User` bank.
- Saved data is written to `presets/user.json`.

## Top-level schema

Each file is a JSON array:

```json
[
  {
    "name": "Preset Name",
    "bank": "Factory",
    "global_octave": 2,
    "global_transpose": 0,
    "pads": [ ... ]
  }
]
```

## Preset object fields

- `name` (string): Display name in browser.
- `bank` (string, optional in file):
  - If present, uses that bank name.
  - If omitted, bank is inferred by source file (`Factory`/`User`).
- `global_octave` (int): `-6..6`, default baseline is `2`.
- `global_transpose` (int): `-12..12`.
- `pads` (array): Up to 32 pad entries.
  - If fewer than 32 are provided, remaining pads are auto-filled with defaults.

## Pad object fields

```json
{
  "octave": 0,
  "root": "c",
  "bass": "none",
  "chord_type": "maj7",
  "inversion": 0,
  "strum": 0,
  "strum_dir": 0,
  "articulation": 0,
  "reverse_art": 0
}
```

- `octave` (int): `-6..6`
- `root` (enum string): `c c# d d# e f f# g g# a a# b`
- `bass` (enum string): `none c c# d d# e f f# g g# a a# b`
- `chord_type` (enum string): any supported type from DSP/UI chord list
- `inversion` (int): `0..3` (`root`, `1st`, `2nd`, `3rd`)
- `strum` (int): `0..100`
- `strum_dir` (int): `0=up`, `1=down`
- `articulation` (int): `0=off`, `1=on`
- `reverse_art` (int): `0=off`, `1=on`

## Notes

- Roots/bass are stored as lowercase sharp-note tokens.
- Flats should be normalized before writing (for example `Bb -> a#`, `Db -> c#`).
- Chord Flow preserves pad data for all 32 pads internally.
- Additional bank files are discovered automatically from `presets/*.json`.
- Filename (without extension) is used as inferred bank name when `bank` is omitted.
- `presets/private/` is ignored by git and can be used for local-only banks.
