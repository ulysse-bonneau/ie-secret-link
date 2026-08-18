# Research notes — Inazuma Eleven GO Galaxy save format

Source: ported from [Tiniifan/InazumaElevenSaveEditor](https://github.com/Tiniifan/InazumaElevenSaveEditor)
(clone it into `reference/`, not committed). All offsets verified against that C# code, not guessed.

## Container format (Galaxy, `.ie` save file)

The raw save file is encrypted with the Level-5 XORShift stream cipher
(same algorithm as Yo-kai Watch saves, originally `yw_save.py` by togenyan).

Layout of the raw (encrypted) file:

| Region              | Content                                                        |
|---------------------|----------------------------------------------------------------|
| `[0 .. len-8)`      | XOR-encrypted save body                                        |
| `[len-8 .. len-4)`  | CRC-32 (zlib polynomial `0xEDB88320`) of the **encrypted** body `[0 .. len-8)`, stored **little-endian** |
| `[len-4 .. len)`    | cipher seed, little-endian u32                                 |

Cipher (see `pctool/iesave.py` for the port):
1. Seed the state list: 3 rounds of `s ^= s >> 30; s = (i+1 + s*0x6C078965) & 0xFFFFFFFF`, then append constant `0x03DF95B3`.
2. Run 4096 xorshift rounds to shuffle a 256-entry byte substitution table (indirect swaps: `swap(table[table[r1]], table[table[r2]])`).
3. Keystream: for byte `i`, every 0x100 bytes recompute `ka = ODD_PRIMES[table[(i & 0xFF00) >> 8]]`; then `key = table[(ka*(i+1)) & 0xFF]`. `ODD_PRIMES` = odd primes 3..1621.
4. XOR is symmetric: same routine encrypts and decrypts. Seed is kept unchanged on re-save; only the CRC is recomputed (over the re-encrypted body).

## Decrypted layout (Galaxy)

Game detection: `u16` at offset `0x4` = magic. `0x40F1` = GO Galaxy
(Big Bang and Supernova share the format; other magics: `0x2CF1`/`0x6CF1` = GO, `0x4CF1`/`0xC4F1` = Chrono Stones).

Offsets from `IEGOGalaxyHelper.cs`:

| Offset    | Type | Meaning                          |
|-----------|------|----------------------------------|
| `0x20`    | i32  | play time in seconds             |
| `0x3C`    | str  | player name (UTF-8, `00 88` terminated) |
| `0x5C`    | str  | team name                        |
| `0x8FEB`  | bits | play-records bitfield            |
| **`0x90B4`** | **u8** | **secret link level (0–3)** ← the thing we patch |
| `0x9F1C`  | u8   | current story chapter            |
| `0xA394`  | —    | item group 1 (index/id/qty)      |
| `0xC020`  | —    | item group 2 (+qty equipped)     |
| `0xDC6C`  | —    | item group 3 (index/id)          |
| `0xF83C`  | —    | player data (336 × PlayerBlock)  |
| `0x268D0` | i32×2| prestige / friendship money      |
| `0x26CC8` | s16×5| coins                            |
| `0x26E28` | —    | player index table               |

## Secret link semantics (from SaveInfoWindow.cs)

- Link level is a single byte, values 0–3.
- The editor caps the level at **2 while chapter < 10**.
- **Level 3 requires having beaten the version-exclusive team in your own version first** —
  setting 3 before that produces a *glitched save*. The editor shows an explicit warning.
  → our tool mirrors these guards (hence per-level options, not one boolean).
- No differential capture needed: the flag was already implemented in the reference editor
  (`IEGOGalaxy.cs:407/447`, `LinkOffset = 0x90B4`).

## Open questions / TODO

- [x] Title IDs (JPN): Big Bang `000400000010BA00`, Supernova `000400000010BB00`
      (sources: iegogalaxyeng.netlify.app, cia-3ds.com).
- [ ] Name + size of the save file inside the 3DS save archive (Checkpoint dump layout).
      Fallback plan: enumerate archive root, pick the file whose decrypt yields magic `0x40F1`.
- [ ] Verify the game accepts an in-place FS write + archive commit (CMAC is recomputed by
      FS on commit; the anti-rollback secure value is not touched by editing file contents).
- [ ] Confirm on real hardware that link levels 1/2 unlock the expected Supernova content.
