#!/bin/bash
# reg-44k.sh -- the 44.1 kHz regression anchor, plus the 48 kHz tempo check.
#
# Claim 1 (byte anchor). ekho-data/pinyin.voice.44k is the upstream 44.1 kHz
# Mandarin pack, and scripts/reg-44k/expected.md5 records what this engine
# rendered for the seven reference texts back when 44.1 kHz / mono was the
# default. The hash covers the whole WAV, header included, so one comparison
# exercises the text path, the pinyin dictionary, the syllable joins, the tempo
# normalisation, the (bypassed) resampler and the WAV writer at once. Rebuild
# from later sources and this must still hold byte for byte, or the change moved
# the Chinese output. The .44k pack is a separate file now that the shipped pack
# is 48 kHz, so this anchor does not depend on the default format at all.
#
# Claim 2 (tempo at 48 kHz). The shipped pack is that same audio resampled once
# offline to 48 kHz, so with the default output of 48 kHz the engine resamples
# nothing at run time. That is not free: the syllable-join walk in
# writeToSonicStream() decides where to splice by looking at the waveform on the
# pack's own sample grid, so a pack resampled offline picks slightly different
# cut points than a stream resampled at run time, and the wall-clock duration of
# a sentence moves by a fraction of a percent. This section prints the per-text
# delta against the 44.1 kHz render and fails if any text is more than 2% off or
# the mean more than 0.5% off.
#
#   Measured 2026-10-19, default output: mean -0.17%, worst -1.32% (07).
#   A rate-scaled join threshold was measured for the 48 kHz pack as well and
#   made it worse (worst 3.4% at quiet_level = 4227 = mOverlap*(48/44.1)^0.405),
#   so quiet_level stays equal to mOverlap for every rate >= 44100 -- see the
#   long comment in src/libekho_impl.cpp.
#
# usage: scripts/reg-44k.sh [outdir]        default: $PWD/reg-44k-out
# env:   EKHO=path/to/ekho                  default: $SRC/out/ekho
#        DATA=path/to/ekho-data             default: $SRC/ekho-data
set -u

SRC=$(cd "$(dirname "$0")/.." && pwd)
EKHO=${EKHO:-$SRC/out/ekho}
DATA=${DATA:-$SRC/ekho-data}
OUT=${1:-$PWD/reg-44k-out}
TEXTS=$SRC/scripts/reg-44k

[ -x "$EKHO" ] || { echo "reg-44k: no ekho binary at $EKHO (build it, or set EKHO=)"; exit 2; }
[ -f "$DATA/pinyin.voice.44k" ] || { echo "reg-44k: $DATA/pinyin.voice.44k is missing"; exit 2; }

mkdir -p "$OUT"

# A data directory whose Mandarin pack is the 44.1 kHz original; everything
# else symlinked from the shipped tree, so trimming the shipped data cannot
# silently change what this anchor runs against.
D44=$OUT/data44
rm -rf "$D44"
mkdir -p "$D44"
for f in "$DATA"/*; do ln -sfn "$f" "$D44/$(basename "$f")"; done
rm -f "$D44/pinyin.voice" "$D44/pinyin.index"
cp "$DATA/pinyin.voice.44k" "$D44/pinyin.voice"
cp "$DATA/pinyin.index.44k" "$D44/pinyin.index"

# Duration in seconds from the file's own header. The byte rate at offset 28
# folds in rate, channels and width, so this is right for both formats.
secs() { awk -v b="$(stat -c%s "$1" 2>/dev/null || echo 0)" \
             -v r="${2:-$(od -An -tu4 -j 28 -N 4 "$1" 2>/dev/null | tr -d ' ')}" \
             'BEGIN{printf "%.4f", (b-44)/(r ? r : 176400)}'; }

fail=0
hr() { printf '\n%s\n' "=== $* ==="; }

hr "1. 44.1 kHz pack, 44.1 kHz / mono: the reference bytes"
for t in "$TEXTS"/*.txt; do
  n=$(basename "$t" .txt)
  EKHO_DATA_PATH="$D44" "$EKHO" -v Mandarin -f "$t" \
    --samplerate=44100 --channels=1 -o "$OUT/$n.wav" >/dev/null 2>&1
done
if ( cd "$OUT" && md5sum -c --quiet "$TEXTS/expected.md5" ); then
  echo "  ok: all 7 renders byte-identical to the recorded reference"
else
  echo "  FAIL: the 44.1 kHz output moved (hashes above)"
  fail=1
fi

hr "2. shipped 48 kHz pack at the default 48 kHz / 2ch: same speed"
printf '  %-14s %10s %10s %10s\n' text 44k1ch 48k2ch delta
sum=0
n=0
worst=0
for t in "$TEXTS"/*.txt; do
  name=$(basename "$t" .txt)
  EKHO_DATA_PATH="$DATA" "$EKHO" -v Mandarin -f "$t" \
    -o "$OUT/$name-48.wav" >/dev/null 2>&1
  a=$(secs "$OUT/$name.wav")
  b=$(secs "$OUT/$name-48.wav")
  d=$(awk -v a="$a" -v b="$b" 'BEGIN{printf "%+.3f", 100*(b/a-1)}')
  printf '  %-14s %10s %10s %9s%%\n' "$name" "$a" "$b" "$d"
  sum=$(awk -v s="$sum" -v d="$d" 'BEGIN{printf "%.3f", s+d}')
  worst=$(awk -v w="$worst" -v d="$d" \
    'BEGIN{x=(d<0?-d:d); if (x>w) w=x; printf "%.3f", w}')
  n=$((n+1))
done
mean=$(awk -v s="$sum" -v n="$n" 'BEGIN{printf "%+.3f", s/n}')
printf '  mean %s%%  worst %s%%  (limits: mean 0.5%%, worst 2%%)\n' "$mean" "$worst"
if awk -v m="$mean" -v w="$worst" 'BEGIN{
      if (m == "" || w == "") exit 1;
      if ((m < 0 ? -m : m) > 0.5) exit 1;
      if (w > 2) exit 1;
      exit 0;
    }'; then
  echo "  ok: the resample-free pack speaks the same speed"
else
  echo "  FAIL: the 48 kHz pack moved the speaking rate"
  fail=1
fi

exit "$fail"
