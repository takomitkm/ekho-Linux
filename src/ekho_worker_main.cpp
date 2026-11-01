/***************************************************************************
 * ekho-worker -- resident line-protocol TTS worker.
 *
 * Why this exists
 * ---------------
 * `ekho` is a one-shot program: every invocation re-reads the voice pack
 * (136 MB of .voice files for the 48 kHz Mandarin pack) before it can speak a
 * single syllable. For a caller that synthesises one line per turn -- an ASR
 * front end, an LLM, a dialogue loop -- that cold start dominates everything
 * else, and it is pure waste: the pack does not change between turns.
 *
 * `ekho-server` solves the same problem with HTTP. That is the right shape when
 * the caller is another machine or another language runtime, but it is a lot of
 * machinery when the caller is a process on the same box that already has a
 * pipe: HTTP costs a socket, a port, a request parser and -- in a service that
 * has no authentication -- an exposure that has to be reasoned about.
 *
 * This worker is the same engine, resident, behind a pipe:
 *
 *   stdin   one UTF-8 line per utterance; a line starting with '!' is a
 *           command (see below)
 *   stdout  binary frames, one per line consumed:
 *              4 bytes  magic
 *              4 bytes  payload length, big endian
 *              N bytes  payload
 *           magic "EKH1" = audio (complete WAV, or raw PCM with --format=pcm)
 *           magic "EKH0" = UTF-8 error text
 *           magic "EKH9" = UTF-8 JSON status (!ping, !info)
 *   stderr  human-readable log only, so stdout stays a clean byte stream
 *
 * The engine is loaded once at startup and stays loaded, so per-utterance cost
 * is synthesis and nothing else. The voice pack is read once.
 *
 * Commands (a line starting with '!'):
 *   !ping                    status frame, {"ok":true,...}
 *   !info                    same, with voice/rate/channels/format/counters
 *   !voice NAME              switch voice (Mandarin, Cantonese, English, ...)
 *   !speed N                 tempo delta, percent (same as ekho -s)
 *   !pitch N                 pitch delta, percent (same as ekho -p)
 *   !volume N                volume delta, percent (same as ekho -a)
 *   !english-speed N         espeak rate delta for English text
 *   !quit                    flush and exit
 *
 * Everything else is text to speak. Blank lines are ignored.
 *
 * Concurrency: none. One line in, one frame out, in order. Synthesis is
 * serialised process-wide anyway -- the engine keeps its callbacks and the
 * espeak-ng voice state in globals (see the comment in ekho_server_main.cpp) --
 * so a second worker thread would buy nothing and could interleave frames.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 **************************************************************************/
#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "ekho.h"

namespace {

volatile sig_atomic_t gStop = 0;

void onSignal(int) { gStop = 1; }

void usage(const char* argv0) {
  fprintf(stderr,
          "usage: %s [options]\n"
          "\n"
          "  Resident TTS worker: reads one UTF-8 line per utterance from stdin,\n"
          "  writes one framed audio payload per line to stdout. Lines starting\n"
          "  with '!' are commands (!ping, !info, !voice, !speed, !pitch,\n"
          "  !volume, !english-speed, !quit).\n"
          "\n"
          "  -v, --voice=VOICE      default voice (default Mandarin)\n"
          "      --samplerate=N     output sample rate (default 48000; 0 = voice native)\n"
          "      --channels=N       output channels, 1 or 2 (default 2)\n"
          "      --english-speed=N  espeak rate delta for English text, percent\n"
          "      --format=FMT       wav (complete file, default) or pcm (raw 16-bit)\n"
          "      --out-dir=DIR      where the intermediate WAV is written (default /tmp)\n"
          "      --no-warmup        skip the startup warm-up synthesis\n"
          "      --verbose          log each utterance to stderr\n"
          "  -h, --help             this text\n",
          argv0);
}

// The names EkhoImpl::setVoice() accepts, and what each selects. Kept in step
// with ekho_server_main.cpp: a client that learns voice names from one of them
// should be able to use them with the other.
struct VoiceAlias {
  const char* name;
  const char* canonical;
  const char* display;
};
const VoiceAlias kVoices[] = {
    {"Mandarin", "pinyin", "Mandarin"},   {"zh", "pinyin", "Mandarin"},
    {"cmn", "pinyin", "Mandarin"},        {"pinyin", "pinyin", "Mandarin"},
    {"Cantonese", "jyutping", "Cantonese"}, {"yue", "jyutping", "Cantonese"},
    {"jyutping", "jyutping", "Cantonese"},
    {"Toisanese", "toisanese", "Toisanese"},
    {"Hakka", "hakka", "Hakka"},
    {"Korean", "hangul", "Korean"},       {"ko", "hangul", "Korean"},
    {"hangul", "hangul", "Korean"},
    {"Tibetan", "tibetan", "Tibetan"},    {"bo", "tibetan", "Tibetan"},
    {"tibetan", "tibetan", "Tibetan"},
    {"Ngangien", "ngangien", "Ngangien"},
    {"English", "English", "English"},    {"en", "English", "English"},
    {nullptr, nullptr, nullptr}};

const VoiceAlias* lookupVoice(const std::string& name) {
  for (int i = 0; kVoices[i].name != nullptr; i++) {
    if (name == kVoices[i].name) return &kVoices[i];
  }
  return nullptr;
}

const char* displayName(const std::string& canonical) {
  for (int i = 0; kVoices[i].name != nullptr; i++) {
    if (canonical == kVoices[i].canonical &&
        strcmp(kVoices[i].name, kVoices[i].display) == 0) {
      return kVoices[i].display;
    }
  }
  return "(unknown)";
}

const char kMagicAudio[4] = {'E', 'K', 'H', '1'};
const char kMagicError[4] = {'E', 'K', 'H', '0'};
const char kMagicStatus[4] = {'E', 'K', 'H', '9'};

// One frame: magic, big-endian length, payload. Returns false when stdout is
// gone (the caller has exited), which is the only way out of the loop.
bool writeFrame(const char magic[4], const void* data, size_t len) {
  unsigned char hdr[8];
  memcpy(hdr, magic, 4);
  hdr[4] = (unsigned char)((len >> 24) & 0xff);
  hdr[5] = (unsigned char)((len >> 16) & 0xff);
  hdr[6] = (unsigned char)((len >> 8) & 0xff);
  hdr[7] = (unsigned char)(len & 0xff);
  if (fwrite(hdr, 1, sizeof hdr, stdout) != sizeof hdr) return false;
  if (len > 0 && fwrite(data, 1, len, stdout) != len) return false;
  return fflush(stdout) == 0;
}

bool writeError(const std::string& text) {
  return writeFrame(kMagicError, text.data(), text.size());
}

// Locate the payload of the first "data" chunk in a RIFF/WAVE buffer.
// libsndfile writes a canonical 44-byte header, but a reader that hard-codes
// 44 is one LIST chunk away from serving its own header as audio, so parse.
size_t wavDataOffset(const std::string& wav, size_t* dataLen) {
  if (wav.size() < 12) return 0;
  if (memcmp(wav.data(), "RIFF", 4) != 0) return 0;
  if (memcmp(wav.data() + 8, "WAVE", 4) != 0) return 0;
  size_t pos = 12;
  while (pos + 8 <= wav.size()) {
    const char* id = wav.data() + pos;
    const unsigned char* s = (const unsigned char*)wav.data() + pos + 4;
    // RIFF chunk sizes are little endian (unlike this worker's frame length,
    // which is big endian on purpose: it is a protocol of our own).
    uint32_t sz = (uint32_t)s[0] | ((uint32_t)s[1] << 8) |
                  ((uint32_t)s[2] << 16) | ((uint32_t)s[3] << 24);
    size_t body = pos + 8;
    if (memcmp(id, "data", 4) == 0) {
      size_t avail = wav.size() - body;
      *dataLen = sz < avail ? sz : avail;
      return body;
    }
    // Chunks are word aligned: an odd-sized chunk is followed by one pad byte.
    pos = body + sz + (sz & 1u);
  }
  return 0;
}

bool readWholeFile(const std::string& path, std::string& out) {
  FILE* f = fopen(path.c_str(), "rb");
  if (f == nullptr) return false;
  out.clear();
  char buf[65536];
  size_t n;
  while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
  fclose(f);
  return true;
}

std::string jsonEscape(const std::string& s) {
  std::string out;
  for (size_t i = 0; i < s.size(); i++) {
    unsigned char c = (unsigned char)s[i];
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char esc[8];
          snprintf(esc, sizeof esc, "\\u%04x", c);
          out += esc;
        } else {
          out += (char)c;
        }
    }
  }
  return out;
}

// Everything the loop needs, so the command handlers stay small.
struct Worker {
  Ekho* engine = nullptr;
  std::string voice;      // canonical
  std::string outDir = "/tmp";
  std::string format = "wav";
  int rate = 48000;
  int channels = 2;
  int englishSpeed = 0;
  bool verbose = false;
  unsigned long utterances = 0;
  unsigned long failures = 0;
  long long startedUs = 0;
  unsigned long counter = 0;  // temp-file counter
};

long long nowUs() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long long)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
}

std::string tempPath(Worker* w) {
  char name[512];
  snprintf(name, sizeof name, "%s/ekho-worker-%d-%lu.wav", w->outDir.c_str(),
           (int)getpid(), ++w->counter);
  return std::string(name);
}

// Synthesise one line and emit it. Returns false if stdout is gone.
bool speakLine(Worker* w, const std::string& text) {
  std::string path = tempPath(w);
  long long t0 = nowUs();
  int rc = w->engine->saveWav(text, path);
  long long genUs = nowUs() - t0;

  std::string wav;
  bool ok = (rc == 0) && readWholeFile(path, wav) && wav.size() > 44;
  unlink(path.c_str());

  if (!ok) {
    w->failures++;
    std::string msg = "synthesis produced no audio (saveWav rc=" +
                      std::to_string(rc) + ", bytes=" +
                      std::to_string(wav.size()) + ")";
    if (w->verbose) fprintf(stderr, "ekho-worker: %s\n", msg.c_str());
    return writeError(msg);
  }

  w->utterances++;

  bool sent = false;
  if (w->format == "pcm") {
    size_t dataLen = 0;
    size_t off = wavDataOffset(wav, &dataLen);
    if (off == 0 || dataLen == 0) {
      w->failures++;
      return writeError("could not locate the data chunk in the synthesised WAV");
    }
    sent = writeFrame(kMagicAudio, wav.data() + off, dataLen);
    if (w->verbose) {
      fprintf(stderr, "ekho-worker: pcm chars=%zu bytes=%zu gen=%.0fms\n",
              text.size(), dataLen, genUs / 1000.0);
    }
  } else {
    sent = writeFrame(kMagicAudio, wav.data(), wav.size());
    if (w->verbose) {
      fprintf(stderr, "ekho-worker: wav chars=%zu bytes=%zu gen=%.0fms\n",
              text.size(), wav.size(), genUs / 1000.0);
    }
  }
  return sent;
}

bool statusFrame(Worker* w, bool includeCounters) {
  std::string json = "{\"ok\":true,\"voice\":\"" + jsonEscape(displayName(w->voice)) +
                     "\",\"rate\":" + std::to_string(w->rate) +
                     ",\"channels\":" + std::to_string(w->channels) +
                     ",\"format\":\"" + jsonEscape(w->format) + "\"";
  if (includeCounters) {
    long long upSec = (nowUs() - w->startedUs) / 1000000LL;
    json += ",\"utterances\":" + std::to_string(w->utterances) +
            ",\"failures\":" + std::to_string(w->failures) +
            ",\"uptime_s\":" + std::to_string(upSec);
  }
  json += "}\n";
  return writeFrame(kMagicStatus, json.data(), json.size());
}

// Returns false when the worker should stop (stdout gone, or !quit).
bool handleCommand(Worker* w, const std::string& line, bool* quit) {
  std::string cmd = line.substr(1);  // drop '!'
  size_t sp = cmd.find_first_of(" \t");
  std::string name = sp == std::string::npos ? cmd : cmd.substr(0, sp);
  std::string arg = sp == std::string::npos ? "" : cmd.substr(sp + 1);
  // Trim leading blanks of the argument.
  size_t b = arg.find_first_not_of(" \t");
  arg = (b == std::string::npos) ? "" : arg.substr(b);

  if (name == "ping") return statusFrame(w, false);
  if (name == "info") return statusFrame(w, true);
  if (name == "quit") {
    *quit = true;
    return true;
  }
  if (name == "voice") {
    const VoiceAlias* a = lookupVoice(arg);
    if (a == nullptr) return writeError("unknown voice '" + arg + "'");
    if (w->engine->setVoice(a->canonical) != 0) {
      return writeError("voice '" + arg + "' is not installed in this voice pack");
    }
    w->voice = a->canonical;
    // A voice change re-creates the resampler and the tempo normalisation; put
    // the requested English rate back, the same way the HTTP server does.
    w->engine->setEnglishSpeed(w->englishSpeed);
    if (w->verbose) {
      fprintf(stderr, "ekho-worker: voice -> %s\n", displayName(w->voice));
    }
    return statusFrame(w, false);
  }
  if (name == "speed" || name == "pitch" || name == "volume" ||
      name == "english-speed") {
    if (arg.empty()) return writeError("!" + name + " needs a number");
    int v = atoi(arg.c_str());
    if (name == "speed") {
      w->engine->setSpeed(v);
    } else if (name == "pitch") {
      w->engine->setPitch(v);
    } else if (name == "volume") {
      w->engine->setVolume(v);
    } else {
      w->englishSpeed = v;
      w->engine->setEnglishSpeed(v);
    }
    return statusFrame(w, false);
  }
  return writeError("unknown command '!" + name + "'");
}

}  // namespace

int main(int argc, char* argv[]) {
  const char* voice = "Mandarin";
  int samplerate = 48000;
  int channels = 2;
  int englishSpeed = 0;
  const char* format = "wav";
  const char* outDir = "/tmp";
  bool verbose = false;
  bool warmup = true;

  static struct option opts[] = {
      {"voice", required_argument, nullptr, 'v'},
      {"samplerate", required_argument, nullptr, 1000},
      {"channels", required_argument, nullptr, 1001},
      {"english-speed", required_argument, nullptr, 1002},
      {"format", required_argument, nullptr, 1003},
      {"out-dir", required_argument, nullptr, 1004},
      {"no-warmup", no_argument, nullptr, 1005},
      {"verbose", no_argument, nullptr, 1006},
      {"help", no_argument, nullptr, 'h'},
      {nullptr, 0, nullptr, 0}};

  int c;
  while ((c = getopt_long(argc, argv, "v:h", opts, nullptr)) != -1) {
    switch (c) {
      case 'v': voice = optarg; break;
      case 'h': usage(argv[0]); return 0;
      case 1000: samplerate = atoi(optarg); break;
      case 1001:
        channels = atoi(optarg);
        if (channels != 1 && channels != 2) {
          fprintf(stderr, "ekho-worker: --channels must be 1 or 2\n");
          return 2;
        }
        break;
      case 1002: englishSpeed = atoi(optarg); break;
      case 1003:
        format = optarg;
        if (strcmp(format, "wav") != 0 && strcmp(format, "pcm") != 0) {
          fprintf(stderr, "ekho-worker: --format must be wav or pcm\n");
          return 2;
        }
        break;
      case 1004: outDir = optarg; break;
      case 1005: warmup = false; break;
      case 1006: verbose = true; break;
      default: usage(argv[0]); return 2;
    }
  }

  // A caller that dies mid-frame must not kill the worker with SIGPIPE; the
  // write returns EPIPE instead and the loop exits cleanly.
  signal(SIGPIPE, SIG_IGN);
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_handler = onSignal;
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);

  const VoiceAlias* alias = lookupVoice(voice);
  if (alias == nullptr) {
    fprintf(stderr, "ekho-worker: unknown voice '%s' (try --help)\n", voice);
    return 3;
  }

  Worker w;
  w.outDir = outDir;
  w.format = format;
  w.englishSpeed = englishSpeed;
  w.verbose = verbose;
  w.channels = channels;
  w.startedUs = nowUs();

  Ekho engine;
  // Order matters: the resampler is built inside setVoice(), so the output
  // format has to be in place before the first voice is loaded (the same
  // ordering note as ekho_server_main.cpp).
  if (samplerate > 0) engine.setSampleRate(samplerate);
  engine.setChannels(channels);
  if (engine.setVoice(alias->canonical) != 0) {
    fprintf(stderr, "ekho-worker: cannot load voice '%s'\n", voice);
    return 3;
  }
  engine.setEnglishSpeed(englishSpeed);
  w.engine = &engine;
  w.voice = alias->canonical;
  w.rate = samplerate > 0 ? samplerate : engine.getSampleRate();
  if (w.rate <= 0) w.rate = 16000;

  // Warm-up: the first synthesis initialises espeak-ng and the dictionary
  // indexes, which is the one-off cost a resident worker exists to hide. Doing
  // it here means the first real line is as fast as the second.
  if (warmup) {
    std::string path = tempPath(&w);
    long long t0 = nowUs();
    engine.saveWav("ni hao", path);
    unlink(path.c_str());
    if (verbose) {
      fprintf(stderr, "ekho-worker: warm-up %.0fms\n", (nowUs() - t0) / 1000.0);
    }
  }

  fprintf(stderr,
          "ekho-worker ready: voice=%s rate=%d Hz channels=%d format=%s "
          "out-dir=%s\n",
          displayName(w.voice), w.rate, w.channels, w.format.c_str(),
          w.outDir.c_str());
  fflush(stderr);

  char* line = nullptr;
  size_t cap = 0;
  ssize_t got;
  bool quit = false;
  while (!gStop && !quit && (got = getline(&line, &cap, stdin)) >= 0) {
    std::string text(line, (size_t)got);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
      text.pop_back();
    }
    if (text.empty()) continue;

    bool alive;
    if (text[0] == '!') {
      alive = handleCommand(&w, text, &quit);
    } else if (text.size() > 4u * 1024u * 1024u) {
      alive = writeError("line too long (> 4 MiB)");
    } else {
      alive = speakLine(&w, text);
    }
    if (!alive) break;  // stdout closed: the caller is gone
  }
  free(line);

  if (verbose) {
    fprintf(stderr, "ekho-worker: exit utterances=%lu failures=%lu\n",
            w.utterances, w.failures);
  }
  return 0;
}
