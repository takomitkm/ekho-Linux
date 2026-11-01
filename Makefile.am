AM_CXXFLAGS = -Wall -O2 -g -DEKHO_DATA_PATH='"$(datadir)/ekho-data"'

noinst_LIBRARIES = libekho.a libsonic.a

# libsonic is vendored rather than taken from the system: it is four source
# files with no dependencies, and building it ourselves keeps the native and
# cross toolchains on exactly the same code.
libsonic_a_SOURCES = third_party/sonic/sonic.c
libsonic_a_CPPFLAGS = -I$(srcdir)/third_party/sonic

include_HEADERS = \
	src/ekho.h \
	src/word.h \
	src/ekho_dict.h \
	src/ssml.h \
	src/audio.h \
	src/character.h \
	src/phonetic_symbol.h \
	src/resample.h \
	src/zhy_symbol_map.h \
	src/zh_symbol_map.h \
	src/ekho_typedef.h \
	src/ekho_impl.h \
	src/text_normalize.h \
	src/streaming.h \
	src/http_server.h \
	src/alsa_backend.h

libekho_a_SOURCES = \
	src/english.cpp \
	src/libekho.cpp \
	src/libekho_impl.cpp \
	src/synthesize.cpp \
	src/ssml.cpp \
	src/audio.cpp \
	src/phonetic_symbol.cpp \
	src/resample.cpp \
	src/word.cpp \
	src/ekho_dict.cpp \
	src/number.cpp \
	src/server.cpp \
	src/musicxml.cpp \
	src/zhy_symbol_map.cpp \
	src/zh_symbol_map.cpp \
	src/symbol_array.cpp \
	src/text_normalize.cpp \
	src/streaming.cpp \
	src/http_server.cpp \
	src/alsa_backend.cpp

libekho_a_CPPFLAGS = \
	-I$(srcdir)/src \
	@SNDFILE_CFLAGS@ \
	@ESPEAK_NG_CFLAGS@ \
	@SONIC_CFLAGS@ \
	@ALSA_CFLAGS@ \
	@PULSEAUDIO_CFLAGS@ \
	@CURL_CFLAGS@

libekho_a_CXXFLAGS = $(AM_CXXFLAGS) @CFLAG_FESTIVAL@ @CFLAG_FLITE@ $(PTHREAD_CFLAGS)

LIBEKHO_LIB = \
	libekho.a \
	libsonic.a \
	@LIB_PULSEAUDIO@ \
	@LIB_FESTIVAL@ \
	@LIB_FLITE@ \
	@LIB_MP3LAME@ \
	@LIB_MPG123@ \
	@LIB_CURL@ \
	@LIB_SOXR@ \
	-ldl \
	-lm \
	-lpthread

bin_PROGRAMS = ekho ekho-stream

ekho_SOURCES = src/ekho.cpp
ekho_CPPFLAGS = -I$(srcdir)/src @SNDFILE_CFLAGS@ @ESPEAK_NG_CFLAGS@ @SONIC_CFLAGS@ @CURL_CFLAGS@
ekho_CXXFLAGS = $(AM_CXXFLAGS) $(PTHREAD_CFLAGS)
ekho_LDADD = $(LIBEKHO_LIB) @SNDFILE_LIBS@ @ESPEAK_NG_LIBS@ $(PTHREAD_LIBS)

# Streaming driver: exercises the incremental append / flush / latency API and
# is what the simulation harness benchmarks.
ekho_stream_SOURCES = src/ekho_stream_main.cpp
ekho_stream_CPPFLAGS = -I$(srcdir)/src @SNDFILE_CFLAGS@ @ESPEAK_NG_CFLAGS@ @SONIC_CFLAGS@
ekho_stream_CXXFLAGS = $(AM_CXXFLAGS) $(PTHREAD_CFLAGS)
ekho_stream_LDADD = $(LIBEKHO_LIB) @SNDFILE_LIBS@ @ESPEAK_NG_LIBS@ $(PTHREAD_LIBS)

# Two service front ends over the same engine; see configure.ac. Both are built
# by default, and either can be dropped at configure time: a build that only
# wants the pipe drops ekho-server, one that only wants HTTP drops ekho-worker.
if BUILD_HTTP_SERVER
bin_PROGRAMS += ekho-server

# Streaming HTTP server: the replacement for the private one-shot TCP protocol
# in server.cpp. Feeds the request body to a StreamingSession as it arrives and
# returns chunked PCM.
ekho_server_SOURCES = src/ekho_server_main.cpp
ekho_server_CPPFLAGS = -I$(srcdir)/src @SNDFILE_CFLAGS@ @ESPEAK_NG_CFLAGS@ @SONIC_CFLAGS@
ekho_server_CXXFLAGS = $(AM_CXXFLAGS) $(PTHREAD_CFLAGS)
ekho_server_LDADD = $(LIBEKHO_LIB) @SNDFILE_LIBS@ @ESPEAK_NG_LIBS@ $(PTHREAD_LIBS)
endif

if BUILD_WORKER
bin_PROGRAMS += ekho-worker

# Resident line-protocol worker: the engine stays loaded, one UTF-8 line in and
# one framed audio payload out, so a same-box caller pays synthesis and not a
# voice-pack cold start per utterance.
ekho_worker_SOURCES = src/ekho_worker_main.cpp
ekho_worker_CPPFLAGS = -I$(srcdir)/src @SNDFILE_CFLAGS@ @ESPEAK_NG_CFLAGS@ @SONIC_CFLAGS@
ekho_worker_CXXFLAGS = $(AM_CXXFLAGS) $(PTHREAD_CFLAGS)
ekho_worker_LDADD = $(LIBEKHO_LIB) @SNDFILE_LIBS@ @ESPEAK_NG_LIBS@ $(PTHREAD_LIBS)
endif

noinst_PROGRAMS = test_ekho
test_ekho_SOURCES = src/test_ekho.cpp
test_ekho_CPPFLAGS = -I$(srcdir)/src @SNDFILE_CFLAGS@ @ESPEAK_NG_CFLAGS@ @SONIC_CFLAGS@
test_ekho_CXXFLAGS = $(AM_CXXFLAGS) $(PTHREAD_CFLAGS)
test_ekho_LDADD = $(LIBEKHO_LIB) @SNDFILE_LIBS@ @ESPEAK_NG_LIBS@ $(PTHREAD_LIBS)

check_PROGRAMS = test_ekho

TESTS = test_ekho

DATA_DIR = $(srcdir)/ekho-data

install-data-hook:
	rm -rf $(DESTDIR)$(datadir)/ekho-data
	mkdir -p $(DESTDIR)$(datadir)/ekho-data
if DIST_VOICE_DATA
	@if [ -d "$(DATA_DIR)/alphabet" ]; then cp -RLf $(DATA_DIR)/alphabet $(DESTDIR)$(datadir)/ekho-data/; fi
	@if [ -d "$(DATA_DIR)/pinyin"   ]; then cp -RLf $(DATA_DIR)/pinyin   $(DESTDIR)$(datadir)/ekho-data/; fi
	@if [ -d "$(DATA_DIR)/jyutping" ]; then cp -RLf $(DATA_DIR)/jyutping $(DESTDIR)$(datadir)/ekho-data/; fi
	@for f in zhy_list zh_list zh_listx zh_patch zhy.dict zh.dict ka_list bo_list ko_list \
	         ngangien_list pinyin.index pinyin.voice jyutping.index jyutping.voice \
	         toisanese_list; do \
	    [ -f "$(DATA_DIR)/$$f" ] && install -m 644 "$(DATA_DIR)/$$f" $(DESTDIR)$(datadir)/ekho-data/; \
	done
endif

uninstall-hook:
	rm -rf $(DESTDIR)$(datadir)/ekho-data