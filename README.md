# IESM — Inazuma Eleven Save Manager

3DS homebrew that edits Inazuma Eleven GO Galaxy (Big Bang / Supernova) saves directly
on console: secret link level (sister-version link) and the SD-Link / data download /
QR content unlock, without a second console or copy.

Save format research lives in [NOTES.md](NOTES.md). Format and offsets are ported from
[Tiniifan/InazumaElevenSaveEditor](https://github.com/Tiniifan/InazumaElevenSaveEditor).

## Install on 3DS

Each [release](https://github.com/ulysse-bonneau/iesm/releases) has a QR code —
scan it with FBI (Remote Install → Scan QR Code), or enter the URL manually:

```
https://github.com/ulysse-bonneau/iesm/releases/latest/download/iesm.cia
```

A `.3dsx` for Homebrew Launcher is on the release page too.

## Features

- Finds the Big Bang/Supernova save automatically (game card or installed title).
- Secret link level 0–3, with the same guards as the PC editor (level 3 warns unless
  the version-exclusive team is beaten — setting it early glitches the save).
- SD-Link unlock: data download + QR code + GO/Chrono Stones link rewards.
- Backs up the save to `sd:/ie-secret-link/` before every write; restore from the menu.
- Log on the bottom screen and in `sd:/ie-secret-link/log.txt`.

## Repo layout

- `3ds/` — the homebrew app (C, libctru).
- `pctool/iesave.py` — PC reference codec + CLI (`info` / `set-link` / `selftest`).
- `tests/parity.sh` — proves the C codec is byte-identical to the Python reference.
- `tools/gen_assets.py` — icon/banner/audio/QR generation.

For personal use on legitimately-owned games and saves only.
