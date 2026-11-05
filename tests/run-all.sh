#!/bin/bash
# ekho-linux: the full test tier.
#
#   * unit binaries (no voice data needed) -- same ones `make check` runs
#   * the synthesis-assertion scripts, only when a voice pack is present
#
# Usage:
#   tests/run-all.sh [--with-reg44k]
#
# Environment (defaults match scripts/sim.sh):
#   SRC   repository root        (default: this checkout)
#   OUT   binary directory       (default: $SRC/out)
#   EKHO_DATA_PATH  voice data   (default: $SRC/ekho-data)
#
# Exit code: number of failed pieces (0 = all green). Skipped pieces are
# reported but not counted -- a skip is a statement about the environment,
# not a pass.
set -u

SRC=${SRC:-$(cd "$(dirname "$0")/.." && pwd)}
OUT=${OUT:-$SRC/out}
export EKHO_DATA_PATH="${EKHO_DATA_PATH:-$SRC/ekho-data}"
WITH_REG44K=0
[ "${1:-}" = "--with-reg44k" ] && WITH_REG44K=1

FAILED=0
SKIPPED=0

hr()  { printf '\n=== %s ===\n' "$*"; }
ok()  { printf '  ok    %s\n' "$*"; }
bad() { printf '  FAIL  %s\n' "$*"; FAILED=$((FAILED + 1)); }
skp() { printf '  skip  %s\n' "$*"; SKIPPED=$((SKIPPED + 1)); }

# ---------------------------------------------------------------------------
hr "0. prerequisites"
# ---------------------------------------------------------------------------
BINARIES="ekho ekho-stream ekho-server ekho-worker test_ekho"
missing_bins=0
for b in $BINARIES; do
  [ -x "$OUT/$b" ] || { echo "  missing: $OUT/$b"; missing_bins=1; }
done
if [ "$missing_bins" = 1 ]; then
  echo "  run scripts/build-native.sh first (see README.md)"
  exit 1
fi
ok "binaries in $OUT"

have_data=0
if [ -f "$EKHO_DATA_PATH/pinyin.voice" ]; then
  have_data=1
  ok "voice data at $EKHO_DATA_PATH"
else
  skp "no voice data at $EKHO_DATA_PATH/pinyin.voice"
  echo "        fetch it with scripts/fetch-voice.sh; the unit tier still runs"
fi

# ---------------------------------------------------------------------------
hr "1. unit tier (no voice data needed)"
# ---------------------------------------------------------------------------
# The binaries were built into $OUT too, but the assertions are the same ones
# `make check` runs from the build tree; prefer the build tree if it exists.
UNIT_DIR=""
if [ -f "$OUT/test_normalize" ]; then
  UNIT_DIR="$OUT"
else
  # fall back: run them from wherever make check left them
  for d in "$SRC" "$PWD"; do
    [ -x "$d/test_normalize" ] && { UNIT_DIR="$d"; break; }
  done
fi
if [ -z "$UNIT_DIR" ]; then
  skp "unit binaries not found (build them: make test_normalize test_pcmring ...)"
else
  for t in test_normalize test_pcmring test_http test_ssml test_resample test_alsa; do
    if [ ! -x "$UNIT_DIR/$t" ]; then
      skp "$t (not built)"
      continue
    fi
    out=$("$UNIT_DIR/$t" 2>&1)
    rc=$?
    line=$(printf '%s\n' "$out" | grep -E 'checks,' | tail -1)
    if [ $rc -eq 0 ]; then
      ok "$t  ($line)"
    else
      bad "$t (rc=$rc)"
      printf '%s\n' "$out" | grep -E 'FAIL |XPASS ' | head -5 | sed 's/^/        /'
    fi
  done
fi

# ---------------------------------------------------------------------------
hr "2. engine smoke (needs voice data)"
# ---------------------------------------------------------------------------
if [ "$have_data" = 1 ]; then
  if [ -x "$OUT/test_ekho" ]; then
    if "$OUT/test_ekho" >/dev/null 2>&1; then
      ok "test_ekho synthesised"
    else
      rc=$?
      if [ $rc -eq 77 ]; then skp "test_ekho (no data where it looked)"; else
        bad "test_ekho rc=$rc"
      fi
    fi
  fi
else
  skp "test_ekho"
fi

# ---------------------------------------------------------------------------
hr "3. synthesis assertions (needs voice data)"
# ---------------------------------------------------------------------------
if [ "$have_data" = 1 ]; then
  for s in sim.sh server-test.sh; do
    if bash "$SRC/scripts/$s" > "/tmp/ekho-run-$s.log" 2>&1; then
      ok "scripts/$s"
    else
      bad "scripts/$s"
      grep -E 'FAIL ' "/tmp/ekho-run-$s.log" | head -5 | sed 's/^/        /'
    fi
  done
  if [ "$WITH_REG44K" = 1 ]; then
    # The 44.1 kHz anchor needs the .44k pack pair, which only exists where
    # the pack was rebuilt from the wav tree (see scripts/fetch-voice.sh).
    if [ -f "$EKHO_DATA_PATH/pinyin.voice.44k" ]; then
      if bash "$SRC/scripts/reg-44k.sh" > /tmp/ekho-run-reg44k.log 2>&1; then
        ok "scripts/reg-44k.sh"
      else
        bad "scripts/reg-44k.sh"
        tail -20 /tmp/ekho-run-reg44k.log | sed 's/^/        /'
      fi
    else
      skp "scripts/reg-44k.sh (no 44.1 kHz anchor pair)"
    fi
  fi
else
  skp "scripts/sim.sh"
  skp "scripts/server-test.sh"
  [ "$WITH_REG44K" = 1 ] && skp "scripts/reg-44k.sh"
fi

# ---------------------------------------------------------------------------
printf '\n'
printf 'run-all: %d failed, %d skipped\n' "$FAILED" "$SKIPPED"
exit "$FAILED"
