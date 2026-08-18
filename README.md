# ie-secret-link

Unlocks the "secret link" (sister-version link) content in Inazuma Eleven GO Galaxy
Big Bang on a single console, without owning Supernova.

The save format research lives in [NOTES.md](NOTES.md). Format and offsets are ported from
[Tiniifan/InazumaElevenSaveEditor](https://github.com/Tiniifan/InazumaElevenSaveEditor).

## Layout

- `pctool/iesave.py` — PC reference implementation: save codec (Level-5 XORShift + CRC),
  `info` / `set-link` / `selftest` commands. Used to validate the logic; the real target is below.
- `3ds/` — 3DS homebrew app (.cia): patches the game save directly on console,
  installable with FBI via "install from URL". (In progress.)

## Install on 3DS

FBI → Remote Install → Scan QR Code, or enter the URL manually:

```
https://github.com/ulysse-bonneau/ie-secret-link/releases/latest/download/ie-secret-link.cia
```

QR code for that URL: [render it here](https://api.qrserver.com/v1/create-qr-code/?size=300x300&data=https%3A%2F%2Fgithub.com%2Fulysse-bonneau%2Fie-secret-link%2Freleases%2Flatest%2Fdownload%2Fie-secret-link.cia) and scan with FBI.

The app finds the Big Bang/Supernova save automatically (game card or installed title),
backs up the original save to `sd:/ie-secret-link/` before every write, then patches the
link level and fixes the checksum in place. A `.3dsx` for Homebrew Launcher is on the
release page too.

## PC tool usage

```
python3 pctool/iesave.py info <save.ie>
python3 pctool/iesave.py set-link <0-3> <save-in> <save-out> [--dry-run] [--force]
python3 pctool/iesave.py selftest
```

Level 3 requires chapter >= 10 and the version-exclusive team beaten, otherwise the
save glitches — the tool refuses without `--force`.

For personal use on legitimately-owned games and saves only.
