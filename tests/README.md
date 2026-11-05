# tests/

Unit tests for ekho-linux: the layer that runs in seconds, with no voice data,
no audio device and no engine, so `make check` says something on every machine
that can build the tree. Before 2026-11-05 the only automated test was
`src/test_ekho.cpp` -- upstream's demo, zero assertions -- which passed green on
a machine with no voice data at all, because `blockSpeak()` is a stub and
nothing it ran was checked. The integration tier (assertions against real
synthesis) already existed as shell scripts; see "Tiers" below.

## Tiers

| tier | command | needs | time |
|---|---|---|---|
| quick (`make check`) | `make check` | build only | seconds |
| full | `tests/run-all.sh` | `out/` binaries + voice data | minutes |

`make check` runs every unit binary first, then `test_ekho`, which **skips
itself (exit 77)** when no voice pack is found -- a green suite that never
synthesised anything is worse than an honest SKIP. The full tier wraps the
unit binaries and the synthesis-assertion scripts (`scripts/sim.sh`,
`scripts/server-test.sh`, optionally `scripts/reg-44k.sh`).

Voice data is deliberately not in git; `scripts/fetch-voice.sh` fetches it
(md5-pinned). Unit tests do not need it.

## Coverage map

| binary | module under test | what can break there |
|---|---|---|
| `test_normalize` | `src/text_normalize.cpp` | digit-to-numeral tables, place values, unit words, cut points landing inside a character or a number, flush policy |
| `test_pcmring` | `PcmRing` in `src/streaming.cpp` | FIFO across wraparound, drop accounting, backpressure, abort of a parked producer, zero capacity |
| `test_http` | `src/http_server.cpp` | request line/header/query parsing, Content-Length and chunked framing, pipelined body bytes, 411/404/400/100-continue, connection-count guard, url/json codecs |
| `test_ssml` | `src/ssml.cpp` | tag stripping, audio extraction, hanzi-space filtering, invalid UTF-8 recovery |
| `test_resample` | `src/resample.cpp` | output length at every rate ratio, unity DC gain, determinism, no-op contract |
| `test_alsa` | `src/alsa_backend.cpp` | dlopen failure and missing-device paths degrade instead of crash |
| `test_ekho` | whole engine | voice pack present and `synth3()` produces PCM (skips without data) |

## Failure register

One row per failure class, with the case that owns it. Every case in the test
files carries a comment naming its row; a case that cannot point at a row is a
case nobody will maintain.

| # | failure mode | asserted by | status |
|---|---|---|---|
| 1 | one digit maps to the wrong numeral | `test_normalize` digits table | green |
| 2 | place words transposed or missing ("12" as 千二, "10086" without 万) | `test_normalize` place sections | **xfail, defect D1/D2/D3** |
| 3 | trailing zero spoken ("10" as 十零) | `test_normalize` zeros section | **xfail, defect D3** |
| 4 | unit word mojibake (kHz as 千赴, dB as 分贤, ...) | `test_normalize` unit words | **xfail, defect D4** |
| 5 | unit spoken before the number ("12.5V" as 伏十二点五) | `test_normalize` unit order | **xfail, defect D5** |
| 6 | a cut lands inside a CJK character or splits "12.5" | `test_normalize` cut sweep (every cap × force) | green (this is the regression net for the fixed `matchLen` byte-match bug) |
| 7 | flush fires on a lonely comma, or never fires | `test_normalize` flush section | green |
| 8 | ring loses or reorders samples across wraparound | `test_pcmring` FIFO/wraparound | green |
| 9 | ring drops the newest frames of an oversized burst and miscounts | `test_pcmring` oversized write | **xfail, defect D6** |
| 10 | ring parks a producer forever / crash on capacity 0 | `test_pcmring` zero capacity, abort | **xfail, defect D7** |
| 11 | backpressure drops instead of blocking | `test_pcmring` blocking-write sections | green |
| 12 | a request line/header/query parsed wrong (dispatch to the wrong handler) | `test_http` live request sections | green |
| 13 | last header line silently dropped (the CRLF bug fixed in the source) | `test_http` "last header is not dropped" | green |
| 14 | body bytes pipelined into the request head are lost | `test_http` pipelined section | green |
| 15 | truncated body reads as a clean end (framing guessed) | `test_http` Content-Length truncation | green |
| 16 | chunked body without its final zero chunk reads as complete | `test_http` chunked truncation | **xfail, defect D8** |
| 17 | malformed chunk size hangs or crashes the decoder | `test_http` chunked malformed table | green |
| 18 | unframed POST reads to EOF instead of 411 | `test_http` 411 section | green |
| 19 | `Expect: 100-continue` unanswered (uploading client stalls) | `test_http` 100-continue section | green |
| 20 | unbounded threads per connection | `test_http` 503 flood section | green |
| 21 | SSML tags with attributes survive stripping | `test_ssml` attribute section | **xfail, defect D9** |
| 22 | consecutive `<speak>` blocks mangled | `test_ssml` two-wrapper section | **xfail, defect D9** |
| 23 | hanzi gaps kept / ASCII spaces dropped | `test_ssml` filterSpaces | green |
| 24 | invalid UTF-8 crashes instead of truncating | `test_ssml` recovery section | green |
| 25 | resampler returns the wrong length (the lone-K 3x bug class) | `test_resample` length table | green |
| 26 | kernel gain != 1 (level shift on steady vowels) | `test_resample` DC section | green |
| 27 | ALSA absent/missing-device path crashes | `test_alsa` degrade section | green |

## Known defects found by this suite (2026-11-05, unfixed)

Marked `EK_KNOWN_*` in the tests: they count as neither pass nor fail, but the
moment a fix lands they turn into XPASS errors and force the marker to be
removed. All were found by reading the code against measured behaviour; none
crashes the synthesis path except D7, which needs a zero-capacity ring to be
constructed first.

* **D1 -- `kPlace[]` transposed** (`appendPlaceValues`, src/text_normalize.cpp).
  The table is indexed by place-from-the-right but written
  highest-place-first, so every multi-digit number reads with the place words
  shifted: "12" → 千二, "1234" → 一十二百三千四. The `n > 9` digit-out path is
  the only one that reads correctly.
* **D2 -- no 万 place for 5-9 digit numbers**: "10086" → 一零八千六 (the hotline
  number; every 5-9 digit number is affected).
* **D3 -- trailing zeros kept**: "10" → 十零, "100" → 一百零.
* **D4 -- seven mojibake rows in `kUnitsTable`**: kHz→千赴, Hz→赤关,
  GHz/MHz→克赴, MB→克字节, GB→免字节, dB→分贤 (one wrong nibble per row).
  Pure hanzi is emitted, so nothing crashes -- it is only audible.
* **D5 -- unit spoken before the number**: "12.5V" → 伏十二点五, contradicting
  the README's own example (十二点五伏). 百分之 is a genuine prefix and is
  placed correctly.
* **D6 -- `PcmRing::write()` keeps the OLDEST frames of an oversized burst**
  and reports dropped=0, against its documented drop-oldest contract. Latent:
  the streaming path only calls `writeBlocking()`.
* **D7 -- `PcmRing` with capacity 0**: `write()` divides by the capacity
  (SIGFPE); `writeBlocking()` parks forever. Constructible through the public
  constructor; the CLI sizes rings from `--ring-ms`.
* **D8 -- chunked body without the final zero chunk reads as a clean end**
  (`HttpBodyReader::read`, the size-line EOF path returns 0): a truncated
  stream is indistinguishable from a finished one. The analogous EOF *inside*
  a chunk correctly reports -1.
* **D9 -- `Ssml::stripTags` loses on real-world SSML**: a `<speak>` with
  attributes builds the closing needle from the whole opening tag
  (`</speak version="1.0">`), which never matches, so tags reach the engine;
  two consecutive wrappers pair the outer tags and leave
  `a</speak><speak>b`.

Fixing D1/D4/D5 changes synthesised bytes for texts containing digits and
units; `scripts/reg-44k.sh`'s 44.1 kHz anchors (02-numbers among them) will
need re-recording after such a fix, which is why the suite records them as
defects instead of failing the build.

## Red/green calibration

Every assertion class above has been seen red once (revert-fix single-line
mutations in a scratch build, then restored to green). Recorded 2026-11-05:

| mutation | goes red at |
|---|---|
| `kPlace` 千→十 (D1 partially fixed) | `test_normalize` XPASS on the place-value cases |
| `matchLen` compares one byte (the pre-fix UTF-8 bug) | cut section + property sweep |
| head parser drops the final CRLF | `test_http` last-header + chunked-as-last-header cases |
| preload bytes not handed to the reader | `test_http` pipelined section |
| `PcmRing::read` advances two slots | `test_pcmring` FIFO/wraparound |
| `nOut` ignores the rate ratio | `test_resample` length table |
| `filterSpaces` without the end-trim | `test_ssml` trailing-space case |

The known-defect rows are red by definition until fixed; the calibration
above covers everything marked green.

## Deliberately not tested

* **Voice quality by ear.** `scripts/make-samples.sh` renders listening sets;
  no assertion can own it.
* **espeak-ng reproducibility.** Text containing ASCII varies run to run (see
  the note in `scripts/sim.sh` §7); byte-equality claims are made only for
  pure-Chinese text, where the pipeline is exact.
* **Real ALSA playback on the board.** No sound card here; `test_alsa` pins
  the degrade paths, and the board-side playback remains a manual check.
* **The 30 s `SO_RCVTIMEO` stall path.** Guarded by design; asserting it would
  make the suite take minutes for one constant.
* **Two pipelined requests on one socket.** The server closes after one
  response by design (header comment in `HttpConnection`).
* **`ekho-worker` line protocol and cross/aarch64 runtime.** Integration-level
  today: the worker has no harness yet, and `cross/verify-kits.sh` covers the
  kit's own integrity. Both are open gaps, recorded here on purpose.

## Conventions

* One case = one failure mode, named in the comment above it.
* String literals stay ASCII (`\xNN` escapes), so no build needs a UTF-8
  locale; a hex escape followed by a hex digit must be split (`"\xE5\x8D\x81" "123"`).
* Exit codes: 0 pass, 1 fail (XPASS counts as fail), 77 skip.
* The harness is `tests/check.h`: header-only, no dependencies, so the same
  suite cross-compiles to the board.
