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

## SD Link / data download unlock (UnlockAllData)

The editor's "Unlock Data Download + QRcode + Link with GO/CS Content" button
(chapter >= 2 required) writes these regions into the decrypted save
(`IEGOGalaxy.cs` SaveSaveInfo, UnlockAllData):

| Offset    | Len | Content                                  |
|-----------|-----|------------------------------------------|
| `0x8F62`  | 2   | `B9 08`                                  |
| `0x8F6A`  | 2   | `46 C9`                                  |
| `0x8FFD`  | 3   | `1C 0E FE`                               |
| `0x9000`  | 11  | flag bits                                |
| `0x902F`  | 9   | flag bits                                |
| `0x2ECB0` | 948 | record table (id + hash pairs, opaque)   |

Ported verbatim into `3ds/source/unlock_data.h` (generated from the C# source).
Blanket unlock — the editor does not split IE1/2/3 vs GO vs CS link content.

## Player block (Galaxy, 250 bytes each, 336 slots at 0xF83C)

Field offsets inside a block (from IEGOGalaxyHelper.PlayerBlock, Pack=1):
ID u32 @ +8 (0 = empty slot), GP s16 @ +28, TP s16 @ +30, Freedom s16 @ +32,
Level u8 @ +34, InvestedPoint s16[8] @ +52, AvatarID u32 @ +68,
equipment indexes s32[4] @ +76, moves 6x9 bytes @ +96.
Player name/pos/element database generated from Players.cs (Galaxy dict, 2024
entries) into `3ds/source/players_db.h` by `tools/gen_players.py`.

## Save info fields

Name @ 0x3C, team name @ 0x5C (32-byte fields, `00 88` terminated),
play time i32 seconds @ 0x20, prestige i32 @ 0x268D0, coins s16[5] @ 0x26CC8.

## Multi-game support (v0.5.0)

Per-game offsets in `3ds/source/games.c`, from the C# helper classes.
Magics: GO EU 0x2CF1 / GO JP 0x6CF1 / CS EU 0x4CF1 / CS JP 0xC4F1 / Galaxy 0x40F1.
Player block strides (C# bool marshals as 4 bytes in MoveBlock, so 12 B/move):
GO 204, CS 260, Galaxy 268. Player count = non-zero entries of the index table
(pindex_off), blocks contiguous from pdata_off — NOT one block per slot.
GP/TP/Freedom/Level are adjacent (s16,s16,s16,u8) in all three games.
GO secret link: u16 0x01C0 + u16 0x0000 written at 0x253 (level-3 unlock, no levels).
Unlock-all-data regions per game/region generated into unlock_data.h by
tools/gen_unlock.py; player name DBs (GO 1001 / CS 1578 / Galaxy 2024) by
tools/gen_players.py.

## Inventory (v0.6.0)

Item groups per game (offsets from helper classes, counts from OpenInventory):
group1 (idx i32, id u32, qty i32 = 12 B) / group2 (+qty-equipped = 16 B):
Galaxy 0xA394x608 / 0xC020x452; CS EU 0x14F4x512 / 0x2D00x336;
CS JP 0x14A4x512 / 0x2CB0x336; GO EU 0xA60x256 / 0x166Cx224;
GO JP 0xA10x256 / 0x161Cx224. Group3 (ownership-only, no qty) not edited.
Item name DBs (GO 1731 / CS 2906 / Galaxy 3728) generated by tools/gen_items.py.
Quantity edited in place (slot offset +8), floor = equipped count for group2.


## The inventory checksum, solved (v0.15.5)

`FE FF`-tagged u16 pair at `0x8F6A` and `0x9F16`. Zero = dormant, game skips
validation (fresh saves stay zero through normal play). Nonzero = the game
maintains it on save and validates it on load; its region covers the save
counters (0x9E14/0x9E94) and the inventory groups, but not the header, link
byte, or player blocks. The reference editor's UnlockAllData bytes for
`0x8F62`/`0x8F6A` are a donor save's checksum values captured by accident —
writing them arms validation (the corruption trap; editor issue #14).
IESM: unlock no longer writes those two fields, and every Galaxy commit zeros
the pair to restore the dormant state. The checksum algorithm itself remains
uncracked (all CRC16 polys, sums, Fletcher, word-hashes excluded on 4 samples)
— and no longer needs to be.


## Checksum: unsolved, feature abandoned (v0.15.6)

The v0.15.5 "zero the pair to disarm" theory FAILED on hardware: a committed
save with 0x8F6A/0x9F16 = 0000 still corrupts. So zeroing does not return the
game to its dormant (validation-skipped) state once it has begun maintaining
the checksum. The game .code (exefs .code, 3.4 MB) contains NO CRC16/CRC32
lookup table, so the routine is bespoke/bitwise. Cracking it needs full ARM
disassembly with no guarantee. Decision: abandon direct inventory editing;
inventory is view-only-with-warning, and item duplication is delegated to AR
cheats (which route through the game's own save path). Kept: gen_unlock.py
still excludes the donor-checksum bytes at 0x8F62/0x8F6A so the SD-Link unlock
never arms validation.
