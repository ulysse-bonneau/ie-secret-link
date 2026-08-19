#!/bin/sh
# C codec (3ds/source/codec.c) must produce byte-identical output to the
# Python reference (pctool/iesave.py) — a mismatch would corrupt real saves.
set -e
cd "$(dirname "$0")"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

cc -O2 -Wall -o "$tmp/drv" drv.c ../3ds/source/codec.c

python3 - "$tmp" <<'EOF'
import random, struct, sys
sys.path.insert(0, '../pctool')
import iesave
rng = random.Random(42)
plain = bytearray(rng.randrange(256) for _ in range(0x30000))
struct.pack_into('<H', plain, 4, iesave.GALAXY_MAGIC)
struct.pack_into('<I', plain, len(plain) - 4, 0x12345678)
open(sys.argv[1] + '/plain', 'wb').write(plain)
open(sys.argv[1] + '/py.bin', 'wb').write(iesave.encrypt(plain))
EOF

"$tmp/drv" "$tmp/plain" "$tmp/c.bin"
cmp "$tmp/c.bin" "$tmp/py.bin"
test "$("$tmp/drv" --magic "$tmp/c.bin")" = "40F1"
echo "parity OK"
