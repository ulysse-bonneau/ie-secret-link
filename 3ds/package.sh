#!/bin/sh
# Package ie-secret-link.elf into a .cia (needs bannertool + makerom in PATH)
set -e
cd "$(dirname "$0")"
mkdir -p build
bannertool makebanner -i assets/banner.png -a assets/audio.wav -o build/banner.bnr
bannertool makesmdh -s "ie-secret-link" -l "Secret link patcher for IE GO Galaxy" \
    -p "ulysse-bonneau" -i assets/icon.png -o build/icon.icn
makerom -f cia -o ie-secret-link.cia -elf ie-secret-link.elf -rsf app.rsf \
    -banner build/banner.bnr -icon build/icon.icn -exefslogo -target t \
    -DAPP_TITLE="ie-secret-link" -DAPP_PRODUCT_CODE="CTR-P-IESL" -DAPP_UNIQUE_ID=0xF9CE5
echo "built ie-secret-link.cia"
