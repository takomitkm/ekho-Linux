#!/bin/bash
# ekho-linux: cross-build the board binaries (aarch64), needing no root anywhere.
#
# Why this script exists
# ----------------------
# The board runs Ubuntu 20.04 with glibc 2.31, and its `ekho` binaries are
# statically linked against libsndfile and espeak-ng. "Just install a cross
# toolchain" is not enough:
#
#   * the distro's own gcc-15 cross produces a binary that asks for
#     GLIBC_2.38 / GLIBCXX_3.4.32 and will not even load on the board;
#   * the board image has no libsndfile headers and no static libsndfile, so
#     that has to be built for the target here;
#   * espeak-ng is only available on the board as headers plus a shared
#     library, whose own dependencies (libpcaudio, libsonic, libasound,
#     libpulse) are on the board but not in the cross sysroot.
#
# So: focal's gcc-9 cross (glibc <= 2.30), a locally built static libsndfile,
# and espeak-ng linked dynamically against the board's own copies -- the link
# is told to leave the shared libraries' internal symbols unresolved and lets
# the board's loader deal with them at run time.
#
# Everything is downloaded and unpacked under $WORK (default ~/ekho-cross-root),
# so nothing here needs sudo and nothing outside that directory is touched.
#
# Usage:
#   scripts/build-cross-aarch64.sh              # prepare + build ekho-worker
#   TARGETS="ekho-worker ekho-server" ...       # pick what to build
#   BOARD=root@192.168.1.113                    # where to copy espeak-ng from
#
set -e

SRC=${SRC:-$(cd "$(dirname "$0")/.." && pwd)}
WORK=${WORK:-$HOME/ekho-cross-root}
BOARD=${BOARD:-root@192.168.1.113}
JOBS=${JOBS:-$(nproc)}
TARGETS=${TARGETS:-ekho-worker}
UBU=${UBU:-https://archive.ubuntu.com/ubuntu/pool/main}

ROOT=$WORK/toolchain          # cross gcc-9 + focal sysroot, unpacked
BOARD_LIBS=$WORK/board         # headers/libs copied from the board
SNDFILE=$WORK/sndfile          # static libsndfile for aarch64
PC=$WORK/pc                    # hand-written espeak-ng.pc for the above
BUILD=$WORK/build              # the mirrored source tree

mkdir -p "$ROOT" "$BOARD_LIBS" "$PC" "$WORK/debs"

# ---------------------------------------------------------------------------
# 1. focal gcc-9 cross toolchain (the same glibc generation as the board)
# ---------------------------------------------------------------------------
if [ ! -x "$ROOT/usr/bin/aarch64-linux-gnu-g++-9" ]; then
  echo "=== 1. toolchain: focal gcc-9 cross (glibc 2.31 era)"
  G=$UBU/g/gcc-9-cross
  C=$UBU/c/cross-toolchain-base
  B=$UBU/b/binutils
  I=$UBU/i/isl
  X=$UBU/g/gcc-10-cross
  dl() {  # dl <url> <filename>
    [ -f "$WORK/debs/$2" ] || curl -sSL -o "$WORK/debs/$2" "$1"
  }
  dl "$G/gcc-9-aarch64-linux-gnu-base_9.4.0-1ubuntu1~20.04.2cross2_amd64.deb" gcc9-base.deb
  dl "$G/cpp-9-aarch64-linux-gnu_9.4.0-1ubuntu1~20.04.2cross2_amd64.deb"   cpp9.deb
  dl "$G/gcc-9-aarch64-linux-gnu_9.4.0-1ubuntu1~20.04.2cross2_amd64.deb"   gcc9.deb
  dl "$G/g++-9-aarch64-linux-gnu_9.4.0-1ubuntu1~20.04.2cross2_amd64.deb"   gpp9.deb
  dl "$G/libgcc-9-dev-arm64-cross_9.4.0-1ubuntu1~20.04.2cross2_all.deb"    libgcc9dev.deb
  dl "$G/libstdc++-9-dev-arm64-cross_9.4.0-1ubuntu1~20.04.2cross2_all.deb" libstdc9dev.deb
  dl "$X/libgcc-s1-arm64-cross_10.5.0-1ubuntu1~20.04cross1_all.deb"        libgccs1.deb
  dl "$C/libc6-arm64-cross_2.31-0ubuntu9.9cross1_all.deb"                  libc6.deb
  dl "$C/libc6-dev-arm64-cross_2.31-0ubuntu9.9cross1_all.deb"              libc6dev.deb
  dl "$B/binutils-aarch64-linux-gnu_2.34-6ubuntu1_amd64.deb"               binutils.deb
  dl "$I/libisl22_0.22.1-1_amd64.deb"                                      libisl22.deb
  for d in "$WORK"/debs/*.deb; do dpkg-deb -x "$d" "$ROOT"; done
fi
export PATH=$ROOT/usr/bin:$PATH
export LD_LIBRARY_PATH=$ROOT/usr/lib/x86_64-linux-gnu:$LD_LIBRARY_PATH

# ---------------------------------------------------------------------------
# 2. espeak-ng headers + libraries, straight from the board
# ---------------------------------------------------------------------------
if [ ! -d "$BOARD_LIBS/usr/include/espeak-ng" ]; then
  echo "=== 2. espeak-ng headers/libs from $BOARD"
  mkdir -p "$BOARD_LIBS"
  ssh -o BatchMode=yes "$BOARD" 'tar cf - -C / usr/include/espeak-ng \
      usr/lib/aarch64-linux-gnu/libespeak-ng.so usr/lib/aarch64-linux-gnu/libespeak-ng.so.1 \
      usr/lib/aarch64-linux-gnu/libespeak-ng.so.1.1.49 \
      usr/lib/aarch64-linux-gnu/libpcaudio.so.0 usr/lib/aarch64-linux-gnu/libsonic.so.0' \
    | tar xf - -C "$BOARD_LIBS"
fi
# utf8cpp is a header-only library used by src/character.h; borrow it from the
# build host so the cross build sees the same headers.
for h in /usr/include/utf8.h /usr/include/utf8cpp /usr/include/utf8; do
  [ -e "$h" ] && cp -a "$h" "$BOARD_LIBS/usr/include/" 2>/dev/null || true
done

cat > "$PC/espeak-ng.pc" <<EOF
prefix=$BOARD_LIBS
libdir=\${prefix}/usr/lib/aarch64-linux-gnu
includedir=\${prefix}/usr/include

Name: espeak-ng
Description: eSpeak NG (headers and shared library taken from the board)
Version: 1.50
Libs: -L\${libdir} -lespeak-ng
Cflags: -I\${includedir}
EOF

# ---------------------------------------------------------------------------
# 3. libsndfile for aarch64 (static, WAV/PCM only, no external codecs)
# ---------------------------------------------------------------------------
if [ ! -f "$SNDFILE/lib/libsndfile.a" ]; then
  echo "=== 3. libsndfile for aarch64 (static)"
  cd "$WORK"
  [ -f libsndfile-1.2.2.tar.xz ] || curl -sSL -O \
      https://github.com/libsndfile/libsndfile/releases/download/1.2.2/libsndfile-1.2.2.tar.xz
  rm -rf sndfile-src && mkdir sndfile-src
  tar xf libsndfile-1.2.2.tar.xz -C sndfile-src --strip-components=1
  cd sndfile-src
  ./configure --host=aarch64-linux-gnu --prefix="$SNDFILE" \
      CC="aarch64-linux-gnu-gcc-9 --sysroot=$ROOT" \
      CFLAGS="-O2 -std=gnu17" \
      --enable-static --disable-shared --disable-external-libs --disable-sqlite \
      --disable-alsa > cfg.log 2>&1 || { tail -20 cfg.log; exit 1; }
  make -j"$JOBS" > make.log 2>&1 || { tail -20 make.log; exit 1; }
  make install > inst.log 2>&1
fi

# ---------------------------------------------------------------------------
# 4. configure + build
# ---------------------------------------------------------------------------
echo "=== 4. mirror source and build: $TARGETS"
mkdir -p "$BUILD"
rsync -a --delete --exclude '.git' --exclude 'out' --exclude 'build-aux' --exclude 'm4' \
      --exclude 'ekho-data/pinyin' --exclude 'ekho-data/jyutping' \
      --exclude 'ekho-data/alphabet' --exclude 'ekho-data/piper' \
      "$SRC/" "$BUILD/"
cd "$BUILD"
./autogen.sh > autogen.log 2>&1

export PKG_CONFIG_LIBDIR=$SNDFILE/lib/pkgconfig:$PC
./configure --host=aarch64-linux-gnu \
            CC="aarch64-linux-gnu-gcc-9 --sysroot=$ROOT" \
            CXX="aarch64-linux-gnu-g++-9 --sysroot=$ROOT" \
            LDFLAGS="-L$BOARD_LIBS/usr/lib/aarch64-linux-gnu -Wl,-rpath-link,$BOARD_LIBS/usr/lib/aarch64-linux-gnu -Wl,--allow-shlib-undefined" \
            --with-alsa --with-sonic --without-pulseaudio --without-curl \
            --without-mpg123 --without-soxr --disable-dist-voice \
            > configure.log 2>&1 || { tail -30 configure.log; exit 1; }
sed -n '/ekho-linux configuration/,/^$/p' configure.log

make -j"$JOBS" $TARGETS > make.log 2>&1 || {
  echo "  MAKE FAILED"; grep -m10 -E "error:" make.log; tail -20 make.log; exit 1; }

echo "=== 5. artefacts ==="
for t in $TARGETS; do
  [ -f "$t" ] || continue
  file "$t"
  echo "  needed:"; aarch64-linux-gnu-readelf -d "$t" | grep NEEDED | sed 's/^/    /'
  echo "  glibc symbols: $(aarch64-linux-gnu-objdump -T "$t" 2>/dev/null | grep -oE 'GLIBC_[0-9.]+' | sort -V -u | tail -1)"
done

echo
echo "copy to the board with:  scp <binaries> $BOARD:/root/ekho-bench/bin/ && ssh $BOARD 'chmod +x /root/ekho-bench/bin/*'"
echo "(scp does not preserve the execute bit on this setup, and the board needs it)"