/***************************************************************************
 * ekho-linux -- unit tests: the ALSA output backend.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * AlsaOutput (src/alsa_backend.cpp) is dlopen-based so the binary runs on a
 * board with no libasound.so.2; the contract under test is that every path
 * degrades to a clean error instead of crashing. These cases run on a host
 * with no sound card, which is exactly the shape to pin. (libasound's own
 * complaint about the missing card goes to stderr; that noise is expected.)
 **************************************************************************/
#include "check.h"

#include <string>

#include "alsa_backend.h"

using ekho::AlsaOutput;
using namespace ektest;

int main() {
  // -------------------------------------------------------------------------
  EK_SECTION("alsa: a missing/absent device must degrade, never crash");
  {
    EK_CHECK(AlsaOutput::available() == AlsaOutput::available());  // stable

    AlsaOutput out;
    int rc = out.openDevice("nonexistent-device-xyz", 48000, 2, 100);
    EK_CHECK(rc < 0);
    EK_CHECK(!out.lastError().empty());
    EK_CHECK(!out.isOpen());

    short pcm[8] = {0};
    EK_EQ(out.write(pcm, 8), 0);  // no device: nothing accepted, no crash
    EK_EQ(out.write(nullptr, 0), 0);
    EK_EQ(out.drain(), 0);
    EK_EQ(out.availableFrames(), 0);

    out.closeDevice();  // closing a never-opened backend is a no-op
    EK_CHECK(!out.isOpen());
  }

  return summary("test_alsa");
}
