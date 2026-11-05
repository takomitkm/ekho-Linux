/***************************************************************************
 * ekho-linux -- text normalisation and segmentation. See text_normalize.h.
 * SPDX-License-Identifier: GPL-3.0-or-later
 **************************************************************************/
#include "text_normalize.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace ekho {

namespace {

inline bool isAsciiDigit(char c) { return c >= '0' && c <= '9'; }

// Sentence terminators: always a safe cut and strong enough to flush.
const char* kSentenceEnd = "\xE3\x80\x82"          // 。
                          "\xEF\xBC\x81"          // ！
                          "\xEF\xBC\x9F"          // ？
                          "!" "?" "\xE2\x80\xA6"; // ! ? …

// Clause separators: a legal cut, but not enough on their own to flush.
const char* kClauseSep = "\xEF\xBC\x8C"     // ，
                        "\xE3\x80\x81"     // 、
                        "\xEF\xBC\x9B"     // ；
                        ";" "\xEF\xBC\x9A"; // ; ：

const char* kOpenBracket = "([{（【《";
const char* kCloseBracket = ")]}）】》";

inline size_t utf8Len(unsigned char c) {
  return (c >= 0xF0) ? 4 : (c >= 0xE0) ? 3 : (c >= 0xC0) ? 2 : 1;
}

// If `s` at `at` begins with a character from `set`, returns that character's
// byte length; otherwise 0.
//
// `set` is a concatenation of whole UTF-8 characters (see kSentenceEnd and
// friends below), so it must be walked a character at a time. The earlier
// version compared single bytes of `s` against single bytes of `set`, which
// made the continuation byte 0x80 -- present inside virtually every CJK
// character -- match the second byte of "。" (E3 80 82). Text then looked full
// of sentence terminators and the cuts landed in the middle of characters,
// reaching the front end as invalid UTF-8.
size_t matchLen(const char* set, const std::string& s, size_t at) {
  if (at >= s.size()) return 0;
  for (const char* p = set; *p;) {
    size_t l = utf8Len(static_cast<unsigned char>(*p));
    if (l == 0) l = 1;
    if (at + l <= s.size() && memcmp(s.data() + at, p, l) == 0) return l;
    p += l;
  }
  return 0;
}

// CJK numeral glyphs, kept as explicit UTF-8 escapes so the file stays pure
// ASCII and no compiler input-charset assumption is needed.
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

// Place words inside one 4-digit section, indexed by place-from-the-right
// within the section: 0 = ones (none), 1 = 十, 2 = 百, 3 = 千.
const char* const kSectionPlace[4] = {
    "",                 // ones
    "\xE5\x8D\x81",     // 十
    "\xE7\x99\xBE",     // 百
    "\xE5\x8D\x83",     // 千
};

// Section unit emitted after a non-zero 4-digit section, indexed by section
// from the right: 0 = ones section (none), 1 = 万, 2 = 亿.
const char* const kSectionUnit[3] = {
    "",
    "\xE4\xB8\x87",     // 万
    "\xE4\xBA\xBF",     // 亿
};

// 1234 -> 一千二百三十四, 12 -> 十二, 10 -> 十, 100 -> 一百,
// 1005 -> 一千零五, 10086 -> 一万零八十六, 1000005 -> 一百万零五,
// 123456789 -> 一亿二千三百四十五万六千七百八十九, 0 -> 零.
//
// The previous version indexed a highest-place-first table by
// place-from-the-right, which transposed the place words of every
// multi-digit number ("12" as 千二), appended a 零 after trailing zeros
// ("10" as 十零), and had no 万/亿 words at all ("10086" as 一零八千六).
// It walks 4-digit sections now: inside a section 零 is only spoken between
// digits (never at the end), a section's unit follows its digits, and a
// leading 一 before 十 is suppressed only when nothing has been spoken in
// that section yet (十二, 十万零十二; but 一百一十).
void appendPlaceValues(std::string& out, const std::string& digits) {
  size_t n = digits.size();
  if (n == 0) return;
  if (n > 9) {
    // Above 999,999,999 just read the digits out; place values do not apply.
    for (char c : digits) out += kDigit[c - '0'];
    return;
  }

  size_t nGroups = (n + 3) / 4;
  size_t firstLen = n - (nGroups - 1) * 4;
  bool pendingZero = false;  // a 零 is owed before the next spoken digit
  bool spoken = false;       // anything emitted for this number so far
  bool anyDigit = false;     // the number contains a non-zero digit at all

  for (size_t g = 0; g < nGroups; g++) {
    size_t begin = g == 0 ? 0 : firstLen + (g - 1) * 4;
    size_t len = g == 0 ? firstLen : 4;
    bool groupHasDigit = false;

    for (size_t i = 0; i < len; i++) {
      int d = digits[begin + i] - '0';
      if (d == 0) {
        pendingZero = true;
        continue;
      }
      size_t j = len - 1 - i;  // place within the section: 0 ones .. 3 千
      if (spoken && pendingZero) {
        out += kDigit[0];
      }
      pendingZero = false;
      // "一十" is read 十二 / 十五 / 十万, but 一百一十 keeps the leading 一.
      if (!(j == 1 && d == 1 && !groupHasDigit)) out += kDigit[d];
      out += kSectionPlace[j];
      spoken = true;
      groupHasDigit = true;
      anyDigit = true;
    }

    if (groupHasDigit) {
      out += kSectionUnit[nGroups - 1 - g];
    }
  }

  if (!anyDigit) out += kDigit[0];  // 0 / 000 -> 零
}

struct UnitEntry {
  const char* ascii;
  const char* spoken;
};

// Units that follow a bare number. Longest ASCII keys must come first so that
// "kHz" is not matched as "Hz".
const UnitEntry kUnitsTable[] = {
    {"mA",   "\xE6\xAF\xAB\xE5\xAE\x89"},              // 毫安
    {"mV",   "\xE6\xAF\xAB\xE4\xBC\x8F"},              // 毫伏
    {"kV",   "\xE5\x8D\x83\xE4\xBC\x8F"},              // 千伏
    {"GHz",  "\xE5\x90\x89\xE8\xB5\xAB"},              // 吉赫
    {"MHz",  "\xE5\x85\x86\xE8\xB5\xAB"},              // 兆赫
    {"kHz",  "\xE5\x8D\x83\xE8\xB5\xAB"},              // 千赫
    {"GB",   "\xE5\x90\x89\xE5\xAD\x97\xE8\x8A\x82"},  // 吉字节
    {"KB",   "\xE5\x8D\x83\xE5\xAD\x97\xE8\x8A\x82"},  // 千字节
    {"MB",   "\xE5\x85\x86\xE5\xAD\x97\xE8\x8A\x82"},  // 兆字节
    {"ms",   "\xE6\xAF\xAB\xE7\xA7\x92"},              // 毫秒
    {"Hz",   "\xE8\xB5\xAB\xE5\x85\xB9"},              // 赫兹
    {"km",   "\xE5\x85\xAC\xE9\x87\x8C"},              // 公里
    {"cm",   "\xE5\x8E\x98\xE7\xB1\xB3"},              // 厘米
    {"mm",   "\xE6\xAF\xAB\xE7\xB1\xB3"},              // 毫米
    {"kg",   "\xE5\x85\xAC\xE6\x96\xA4"},              // 公斤
    {"mg",   "\xE6\xAF\xAB\xE5\x85\x8B"},              // 毫克
    {"dB",   "\xE5\x88\x86\xE8\xB4\x9D"},              // 分贝
    {"%",    "\xE7\x99\xBE\xE5\x88\x86\xE4\xB9\x8B"},  // 百分之
    {"V",    "\xE4\xBC\x8F"},                          // 伏
    {"A",    "\xE5\xAE\x89"},                          // 安
    {"W",    "\xE7\x93\xA6"},                          // 瓦
    {"C",    "\xE6\xB0\x8F\xE5\xBA\xA6"},              // 摄氏度
    {"s",    "\xE7\xA7\x92"},                          // 秒
    {"g",    "\xE5\x85\x8B"},                          // 克
    {"m",    "\xE7\xB1\xB3"},                          // 米
    {"L",    "\xE5\x8D\x87"},                          // 升
};

size_t unitLength(const std::string& s, size_t at) {
  for (const UnitEntry& e : kUnitsTable) {
    size_t n = std::strlen(e.ascii);
    if (at + n <= s.size() && s.compare(at, n, e.ascii) == 0) return n;
  }
  return 0;
}

std::string lookupUnit(const std::string& s, size_t at) {
  for (const UnitEntry& e : kUnitsTable) {
    size_t n = std::strlen(e.ascii);
    if (at + n <= s.size() && s.compare(at, n, e.ascii) == 0) return e.spoken;
  }
  return std::string();
}

}  // namespace

size_t charCount(const std::string& s, size_t from, size_t to) {
  size_t n = 0;
  for (size_t i = from; i < to && i < s.size();) {
    i += utf8Len(static_cast<unsigned char>(s[i]));
    n++;
  }
  return n;
}

std::string normalizeText(const std::string& in, const NormalizeOptions& opt) {
  std::string out;
  out.reserve(in.size() * 2);

  size_t i = 0;
  while (i < in.size()) {
    if (opt.numbers && isAsciiDigit(in[i])) {
      // Gather 123 / 12.5 / 1,234
      std::string digits;
      while (i < in.size() &&
             (isAsciiDigit(in[i]) ||
              (in[i] == '.' && i + 1 < in.size() && isAsciiDigit(in[i + 1])) ||
              (in[i] == ',' && i + 2 < in.size() && isAsciiDigit(in[i + 1]) &&
               isAsciiDigit(in[i + 2])))) {
        if (in[i] == '.') digits += '.';
        else if (in[i] != ',') digits += in[i];
        i++;
      }

      size_t unitLen = (opt.units || opt.percent) ? unitLength(in, i) : 0;
      bool isPercent = opt.percent && in.compare(i, 1, "%") == 0;
      if (isPercent) {
        out += "\xE7\x99\xBE\xE5\x88\x86\xE4\xB9\x8B";  // 百分之
      }

      size_t dot = digits.find('.');
      if (dot == std::string::npos) {
        appendPlaceValues(out, digits);
      } else {
        std::string ip = digits.substr(0, dot);
        std::string fp = digits.substr(dot + 1);
        if (ip.empty()) ip = "0";
        appendPlaceValues(out, ip);
        out += "\xE7\x82\xB9";  // 点
        for (char c : fp) out += kDigit[c - '0'];
      }

      // Units are spoken after the number ("12.5V" -> 十二点五伏); 百分之 is
      // the one genuine prefix. The unit used to be emitted before the
      // digits, which made the README's own example come out 伏十二点五.
      if (!isPercent && unitLen) {
        out += lookupUnit(in, i);
      }
      i += unitLen;
      continue;
    }

    if (opt.collapseSpace && in[i] == ' ' && !out.empty() && out.back() == ' ') {
      i++;
      continue;
    }

    out += in[i];
    i++;
  }
  return out;
}

CutPoint lastSafeCut(const std::string& buf, size_t minKeepBytes,
                     size_t maxCutBytes, bool forceAtMax) {
  CutPoint r;
  if (buf.empty()) return r;

  size_t limit = (maxCutBytes == static_cast<size_t>(-1)) ? buf.size() : maxCutBytes;
  if (limit > buf.size()) limit = buf.size();
  if (limit <= minKeepBytes) return r;

  int depth = 0;
  std::vector<size_t> cuts;

  for (size_t i = 0; i < limit; i++) {
    unsigned char c = static_cast<unsigned char>(buf[i]);

    size_t ob = matchLen(kOpenBracket, buf, i);
    if (ob) {
      depth++;
      i += ob - 1;
      continue;
    }
    size_t cb = matchLen(kCloseBracket, buf, i);
    if (cb) {
      if (depth > 0) depth--;
      i += cb - 1;
      continue;
    }
    if (depth != 0) continue;

    if (isAsciiDigit(c)) {
      // Skip the whole numeric run so a cut cannot land inside "12.5V".
      size_t j = i;
      while (j < limit && (isAsciiDigit(buf[j]) || buf[j] == '.' ||
                           buf[j] == ',')) {
        j++;
      }
      if (j > i) i = j - 1;
      continue;
    }

    size_t se = matchLen(kSentenceEnd, buf, i);
    size_t cs = se ? 0 : matchLen(kClauseSep, buf, i);
    size_t w = se ? se : cs;
    if (w) {
      cuts.push_back(i + w);
      i += w - 1;
      continue;
    }
  }

  if (!cuts.empty()) {
    for (auto it = cuts.rbegin(); it != cuts.rend(); ++it) {
      if (*it <= minKeepBytes) break;
      r.offset = *it;
      r.tailBytes = buf.size() - *it;
      return r;
    }
  }

  if (!forceAtMax) return r;

  // No boundary in range. Cut at the cap, but make sure the offset is a real
  // character boundary and does not sit inside a number.
  size_t at = limit;
  if (at > buf.size()) at = buf.size();
  if (at <= minKeepBytes) return r;

  // `at` is a boundary when buf[at] starts a character, i.e. is not a
  // continuation byte. Testing buf[at - 1] instead -- as an earlier revision
  // did -- walks back to just *after* a lead byte, which splits the character
  // into a lone lead byte plus orphaned continuations.
  while (at > 0 && at < buf.size() &&
         (static_cast<unsigned char>(buf[at]) & 0xC0) == 0x80) {
    at--;
  }

  // step forward past any number we may have landed inside
  size_t j = at;
  while (j < buf.size() &&
         (isAsciiDigit(buf[j]) || buf[j] == '.' || buf[j] == ',')) {
    j++;
  }
  if (j > at) at = j;
  // Stepping over the number may have landed mid-character again.
  while (at > 0 && at < buf.size() &&
         (static_cast<unsigned char>(buf[at]) & 0xC0) == 0x80) {
    at--;
  }

  if (at == 0) return r;
  r.offset = at;
  r.tailBytes = buf.size() - at;
  return r;
}

bool isFlushable(const std::string& text, size_t minTailChars) {
  for (size_t i = 0; i < text.size(); i++) {
    size_t se = matchLen(kSentenceEnd, text, i);
    if (se) return true;
    size_t cs = matchLen(kClauseSep, text, i);
    if (cs) {
      if (charCount(text, i + cs, text.size()) >= minTailChars) return true;
    }
    size_t w = se ? se : cs;
    if (w) i += w - 1;
  }
  return false;
}

}  // namespace ekho