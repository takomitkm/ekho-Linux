/***************************************************************************
 * ekho-linux -- assertion harness for the unit tests.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Deliberately dependency-free: the target board has no package manager, the
 * fork cross-compiles against libc/pthread only, and a test harness that cannot
 * be built on the board is a harness nobody runs there. Header-only, C++11.
 *
 * Conventions the rest of tests/ follows:
 *
 *   * every case names the failure mode it exists for in a comment above it;
 *     a case that cannot point at one is a case nobody will maintain
 *   * a failing check prints the measured value, not just the expected one
 *   * exit 0 = all passed, 1 = at least one failure, 77 = skipped (the
 *     automake convention; `make check` reports it as SKIP, not PASS)
 *   * EK_KNOWN_BUG() marks a defect this suite found but does not fix: it
 *     counts as neither pass nor fail, but an unexpected pass is an error, so
 *     a fix cannot land silently without deleting the marker
 **************************************************************************/
#ifndef EKHO_TESTS_CHECK_H
#define EKHO_TESTS_CHECK_H

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace ektest {

struct State {
  int checks = 0;
  int failures = 0;
  int known = 0;
  int unexpectedPasses = 0;
};

inline State& state() {
  static State s;
  return s;
}

// Renders a byte string readably: printable ASCII as-is, everything else
// (including every CJK byte) as \xNN, so a mismatch shows the exact bytes.
inline std::string escape(const std::string& in) {
  std::string out;
  for (size_t i = 0; i < in.size(); i++) {
    unsigned char c = static_cast<unsigned char>(in[i]);
    if (c == '\\') {
      out += "\\\\";
    } else if (c == '"') {
      out += "\\\"";
    } else if (c >= 0x20 && c < 0x7f) {
      out += static_cast<char>(c);
    } else {
      char buf[8];
      std::snprintf(buf, sizeof buf, "\\x%02X", c);
      out += buf;
    }
  }
  return out;
}

inline void section(const char* name) { std::printf("\n  [%s]\n", name); }

// Context line for measurements that are printed but not asserted on.
inline void note(const char* fmt, const std::string& a = std::string(),
                 const std::string& b = std::string(),
                 const std::string& c = std::string()) {
  std::printf("    ");
  std::printf(fmt, escape(a).c_str(), escape(b).c_str(), escape(c).c_str());
  std::printf("\n");
}

inline bool check(bool ok, const char* file, int line, const std::string& what,
                  const std::string& detail = std::string()) {
  state().checks++;
  if (ok) return true;
  state().failures++;
  std::printf("    FAIL %s:%d  %s", file, line, what.c_str());
  if (!detail.empty()) std::printf("  [%s]", detail.c_str());
  std::printf("\n");
  return false;
}

inline void checkStr(bool ok, const char* file, int line, const std::string& what,
                     const std::string& got, const std::string& want) {
  state().checks++;
  if (ok) return;
  state().failures++;
  std::printf("    FAIL %s:%d  %s\n", file, line, what.c_str());
  std::printf("         got  \"%s\"\n", escape(got).c_str());
  std::printf("         want \"%s\"\n", escape(want).c_str());
}

inline void checkInt(bool ok, const char* file, int line, const std::string& what,
                     long long got, long long want) {
  state().checks++;
  if (ok) return;
  state().failures++;
  std::printf("    FAIL %s:%d  %s\n", file, line, what.c_str());
  std::printf("         got  %lld\n", got);
  std::printf("         want %lld\n", want);
}

// A defect found by this suite and deliberately not fixed here. It is not a
// pass (the code is still wrong) and not a failure (the suite must be usable
// before the fix lands). It *is* an error if the predicate starts holding:
// that means the bug was fixed without updating the test's record.
//
// `got` / `want` are printed on the xfail line so the defect shows its
// measured shape, not just its label.
inline void knownBug(bool predicateHolds, const char* file, int line,
                     const std::string& what, const std::string& why,
                     const std::string& got = std::string(),
                     const std::string& want = std::string()) {
  state().checks++;
  if (predicateHolds) {
    state().unexpectedPasses++;
    std::printf("    XPASS %s:%d  %s\n", file, line, what.c_str());
    std::printf("          the documented defect no longer reproduces: %s\n",
                why.c_str());
    return;
  }
  state().known++;
  std::printf("    xfail %s:%d  %s (%s)\n", file, line, what.c_str(), why.c_str());
  if (!got.empty() || !want.empty()) {
    std::printf("          got  \"%s\"\n", escape(got).c_str());
    std::printf("          want \"%s\"\n", escape(want).c_str());
  }
}

inline int summary(const char* name) {
  State& s = state();
  std::printf("\n%s: %d checks, %d failed, %d known-defect, %d unexpected pass\n",
              name, s.checks, s.failures, s.known, s.unexpectedPasses);
  if (s.failures == 0 && s.unexpectedPasses == 0) {
    std::printf("%s: PASS\n", name);
    return 0;
  }
  std::printf("%s: FAIL\n", name);
  return 1;
}

// 77 is automake's SKIP. A test that cannot run here (missing voice data,
// missing device) must say so rather than pass -- a green suite that never
// synthesised anything is worse than no suite.
inline int skip(const char* name, const std::string& why) {
  std::printf("%s: SKIP -- %s\n", name, why.c_str());
  return 77;
}

}  // namespace ektest

#define EK_SECTION(name) ektest::section(name)

#define EK_CHECK(cond) \
  ektest::check((cond), __FILE__, __LINE__, #cond)

#define EK_CHECK_MSG(cond, msg) \
  ektest::check((cond), __FILE__, __LINE__, (msg))

#define EK_STREQ(got, want)                                              \
  do {                                                                   \
    const std::string ek_g = (got);                                      \
    const std::string ek_w = (want);                                     \
    ektest::checkStr(ek_g == ek_w, __FILE__, __LINE__,                   \
                     std::string(#got) + " == " + #want, ek_g, ek_w);    \
  } while (0)

#define EK_EQ(got, want)                                                 \
  do {                                                                   \
    const long long ek_g = static_cast<long long>(got);                  \
    const long long ek_w = static_cast<long long>(want);                 \
    ektest::checkInt(ek_g == ek_w, __FILE__, __LINE__,                   \
                     std::string(#got) + " == " + #want, ek_g, ek_w);    \
  } while (0)

#define EK_NEAR(got, want, tol, what)                                    \
  do {                                                                   \
    const double ek_g = (got);                                           \
    const double ek_w = (want);                                          \
    ektest::check((ek_g - ek_w <= (tol)) && (ek_w - ek_g <= (tol)),      \
                  __FILE__, __LINE__, (what));                           \
  } while (0)

#define EK_KNOWN_BUG(cond, why) \
  ektest::knownBug((cond), __FILE__, __LINE__, #cond, (why))

// Short form for predicates built from several calls.
#define EK_KNOWN(cond, why) EK_KNOWN_BUG((cond), (why))

// Known-defect form of the string equality: xfail while got != want, XPASS
// (an error) once the fix lands, so the marker cannot outlive the defect.
#define EK_KNOWN_STREQ(got, want, why)                                   \
  do {                                                                   \
    const std::string ek_g = (got);                                      \
    const std::string ek_w = (want);                                     \
    ektest::knownBug(ek_g == ek_w, __FILE__, __LINE__,                   \
                     std::string(#got) + " == " + #want, (why),          \
                     ek_g, ek_w);                                        \
  } while (0)

#endif  // EKHO_TESTS_CHECK_H
