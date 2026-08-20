# IESM — Inazuma Eleven Save Manager

3DS homebrew that edits Inazuma Eleven GO-series saves (GO / Chrono Stones / Galaxy,
EU & JP) directly on console — no PC, no second console, no second copy.

Save format research lives in [NOTES.md](NOTES.md). Format and offsets are ported from
[Tiniifan/InazumaElevenSaveEditor](https://github.com/Tiniifan/InazumaElevenSaveEditor).

## Install on 3DS

Each [release](https://github.com/ulysse-bonneau/iesm/releases) has a QR code —
scan it with FBI (Remote Install → Scan QR Code), or enter the URL manually:

```
https://github.com/ulysse-bonneau/iesm/releases/latest/download/iesm.cia
```

A `.3dsx` for Homebrew Launcher is on the release page too. Once installed, use
**Check for updates** in the app to pull new versions.

## Features

- Auto-detects every GO-series save on the console (game card or installed title).
- **Secret link** level 0–3 (Galaxy/CS) or link unlock (GO), with the game's own guards.
- **SD-Link unlock**: data download + QR + GO/Chrono Stones link rewards.
- **Players**: level, GP/TP, freedom, invested stats (with the real seesaw rules on
  GO/CS), moves, avatars/totems, equipment, flags; recruit / replace / dismiss;
  a per-player **bank** on SD.
- **Teams**: coach/formation/kit/emblem, roster, kit numbers, position swap; a **team
  bank** on SD that restores across saves by stable IDs.
- **Save info**: name, team name, play time, prestige/friendship, coins.
- **Play records**: unlock all or toggle individually (Galaxy/CS).
- **Backups**: auto-backup before every write, plus manual create/rename/delete/restore,
  sorted per game under `sd:/IESM/<game>/`. Advanced submenu has debug diff/checksum tools.
- Two-screen UI; a running log on the bottom screen and in `sd:/IESM/log.txt`.

## The Galaxy inventory checksum (solved)

Galaxy guards its counters + inventory region with a `FE FF`-tagged u16 checksum stored
at `0x8F6A` and `0x9F16`. On fresh saves the pair is **zero and the game skips
validation**. The reference editor's "unlock all data" byte-set accidentally contains a
donor save's checksum value at `0x8F6A` — writing it **arms** the game's validation, and
from then on any external inventory edit reads as "corrupted" (that's the desktop
editor's issue #14, and it bit this project too).

IESM handles it both ways: the unlock no longer writes those two bytes, and every Galaxy
commit **zeros the checksum pair**, returning the save to the dormant no-checksum state
the game itself uses. Inventory editing is therefore safe again.

If a "×99 items" cheat leaves your **PalPack cards at ×99**, Inventory → Batch actions →
**PalPack cards → x1** trims them back.

Recovery note: if a save ever shows "corrupted", restore the **newest** backup first —
the save carries an anti-rollback counter, so older backups can be rejected as rollbacks.

## Repo layout

- `3ds/` — the homebrew app (C, libctru).
- `pctool/iesave.py` — PC reference codec + CLI (`info` / `set-link` / `selftest`).
- `tests/parity.sh` — proves the C codec is byte-identical to the Python reference.
- `tools/` — asset and database generators (icon/banner/QR, players/items/moves/etc.).

For personal use on legitimately-owned games and saves only.
