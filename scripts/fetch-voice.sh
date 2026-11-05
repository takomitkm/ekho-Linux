#!/bin/bash
# ekho-linux: fetch the voice data git deliberately does not carry, and lay it
# out where the tracked pipeline expects it.
#
# The packs stay out of the repository on purpose. The 44.1 kHz Mandarin wav
# source is ~120 MiB of audio, the 48 kHz pack built from it is 130 MiB, and
# GitHub rejects blobs over 100 MiB (limit effective since 2015-12), so no
# fitting repository can carry them directly. Upstream's own README says the
# same: voice files come from a download page. Upstream *does* track the small
# Cantonese blob (`jyutping.voice`, 5.6 MB); this tree carries neither that blob
# nor any Cantonese data at all -- since 2026-11-02 the only voice data here is
# Mandarin (the other voices were removed on request), so what this script lays
# out is the Mandarin wav tree plus the letter recordings. What a fresh clone
# misses, this script fetches from SourceForge (page
# `https://sourceforge.net/projects/e-guidedog/files/Ekho-Voice-Data/0.2/`),
# verifies by size and md5, and unpacks into the working tree:
#
#   pinyin-huang-44100-wav-v3.tar.xz   86,325,048 B  md5 10dd1cff22f88c202ef31b368c672184
#     -> ekho-data/pinyin/   44.1 kHz Mandarin wav tree (huang voice; see the
#                             provenance note below for why this and not yali)
#   alphabet-wong-16000.tar.bz2         1,023,561 B  md5 12afa3533f718c6e48b1734a0bccf6e8
#     -> ekho-data/alphabet/  16 kHz letter wavs (scripts/convert-alphabet.c upgrades
#                             them to the pack's rate and keeps `*.wav.16k` beside)
#
# Nothing is ever overwritten: every copy step uses no-clobber semantics and
# tar extraction uses --skip-old-files, so the script is idempotent and cannot
# clobber a working data directory. The download cache lives outside the tree,
# in $HOME/.cache/ekho-voice by default (override with EKHO_VOICE_CACHE).
# On networks where SourceForge needs a tunnel, run it as
#     HTTPS_PROXY=http://host:port scripts/fetch-voice.sh
# (curl honours the lower-case https_proxy too).
#
# After this script, the tracked pipeline finishes the packs (details in
# README "This fork: Linux only" and scripts/reg-44k.sh, usage lines in the
# two converter sources):
#
#   scripts/build-native.sh                       # engine + tools
#   # engine-generate the 44.1 kHz originals out of ekho-data/pinyin/
#   # (upstream README: with pinyin.index/pinyin.voice absent, run the engine
#   # once; it writes both from the wav tree), then rename the pair to
#   # ekho-data/pinyin.index.44k / pinyin.voice.44k
#   convert-voice ekho-data/pinyin.voice.44k ekho-data/pinyin.index.44k 48000 \
#                 ekho-data/pinyin.voice ekho-data/pinyin.index
#   convert-alphabet ekho-data/alphabet 48000     # 16 kHz -> pack rate, keeps *.wav.16k
#   scripts/reg-44k.sh                            # 44.1 kHz byte anchor + 48 kHz tempo check
#
# Provenance, recorded 2026-10-21 and settled 2026-11-06: the tracked pair of
# 44.1 kHz originals (pinyin.voice.44k, 125,287,518 B) was built before any
# source artifact was recorded on disk. The 2026-10-21 guess -- yali-v10 --
# turned out wrong when the whole pipeline was replayed from the 0.2 page:
# yali-v10 regenerates a pack of 63,679,170 B whose de5 chunk is half as long,
# so setSpeed()'s baseDelta lands on 0 (no tempo normalisation, speech 2.4x
# slower than intended) and one of its chunks has a WAV header with zero
# frames, which convert-voice refuses. pinyin-huang-44100-wav-v3 is the tree
# the pack came from: regenerated 44.1 kHz pair is exactly 125,287,518 B, the
# 48 kHz conversion exactly 136,359,454 B, setSpeed baseDelta=139, all seven
# scripts/reg-44k.sh byte anchors reproduce identically and sim.sh/server-test
# pass end to end (verified 2026-11-06). The container md5 of a regenerated
# pair still differs from the tracked one (E9632C7F4AD3C829B5880699B16A2A39 for
# pinyin.voice.44k) -- same size, same renders, different chunk-writer bytes;
# the anchors are the authority, not the container hash.

set -e

SF_BASE=${SF_BASE:-https://downloads.sourceforge.net/project/e-guidedog/Ekho-Voice-Data/0.2}
CACHE=${EKHO_VOICE_CACHE:-$HOME/.cache/ekho-voice}
DATA=$(cd "$(dirname "$0")/.." && pwd)/ekho-data
STAGE=$CACHE/unpack
PINYIN_TAR=${PINYIN_TAR:-pinyin-huang-44100-wav-v3.tar.xz}
PINYIN_TAR_MD5=${PINYIN_TAR_MD5:-10dd1cff22f88c202ef31b368c672184}
ALPHABET_TAR=alphabet-wong-16000.tar.bz2
ALPHABET_MD5=12afa3533f718c6e48b1734a0bccf6e8

for tool in curl tar md5sum; do
  command -v "$tool" >/dev/null 2>&1 || { echo "MISSING tool: $tool (needs curl, tar with xz/bz2 support, md5sum)" >&2; exit 1; }
done

mkdir -p "$CACHE" "$STAGE" "$DATA"

echo "=== 1. cache + md5 check"
fetch() { # <tarball> <md5>
  local dst="$CACHE/$1"
  if [ ! -f "$dst" ]; then
    echo "  downloading $1"
    curl -L --fail --retry 2 -o "$dst.tmp" "$SF_BASE/$1" \
      || curl -L --fail --retry 2 -o "$dst.tmp" "https://sourceforge.net/projects/e-guidedog/files/Ekho-Voice-Data/0.2/$1/download"
    mv "$dst.tmp" "$dst"
  else
    echo "  cached: $1"
  fi
  echo "  $1  $(stat -c %s "$dst") bytes"
  echo "$2  $dst" | md5sum -c - >/dev/null || { echo "MD5 MISMATCH: $dst" >&2; exit 1; }
}
fetch "$PINYIN_TAR"    "$PINYIN_TAR_MD5"
fetch "$ALPHABET_TAR"  "$ALPHABET_MD5"

echo "=== 2. lay out 44.1 kHz Mandarin wav tree -> ekho-data/pinyin/"
mkdir -p "$DATA/pinyin"
if ls "$DATA/pinyin/"*.wav >/dev/null 2>&1; then
  echo "  ekho-data/pinyin/ already has wavs -- kept as is"
else
  tar -xf "$CACHE/$PINYIN_TAR" -C "$STAGE" --skip-old-files
  root=${PINYIN_TAR%.tar.xz}
  cp -n "$STAGE/$root"/*.wav "$DATA/pinyin/" 2>/dev/null || true
  cp -n "$STAGE/$root"/COPYING-* "$DATA/pinyin/" 2>/dev/null || true
  # the tarball root also carries a pinyin/README-style note if present
  p="$STAGE/$root/README"
  [ -f "$p" ] && cp -n "$p" "$DATA/pinyin/README" || true
fi
echo "  ekho-data/pinyin/: $(ls "$DATA/pinyin" | wc -l) entries"

echo "=== 3. lay out 16 kHz letter wavs -> ekho-data/alphabet/"
mkdir -p "$DATA/alphabet"
tar -xf "$CACHE/$ALPHABET_TAR" -C "$STAGE" --skip-old-files
cp -n "$STAGE"/alphabet/*.wav "$DATA/alphabet/" 2>/dev/null || true
echo "  ekho-data/alphabet/: $(ls "$DATA/alphabet"/*.wav 2>/dev/null | wc -l) letter wavs (16 kHz originals; convert-alphabet does the rest)"

echo "=== 4. (removed 2026-11-02) the 16 kHz Cantonese wav tree ==="
# This step used to fetch jyutping-wong-16000-v6 and lay it out as
# ekho-data/jyutping/. The Cantonese data was removed from the tree on request,
# so fetching it again would only put back what the tree deliberately dropped.

echo "=== done. Next steps are in this file's header: build-native, engine-generate 44.1k, convert-voice, convert-alphabet, reg-44k."
