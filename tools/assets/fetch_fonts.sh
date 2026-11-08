#!/bin/sh
# fetch_fonts.sh - download the fonts that gen_assets.py rasterizes into ui/fonts_data.cpp
#
# Source: the Google Fonts repository (github.com/google/fonts), all under the SIL Open Font License 1.1.
# Pinned to the same revision as the round gauge's pc_test/fetch_fonts.sh (2026-10-13), and every file is
# checked against the SHA-256 below, so the generated data is reproducible.
#   Barlow Condensed Bold Italic   digits (lap times, rpm, delta)
#   Chakra Petch Bold / SemiBold   Latin labels (LAP, LAST, BEST, VS BEST ...)
#   IBM Plex Sans JP Bold / SemiBold  Japanese UI text
#
# Usage: sh tools/assets/fetch_fonts.sh [dest dir]      default dest: build/fonts in the repository
# (build/ is ignored by git, so the downloaded files do not show up as changes)
set -eu

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
DEST="${1:-$ROOT/build/fonts}"
REV=66928797f271fed5ee428544e7229685e2d90127
BASE="https://raw.githubusercontent.com/google/fonts/$REV/ofl"

sha256_of() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1" | cut -d ' ' -f 1
  else
    shasum -a 256 "$1" | cut -d ' ' -f 1
  fi
}

# get <path in the repository> <file name> <sha256>
get() {
  if [ -f "$DEST/$2" ] && [ "$(sha256_of "$DEST/$2")" = "$3" ]; then
    echo "ok    $2"
    return 0
  fi
  if ! curl -fsSL -o "$DEST/$2.part" "$BASE/$1"; then
    rm -f "$DEST/$2.part"
    echo "FAIL: download $BASE/$1"
    exit 1
  fi
  got="$(sha256_of "$DEST/$2.part")"
  if [ "$got" != "$3" ]; then
    rm -f "$DEST/$2.part"
    echo "FAIL: $2 has SHA-256 $got, expected $3"
    exit 1
  fi
  mv "$DEST/$2.part" "$DEST/$2"
  echo "got   $2"
}

mkdir -p "$DEST"
get barlowcondensed/BarlowCondensed-BoldItalic.ttf BarlowCondensed-BoldItalic.ttf d8694f65cd70c3e5e600ead37657aab0adb5f63b275d0c1f6f4a7046de222145
get barlowcondensed/OFL.txt BarlowCondensed-OFL.txt 186d750eb496a4c17a76385f82be6aea2ac1cf2de074a811d63786cf374ea73f
get chakrapetch/ChakraPetch-Bold.ttf ChakraPetch-Bold.ttf 65fbf76d95651697275e19db4d717c0e95a789ddd3476478b05292104db278a0
get chakrapetch/ChakraPetch-SemiBold.ttf ChakraPetch-SemiBold.ttf 45264de3204ddbd5fb3e14a2402acd5c630d16650ae5fc221d2c52da46a6734b
get chakrapetch/OFL.txt ChakraPetch-OFL.txt 13831d02389d917d22fcfa6c79f98f8acbf61d230add0e73a4d4dc5f3bfb9e56
get ibmplexsansjp/IBMPlexSansJP-Bold.ttf IBMPlexSansJP-Bold.ttf bd8a98b9b55c9ea3f0fa1ed094c15981788cc9a41b0357f4453014fd1d44da19
get ibmplexsansjp/IBMPlexSansJP-SemiBold.ttf IBMPlexSansJP-SemiBold.ttf 5985c29d2d5d444072915cf5874dc80e0a3f5fadbf14a9e3013a03a8fe6d7ff5
get ibmplexsansjp/OFL.txt IBMPlexSansJP-OFL.txt 7e6b2818edbd8f6a01ae80641cc8f16a51080d08fb4e532be3a0b6f74adb07da
echo "fonts in $DEST"
