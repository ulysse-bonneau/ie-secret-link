#!/usr/bin/env python3
"""Inazuma Eleven GO Galaxy save codec + secret-link patcher.

Format ported from Tiniifan/InazumaElevenSaveEditor (see NOTES.md):
- Level-5 XORShift stream cipher, seed = last 4 bytes (LE u32).
- CRC-32 (zlib) of the encrypted body [0:-8], stored LE at [-8:-4].
- Decrypted Galaxy save: u16 magic 0x40F1 at offset 4.
- Secret link level: u8 at 0x90B4 (0..3). Chapter: u8 at 0x9F1C.
"""

import argparse
import struct
import sys
import zlib

GALAXY_MAGIC = 0x40F1
LINK_OFFSET = 0x90B4
CHAPTER_OFFSET = 0x9F1C


def _odd_primes(limit=1621):
    sieve = bytearray([1]) * (limit + 1)
    sieve[0:2] = b"\x00\x00"
    for i in range(2, int(limit**0.5) + 1):
        if sieve[i]:
            sieve[i * i :: i] = bytearray(len(sieve[i * i :: i]))
    return [i for i in range(3, limit + 1) if sieve[i]]


ODD_PRIMES = _odd_primes()


def _keytable(seed):
    s = seed
    states = []
    for i in range(3):
        s ^= s >> 30
        s = (i + 1 + s * 0x6C078965) & 0xFFFFFFFF
        states.append(s)
    states.append(0x03DF95B3)
    table = list(range(256))
    for _ in range(4096):
        x, y = states[0], states[3]
        states[0], states[1], states[2] = states[1], states[2], states[3]
        x ^= (x << 11) & 0xFFFFFFFF
        x ^= x >> 8
        y ^= y >> 19
        states[3] = x ^ y
        r = states[3] % 0x10000
        r1, r2 = r & 0xFF, (r >> 8) & 0xFF
        if r1 != r2:
            a, b = table[r1], table[r2]
            table[a], table[b] = table[b], table[a]
    return table


def _xor_body(data):
    """Symmetric XOR of everything but the 8-byte trailer. Seed read from trailer."""
    seed = struct.unpack_from("<I", data, len(data) - 4)[0]
    table = _keytable(seed)
    out = bytearray(data)
    ka = 0
    for i in range(len(data) - 8):
        if i % 0x100 == 0:
            ka = ODD_PRIMES[table[(i & 0xFF00) >> 8]]
        out[i] ^= table[(ka * (i + 1)) & 0xFF]
    return out


def decrypt(raw):
    return _xor_body(raw)


def encrypt(plain):
    """Re-encrypt with the seed kept in the trailer, recompute CRC over encrypted body."""
    out = _xor_body(plain)
    struct.pack_into("<I", out, len(out) - 8, zlib.crc32(bytes(out[:-8])))
    return out


def check(raw):
    """Return (crc_ok, magic) for an encrypted save."""
    crc_ok = struct.unpack_from("<I", raw, len(raw) - 8)[0] == zlib.crc32(bytes(raw[:-8]))
    magic = struct.unpack_from("<H", decrypt(raw), 4)[0]
    return crc_ok, magic


def cmd_info(args):
    raw = open(args.save, "rb").read()
    crc_ok, magic = check(raw)
    plain = decrypt(raw)
    print(f"size:       {len(raw)} bytes")
    print(f"crc:        {'OK' if crc_ok else 'MISMATCH'}")
    print(f"magic:      0x{magic:04X} ({'GO Galaxy' if magic == GALAXY_MAGIC else 'not Galaxy'})")
    if magic == GALAXY_MAGIC:
        print(f"chapter:    {plain[CHAPTER_OFFSET]}")
        print(f"link level: {plain[LINK_OFFSET]}")
    return 0


def cmd_set_link(args):
    raw = open(args.save_in, "rb").read()
    crc_ok, magic = check(raw)
    if magic != GALAXY_MAGIC:
        sys.exit(f"refusing: magic 0x{magic:04X} is not a GO Galaxy save")
    if not crc_ok:
        sys.exit("refusing: CRC mismatch, input save is corrupt")
    plain = decrypt(raw)
    chapter, old = plain[CHAPTER_OFFSET], plain[LINK_OFFSET]
    if args.level == 3 and chapter < 10 and not args.force:
        sys.exit(f"refusing: level 3 requires chapter >= 10 (save is at {chapter}) "
                 "and the version-exclusive team beaten; use --force if sure")
    if args.level > 2 and chapter >= 10 and not args.force:
        sys.exit("refusing: level 3 glitches the save unless the version-exclusive team "
                 "is beaten; use --force to confirm you beat it")
    print(f"link level: {old} -> {args.level} (byte at 0x{LINK_OFFSET:04X}), chapter {chapter}")
    if args.dry_run:
        print("dry run, nothing written")
        return 0
    plain[LINK_OFFSET] = args.level
    open(args.save_out, "wb").write(encrypt(plain))
    print(f"written: {args.save_out}")
    return 0


def cmd_selftest(args):
    import random
    rng = random.Random(0)
    plain = bytearray(rng.randrange(256) for _ in range(0x1000))
    struct.pack_into("<H", plain, 4, GALAXY_MAGIC)
    struct.pack_into("<I", plain, len(plain) - 4, 0xDEADBEEF)
    raw = encrypt(plain)
    assert raw != plain, "cipher is a no-op"
    dec = decrypt(raw)
    assert dec[:-8] == plain[:-8], "decrypt(encrypt(x)) != x"
    assert encrypt(dec) == raw, "round-trip not byte-identical"
    assert check(raw) == (True, GALAXY_MAGIC), "check() broken"
    print("selftest OK")
    return 0


def main():
    p = argparse.ArgumentParser(prog="iesave")
    sub = p.add_subparsers(dest="cmd", required=True)
    i = sub.add_parser("info", help="show save info")
    i.add_argument("save")
    i.set_defaults(func=cmd_info)
    s = sub.add_parser("set-link", help="set secret link level")
    s.add_argument("level", type=int, choices=range(4))
    s.add_argument("save_in")
    s.add_argument("save_out")
    s.add_argument("--dry-run", action="store_true")
    s.add_argument("--force", action="store_true")
    s.set_defaults(func=cmd_set_link)
    t = sub.add_parser("selftest", help="codec round-trip test")
    t.set_defaults(func=cmd_selftest)
    args = p.parse_args()
    sys.exit(args.func(args))


if __name__ == "__main__":
    main()
