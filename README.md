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

## Galaxy inventory editing is unsafe — use AR cheats instead

Galaxy guards its counter+inventory region with a custom Level-5 checksum (a
`FE FF`-tagged u16 pair at `0x8F6A`/`0x9F16`, plus related fields). IESM can't
reproduce it — every standard CRC16/CRC32/sum/Fletcher/word-hash was ruled out
against real samples, and the game's executable contains no CRC table, so it's a
bespoke bitwise routine. **Committing inventory quantity edits corrupts Galaxy
saves.** The reference desktop editor has the identical limitation (its issue #14).

To duplicate items, use **AR cheat codes** via Checkpoint or Luma's Rosalina cheat
menu (e.g. "All items ×99"). Cheats patch live RAM and the *game* writes the save,
so the checksum is always valid. If a ×99 cheat inflates your PalPack cards, note
that IESM can't safely trim them either — do it in-game.

IESM keeps the inventory **viewer** (browse what you own, by category) but warns
before any edit. Everything else IESM writes — secret link, players, teams, save
info, records, banks — lives outside this region and is safe.

Recovery: if a save shows "corrupted", restore the **newest** backup first — the
save carries an anti-rollback counter and older backups can be rejected.

## Repo layout

- `3ds/` — the homebrew app (C, libctru).
- `pctool/iesave.py` — PC reference codec + CLI (`info` / `set-link` / `selftest`).
- `tests/parity.sh` — proves the C codec is byte-identical to the Python reference.
- `tools/` — asset and database generators (icon/banner/QR, players/items/moves/etc.).

For personal use on legitimately-owned games and saves only.
