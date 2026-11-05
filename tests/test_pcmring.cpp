/***************************************************************************
 * ekho-linux -- unit tests: the PCM ring.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Covers ekho::PcmRing (src/streaming.cpp): the single-producer/single-consumer
 * buffer every synthesised sample passes through. Runs without voice data or
 * an engine.
 *
 * The contract under test is the one the header documents:
 *   * write()          copies as much as fits, discards the OLDEST frames on
 *                      overflow, counts what it discarded
 *   * writeBlocking()  never drops; blocks; 0 after abort()
 *   * read()           FIFO, never blocks
 *   * abort()          releases a parked producer
 * The streaming path only uses writeBlocking() -- silent sample loss is an
 * audible artefact -- but write() is public API with its own documented
 * behaviour, and the drop-accounting defect this suite found in it
 * (2026-11-05, unfixed) is marked with EK_KNOWN_BUG below.
 **************************************************************************/
#include "check.h"

#include <pthread.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <string.h>

#include <string>
#include <vector>

#include "streaming.h"

using ekho::PcmRing;
using namespace ektest;

namespace {

// A producer that writes [from, from+n) as sample values, respecting abort.
struct ProducerArg {
  PcmRing* ring;
  short from;
  size_t n;
  size_t written;
};

void* producerMain(void* arg) {
  ProducerArg* a = static_cast<ProducerArg*>(arg);
  std::vector<short> buf(a->n);
  for (size_t i = 0; i < a->n; i++) buf[i] = static_cast<short>(a->from + i);
  a->written = a->ring->writeBlocking(buf.data(), a->n);
  return nullptr;
}

void msleep(int ms) {
  struct timespec ts;
  ts.tv_sec = ms / 1000;
  ts.tv_nsec = (ms % 1000) * 1000000L;
  nanosleep(&ts, nullptr);
}

}  // namespace

int main() {
  // -------------------------------------------------------------------------
  EK_SECTION("ring: basics -- capacity, FIFO order, available()");
  {
    PcmRing r(4);
    EK_EQ(r.capacitySamples(), 4);
    EK_EQ(r.available(), 0);
    EK_EQ(r.droppedFrames(), 0);

    short in[3] = {10, 20, 30};
    EK_EQ(r.write(in, 3), 3);
    EK_EQ(r.available(), 3);

    short out[4] = {0};
    EK_EQ(r.read(out, 4), 3);  // never more than what is there
    EK_EQ(out[0], 10);
    EK_EQ(out[1], 20);
    EK_EQ(out[2], 30);
    EK_EQ(r.available(), 0);
    EK_EQ(r.read(out, 4), 0);  // empty read never blocks, returns 0
  }

  // -------------------------------------------------------------------------
  EK_SECTION("ring: FIFO survives wraparound");
  // Failure mode: an off-by-one in the modular index makes the first samples
  // of every ring lap come out swapped or repeated -- inaudible on noise,
  // obvious on a ramp, and exactly what a wraparound test pins.
  {
    PcmRing r(4);
    short in[3] = {1, 2, 3};
    short out[4] = {0};
    r.write(in, 3);
    EK_EQ(r.read(out, 2), 2);
    short in2[3] = {4, 5, 6};
    r.write(in2, 3);  // wraps: writePos was 3
    EK_EQ(r.available(), 4);
    EK_EQ(r.read(out, 4), 4);
    for (int i = 0; i < 4; i++) EK_EQ(out[i], 3 + i);
  }

  // -------------------------------------------------------------------------
  EK_SECTION("ring: write() drop-oldest when one frame overflows");
  {
    PcmRing r(4);
    short in[4] = {1, 2, 3, 4};
    r.write(in, 4);
    short one = 5;
    EK_EQ(r.write(&one, 1), 1);
    EK_EQ(r.available(), 4);
    EK_EQ(r.droppedFrames(), 1);
    short out[4] = {0};
    EK_EQ(r.read(out, 4), 4);
    for (int i = 0; i < 4; i++) EK_EQ(out[i], 2 + i);  // oldest (1) gone
  }

  // -------------------------------------------------------------------------
  EK_SECTION("ring: write() with more frames than capacity");
  // Failure mode (documented contract): the NEWEST min(frames, capacity)
  // frames must survive and the discarded ones must be counted. The code
  // instead keeps the oldest frames and reports dropped=0, silently losing
  // the tail of the burst. Found 2026-11-05; unfixed. Latent today: the
  // streaming path only calls writeBlocking(), never write().
  {
    PcmRing r(4);
    short in[6] = {1, 2, 3, 4, 5, 6};
    EK_EQ(r.write(in, 6), 4);
    EK_KNOWN(r.droppedFrames() == 2 && r.available() == 4,
             "write(6 frames) into an empty 4-slot ring keeps the OLDEST 4 "
             "and counts no drop");
    short out[4] = {0};
    r.read(out, 4);
    bool newest = out[0] == 3 && out[1] == 4 && out[2] == 5 && out[3] == 6;
    EK_KNOWN(newest, "the ring must hold the newest frames of an oversized "
                     "burst, not the first four");
  }

  // -------------------------------------------------------------------------
  EK_SECTION("ring: clear() empties without breaking the indices");
  {
    PcmRing r(4);
    short in[3] = {1, 2, 3};
    r.write(in, 3);
    r.clear();
    EK_EQ(r.available(), 0);
    short out[4] = {0};
    EK_EQ(r.read(out, 4), 0);
    EK_EQ(r.write(in, 3), 3);  // and it still works afterwards
    EK_EQ(r.read(out, 4), 3);
    EK_EQ(out[0], 1);
  }

  // -------------------------------------------------------------------------
  EK_SECTION("ring: zero capacity -- read/available are safe, write crashes");
  // Failure mode: capacity 0 is constructible (PcmRing is public and the CLI
  // sizes the ring from --ring-ms) and the modulo in write() divides by it.
  // Contained in a child process: a SIGFPE here must be a reported defect, not
  // a dead test binary with no output.
  {
    PcmRing r(0);
    EK_EQ(r.capacitySamples(), 0);
    EK_EQ(r.available(), 0);
    short out[2] = {0};
    EK_EQ(r.read(out, 2), 0);  // safe: min(frames, 0)

    pid_t pid = fork();
    if (pid == 0) {
      short in[2] = {1, 2};
      r.write(in, 2);  // currently SIGFPE: (readPos + need) % capacity_
      _exit(0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    bool clean = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    EK_KNOWN(clean, "PcmRing(0).write() divides by capacity and dies with "
                    "SIGFPE (writeBlocking would park forever instead)");
  }

  // -------------------------------------------------------------------------
  EK_SECTION("ring: writeBlocking applies backpressure, drops nothing");
  // Failure mode: a producer outrunning the consumer must slow down, never
  // discard -- sim.sh asserts dropped_frames == 0 at every ring size for
  // exactly this reason. Here the consumer is slower than the producer on
  // purpose.
  {
    const size_t kCap = 100;
    const size_t kTotal = 1000;
    PcmRing r(kCap);
    ProducerArg pa = {&r, 0, kTotal, 0};
    pthread_t tid;
    EK_CHECK(pthread_create(&tid, nullptr, producerMain, &pa) == 0);

    size_t got = 0;
    short out[97];
    int spins = 0;
    bool orderBad = false;
    size_t badAt = 0;
    while (got < kTotal && spins < 100000) {
      size_t n = r.read(out, sizeof out / sizeof out[0]);
      if (n == 0) {
        msleep(1);
        spins++;
        continue;
      }
      for (size_t i = 0; i < n; i++) {
        // In-order: sample values must come back exactly as written. Record
        // the first divergence instead of checking every sample, or the
        // summary counts a thousand checks that all say the same thing.
        if (!orderBad && out[i] != static_cast<short>(got + i)) {
          orderBad = true;
          badAt = got + i;
        }
      }
      got += n;
    }
    EK_CHECK(pthread_join(tid, nullptr) == 0);
    check(!orderBad, __FILE__, __LINE__,
          "sample " + std::to_string(badAt) + " came back out of order");
    EK_EQ(pa.written, kTotal);
    EK_EQ(got, kTotal);
    EK_EQ(r.droppedFrames(), 0);
  }

  // -------------------------------------------------------------------------
  EK_SECTION("ring: writeBlocking larger than the capacity still completes");
  {
    PcmRing r(64);
    ProducerArg pa = {&r, 100, 500, 0};
    pthread_t tid;
    pthread_create(&tid, nullptr, producerMain, &pa);
    short out[128];
    size_t got = 0;
    int spins = 0;
    while (got < 500 && spins < 20000) {
      size_t n = r.read(out, sizeof out / sizeof out[0]);
      if (n == 0) {
        msleep(1);
        spins++;
        continue;
      }
      got += n;
    }
    pthread_join(tid, nullptr);
    EK_EQ(got, 500);
    EK_EQ(r.droppedFrames(), 0);
  }

  // -------------------------------------------------------------------------
  EK_SECTION("ring: abort() releases a parked producer");
  // Failure mode: a consumer that stops reading must not leave the synth
  // thread parked forever -- StreamingSession::stop() depends on abort().
  {
    PcmRing r(16);
    ProducerArg pa = {&r, 0, 1000, 0};
    pthread_t tid;
    pthread_create(&tid, nullptr, producerMain, &pa);
    msleep(100);  // let it fill and park
    EK_EQ(r.available(), 16);
    r.abort();
    pthread_join(tid, nullptr);
    EK_CHECK(pa.written < 1000);   // it gave up
    // After abort, further blocking writes return immediately (0 stored).
    short in[4] = {1, 2, 3, 4};
    EK_EQ(r.writeBlocking(in, 4), 0);
    EK_EQ(r.available(), 16);      // nothing new was written
  }

  return summary("test_pcmring");
}
