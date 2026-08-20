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

## Inventory: use Checkpoint cheats, not IESM, to duplicate items

The game protects the inventory region with an undocumented checksum that IESM can't yet
reproduce, so **committing inventory quantity edits corrupts Galaxy saves** (the reference
editor has the same limitation). IESM keeps inventory editing available behind a warning,
but for item duplication the reliable route is **AR cheat codes via Checkpoint / Luma's
Rosalina cheat menu**: cheats patch live RAM and the *game* writes the save (with a valid
checksum), so they never corrupt.

The "item ×99" style cheats also duplicate your **PalPack cards to ×99**, which clutters
the card list. IESM's **Inventory → Batch actions → PalPack cards → x1** trims them back
to one each — that in-place quantity-lowering is safe to commit.

Recovery note: if a save ever shows "corrupted" (e.g. after cheat use), restore the
**newest** backup — the save carries an anti-rollback counter, so older backups can be
rejected as rollbacks.

## Repo layout

- `3ds/` — the homebrew app (C, libctru).
- `pctool/iesave.py` — PC reference codec + CLI (`info` / `set-link` / `selftest`).
- `tests/parity.sh` — proves the C codec is byte-identical to the Python reference.
- `tools/` — asset and database generators (icon/banner/QR, players/items/moves/etc.).

For personal use on legitimately-owned games and saves only.
