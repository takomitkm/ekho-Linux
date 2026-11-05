#!/bin/bash
# ASCII only. Re-derive the Linux fork from the pristine upstream tree, then
# apply every post-strip fix, then verify the result is structurally sound.
#
# The strip scripts are destructive by design, so they only ever run against a
# fresh copy: anything added to src/ after the strip (streaming.*, alsa_*,
# text_normalize.*) lives in git and is restored afterwards, never deleted.
set -e

PRISTINE=${PRISTINE:-/mnt/c/Users/takomi/AppData/Local/Temp/opencode/ekho-patched}
REPO=${REPO:-/mnt/s/work/WCIS/repos/ekho-linux}
PY=/mnt/c/Users/takomi/AppData/Local/Temp/opencode
DRV='/c/Program Files/Python313/python.exe'
[ -x "$DRV" ] || DRV=$(command -v python3)

cd "$REPO"

echo "=== 1. stash anything not in the pristine tree ==="
mkdir -p /tmp/ekho-extra
rm -rf /tmp/ekho-extra/*
for f in text_normalize.h text_normalize.cpp streaming.h streaming.cpp \
         alsa_backend.h alsa_backend.cpp ekho_stream_main.cpp; do
  [ -f "src/$f" ] && cp -f "src/$f" /tmp/ekho-extra/
done
ls /tmp/ekho-extra | sed 's/^/  /'

echo
echo "=== 2. restore src/ from pristine upstream ==="
find src -maxdepth 1 -type f -delete
cp "$PRISTINE"/src/*.cpp "$PRISTINE"/src/*.h src/
rm -f src/mingw_compat.h src/stdafx.h src/gtk2.cpp
echo "  $(ls src | wc -l) files"

echo
echo "=== 3. strip windows branches (condition-evaluating, comment-aware) ==="
"$DRV" "$PY/strip2.py"
"$DRV" "$PY/fix_inc.py"    > /dev/null
"$DRV" "$PY/fix_posix.py"

echo
echo "=== 4. restore the added modules ==="
cp -f /tmp/ekho-extra/* src/
echo "  $(ls src/text_normalize.* src/streaming.* src/alsa_backend.* src/ekho_stream_main.cpp 2>/dev/null | wc -l) files back"

echo
echo "=== 5. guard the remaining libcurl uses ==="
"$DRV" "$PY/guard_curl.py"

echo
echo "=== 6. uninitialised TextType in Word::split ==="
grep -n '^  TextType type;' src/word.cpp || true
sed -i 's|^  TextType type;$|  // gcc cannot prove every path that reaches a push_back()\n  // assigns this, and an uninitialised enum read is UB, not a warning.\n  TextType type = PHONETIC;|' src/word.cpp
grep -n -A1 'TextType type = PHONETIC' src/word.cpp | sed 's/^/  /'

echo
echo "=== 7. structural check ==="
"$DRV" - "$REPO/src" <<'PYEOF'
import os, re, sys
src = sys.argv[1]
bad = 0
for name in sorted(os.listdir(src)):
    if not name.endswith((".cpp", ".h")):
        continue
    t = open(os.path.join(src, name), encoding="utf-8", errors="surrogateescape").read()
    # strip block and line comments before counting braces
    nc = re.sub(r"/\*.*?\*/", "", t, flags=re.S)
    nc = re.sub(r"//[^\n]*", "", nc)
    depth = minv = 0
    for ln in nc.split("\n"):
        s = ln.strip()
        if re.match(r"^#\s*(if|ifdef|ifndef)\b", s): depth += 1
        elif re.match(r"^#\s*endif\b", s):
            depth -= 1; minv = min(minv, depth)
    o, c = nc.count("{"), nc.count("}")
    ok = (depth == 0 and minv == 0 and o == c)
    if not ok:
        bad += 1
        print("  %-24s #if %2d (min %2d)  braces %5d/%5d  BAD"
              % (name, depth, minv, o, c))
print("  %s" % ("all files balanced" if bad == 0 else "%d FILE(S) BROKEN" % bad))
PYEOF

echo
echo "=== 8. no windows leftovers ==="
if grep -rn 'stdafx\|mingw_compat\|ekho_access\|ekho_mkdir\|ekho_bzero\|ekho_setenv\|ENABLE_WIN32\|__MINGW32__' src/ >/dev/null 2>&1; then
  grep -rn 'stdafx\|mingw_compat\|ekho_access\|ekho_mkdir\|ekho_bzero\|ekho_setenv\|ENABLE_WIN32\|__MINGW32__' src/ | head -10 | sed 's/^/  /'
else
  echo "  clean"
fi