#!/bin/bash
# ASCII only. kit-ekho: cross-link ekho, ekho-stream and ekho-server against
# the three kits, then apply the L1 gates.
#
# The gates are the ones the wiki's container-free cross-compile page settled
# on, because they are the three questions answerable without the board:
#
#   1. is it aarch64?                       (readelf -h)
#   2. will every DT_NEEDED be there?       (readelf -d against the factory rootfs)
#   3. does it ask for a symbol version the board's libc/libstdc++ does not have?
#                                           (readelf -V against 2.31 / 3.4.28)
#
# Gate 2 is checkable because the target is not guessed at: the board's library
# inventory is a recorded fact (ref/board-libs.txt, taken from the factory
# rootfs). Nothing here runs on the board.
. /c/Users/YiZhidi/x-common.sh
set -u

KIT=$XROOT/kit-ekho
# Source tarball, produced on the dev machine with
#   git archive --format=tar.gz -o ekho-linux-<sha>.tar.gz <sha>     (no --prefix)
# and scp'd into the guest's home. The commit in the file name is what the
# kit's licence file and MANIFEST will carry, so keep the two in step; the
# archive has no .git, which is why the commit is read from the name.
#
# The default is the newest tarball sitting in the guest's home, not a
# hardcoded sha: a stale default silently packs the *previous* commit and
# stamps its sha into the licence file and MANIFEST. Pass a path as $1 to
# override.
TB=${1:-$(ls -t /c/Users/YiZhidi/ekho-linux-*.tar.gz 2>/dev/null | head -1)}
[ -n "$TB" ] || die "no ekho-linux-*.tar.gz in /c/Users/YiZhidi -- pass one as \$1"
COMMIT=$(basename "$TB" | sed -n 's/^ekho-linux-\([0-9a-f]\{7,\}\)\.tar\.gz$/\1/p')
[ -n "$COMMIT" ] || die "cannot read a commit out of $(basename "$TB")"
SRC=$XROOT/src/ekho-linux
BOARD=/c/Users/YiZhidi/board-libs.txt
SHIM=$XROOT/kit-glibc-compat/include/ekho-cross-stat.h
LOG=$XROOT/logs/ekho-configure.log

echo "=== kit-ekho -> $KIT ==="
[ -f "$TB" ] || die "no $TB"
kit_reset "$KIT"

echo "--- fresh source tree ---"
rm -rf "$SRC"; mkdir -p "$SRC"
# The dev machine's clock runs ~15 s ahead of the guest's, and git archive
# stamps every file with the commit time, so a tarball made from a fresh
# commit extracts as "time stamp is N s in the future". GNU tar reports that
# on stderr and exits non-zero, which the `|| die` below used to read as a
# failed extraction -- of an extraction that had in fact succeeded. Genuine
# extraction failures are still fatal.
tar --warning=no-timestamp -xzf "$TB" -C "$SRC" || die "extract failed"
cd "$SRC" || exit 1
echo "  $(find . -type f | wc -l) files"

echo "--- autotools (--copy, because Windows will not make the symlinks) ---"
aclocal -I m4             > "$XROOT/logs/ekho-auto.log" 2>&1 || { echo "  aclocal FAILED";    tail -8 "$XROOT/logs/ekho-auto.log" | sed 's/^/    /'; exit 1; }
libtoolize --copy --force >> "$XROOT/logs/ekho-auto.log" 2>&1 || { echo "  libtoolize FAILED"; exit 1; }
autoheader                >> "$XROOT/logs/ekho-auto.log" 2>&1 || { echo "  autoheader FAILED"; exit 1; }
automake --add-missing --copy --force-missing >> "$XROOT/logs/ekho-auto.log" 2>&1 || { echo "  automake FAILED"; tail -8 "$XROOT/logs/ekho-auto.log" | sed 's/^/    /'; exit 1; }
autoconf                  >> "$XROOT/logs/ekho-auto.log" 2>&1 || { echo "  autoconf FAILED"; exit 1; }
echo "  configure: $(test -f configure && echo ok || echo MISSING)"

echo "--- configure ---"
# utf8cpp is header-only and the fork includes it as plain "utf8.h"
# (src/character.h:28). On a Linux host that resolves out of /usr/include; off
# MSYS2 the headers live one level deeper and need an explicit -I, which is what
# the Windows build used too (-I/mingw64/include/utf8cpp in build_final.sh).
# This is the one thing the tree genuinely needs from its build host.
U8=/d/yzd/MSYS2/mingw64/include/utf8cpp
[ -f "$U8/utf8.h" ] || die "no utf8cpp headers at $U8"
echo "  utf8cpp: $U8  ($(grep -hoE 'UTF8_CPP_VERSION[^0-9]*[0-9]+' "$U8/utf8.h" 2>/dev/null | head -1))"
echo "  $($CXX --version | head -1)"
# No -std here on purpose. The tree used to need one handed to it, because gcc
# 11+ default to gnu++17 and the code includes <filesystem> unconditionally, so
# it built on the dev machine and nowhere else. configure.ac now probes for C++17
# and adds -std=gnu++17 itself; that this build works without a CXXFLAGS here is
# the check that the fix is real.
echo "  compiler default __cplusplus: $(echo | "$CXX" -x c++ -E -dM - 2>/dev/null | awk '/__cplusplus/{print $3}')  (gnu++14: the tree must ask for C++17 itself)"
# The glibc shim, and it is not optional. The toolchain's sysroot is glibc 2.33;
# the board runs 2.31. That is invisible until a symbol's *version* moved between
# the two, and in this tree exactly three did: stat, fstat and lstat, which glibc
# only began exporting as real symbols in 2.33. Building against 2.33's headers
# therefore emits stat@GLIBC_2.33, which the board's libc has no version entry
# for at all, so the loader refuses the binary. The shim points those three at
# __xstat/__fxstat/__lxstat, exported at GLIBC_2.17 by every aarch64 glibc
# including the board's. It is a forced include rather than a patch because it is
# a property of this toolchain and this target, not of ekho; it is inert unless
# __aarch64__ && __linux__, and it works at any -O level.
#
# The honest alternative -- pin the headers to focal's glibc 2.31 -- was built and
# does not work: libstdc++'s <cstdlib> reaches the C headers with
# `#include_next <stdlib.h>`, which skips anything added ahead of the C++
# directory, so the overlay pins <sys/stat.h> only where it is included directly
# and mixes 2.31 into 2.33 everywhere else.
[ -f "$SHIM" ] || die "no glibc shim at $SHIM (run x-kit-glibc-compat.sh first)"
echo "  glibc shim: $(basename "$SHIM")  (stat/fstat/lstat -> __xstat/__fxstat/__lxstat @ GLIBC_2.17)"
# PKG_CONFIG_LIBDIR replaces the search path outright rather than adding to it,
# so the kits are the ONLY place sndfile and espeak-ng can come from. Without
# that the host's own .pc files would be found and the build would quietly link
# x86_64 libraries.
PKG_CONFIG_LIBDIR="$XROOT/kit-sndfile/lib/pkgconfig:$XROOT/kit-espeak/lib/pkgconfig" \
./configure \
    --host=aarch64-none-linux-gnu \
    --prefix="$KIT" \
    --without-pulseaudio --without-curl --without-mpg123 --without-mp3lame --without-soxr \
    CC="$CC" CXX="$CXX" AR="$AR" RANLIB="$RANLIB" STRIP="$STRIP" \
    CPPFLAGS="-I$U8 -include $SHIM" \
    > "$LOG" 2>&1
rc=$?
if [ $rc -ne 0 ]; then
    echo "  configure FAILED rc=$rc"
    tail -25 "$LOG" | sed 's/^/    /'
    exit 1
fi
echo "  --- what configure decided ---"
sed -n '/ekho-linux configuration/,/^$/p' "$LOG" | sed 's/^/    /'
echo "  --- the .pc files it actually used ---"
grep -E 'sndfile|espeak' "$LOG" | head -6 | sed 's/^/    /'

echo "--- make ---"
make -j8 > "$XROOT/logs/ekho-make.log" 2>&1
rc=$?
if [ $rc -ne 0 ]; then
    # A parallel build interleaves twelve compilers into one log, and this host
    # has been seen to fail a compile with no message at all. Retry serially: if
    # it is a real error the message will be readable, and if it was the host,
    # the retry simply succeeds.
    echo "  make -j8 rc=$rc -- retrying serially so nothing is interleaved"
    grep -nE 'error|Error' "$XROOT/logs/ekho-make.log" | head -10 | sed 's/^/    /'
    make -j1 > "$XROOT/logs/ekho-make-serial.log" 2>&1
    rc=$?
fi
if [ $rc -ne 0 ]; then
    echo "  make FAILED rc=$rc"
    grep -iE 'error|Error' "$XROOT/logs/ekho-make-serial.log" | head -25 | sed 's/^/    /'
    exit 1
fi
echo "  ok -- $(grep -c 'CXX ' "$XROOT/logs/ekho-make.log") translation units, warnings: $(grep -ci 'warning:' "$XROOT/logs/ekho-make.log")"

echo
echo "=== collect the binaries ==="
mkdir -p "$KIT/bin"
for b in ekho ekho-stream ekho-server; do
    [ -f "$b" ] || die "$b was not built"
    cp "$b" "$KIT/bin/$b"
    "$STRIP" "$KIT/bin/$b"
    printf '  %-12s %9s bytes\n' "$b" "$(stat -c%s "$KIT/bin/$b")"
done

echo
echo "============================================================"
echo "== L1 GATE 1: is it aarch64?"
echo "============================================================"
g1=0
for b in ekho ekho-stream ekho-server; do
    H=$("$READELF" -h "$KIT/bin/$b" 2>/dev/null)
    cls=$(echo "$H" | awk '/Class:/{print $2}')
    mach=$(echo "$H" | awk '/Machine:/{print $2}')
    type=$(echo "$H" | awk '/Type:/{print $2}')
    if [ "$cls" = "ELF64" ] && [ "$mach" = "AArch64" ]; then
        printf '  PASS  %-12s %s %s %s\n' "$b" "$cls" "$mach" "$type"
    else
        printf '  FAIL  %-12s %s %s %s\n' "$b" "$cls" "$mach" "$type"; g1=1
    fi
done

echo
echo "============================================================"
echo "== L1 GATE 2: DT_NEEDED closure against the factory rootfs"
echo "============================================================"
g2=0
[ -f "$BOARD" ] || die "no $BOARD (the board's library inventory)"
echo "  board inventory: $(wc -l < "$BOARD") entries"
for b in ekho ekho-stream ekho-server; do
    echo "  --- $b ---"
    need=$("$READELF" -d "$KIT/bin/$b" | grep -o 'Shared library: *\[[^]]*\]' | sed 's/.*\[//;s/\]//')
    for so in $need; do
        if grep -qxF "$so" "$BOARD"; then
            printf '    ok    %s (on the board)\n' "$so"
        elif [ -e "$KIT/lib/$so" ]; then
            printf '    ok    %s (shipped in kit-ekho)\n' "$so"
        else
            printf '    FAIL  %s  -- not on the board and not in the kit\n' "$so"; g2=1
        fi
    done
    rp=$("$READELF" -d "$KIT/bin/$b" | grep -E 'RPATH|RUNPATH' | sed 's/.*\[//;s/\]//')
    [ -n "$rp" ] && echo "    note  RUNPATH=$rp"
done

echo
echo "============================================================"
echo "== L1 GATE 3: symbol version ceiling"
echo "==   board glibc $BOARD_GLIBC_MAX, libstdc++ $BOARD_GLIBCXX_MAX, cxxabi $BOARD_CXXABI_MAX"
echo "============================================================"
g3=0
le() {  # le <a> <b> : true when a <= b under version ordering
    [ "$(printf '%s\n%s\n' "$1" "$2" | sort -V | tail -1)" = "$2" ]
}
ceiling() {  # ceiling <binaries...> <prefix> <max>
    local max=$2 prefix=$1 bin
    shift 2
    for bin in "$@"; do
        local found want over=""
        found=$("$READELF" -V "$KIT/bin/$bin" 2>/dev/null \
                | grep -oE "${prefix}_[0-9]+(\.[0-9]+)*" | sort -uV)
        want=$(echo "$found" | tail -1)
        for v in $found; do le "${v#${prefix}_}" "$max" || over="$over $v"; done
        if [ -z "$over" ]; then
            printf '  PASS  %-12s %-8s max=%-10s (ceiling %s)\n' "$bin" "$prefix" "${want:-none}" "$max"
        else
            printf '  FAIL  %-12s %-8s needs%s -- above the ceiling %s\n' "$bin" "$prefix" "$over" "$max"; g3=1
        fi
    done
}
for b in ekho ekho-stream ekho-server; do
    ceiling GLIBC   "$BOARD_GLIBC_MAX"   "$b"
    ceiling GLIBCXX "$BOARD_GLIBCXX_MAX" "$b"
    ceiling CXXABI  "$BOARD_CXXABI_MAX"  "$b"
done

echo
echo "============================================================"
echo "== linked statically against the kits? (no libsndfile/espeak DT_NEEDED)"
echo "============================================================"
for b in ekho ekho-stream ekho-server; do
    # DT_NEEDED is read from the shipped (stripped) binary, because that is what
    # the loader will see. The symbols have to be read from the unstripped build
    # -- a stripped binary has no symbol table, which is exactly why the first
    # version of this check reported 0 for everything.
    n=$("$READELF" -d "$KIT/bin/$b" | grep -cE 'NEEDED.*lib(sndfile|espeak|sonic)')
    ns=$("$NM" -a "$SRC/$b" 2>/dev/null | grep -c ' T espeak_Initialize')
    nf=$("$NM" -a "$SRC/$b" 2>/dev/null | grep -c ' T sf_open')
    nc=$("$NM" -a "$SRC/$b" 2>/dev/null | grep -c ' T sonicSetSpeed')
    printf '  %-12s sndfile/espeak/sonic DT_NEEDED=%s   espeak_Initialize=%s  sf_open=%s  sonicSetSpeed=%s\n' \
        "$b" "$n" "$ns" "$nf" "$nc"
done
echo "  (each of the three must be DT_NEEDED=0 with its symbols present: that is static,"
echo "   and it is the only option for espeak-ng and sonic, which the board does not have)"

echo
echo "============================================================"
echo "== runtime data, and where espeak-ng looks for its own"
echo "============================================================"
mkdir -p "$KIT/share"
if [ -d /d/yzd/ekho_inst/share/ekho-data ]; then
    cp -RLf /d/yzd/ekho_inst/share/ekho-data "$KIT/share/ekho-data"
    echo "  voice pack: $(find "$KIT/share/ekho-data" -type f | wc -l) files, $(du -sh "$KIT/share/ekho-data" | cut -f1)"
    echo "    .voice blobs: $(ls -1 "$KIT/share/ekho-data"/*.voice 2>/dev/null | xargs -r -n1 basename | tr '\n' ' ')"
else
    echo "  voice pack: MISSING at /d/yzd/ekho_inst/share/ekho-data"
fi
if [ -d "$XROOT/kit-espeak/share/espeak-ng-data" ]; then
    cp -RLf "$XROOT/kit-espeak/share/espeak-ng-data" "$KIT/share/espeak-ng-data"
    echo "  espeak data: $(find "$KIT/share/espeak-ng-data" -type f | wc -l) files (snapshot of kit-espeak's, so kit-ekho stands alone)"
fi
# Trim to Mandarin + English. Both sources above are left whole;
# only the kit's copies are cut. See cross/x-trim-data.sh for why each kept
# file is read at runtime and each dropped one is not.
if [ -f "$SRC/cross/x-trim-data.sh" ]; then
    echo "  --- trim to Mandarin + English ---"
    bash "$SRC/cross/x-trim-data.sh" "$KIT/share/ekho-data" "$KIT/share/espeak-ng-data" \
        | sed 's/^/    /'
else
    echo "  trim script missing: $SRC/cross/x-trim-data.sh"
    echo "  (kit will carry the full data pack)"
fi
echo "  --- how espeak-ng 1.50 resolves an unset data path ---"
grep -rn 'ESPEAK_DATA_PATH' "$XROOT/src/espeak-ng-1.50+dfsg/src/libespeak-ng" 2>/dev/null | head -5 | sed 's/^/    /'

cat > "$KIT/run.sh" <<'EOF'
#!/bin/sh
# Run one of the kit's binaries with its runtime data, from anywhere.
# ekho reads EKHO_DATA_PATH; espeak-ng reads ESPEAK_DATA_PATH. Neither is
# compiled in, so pointing them at the kit is what makes this a kit rather
# than a set of binaries that only work from /usr/share.
K=$(cd "$(dirname "$0")" && pwd)
EKHO_DATA_PATH="$K/share/ekho-data"
ESPEAK_DATA_PATH="$K/share/espeak-ng-data"
export EKHO_DATA_PATH ESPEAK_DATA_PATH
case "${1:-}" in
  ""|-h|--help)
    echo "usage: $0 ekho|ekho-stream|ekho-server [args...]"
    echo "  EKHO_DATA_PATH   = $EKHO_DATA_PATH"
    echo "  ESPEAK_DATA_PATH = $ESPEAK_DATA_PATH"
    exit 0 ;;
esac
prog=$1; shift
exec "$K/bin/$prog" "$@"
EOF
chmod +x "$KIT/run.sh"
echo "  launcher: $KIT/run.sh"

echo
echo "============================================================"
echo "== licences: what has to travel with these binaries"
echo "============================================================"
# ekho is GPL-2.0-or-later; the files this fork adds (src/alsa_backend.*,
# streaming, http_server, text_normalize, ekho_stream_main.cpp,
# ekho_server_main.cpp) are GPL-3.0-or-later. The three libraries are linked in
# *statically*, so their licences attach to this binary, not to a .so the loader
# would fetch. espeak-ng is GPL-3.0-or-later and both sides are "or later", so
# the combination has to be conveyed under GPL-3. GPL also requires the
# corresponding source to travel with the binary, so the kit says where the
# source is and carries the notices.
mkdir -p "$KIT/share/doc"
for f in COPYING LICENSE AUTHORS; do
    [ -f "$SRC/$f" ] && cp -f "$SRC/$f" "$KIT/share/doc/ekho-$f"
done
[ -f "$XROOT/src/espeak-ng-1.50+dfsg/COPYING" ] && \
    cp -f "$XROOT/src/espeak-ng-1.50+dfsg/COPYING" "$KIT/share/doc/espeak-ng-COPYING"
[ -f "$SRC/third_party/sonic/sonic.c" ] && \
    sed -n '1,8p' "$SRC/third_party/sonic/sonic.c" > "$KIT/share/doc/libsonic-NOTICE.txt"
for f in "$XROOT/src/libsndfile-1.0.28/COPYING" "$XROOT/src/libsndfile-1.0.28/COPYING.LESSER"; do
    [ -f "$f" ] && cp -f "$f" "$KIT/share/doc/libsndfile-$(basename "$f")"
done
{
    echo "kit-ekho: licences and corresponding source"
    echo "==========================================="
    echo
    echo "Statically linked into bin/ekho, bin/ekho-stream and bin/ekho-server:"
    echo
    echo "  ekho-linux (this fork)         GPL-2.0-or-later    see ekho-COPYING / ekho-LICENSE"
    echo "      the files this fork adds (src/alsa_backend.*, src/streaming.*,"
    echo "      src/http_server.*, src/text_normalize.*, src/ekho_stream_main.cpp,"
    echo "      src/ekho_server_main.cpp) are GPL-3.0-or-later"
    echo "  espeak-ng 1.50+dfsg            GPL-3.0-or-later    see espeak-ng-COPYING"
    echo "  libsndfile 1.0.28              LGPL-2.1-or-later   see libsndfile-COPYING.LESSER"
    echo "  libsonic 0.2.0                 Apache-2.0          see libsonic-NOTICE.txt"
    echo
    echo "Not linked into the binary:"
    echo
    echo "  alsa-lib headers (compile time only; libasound.so.2 is dlopen'd at run"
    echo "  time, so no ALSA code is part of this binary)   LGPL-2.1-or-later"
    echo "  utf8cpp (build host, header only)               BSD-2-Clause"
    echo
    echo "Because the libraries are static, their licences attach to the whole"
    echo "binary, and espeak-ng (GPL-3.0-or-later) plus this fork's own additions"
    echo "make the combination GPL-3.0-or-later: the static binary is conveyed"
    echo "under GPL-3, while the ekho files inherited from upstream stay available"
    echo "under GPL-2.0-or-later as well. Either way the corresponding source has"
    echo "to travel with the binary. The source is:"
    echo
    echo "  S:\\work\\WCIS\\repos\\ekho-linux          (git, commit $COMMIT)"
    echo "  https://github.com/takomitkm/ekho         (the same commit, public)"
    echo "  S:\\apps\\llm\\agentwork\\crossekho         (the x-*.sh scripts that fetch, build, link and gate;"
    echo "                                            a copy of them travels in the fork under cross/)"
    echo "  upstream tarballs used to build the kits live in the build host's"
    echo "  $XROOT/dl/ directory."
    echo
    echo "espeak-ng data shipped in share/espeak-ng-data is part of espeak-ng"
    echo "(GPL-3.0-or-later). The ekho voice pack in share/ekho-data is from the"
    echo "ekho voice-data distribution; see the ekho project for its terms."
    echo
    echo "Built: $(date -u '+%Y-%m-%dT%H:%M:%SZ')"
} > "$KIT/share/doc/SOURCE-AND-LICENCES.txt"
echo "  $(ls -1 "$KIT/share/doc" | tr '\n' ' ')"

kit_finish "$KIT" ekho 11.0-linux.1 \
  "ekho-linux fork commit $COMMIT, cross-linked against kit-sonic + kit-sndfile + kit-espeak" \
  "bin/{ekho,ekho-stream,ekho-server} (stripped) + share/ekho-data (voice pack) + share/espeak-ng-data + run.sh + share/doc (licences and source pointer)"

echo
echo "============================================================"
if [ $((g1 + g2 + g3)) -eq 0 ]; then
    echo "== L1: ALL GATES PASS"
else
    echo "== L1: FAILED  arch=$g1 closure=$g2 versions=$g3"
fi
echo "============================================================"
exit $((g1 + g2 + g3))
