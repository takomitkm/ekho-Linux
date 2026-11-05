/***************************************************************************
 * ekho-linux -- unit tests: the resampler and the ALSA backend stubs.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * resampleMono16 (src/resample.cpp) is the windowed-sinc kernel that keeps
 * the 16 kHz alphabet recordings from playing 3x too fast on the 48 kHz pack.
 * Needs no voice data and no audio device.
 **************************************************************************/
#include "check.h"

#include <math.h>
#include <string.h>

#include <string>
#include <vector>

#include "resample.h"

using ekho::resampleMono16;
using namespace ektest;

namespace {

// An output length is "preserved to one sample" (header contract), so a
// letter keeps its duration at every rate pair.
void checkLength(const char* what, int inFrames, int rateIn, int rateOut) {
  int n = -1;
  long want = lround((double)inFrames * rateOut / rateIn);
  std::vector<short> buf(inFrames > 0 ? inFrames : 1, 0);
  short* got = resampleMono16(buf.data(), inFrames, rateIn, rateOut, &n);
  if (got == nullptr) {
    check(false, __FILE__, __LINE__,
          std::string(what) + ": resample returned NULL");
  } else {
    check(n - want <= 1 && want - n <= 1, __FILE__, __LINE__,
          std::string(what) + ": out frames " + std::to_string(n) +
              ", want ~" + std::to_string(want));
    delete[] got;
  }
}

}  // namespace

int main() {
  // =========================================================================
  EK_SECTION("resample: no-op requests return NULL, not an error");
  // Documented: callers check for NULL and keep the original buffer.
  {
    short in[8] = {0};
    int n = -1;
    EK_CHECK(resampleMono16(in, 8, 48000, 48000, &n) == nullptr);
    EK_CHECK(resampleMono16(in, 0, 16000, 48000, &n) == nullptr);
    EK_CHECK(resampleMono16(nullptr, 8, 16000, 48000, &n) == nullptr);
    EK_CHECK(resampleMono16(in, 8, 0, 48000, &n) == nullptr);
    EK_CHECK(resampleMono16(in, 8, 16000, -1, &n) == nullptr);
    EK_CHECK(resampleMono16(in, 8, 16000, 48000, nullptr) == nullptr);
  }

  // =========================================================================
  EK_SECTION("resample: length preserved to one sample at every ratio");
  // The lone-K regression (0.041 s instead of 0.110 s) was a length bug: the
  // frames came back at the wrong rate's count.
  checkLength("16k -> 48k", 480, 16000, 48000);
  checkLength("48k -> 16k", 1440, 48000, 16000);
  checkLength("44.1k -> 48k", 441, 44100, 48000);
  checkLength("48k -> 44.1k", 480, 48000, 44100);
  checkLength("16k -> 44.1k", 160, 16000, 44100);

  // =========================================================================
  EK_SECTION("resample: DC passes at unity gain");
  // Failure mode: a kernel whose weights do not sum to 1 changes the level
  // of steady vowels; DC is the worst case and cheap to check exactly.
  {
    const int kIn = 480;
    std::vector<short> dc(kIn, 32000);
    const int kRatesIn[] = {16000, 32000, 44100};
    for (int rateIn : kRatesIn) {
      int n = 0;
      short* out = resampleMono16(dc.data(), kIn, rateIn, 48000, &n);
      if (!out) {
        check(false, __FILE__, __LINE__, "DC resample returned NULL");
        continue;
      }
      int mn = 32767, mx = -32768;
      for (int i = 0; i < n; i++) {
        if (out[i] < mn) mn = out[i];
        if (out[i] > mx) mx = out[i];
      }
      check(mn >= 31998 && mx <= 32002, __FILE__, __LINE__,
            "DC 32000 came back as [" + std::to_string(mn) + ", " +
                std::to_string(mx) + "] at " + std::to_string(rateIn) +
                " -> 48000");
      delete[] out;
    }
  }

  // =========================================================================
  EK_SECTION("resample: silence stays silent, output is finite");
  {
    std::vector<short> silence(960, 0);
    int n = 0;
    short* out = resampleMono16(silence.data(), 960, 16000, 48000, &n);
    EK_CHECK(out != nullptr);
    bool quiet = true;
    for (int i = 0; i < n; i++) quiet = quiet && out[i] == 0;
    EK_CHECK(quiet);
    delete[] out;
  }

  // =========================================================================
  EK_SECTION("resample: deterministic, no ringing blow-up on edges");
  // Failure mode: an unnormalised kernel or an accumulator bug shows up as
  // values beyond the input envelope on a square-ish edge.
  {
    std::vector<short> square(960);
    for (int i = 0; i < 960; i++) square[i] = (i / 48) % 2 ? -8000 : 8000;
    int n1 = 0, n2 = 0;
    short* a = resampleMono16(square.data(), 960, 16000, 48000, &n1);
    short* b = resampleMono16(square.data(), 960, 16000, 48000, &n2);
    EK_EQ(n1, n2);
    EK_CHECK(n1 > 0);
    bool same = memcmp(a, b, (size_t)n1 * sizeof(short)) == 0;
    EK_CHECK(same);  // two runs, identical bytes
    // A square edge through a windowed sinc overshoots (Gibbs); the bound is
    // "no blow-up", i.e. within ~1.3x of the input peak and inside int16.
    int mn = 32767, mx = -32768;
    for (int i = 0; i < n1; i++) {
      if (a[i] < mn) mn = a[i];
      if (a[i] > mx) mx = a[i];
    }
    check(mn >= -10400 && mx <= 10400, __FILE__, __LINE__,
          "square 8000 came back as [" + std::to_string(mn) + ", " +
              std::to_string(mx) + "]");
    delete[] a;
    delete[] b;
  }

  return summary("test_resample");
}
