#!/bin/bash
# ekho-linux: native build in the WSL simulation environment.
#
# The tree lives on a DrvFs mount, which is slow enough to dominate link time,
# so the source is mirrored into the WSL native filesystem, built there, and
# only the artefacts are copied back.
#
# Requires no root: libsonic and the alsa-lib headers are vendored under
# third_party/, so the only system packages needed are libsndfile1-dev and
# libespeak-ng-dev.
set -e

SRC=${SRC:-/mnt/s/work/WCIS/repos/ekho-linux}
WORK=${WORK:-~/ekho-build}
PREFIX=${PREFIX:-$WORK/prefix}
JOBS=${JOBS:-$(nproc)}

echo "=== 0. toolchain ==="
for t in gcc g++ make autoconf automake libtoolize pkg-config; do
  printf '  %-12s %s\n' "$t" "$(command -v "$t" 2>/dev/null || echo MISSING)"
done

echo
echo "=== 1. dependencies ==="
missing=0
for m in sndfile espeak-ng; do
  v=$(pkg-config --modversion "$m" 2>/dev/null)
  if [ -n "$v" ]; then
    printf '  %-14s %s\n' "$m" "$v"
  else
    printf '  %-14s MISSING (system package)\n' "$m"
    missing=1
  fi
done
for f in third_party/sonic/sonic.h third_party/alsa/include/alsa/asoundlib.h; do
  if [ -f "$SRC/$f" ]; then
    printf '  %-14s %s B (vendored)\n' "$(echo "$f" | cut -d/ -f2)" "$(wc -c < "$SRC/$f")"
  else
    printf '  %-14s MISSING (vendored)\n' "$(echo "$f" | cut -d/ -f2)"
    missing=1
  fi
done
if [ "$missing" = 1 ]; then
  echo "  FATAL: this build needs no root, so it cannot install anything."
  echo "  Install libsndfile1-dev + libespeak-ng-dev, or restore third_party/."
  exit 1
fi

echo
echo "=== 2. mirror to $WORK ==="
mkdir -p "$WORK"
rsync -a --delete \
      --exclude '.git' --exclude 'out' --exclude 'build-aux' --exclude 'm4' \
      --exclude 'ekho-data/pinyin' --exclude 'ekho-data/jyutping' \
      --exclude 'ekho-data/alphabet' --exclude 'ekho-data/piper' \
      "$SRC/" "$WORK/"
echo "  files: $(find "$WORK" -type f | wc -l)"

cd "$WORK"

echo
echo "=== 3. autogen ==="
./autogen.sh > "$WORK/autogen.log" 2>&1 \
  || { echo "  autogen FAILED"; tail -25 "$WORK/autogen.log"; exit 1; }
echo "  configure generated"

echo
echo "=== 4. configure ==="
./configure --prefix="$PREFIX" \
            --with-alsa \
            --with-sonic \
            --without-pulseaudio \
            --without-curl \
            --disable-dist-voice \
            > "$WORK/configure.log" 2>&1 \
  || { echo "  configure FAILED"; tail -40 "$WORK/configure.log"; exit 1; }
sed -n '/ekho-linux configuration/,/^$/p' "$WORK/configure.log" | sed 's/^/  /'

echo
echo "=== 5. make -j$JOBS ==="
make -j"$JOBS" > "$WORK/make.log" 2>&1 || {
  echo "  make FAILED"
  grep -E "error:|Error [0-9]" "$WORK/make.log" | head -25 | sed 's/^/    /'
  echo "  --- last 30 lines ---"
  tail -30 "$WORK/make.log" | sed 's/^/    /'
  exit 1
}
echo "  OK"
ls -la ekho ekho-stream ekho-server ekho-worker 2>/dev/null | sed 's/^/  /'

echo
echo "=== 6. warnings ==="
echo "  total: $(grep -c 'warning:' "$WORK/make.log")"
grep 'warning:' "$WORK/make.log" | sed 's/.*warning: /  /' | sort | uniq -c | sort -rn | head -12 | sed 's/^/  /'

echo
echo "=== 7. copy artefacts back ==="
mkdir -p "$SRC/out"
for f in ekho ekho-stream ekho-server ekho-worker test_ekho \n         test_normalize test_pcmring test_http test_ssml test_resample test_alsa; do
  [ -f "$f" ] && cp -f "$f" "$SRC/out/"
done
ls -la "$SRC/out" | sed 's/^/  /'

echo
echo "build complete: native in $WORK, artefacts in $SRC/out"