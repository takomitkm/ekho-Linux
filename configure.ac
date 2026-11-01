# -*- Autoconf -*-
# ekho-linux: Linux-only fork of hgneng/ekho.
#
# Differences from upstream 11.0:
#   * --enable-win32 removed (this fork is Linux-only)
#   * ALSA added as a first-class output backend (upstream has pulseaudio only)
#   * pulseaudio no longer defaults to "on" through an undefined shell variable,
#     which made plain ./configure fail on any box without libpulse
#   * a missing libsonic now warns instead of silently disabling resampling
#   * libcurl is optional (it is only used by the optional piper backend)

AC_PREREQ([2.69])
AC_INIT([ekho], [11.0-linux.1], [fork of hgneng/ekho])
AC_CONFIG_AUX_DIR([build-aux])
AC_CONFIG_MACRO_DIRS([m4])
AM_INIT_AUTOMAKE([1.11 foreign subdir-objects -Wall])
m4_ifdef([AM_SILENT_RULES], [AM_SILENT_RULES([yes])])
AC_CONFIG_HEADERS([config.h])

AC_PROG_CC
AC_PROG_CXX
AC_PROG_INSTALL
AC_PROG_MKDIR_P
AM_PROG_AR
AC_PROG_RANLIB

# ---------------------------------------------------------------------------
# Language standard
# ------------------
# src/libekho.cpp includes <filesystem> unconditionally, so C++17 is a
# requirement -- but nothing ever said so. That was invisible on the machine
# this was developed on, because gcc 11 and later default to gnu++17, and
# immediately fatal on the Arm 10.3 toolchain, whose gcc 10.3 defaults to
# gnu++14 and stops at "'filesystem' has not been declared". Ask for the
# standard explicitly, and refuse to configure if the compiler cannot do it.
AC_LANG_PUSH([C++])
AC_MSG_CHECKING([whether $CXX can compile the C++17 <filesystem> header])
ax_save_CXXFLAGS="$CXXFLAGS"
CXXFLAGS="$CXXFLAGS -std=gnu++17"
AC_COMPILE_IFELSE(
  [AC_LANG_PROGRAM([[#include <filesystem>
namespace ax_fs = std::filesystem;]],
                   [[return 0;]])],
  [AC_MSG_RESULT([yes])],
  [CXXFLAGS="$ax_save_CXXFLAGS"
   AC_MSG_RESULT([no])
   AC_MSG_ERROR([a C++17 compiler is required: src/libekho.cpp uses <filesystem>])])
AC_LANG_POP([C++])

LT_INIT([disable-shared])
PKG_PROG_PKG_CONFIG

# ---------------------------------------------------------------------------
# Headers, types, functions
# ---------------------------------------------------------------------------
AC_HEADER_STDBOOL
AC_C_CONST
AC_C_INLINE
AC_TYPE_SIZE_T
AC_CHECK_HEADERS([locale.h stdlib.h string.h unistd.h sys/time.h])
AC_CHECK_FUNCS([memset setlocale strstr clock_gettime])

# Pthreads. Hand-rolled rather than AX_PTHREAD so the fork needs no
# autoconf-archive macros on the build host.
PTHREAD_CFLAGS="-pthread"
AC_CHECK_LIB([pthread], [pthread_create],
  [PTHREAD_LIBS="-lpthread"],
  [PTHREAD_LIBS=""], [-lpthread])
AC_SUBST([PTHREAD_CFLAGS])
AC_SUBST([PTHREAD_LIBS])

# ---------------------------------------------------------------------------
# Mandatory dependency: libsndfile (wav/mp3/ogg container I/O)
# ---------------------------------------------------------------------------
PKG_CHECK_MODULES([SNDFILE], [sndfile >= 1.0.28],
  [AC_DEFINE([HAVE_SNDFILE], [1], [Define if libsndfile is available])],
  [AC_MSG_ERROR([libsndfile >= 1.0.28 is required (libsndfile1-dev)])])

# ---------------------------------------------------------------------------
# Mandatory dependency: espeak-ng (English front end)
#
# Upstream probed for the library but never AC_DEFINE'd ENABLE_ESPEAK, so the
# whole English path compiled out and multi-letter words were dropped silently.
# ---------------------------------------------------------------------------
PKG_CHECK_MODULES([ESPEAK_NG], [espeak-ng >= 1.45],
  [AC_DEFINE([ENABLE_ESPEAK], [1], [Enable English via espeak-ng])],
  [AC_MSG_ERROR([espeak-ng >= 1.45 is required (libespeak-ng-dev)])])

# ---------------------------------------------------------------------------
# libsonic: streaming resampler / speed / pitch.
#
# Upstream used a bare AC_CHECK_LIB with an empty else, so a missing libsonic
# silently produced HAVE_SONIC undefined and resampling stopped happening with
# no diagnostic at all.
#
# Here libsonic is vendored (third_party/sonic, two files, no dependencies) so
# the native build and the aarch64 cross build run identical code, and the
# macro is defined unconditionally when that directory is present.
# ---------------------------------------------------------------------------
AC_ARG_WITH([sonic],
  [AS_HELP_STRING([--without-sonic],
    [build without resampling; output stays at the voice pack sample rate])],
  [], [with_sonic=yes])
LIB_SONIC=""
SONIC_CFLAGS="-I\$(top_srcdir)/third_party/sonic"
if test "x$with_sonic" != xno; then
  if test -f "$srcdir/third_party/sonic/sonic.h"; then
    AC_DEFINE([HAVE_SONIC], [1], [Define if you have libsonic])
  else
    AC_MSG_ERROR([third_party/sonic/sonic.h is missing; restore it or pass --without-sonic])
  fi
fi
AC_SUBST([SONIC_CFLAGS])

# ---------------------------------------------------------------------------
# ALSA: primary output backend on embedded boards.
#
# Compiled against the vendored alsa-lib headers in third_party/alsa and bound
# at runtime with dlopen, so the build host needs no libasound2-dev and the
# result needs no libasound.so at link time -- only libasound.so.2 at run time,
# which is exactly what the target board has.
# ---------------------------------------------------------------------------
AC_ARG_WITH([alsa],
  [AS_HELP_STRING([--without-alsa], [do not build the ALSA output backend])],
  [], [with_alsa=auto])
ALSA_CFLAGS="-I\$(top_srcdir)/third_party/alsa/include"
if test -f "$srcdir/third_party/alsa/include/alsa/asoundlib.h"; then
  have_alsa_headers=yes
else
  have_alsa_headers=no
fi
if test "x$with_alsa" != xno; then
  if test "x$have_alsa_headers" = xyes; then
    AC_DEFINE([HAVE_ALSA], [1], [Define to build the ALSA output backend])
  else
    if test "x$with_alsa" = xyes; then
      AC_MSG_ERROR([--with-alsa given but third_party/alsa/include/alsa/asoundlib.h is missing])
    fi
    AC_MSG_WARN([third_party/alsa headers missing; the ALSA backend will not be built])
  fi
fi
AC_SUBST([ALSA_CFLAGS])

# ---------------------------------------------------------------------------
# PulseAudio: optional secondary backend.
#
# Upstream tested `$with_pulseaudio` without ever declaring it. An unset value
# compares unequal to "no", so the branch always ran and configure aborted on
# any machine without libpulse unless the user guessed the right flag.
# ---------------------------------------------------------------------------
AC_ARG_WITH([pulseaudio],
  [AS_HELP_STRING([--with-pulseaudio], [build the PulseAudio output backend])],
  [], [with_pulseaudio=no])
LIB_PULSEAUDIO=""
CFLAG_PULSEAUDIO=""
if test "x$with_pulseaudio" != xno; then
  PKG_CHECK_MODULES([PULSE], [libpulse-simple],
    [LIB_PULSEAUDIO="-lpulse-simple -lpulse"
     AC_DEFINE([HAVE_PULSEAUDIO], [1], [Define if you have libpulse])],
    [AC_MSG_ERROR([--with-pulseaudio given but libpulse-simple not found])])
fi
AC_SUBST([LIB_PULSEAUDIO])
AC_SUBST([PULSEAUDIO_CFLAGS])
AC_SUBST([PULSEAUDIO_LIBS])

# ---------------------------------------------------------------------------
# mpg123 / lame: compressed file output.
# ---------------------------------------------------------------------------
AC_ARG_WITH([mpg123],
  [AS_HELP_STRING([--without-mpg123], [do not build the mp3 input reader])],
  [], [with_mpg123=yes])
LIB_MPG123=""
if test "x$with_mpg123" != xno; then
  # Upstream probed for mpg123 and defined HAVE_MPG123 but never put -lmpg123
  # on the link line, so any build that actually used the macro failed at link
  # time with an unresolved mpg123_new.
  AC_CHECK_LIB([mpg123], [mpg123_new],
    [LIB_MPG123="-lmpg123"
     AC_DEFINE([HAVE_MPG123], [1], [Define if you have mpg123])],
    [AC_MSG_WARN([libmpg123 not found; mp3 input disabled])])
fi
AC_SUBST([LIB_MPG123])

AC_ARG_WITH([mp3lame],
  [AS_HELP_STRING([--with-mp3lame], [support MP3 audio output])],
  [], [with_mp3lame=no])
LIB_MP3LAME=""
if test "x$with_mp3lame" != xno; then
  AC_CHECK_LIB([mp3lame], [main],
    [LIB_MP3LAME="-lmp3lame -lm"
     AC_DEFINE([HAVE_MP3LAME], [1], [Define if you have libmp3lame])],
    [AC_MSG_ERROR([--with-mp3lame given but libmp3lame not found])])
fi
AC_SUBST([LIB_MP3LAME])

# soxr: optional higher quality rate conversion.
AC_ARG_WITH([soxr],
  [AS_HELP_STRING([--with-soxr], [use libsoxr for sample rate conversion])],
  [], [with_soxr=no])
LIB_SOXR=""
if test "x$with_soxr" != xno; then
  AC_CHECK_LIB([soxr], [main],
    [LIB_SOXR="-lsoxr"
     AC_DEFINE([HAVE_SOXR], [1], [Define if you have libsoxr])],
    [AC_MSG_ERROR([--with-soxr given but libsoxr not found])])
fi
AC_SUBST([LIB_SOXR])

# ---------------------------------------------------------------------------
# libcurl: only the optional piper backend needs it.
# Upstream made it mandatory, which is why the Windows build shipped curl.dll.
# ---------------------------------------------------------------------------
AC_ARG_WITH([curl],
  [AS_HELP_STRING([--without-curl], [do not build the optional piper backend])],
  [], [with_curl=yes])
LIB_CURL=""
CURL_CFLAGS=""
if test "x$with_curl" != xno; then
  PKG_CHECK_MODULES([CURL], [libcurl],
    [LIB_CURL="-lcurl"
     AC_DEFINE([ENABLE_CURL], [1], [Enable the piper HTTP back end])],
    [AC_MSG_WARN([libcurl not found; piper backend disabled])])
fi
AC_SUBST([LIB_CURL])

# ---------------------------------------------------------------------------
# Optional English back ends.
# ---------------------------------------------------------------------------
AC_ARG_ENABLE([festival],
  [AS_HELP_STRING([--enable-festival], [synthesize English through Festival])],
  [], [enable_festival=no])
LIB_FESTIVAL=""
CFLAG_FESTIVAL=""
if test "x$enable_festival" = xyes; then
  AC_CHECK_LIB([Festival], [main],
    [LIB_FESTIVAL="-lFestival -lestbase -lestools -leststring -lncurses"
     CFLAG_FESTIVAL="-I/usr/include/festival -I/usr/include/speech_tools"
     AC_DEFINE([ENABLE_FESTIVAL], [1], [Enable Festival])],
    [AC_MSG_ERROR([--enable-festival given but libFestival not found])])
fi
AC_SUBST([LIB_FESTIVAL])
AC_SUBST([CFLAG_FESTIVAL])

AC_ARG_ENABLE([flite],
  [AS_HELP_STRING([--enable-flite], [synthesize English through Flite])],
  [], [enable_flite=no])
LIB_FLITE=""
CFLAG_FLITE=""
if test "x$enable_flite" = xyes; then
  AC_CHECK_LIB([flite], [main],
    [LIB_FLITE="-lflite"
     AC_DEFINE([ENABLE_FLITE], [1], [Enable Flite])],
    [AC_MSG_ERROR([--enable-flite given but libflite not found])])
fi
AC_SUBST([LIB_FLITE])
AC_SUBST([CFLAG_FLITE])

# ---------------------------------------------------------------------------
# Voice data
# ---------------------------------------------------------------------------
AC_ARG_ENABLE([dist-voice],
  [AS_HELP_STRING([--disable-dist-voice], [do not bundle the voice packs])],
  [], [dist_voice=true])
AM_CONDITIONAL([DIST_VOICE_DATA], [test "x$dist_voice" = xtrue])

# ---------------------------------------------------------------------------
# Service front ends
#
# The engine can be driven two ways, and a build only needs the one it will
# actually run: ekho-server speaks HTTP (right when the caller is another
# machine or another language runtime), ekho-worker speaks a line protocol over
# a pipe (right when the caller is a process on the same box that already has
# one, and wants the engine resident without opening a port). Both are built by
# default so `make` keeps producing what it always did; each can be dropped
# with its --disable flag.
# ---------------------------------------------------------------------------
AC_ARG_ENABLE([http-server],
  [AS_HELP_STRING([--disable-http-server],
                  [do not build ekho-server, the HTTP front end])],
  [], [http_server=true])
AM_CONDITIONAL([BUILD_HTTP_SERVER], [test "x$http_server" = xtrue])

AC_ARG_ENABLE([worker],
  [AS_HELP_STRING([--disable-worker],
                  [do not build ekho-worker, the line-protocol front end])],
  [], [worker=true])
AM_CONDITIONAL([BUILD_WORKER], [test "x$worker" = xtrue])

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
AC_MSG_NOTICE([
  ekho-linux configuration
  ------------------------
  host              : $host
  prefix            : $prefix
  libsndfile        : yes ($sndfile_VERSION)
  espeak-ng         : yes ($espeak_ng_VERSION)  [ENABLE_ESPEAK]
  libsonic          : $with_sonic             [HAVE_SONIC]
  ALSA backend      : $have_alsa_headers  [HAVE_ALSA, dlopen]
  PulseAudio backend: $with_pulseaudio        [HAVE_PULSEAUDIO]
  mpg123            : $with_mpg123             [HAVE_MPG123]
  mp3lame           : $with_mp3lame
  soxr              : $with_soxr
  festival          : $enable_festival
  flite             : $enable_flite
  libcurl           : $with_curl
  voice data        : $dist_voice
  HTTP server       : $http_server             [ekho-server]
  line-protocol wkr : $worker                  [ekho-worker]
])

AC_CONFIG_FILES([Makefile])
AC_OUTPUT

AC_MSG_NOTICE([
run "$ECHO $PACKAGE_NAME" to see what this build can do.
])