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

## PC tool usage

```
python3 pctool/iesave.py info <save.ie>
python3 pctool/iesave.py set-link <0-3> <save-in> <save-out> [--dry-run] [--force]
python3 pctool/iesave.py selftest
```

Level 3 requires chapter >= 10 and the version-exclusive team beaten, otherwise the
save glitches — the tool refuses without `--force`.

For personal use on legitimately-owned games and saves only.
