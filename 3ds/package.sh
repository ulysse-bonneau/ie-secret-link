#!/bin/sh
# Package iesm.elf into a .cia (needs bannertool + makerom in PATH)
set -e
cd "$(dirname "$0")"
mkdir -p build

# HOME Menu caches icons per title+version: bump the CIA version each tag
# (vMAJ.MIN.MIC -> major<<10 | minor<<4 | micro)
case "${GITHUB_REF_NAME:-}" in
  v*) V="${GITHUB_REF_NAME#v}" ;;
  *)  V="0.0.1" ;;
esac
MAJ=$(echo "$V" | cut -d. -f1); MIN=$(echo "$V" | cut -d. -f2); MIC=$(echo "$V" | cut -d. -f3)
VER=$(( MAJ * 1024 + MIN * 16 + MIC ))
bannertool makebanner -i assets/banner.png -a assets/audio.wav -o build/banner.bnr
bannertool makesmdh -s "IESM" -l "Inazuma Eleven Save Manager" \
    -p "ulysse-bonneau" -i assets/icon.png -o build/icon.icn
makerom -f cia -o iesm.cia -elf iesm.elf -rsf app.rsf \
    -banner build/banner.bnr -icon build/icon.icn -exefslogo -target t -ver "$VER" \
    -DAPP_TITLE="IESM" -DAPP_PRODUCT_CODE="CTR-P-IESM" -DAPP_UNIQUE_ID=0xF9CE5
echo "built iesm.cia"
