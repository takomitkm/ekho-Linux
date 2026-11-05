/***************************************************************************
 * ekho-linux -- unit tests: text normalisation and segmentation.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Covers src/text_normalize.cpp: the pure-logic layer every streaming byte
 * passes through before the engine sees it. Runs without voice data, an audio
 * device or an engine -- that is the point: `make check` used to run only a
 * demo that went green with no data at all, while the streaming front end fed
 * the engine mis-converted numbers -- the defects this suite found on
 * 2026-11-05 are fixed as of 2026-11-06 and asserted directly below.
 *
 * Every case names the failure mode it is here for. Escapes follow
 * src/text_normalize.cpp: the file stays pure ASCII so no build needs a
 * UTF-8 locale.
 **************************************************************************/
#include "check.h"

#include <string>

#include "text_normalize.h"

using ekho::CutPoint;
using ekho::NormalizeOptions;
using ekho::charCount;
using ekho::isFlushable;
using ekho::lastSafeCut;
using ekho::normalizeText;
using namespace ektest;

namespace {

// CJK numerals and the unit words the normaliser is supposed to produce.
const char* const kDigit[10] = {
    "\xE9\x9B\xB6",  // 零
    "\xE4\xB8\x80",  // 一
    "\xE4\xBA\x8C",  // 二
    "\xE4\xB8\x89",  // 三
    "\xE5\x9B\x9B",  // 四
    "\xE4\xBA\x94",  // 五
    "\xE5\x85\xAD",  // 六
    "\xE4\xB8\x83",  // 七
    "\xE5\x85\xAB",  // 八
    "\xE4\xB9\x9D",  // 九
};
const char* const kShi = "\xE5\x8D\x81";         // 十
const char* const kBai = "\xE7\x99\xBE";         // 百
const char* const kQian = "\xE5\x8D\x83";        // 千
const char* const kWan = "\xE4\xB8\x87";         // 万
const char* const kDian = "\xE7\x82\xB9";        // 点
const char* const kFenzhi = "\xE7\x99\xBE\xE5\x88\x86\xE4\xB9\x8B";  // 百分之

const char* const kFu = "\xE4\xBC\x8F";          // 伏
const char* const kAn = "\xE5\xAE\x89";          // 安
const char* const kHe = "\xE8\xB5\xAB";          // 赫
const char* const kZi = "\xE5\x85\xB9";          // 兹
const char* const kFen = "\xE5\x88\x86";         // 分
const char* const kBei = "\xE8\xB4\x9D";         // 贝
const char* const kZhao = "\xE5\x85\x86";        // 兆
const char* const kJi = "\xE5\x90\x89";          // 吉
const char* const kZiJie = "\xE5\xAD\x97\xE8\x8A\x82";    // 字节
const char* const kHao = "\xE6\xAF\xAB";         // 毫
const char* const kMiao = "\xE7\xA7\x92";        // 秒
const char* const kMi = "\xE7\xB1\xB3";          // 米

std::string cat() { return std::string(); }
template <typename... Rest>
std::string cat(const char* first, Rest... rest) {
  return std::string(first) + cat(rest...);
}

}  // namespace

int main() {
  // -------------------------------------------------------------------------
  EK_SECTION("normalize.digits: one digit maps to one numeral");
  // Failure mode: the table itself. "0.123456789" is in the upstream demo.
  for (int d = 0; d < 10; d++) {
    EK_STREQ(normalizeText(std::to_string(d)), kDigit[d]);
  }

  // -------------------------------------------------------------------------
  EK_SECTION("normalize.digits: place values");
  // Failure mode: kPlace[] is indexed by place-from-the-right but written
  // highest-place-first, so every multi-digit number reads with transposed
  // place words ("12" as 千二). Found 2026-11-05, fixed 2026-11-06.
  EK_STREQ(normalizeText("12"), cat(kShi, kDigit[2]));
  EK_STREQ(normalizeText("50"), cat(kDigit[5], kShi));
  EK_STREQ(normalizeText("123"), cat(kDigit[1], kBai, kDigit[2], kShi, kDigit[3]));
  EK_STREQ(normalizeText("1234"),
                 cat(kDigit[1], kQian, kDigit[2], kBai, kDigit[3], kShi, kDigit[4]));
  EK_STREQ(normalizeText("1,234"),
                 cat(kDigit[1], kQian, kDigit[2], kBai, kDigit[3], kShi, kDigit[4]));

  // -------------------------------------------------------------------------
  EK_SECTION("normalize.digits: zeros inside and at the end");
  // Failure mode: the pendingZero branch also fires for trailing zeros, so
  // "10" reads "ten zero". Internal zeros (1005 -> 一千零五) are correct.
  EK_STREQ(normalizeText("10"), kShi);
  EK_STREQ(normalizeText("100"), cat(kDigit[1], kBai));
  EK_STREQ(normalizeText("1005"), cat(kDigit[1], kQian, kDigit[0], kDigit[5]));

  // -------------------------------------------------------------------------
  EK_SECTION("normalize.digits: five to nine places");
  // Failure mode: kPlace[] covers four places, so "10086" (the classic
  // hotline number) loses the 万 place and reads 一零八千六.
  EK_STREQ(normalizeText("10086"),
                 cat(kDigit[1], kWan, kDigit[0], kDigit[8], kShi, kDigit[6]));
  EK_STREQ(normalizeText("12345"),
                 cat(kDigit[1], kWan, kDigit[2], kQian, kDigit[3], kBai, kDigit[4],
                     kShi, kDigit[5]));

  // -------------------------------------------------------------------------
  EK_SECTION("normalize.digits: more than nine places read out");
  // Documented contract (appendPlaceValues): above 999,999,999 read the digits.
  std::string want;
  for (char c : std::string("1234567890")) want += kDigit[c - '0'];
  EK_STREQ(normalizeText("1234567890"), want);

  // -------------------------------------------------------------------------
  EK_SECTION("normalize.digits: decimals and grouping");
  // ".5" has no integer part so the number scan never starts (documented: a
  // leading '.' is not a digit). A trailing dot is punctuation.
  EK_STREQ(normalizeText("0.5"), cat(kDigit[0], kDian, kDigit[5]));
  EK_STREQ(normalizeText("3.14"), cat(kDigit[3], kDian, kDigit[1], kDigit[4]));
  EK_STREQ(normalizeText("1,2"), cat(kDigit[1]) + "," + kDigit[2]);
  EK_STREQ(normalizeText("12."), cat(kShi, kDigit[2]) + ".");

  // -------------------------------------------------------------------------
  EK_SECTION("normalize.units: spoken after the number");
  // Failure mode: the unit word is emitted *before* the digits, so the
  // README's own example ("12.5V" -> 十二点五伏) comes out as 伏十二点五.
  // Found 2026-11-05, fixed 2026-11-06.
  EK_STREQ(normalizeText("12.5V"), cat(kShi, kDigit[2], kDian, kDigit[5], kFu));
  EK_STREQ(normalizeText("2.4A"), cat(kDigit[2], kDian, kDigit[4], kAn));
  EK_STREQ(normalizeText("3m"), cat(kDigit[3], kMi));

  // -------------------------------------------------------------------------
  EK_SECTION("normalize.units: the unit words themselves");
  // Failure mode: seven rows of kUnitsTable carry wrong UTF-8 escapes
  // (kHz -> 千赴, Hz -> 赤关, dB -> 分贤, MB -> 克字节, GB -> 免字节,
  //  GHz/MHz -> 克赴). Nonsense words reach the front end as ordinary hanzi
  // and are pronounced -- the defect is audible only, nothing crashes.
  // One wrong nibble per row; found 2026-11-05, fixed 2026-11-06.
  EK_STREQ(normalizeText("16KB"), cat(kShi, kDigit[6], kQian, kZiJie));
  EK_STREQ(normalizeText("3ms"), cat(kDigit[3], kHao, kMiao));
  EK_STREQ(normalizeText("2.4kHz"),
                 cat(kDigit[2], kDian, kDigit[4], kQian, kHe));
  EK_STREQ(normalizeText("2.4Hz"),
                 cat(kDigit[2], kDian, kDigit[4], kHe, kZi));
  EK_STREQ(normalizeText("2.4MHz"),
                 cat(kDigit[2], kDian, kDigit[4], kZhao, kHe));
  EK_STREQ(normalizeText("2.4GHz"),
                 cat(kDigit[2], kDian, kDigit[4], kJi, kHe));
  EK_STREQ(normalizeText("16MB"),
                 cat(kShi, kDigit[6], kZhao, kZiJie));
  EK_STREQ(normalizeText("80dB"),
                 cat(kDigit[8], kShi, kFen, kBei));

  // -------------------------------------------------------------------------
  EK_SECTION("normalize.units: longest key wins");
  // The table comment pins this: "kHz" must not be matched as "Hz", "mm" not
  // as "m", "ms" not as "m"/"s".
  EK_CHECK(normalizeText("2.4kHz").find("Hz") == std::string::npos);
  EK_STREQ(normalizeText("3mm"), cat(kDigit[3], kHao, kMi));

  // -------------------------------------------------------------------------
  EK_SECTION("normalize.percent: prefix before the number");
  // 百分之 is a genuine prefix, so the order is right; the digits inside are
  // the place-value defect above, which is why these are xfail too.
  EK_STREQ(normalizeText("50%"), cat(kFenzhi, kDigit[5], kShi));
  EK_STREQ(normalizeText("93.7%"),
                 cat(kFenzhi, kDigit[9], kShi, kDigit[3], kDian, kDigit[7]));

  // -------------------------------------------------------------------------
  EK_SECTION("normalize.options: each switch disables its own rewrite");
  {
    NormalizeOptions o;
    o.numbers = false;
    EK_STREQ(normalizeText("123", o), "123");
    EK_STREQ(normalizeText("\xE4\xBD\xA0\xE5\xA5\xBD" "123", o), "\xE4\xBD\xA0\xE5\xA5\xBD" "123");

    NormalizeOptions q;
    q.percent = false;
    q.units = false;
    EK_CHECK(normalizeText("50%", q).find(kFenzhi) == std::string::npos);

    NormalizeOptions s;
    s.collapseSpace = false;
    EK_STREQ(normalizeText("a  b", s), "a  b");
    EK_STREQ(normalizeText("a  b", NormalizeOptions()), "a b");
  }

  // -------------------------------------------------------------------------
  EK_SECTION("normalize: CJK passes through untouched");
  EK_STREQ(normalizeText("\xE4\xBD\xA0\xE5\xA5\xBD\xEF\xBC\x81\xE4\xB8\x96\xE7\x95\x8C\xE3\x80\x82"), "\xE4\xBD\xA0\xE5\xA5\xBD\xEF\xBC\x81\xE4\xB8\x96\xE7\x95\x8C\xE3\x80\x82");

  // =========================================================================
  EK_SECTION("cut: clause boundaries");
  // Failure mode: cutting on every comma multiplies the clause pause; never
  // cutting waits for the whole paragraph. lastSafeCut returns the LAST cut.
  EK_EQ(lastSafeCut("\xE4\xBD\xA0\xE5\xA5\xBD\xE3\x80\x82").offset, 9);
  EK_EQ(lastSafeCut("\xE4\xBD\xA0\xE5\xA5\xBD\xEF\xBC\x8C\xE4\xB8\x96\xE7\x95\x8C").offset, 9);
  EK_EQ(lastSafeCut("\xE4\xBD\xA0\xE5\xA5\xBD\xEF\xBC\x8C\xE4\xB8\x96\xE7\x95\x8C\xEF\xBC\x8C\xE5\x86\x8D\xE8\xA7\x81").offset, 18);
  EK_EQ(lastSafeCut("\xE4\xBD\xA0\xE5\xA5\xBD\xEF\xBC\x8C\xE4\xB8\x96\xE7\x95\x8C\xEF\xBC\x8C\xE5\x86\x8D\xE8\xA7\x81").tailBytes, 6);

  // minKeepBytes: a cut that would leave less than the floor is not taken.
  EK_EQ(lastSafeCut("\xE4\xBD\xA0\xE5\xA5\xBD\xEF\xBC\x8C\xE4\xB8\x96\xE7\x95\x8C\xEF\xBC\x8C\xE5\x86\x8D\xE8\xA7\x81", 9).offset, 18);
  EK_EQ(lastSafeCut("\xE4\xBD\xA0\xE5\xA5\xBD\xEF\xBC\x8C\xE4\xB8\x96\xE7\x95\x8C", 9).offset, 0);
  EK_EQ(lastSafeCut("\xE4\xBD\xA0\xE5\xA5\xBD\xEF\xBC\x8C\xE4\xB8\x96\xE7\x95\x8C", 18).offset, 0);

  // -------------------------------------------------------------------------
  EK_SECTION("cut: brackets, numbers and bare text");
  // A cut inside brackets would orphan the closing half; inside "12.5V" it
  // would feed the engine a dangling unit.
  EK_EQ(lastSafeCut("\xEF\xBC\x88\xE4\xBD\xA0\xE5\xA5\xBD\xEF\xBC\x8C\xE4\xB8\x96\xE7\x95\x8C\xEF\xBC\x89").offset, 0);
  EK_EQ(lastSafeCut("12.5V\xEF\xBC\x8C\xE4\xBD\xA0\xE5\xA5\xBD").offset, 8);
  EK_EQ(lastSafeCut("\xE4\xBD\xA0\xE5\xA5\xBD\xE4\xB8\x96\xE7\x95\x8C").offset, 0);
  EK_EQ(lastSafeCut("").offset, 0);

  // Sentence terminators recognised: 。！？! ? … ，、；: ;
  EK_EQ(lastSafeCut("hi! there").offset, 3);
  EK_EQ(lastSafeCut("a;b").offset, 2);
  EK_EQ(lastSafeCut("\xE4\xBD\xA0\xE5\xA5\xBD\xE2\x80\xA6\xE2\x80\xA6").offset, 12);

  // -------------------------------------------------------------------------
  EK_SECTION("cut: forceAtMax keeps the ring fed without splitting chars");
  {
    const std::string s = "\xE4\xB8\x80\xE4\xBA\x8C\xE4\xB8\x89\xE5\x9B\x9B\xE4\xBA\x94";
    CutPoint r = lastSafeCut(s, 0, 6, true);
    EK_EQ(r.offset, 6);
    // A cap landing inside a character backs off to the boundary below.
    r = lastSafeCut(s, 0, 7, true);
    EK_EQ(r.offset, 6);
    // A cap landing inside a number steps past it: the cut may exceed the
    // cap, but the number reaches the engine whole.
    r = lastSafeCut("abc12.5def", 0, 5, true);
    EK_EQ(r.offset, 7);
    // Without force there is no cut at all.
    EK_EQ(lastSafeCut(s, 0, 6, false).offset, 0);
    EK_EQ(lastSafeCut("\xE4\xBD\xA0\xE5\xA5\xBD\xEF\xBC\x8C\xE4\xB8\x96\xE7\x95\x8C\xEF\xBC\x8C\xE5\x86\x8D\xE8\xA7\x81", 0, 6, false).offset, 0);
  }

  // -------------------------------------------------------------------------
  EK_SECTION("cut: property sweep -- a cut never lands inside a character");
  // The matchLen UTF-8 bug documented in the source put cuts in the middle of
  // CJK characters (a continuation byte matched the second byte of "。").
  // This sweep over every cap reproduces that class of defect: any offset the
  // function returns must start a character and must not split a number run.
  {
    const std::string bufs[] = {
        "\xE7\xB3\xBB\xE7\xBB\x9F\xE5\x90\xAF\xE5\x8A\xA8\xE5\xAE\x8C\xE6\x88\x90\xE3\x80\x82\xE7\xAC\xAC\xE4\xB8\x80\xE9\xA1\xB9\xEF\xBC\x8C\xE7\x94\xB5\xE6\xBA\x90\xE7\x8A\xB6\xE6\x80\x81\xE6\xAD\xA3\xE5\xB8\xB8\xE3\x80\x82",
        "\xE8\xBE\x93\xE5\x87\xBA\xE7\x94\xB5\xE5\x8E\x8B 12.5V\xEF\xBC\x8C\xE7\x94\xB5\xE6\xB5\x81 2.4A\xE3\x80\x82" "CPU 2.4GHz\xE3\x80\x82",
        "\xEF\xBC\x88\xE6\x8B\xAC\xE5\x8F\xB7\xE5\x86\x85\xEF\xBC\x8C\xE4\xB8\x8D\xE5\x88\x87\xEF\xBC\x89\xE5\xA4\x96\xE9\x9D\xA2\xEF\xBC\x8C\xE5\x88\x87\xE3\x80\x82",
        "\xE4\xB8\x80\xE4\xBA\x8C\xE4\xB8\x89\xE5\x9B\x9B\xE4\xBA\x94\xE5\x85\xAD\xE4\xB8\x83\xE5\x85\xAB\xE4\xB9\x9D\xE5\x8D\x81",
        "\xE4\xBD\xA0\xE5\xA5\xBD\xE2\x80\xA6\xE2\x80\xA6\xE5\x86\x8D\xE8\xA7\x81\xEF\xBC\x81\xE5\xA5\xBD\xE5\x90\x97\xEF\xBC\x9F",
    };
    for (const std::string& buf : bufs) {
      for (size_t cap = 1; cap <= buf.size() + 2; cap++) {
        for (int force = 0; force < 2; force++) {
          CutPoint r = lastSafeCut(buf, 0, cap, force != 0);
          if (r.offset == 0) continue;
          bool ok = r.offset <= buf.size();
          // r.offset == size is legal (cut after the last character);
          // std::string::operator[] hands back the terminating NUL there.
          ok = ok && (r.offset == buf.size() ||
                      (buf[r.offset] & 0xC0) != 0x80);
          // not directly before another number byte (would split "12.5")
          ok = ok && !(r.offset < buf.size() &&
                       ((buf[r.offset] >= '0' && buf[r.offset] <= '9') ||
                        buf[r.offset] == '.' || buf[r.offset] == ',') &&
                       r.offset >= 1 && buf[r.offset - 1] >= '0' &&
                       buf[r.offset - 1] <= '9');
          check(ok, __FILE__, __LINE__,
                "cut offset " + std::to_string(r.offset) + "/" +
                    std::to_string(buf.size()) + " cap=" + std::to_string(cap) +
                    " force=" + std::to_string(force) + " buf=" + escape(buf));
        }
      }
    }
  }

  // -------------------------------------------------------------------------
  EK_SECTION("flush: sentence end flushes, lonely comma does not");
  // Failure mode: flushing on any comma makes the engine pause after every
  // "，"; never flushing stalls the stream until the paragraph ends.
  EK_CHECK(isFlushable("\xE4\xBD\xA0\xE5\xA5\xBD\xE3\x80\x82"));
  EK_CHECK(isFlushable("\xE3\x80\x82"));
  EK_CHECK(isFlushable("\xE4\xBD\xA0\xE5\xA5\xBD\xE4\xB8\x96\xE7\x95\x8C\xE3\x80\x82\xE5\x86\x8D\xE8\xA7\x81"));
  EK_CHECK(!isFlushable("\xE4\xBD\xA0\xE5\xA5\xBD\xEF\xBC\x8C\xE4\xB8\x96\xE7\x95\x8C"));          // comma, fewer than 6 tail chars
  EK_CHECK(isFlushable("\xE4\xBD\xA0\xE5\xA5\xBD\xEF\xBC\x8C\xE4\xB8\x96\xE7\x95\x8C\xE4\xBD\xA0\xE5\xA5\xBD\xE4\xBD\xA0\xE5\xA5\xBD"));  // 8 chars after the comma
  EK_CHECK(!isFlushable("\xE4\xBD\xA0\xE5\xA5\xBD"));
  EK_CHECK(!isFlushable(""));
  // ASCII comma is not a clause separator (only ";"), so this never flushes.
  EK_CHECK(!isFlushable("a,bcdefg"));

  // -------------------------------------------------------------------------
  EK_SECTION("charCount: characters, not bytes");
  EK_EQ(charCount("\xE4\xBD\xA0\xE5\xA5\xBD\xEF\xBC\x8C\xE4\xB8\x96\xE7\x95\x8C", 0, 15), 5);
  EK_EQ(charCount("abc", 0, 3), 3);
  EK_EQ(charCount("\xE4\xBD\xA0\xE5\xA5\xBD", 0, 4), 2);
  EK_EQ(charCount("\xE4\xBD\xA0\xE5\xA5\xBD", 3, 4), 1);  // a partial tail still counts as one

  return summary("test_normalize");
}
