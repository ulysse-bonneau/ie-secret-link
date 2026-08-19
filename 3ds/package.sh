#!/bin/sh
# Package iesm.elf into a .cia (needs bannertool + makerom in PATH)
set -e
cd "$(dirname "$0")"
mkdir -p build
bannertool makebanner -i assets/banner.png -a assets/audio.wav -o build/banner.bnr
bannertool makesmdh -s "IESM" -l "Inazuma Eleven Save Manager" \
    -p "ulysse-bonneau" -i assets/icon.png -o build/icon.icn
makerom -f cia -o iesm.cia -elf iesm.elf -rsf app.rsf \
    -banner build/banner.bnr -icon build/icon.icn -exefslogo -target t \
    -DAPP_TITLE="IESM" -DAPP_PRODUCT_CODE="CTR-P-IESM" -DAPP_UNIQUE_ID=0xF9CE5
echo "built iesm.cia"
