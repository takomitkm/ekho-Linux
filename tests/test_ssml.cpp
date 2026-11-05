/***************************************************************************
 * ekho-linux -- unit tests: SSML helpers.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Covers src/ssml.cpp. The screen-reader path feeds SSML straight into these
 * functions, so the contract is "tags in, plain text out". Two leaks in that
 * contract -- a <speak> tag with attributes, and consecutive <speak> blocks
 * coming out with tags still in them or mangled -- were found 2026-11-05 and
 * fixed 2026-11-06; the cases below assert the fixed behaviour directly.
 **************************************************************************/
#include "check.h"

#include <string>

#include "ssml.h"

using ekho::Ssml;
using namespace ektest;

namespace {

std::string fstrip(const std::string& in) {
  std::string s = in;
  Ssml::filterSpaces(s);
  return s;
}

}  // namespace

int main() {
  // -------------------------------------------------------------------------
  EK_SECTION("stripTags: plain text passes through");
  EK_STREQ(Ssml::stripTags("plain"), "plain");
  EK_STREQ(Ssml::stripTags(""), "");

  // -------------------------------------------------------------------------
  EK_SECTION("stripTags: bare <speak> wrapper");
  EK_STREQ(Ssml::stripTags("<speak>\xE4\xBD\xA0\xE5\xA5\xBD</speak>"), "\xE4\xBD\xA0\xE5\xA5\xBD");

  // -------------------------------------------------------------------------
  EK_SECTION("stripTags: <mark/> removal, before/after/mid");
  // The sample from the header comment: a screen reader announces around marks.
  EK_STREQ(Ssml::stripTags("<speak><mark name=\"0:8\"/>\xE5\xB1\x8F\xE5\xB9\x95\xE9\x98\x85\xE8\xAF\xBB\xE5\x99\xA8\xE5\x90\xAF\xE7\x94\xA8\xE3\x80\x82</speak>"),
           "\xE5\xB1\x8F\xE5\xB9\x95\xE9\x98\x85\xE8\xAF\xBB\xE5\x99\xA8\xE5\x90\xAF\xE7\x94\xA8\xE3\x80\x82");
  EK_STREQ(Ssml::stripTags("<mark name=\"1\"/>hello"), "hello");
  EK_STREQ(Ssml::stripTags("pre<mark name=\"1\"/>post"), "prepost");

  // -------------------------------------------------------------------------
  EK_SECTION("stripTags: attributes and repeated wrappers");
  // A <speak> with attributes used to build the closing-tag needle from the
  // whole opening tag text (</speak version="1.0">), which never matches, so
  // the tags reached the engine and were read aloud. Real SSML always
  // carries version/xml:lang (found 2026-11-05, fixed 2026-11-06).
  EK_STREQ(Ssml::stripTags("<speak version=\"1.0\">\xE4\xBD\xA0\xE5\xA5\xBD</speak>"), "\xE4\xBD\xA0\xE5\xA5\xBD");
  // Two <speak> blocks used to pair the first opener with the LAST closer
  // and mangle what is between; tags are removed positionally now.
  EK_STREQ(Ssml::stripTags("<speak>a</speak><speak>b</speak>"), "ab");
  // Content after the closing tag used to block the match as well; tags are
  // markup wherever they sit, so they go now.
  EK_STREQ(Ssml::stripTags("<speak>\xE4\xBD\xA0\xE5\xA5\xBD</speak>trailing"),
           "\xE4\xBD\xA0\xE5\xA5\xBDtrailing");

  // -------------------------------------------------------------------------
  EK_SECTION("isAudio / getAudioPath");
  EK_CHECK(Ssml::isAudio("<audio src=\"/a.wav\">x</audio>"));
  EK_CHECK(!Ssml::isAudio("plain"));
  EK_CHECK(!Ssml::isAudio(" <audio src=\"/a.wav\">x</audio>"));  // must be at 0
  EK_STREQ(Ssml::getAudioPath("<audio src=\"/a/b.wav\">x</audio>"), "/a/b.wav");
  EK_STREQ(Ssml::getAudioPath("no quotes"), "");

  // -------------------------------------------------------------------------
  EK_SECTION("filterSpaces: spaces between hanzi go, ASCII stays");
  // Failure mode: a screen reader splits hanzi with spaces ("你 好 世 界");
  // those gaps become audible pauses. ASCII contexts must keep theirs.
  EK_STREQ(fstrip("  \xE4\xBD\xA0\xE5\xA5\xBD \xE4\xB8\x96\xE7\x95\x8C  "), "\xE4\xBD\xA0\xE5\xA5\xBD\xE4\xB8\x96\xE7\x95\x8C");
  EK_STREQ(fstrip("\xE4\xBD\xA0\xE5\xA5\xBD \xE4\xB8\x96\xE7\x95\x8C"), "\xE4\xBD\xA0\xE5\xA5\xBD\xE4\xB8\x96\xE7\x95\x8C");
  EK_STREQ(fstrip("a b"), "a b");
  // Trailing spaces after an ASCII context are kept by the gap rule and must
  // be removed by the trim instead -- the only case the trim uniquely owns.
  EK_STREQ(fstrip("abc  "), "abc");
  EK_STREQ(fstrip("   "), "");
  EK_STREQ(fstrip(""), "");
  // Full-width space (U+3000) counts as a gap too.
  EK_STREQ(fstrip("\xE4\xBD\xA0\xE5\xA5\xBD\xE3\x80\x80\xE4\xB8\x96\xE7\x95\x8C"), "\xE4\xBD\xA0\xE5\xA5\xBD\xE4\xB8\x96\xE7\x95\x8C");

  // -------------------------------------------------------------------------
  EK_SECTION("filterSpaces: invalid UTF-8 truncates at the bad byte");
  // The utf8++ walker throws; the documented recovery keeps the prefix read
  // so far and drops the rest. Pin that (data loss beats a crash), so a
  // future change to "drop only the bad byte" shows up as a deliberate one.
  EK_STREQ(fstrip("\xE4\xBD\xA0\xE5\xA5\xBD\xFF\xE4\xB8\x96\xE7\x95\x8C"), "\xE4\xBD\xA0\xE5\xA5\xBD");

  return summary("test_ssml");
}
