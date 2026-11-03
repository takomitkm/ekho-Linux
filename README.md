# ekho

## This fork: Linux only

This tree is a Linux-only fork of <https://github.com/hgneng/ekho>: Windows build
products are gone, the build is autotools only, and a few things were added or
fixed. It is published at <https://github.com/takomitkm/ekho-AIGC> (the
repository was renamed from `/ekho`; the old path redirects), pushed as a fresh
snapshot -- the two histories share no common ancestor, and the fork's `master`
is this tree. The upstream project's own description closes this file. What it
needs and what it gives you:

    scripts/build-native.sh      # configure + make into out/ (ekho, ekho-stream, ekho-server)
    scripts/sim.sh               # correctness and streaming assertions; non-zero exit on failure
    scripts/server-test.sh       # real HTTP server, sections 0-8
    scripts/reg-44k.sh           # the 44.1 kHz byte-exact anchor + the 48 kHz tempo check
    scripts/make-samples.sh      # render listening samples to $HOME/ekho-samples
    scripts/convert-voice.c      # re-encode a .voice pack at another rate (built by hand)
    scripts/convert-alphabet.c   # same for the 26 per-letter samples (built by hand)
    scripts/fetch-voice.sh       # fetch the voice data git does not carry (SourceForge 0.2, md5-pinned)
    scripts/funasr_bridge.py     # ASR-side glue: POST text to ekho-server, play or save the reply

Added on top of upstream:

* **ALSA output** (`src/alsa_backend.{h,cpp}`): `dlopen`-based, so building needs
  the vendored headers only -- no `libasound2-dev`, and no `libasound.so.2` in
  DT_NEEDED. It is **not wired to anything yet**: there is no `--play` option,
  `Ekho::play()` shells out to an external player that is never configured, and
  `Ekho::blockSpeak()` is a stub that returns 0 without synthesising. Writing a
  WAV file, or reading the HTTP stream, is the supported path.
* **Output format: 48 kHz / 16-bit / 2 channels by default.** That is the only
  format the board's USB speaker accepts, so the default needs no conversion
  anywhere on the target. `--samplerate=HZ` and `--channels=1|2` override it in
  all three binaries, and `--samplerate=0` means "use the voice pack's own rate"
  (48 kHz for the shipped pack). The old default, 44.1 kHz / mono, is still
  reachable, and `scripts/reg-44k.sh` pins its output byte for byte.
* **`-v English` synthesises.** It used to run into a stub in
  `src/synthesize.cpp` that returned without producing audio: 44-byte silent
  WAV, exit status 0. It now goes through espeak-ng like the mixed-text path
  does, with the same 100 wpm / `en-us+f3` default as above.
* **Streaming synthesis** (`src/streaming.{h,cpp}`, `Ekho::synth4()`): append text as
  it arrives and get PCM out before the text is finished. Punctuation forces a
  segment break; numbers and units are normalised first.
* **`ekho-server`** (`src/ekho_server_main.cpp`): `POST /tts` chunked in, chunked
  `audio/L16` out, plus `POST /tts?wait=1`, `GET /health`, `GET /voices`.
  Default port 2046. It needs no privileges: bind above 1024, data path from the
  environment, output path from `--out-dir`.
* **English speed is a real knob**, independent of the Chinese tempo: `--english-speed`
  in all three binaries (`ekho-server`: as the default, plus `english_speed=DELTA` per
  request), and `EKHO_ESPEAK_RATE=WPM` to move the base rate without a rebuild. Before
  this it did nothing: the value was set before espeak existed, then overwritten by a
  delta 20 points off, and out-of-range values were dropped without a word. The base
  rate is 100 wpm and the espeak voice is `en-us+f3`, both picked by ear on 2026-10-17
  (see `scripts/make-samples.sh` groups 05 and 06); `--english-voice` overrides the
  voice for one run and `EKHO_ESPEAK_VOICE` moves the default without a rebuild.
* **The Mandarin pack is 48 kHz, so pack rate equals output rate.** The tracked
  `ekho-data/pinyin.index` describes a 48 kHz version of the upstream 44.1 kHz pack,
  and `scripts/convert-voice.c` produces the matching `pinyin.voice` from the upstream
  one. The 44.1 kHz originals stay next to them as `*.44k` -- they are what the
  converter reads, and both pairs are gitignored, so a clone has to supply them before
  the pack can be re-derived or the regression anchor run. `scripts/fetch-voice.sh`
  downloads the pinned upstream sources (SourceForge 0.2, sizes and md5s in its
  header) and lays `pinyin/` and the 16 kHz letter wavs out.
  The rate follows the target board: its only playback device accepts
  48 kHz / S16_LE / 2ch and nothing else, so `ekho`, `ekho-server` and `ekho-stream`
  all default there, and pack rate equals output rate -- no sonic resample on every
  sample of every syllable. `--samplerate=0` asks for the pack's own rate,
  `--channels=1` for mono.
  **This replaced an 8 kHz conversion made the same day.** Ekho decodes each syllable
  chunk on first use and keeps the PCM for the life of the process, so 8 kHz had cut
  peak RSS from 117 MB to 48 MB on a text reaching every syllable in the dictionary
  (small sentences 25.3 -> 24.5 MB). It was dropped because that resampler's 4 kHz
  low-pass dulls the sibilants. The trade is explicit: the 48 kHz pack is
  136,359,454 B against the 44.1 kHz original's 125,287,518 B (+8.8%), so the
  resident-RAM saving is gone. Measured on `07-long-news`, which reaches a subset of
  the dictionary: peak RSS 29.5 MB one-shot and 28.8 MB streaming, against 29.1 and
  28.5 MB with the 44.1 kHz pack -- +1% there; the full +8.8% only shows up in a
  session that touches every syllable. Synthesis CPU is unchanged within noise
  (554 vs 553 ms warm cache), so the resample was never the bottleneck on x86 and no
  ARM saving is claimed. What does change is the output itself: energy above the
  source's 22.05 kHz band drops from -70 dB to -83 dB relative to full-band RMS,
  i.e. the interpolating resampler's imaging is gone.
  Speaking rate holds: `scripts/reg-44k/expected.md5` records the 44.1 kHz / mono
  bytes of seven reference texts and `scripts/reg-44k.sh` re-checks them, then measures
  the 48 kHz / 2ch renders against them (recorded 2026-10-19: mean -0.17%, worst
  -1.32%; it fails past mean 0.5% or any single text 2%). Making that hold needed one
  engine fix: `setSpeed()`'s tempo constant was computed with two integer divisions,
  which truncated a duration expressed in 44.1 kHz frames and cost exactly one tempo
  step (-0.42%, `baseDelta` 138 instead of 139) on a 48 kHz pack; it is floating point
  now and the 44.1 kHz result is unchanged. The syllable-join threshold
  is **not** rate-scaled at 44.1 kHz and above -- scaling it was measured on the
  48 kHz pack and made things worse (worst 3.4%), so `quiet_level` stays equal to
  `mOverlap` there; see the comment in `src/libekho_impl.cpp`. The ~11% and ~12%
  figures that used to sit in this section were artifacts of the rejected 8 kHz pack.
  **Only the Mandarin pack is shipped**, and it is the 48 kHz one, so nothing is
  resampled at run time for the default voice. (Cantonese used to be the exception
  -- its `jyutping.voice` stayed 16 kHz -- but that voice is no longer in the tree;
  see "This fork: Linux only".)

  **The alphabet samples are converted as well, and conversion alone is not
  enough.** `ekho-data/alphabet/*.wav` -- the 26 per-letter recordings behind
  `A 股`, initials and spelled-out letters -- are 16 kHz upstream, so they used to
  play 3x too fast on the 48 kHz pack and 2.76x on the 44.1 kHz one (upstream's
  `@fixme: disable alphabet because the samplerate may not correct`).
  `scripts/convert-alphabet.c` rewrites them at 48 kHz (originals kept beside them
  as `*.wav.16k`), but one converted copy cannot serve three packs: the engine
  resamples any sample whose rate differs from the pack's rate at load time, in
  `PhoneticSymbol::readSndfile()` (`src/resample.h`, the same windowed-sinc kernel
  as `convert-voice`). A lone `K` now renders in 0.110 s at *every* combination --
  48 kHz or 44.1 kHz pack, 48 kHz or 44.1 kHz output, mono or stereo -- where it
  used to be 0.041-0.044 s. (The Cantonese letters were 0.291 s under the 16 kHz
  pack; that voice data is no longer carried.) The tempo itself is the pack's
  usual one: Mandarin speeds the recorded syllables up 2.39x (`baseDelta` 139),
  letters included.

### Known issues

* **Voice data must be supplied.** `ekho-data/*.voice` and the pinyin sample
  tree are not in the repository (they are large binaries); the `.index` files
  are. A fresh clone builds but cannot synthesise until voice data is
  dropped into `ekho-data/`, or `EKHO_DATA_PATH` points at a directory that has
  it. Ekho writes `.index`/`.voice` into that directory when the index is missing,
  so it must be writable -- that is the historical reason people ran it as root
  once; a relocatable kit with its own data directory has no such problem.
  `scripts/fetch-voice.sh` fetches the upstream sources (md5-pinned, SourceForge
  0.2 page) and lays them out; the build-generate-convert-anchor recipe sits in
  its header.
* **Mandarin is the only voice this tree carries** (decided 2026-11-02). The
  dictionaries and sample trees for Cantonese, Toisanese, Hakka, Tibetan,
  Korean and Ngangien were removed; the code still knows those languages, so a
  data directory that supplies them still works, but nothing here ships them and
  `scripts/fetch-voice.sh` will not bring them back. `GET /voices` on a server
  started from this tree lists what it actually loaded -- Mandarin, plus English
  through espeak-ng.
* **Nothing plays audio by itself.** `ekho "text"` without `-o` produces no sound
  (there is no `--play`; see the ALSA bullet above), and `Ekho::blockSpeak()` is a
  stub. The board's speaker has been probed and accepts 48 kHz / S16_LE / 2ch only
  -- which is why that is the default -- but the ALSA backend has never been called
  on it.

## Making sound

Two ways to get audio out of this tree, from the simplest to the one that
serves other programs. Both were run against a real board with the USB
speaker attached, so the commands below are known to work, not guessed.

### 1. One shot: render a file, then play it

The shortest path from a line of text to sound. It needs the voice data and
a sound device, nothing else:

```bash
export EKHO_DATA_PATH=/path/to/ekho-data          # pinyin.index/pinyin.voice, the dicts
export ESPEAK_DATA_PATH=/path/to/espeak-ng-data    # only for the English voice

bin/ekho -v Mandarin --samplerate=48000 --channels=2 -f text.txt -o out.wav
aplay out.wav                                      # or: paplay out.wav
```

* `-f text.txt` reads the text from a file and `-f -` from stdin, which keeps
  UTF-8 Chinese out of the shell -- a terminal or a Windows console can mangle
  it, and a mangled character is a mangled syllable, not a display glitch.
* The WAV is 16-bit; `aplay` reads the header, so no format flags are needed.
* `--samplerate=48000 --channels=2` is already the default, and it is the
  board speaker's only accepted format. `--samplerate=0` means "use the voice
  pack's own rate".
* If the speaker is not the default card, name it: `aplay -l` lists the
  cards, e.g. `aplay -D plughw:CARD=MCP01,DEV=0 out.wav`.

On the board, whose kit lives in `/root/ekho-bench` with the 48 kHz pack in
`p48/`:

```bash
cd /root/ekho-bench
export ESPEAK_DATA_PATH=/root/ekho-bench/espeak-ng-data EKHO_DATA_PATH=/root/ekho-bench/p48
echo 你好，世界。 | bin/ekho -v Mandarin --samplerate=48000 --channels=2 -f - -o /tmp/say.wav \
  && aplay -D plughw:CARD=MCP01,DEV=0 /tmp/say.wav
```

Two traps that cost real time once:

* **A build can carry a compiler-baked espeak path.** The board's binary
  defaults to the cross-compile host's `D:/yzd/...` and dies with
  `Error processing file '...espeak-ng-data/phontab'` unless
  `ESPEAK_DATA_PATH` is set. Setting it always is the safe habit.
* **`ekho --help` can be older than the binary.** The board build does not
  list `--samplerate`/`--channels` in its help text, but both are accepted;
  `strings bin/ekho | grep -- --samplerate` tells the truth.

### 2. As a service: ekho-server

`ekho-server` is an HTTP daemon that takes text and returns audio. It does
not play anything itself (see Known issues above): the client plays the
reply.

```bash
export EKHO_DATA_PATH=/path/to/ekho-data
export ESPEAK_DATA_PATH=/path/to/espeak-ng-data
out/ekho-server --port 2046 --out-dir /tmp
```

Useful flags: `--host=ADDR` (default `0.0.0.0` -- it listens on every
interface unless told otherwise), `--voice=VOICE`, `--samplerate=HZ`,
`--channels=1|2`, `--english-speed=DELTA`, `--ring-ms=N`, `--max-segment-chars=N`,
`--no-save`, `--verbose`.

| request | answer |
|---|---|
| `POST /tts` | chunked 16-bit PCM (`audio/L16;rate=48000;channels=2;bit=16`), emitted clause by clause while the text is still being synthesised |
| `POST /tts?wait=1` | one complete WAV (`audio/wav`) with a correct `Content-Length` |
| `GET /health` | `{"status":"ok","voice":"Mandarin","sample_rate":48000,"channels":2,"ring_ms":4000}` |
| `GET /voices` | `{"default":"Mandarin","voices":["Mandarin","English"]}` (probed at startup; this tree ships Mandarin data, so Cantonese and the other languages are absent -- the list shrinks to what could actually be loaded) |

The request body is the UTF-8 text, either Content-Length framed or chunked.
`?wait=1` and the stream carry the same samples -- the WAV is exactly the
44-byte RIFF header in front of the PCM. `X-Ekho-File` names the copy the
server kept under `--out-dir`, and it arrives before the audio, so a client
can note the path and keep the file instead of the stream;
`X-Ekho-Sample-Rate` and `X-Ekho-Channels` state the format of the body.

Making sound from it:

```bash
# a WAV you can play
curl -s -X POST --data-binary '你好，世界。' \
     'http://127.0.0.1:2046/tts?wait=1' -o out.wav
aplay out.wav

# the same audio as raw PCM, as it is synthesised
curl -sN -X POST --data-binary '你好，世界。' \
     http://127.0.0.1:2046/tts -o out.pcm
aplay -f S16_LE -r 48000 -c 2 -t raw out.pcm
```

Query parameters on `/tts`: `voice=NAME`, `speed=DELTA`, `pitch=DELTA`,
`volume=DELTA`, `english_speed=DELTA`, and `fmt=wav` as an alias for
`wait=1`.

The board image has no `curl`; the same two calls from there:

```bash
python3 - <<'PY'
import urllib.request
req = urllib.request.Request('http://127.0.0.1:2046/tts?wait=1',
                             data='你好，世界。'.encode('utf-8'), method='POST')
open('/tmp/out.wav', 'wb').write(urllib.request.urlopen(req, timeout=120).read())
PY
aplay /tmp/out.wav
```

Under systemd, a unit that keeps it up:

```ini
[Unit]
Description=ekho TTS server
After=network.target

[Service]
Environment=EKHO_DATA_PATH=/opt/ekho/ekho-data
Environment=ESPEAK_DATA_PATH=/opt/ekho/espeak-ng-data
ExecStart=/opt/ekho/out/ekho-server --host 127.0.0.1 --port 2046 --out-dir /var/tmp/ekho
Restart=on-failure

[Install]
WantedBy=multi-user.target
```

`--host 127.0.0.1` on purpose: this is plain HTTP with no authentication, so
do not leave it on `0.0.0.0` without a firewall in front of it.

### 3. As a resident worker: ekho-worker

Same engine, resident, behind a **pipe** instead of a socket. This is the right
shape when the caller is a process on the same box: no port, no request parser,
and -- the point of the exercise -- no per-utterance cold start.

```bash
out/ekho-worker --voice Mandarin --samplerate 48000 --channels 2 --format wav
```

It reads one UTF-8 line per utterance from stdin and writes one frame per line
to stdout:

```
4 bytes magic | 4 bytes payload length (big endian) | N bytes payload
"EKH1" audio (complete WAV, or raw PCM with --format pcm)
"EKH0" UTF-8 error text
"EKH9" UTF-8 JSON status (!ping, !info)
```

stderr carries the log only, so stdout stays a clean byte stream. Lines
starting with `!` are commands: `!ping`, `!info`, `!voice NAME`, `!speed N`,
`!pitch N`, `!volume N`, `!english-speed N`, `!quit`. Everything else is text
to speak; blank lines are ignored.

The voice pack is loaded once, at startup, and kept: a cold `ekho` run re-reads
136 MB of `.voice` files before the first syllable, and the worker pays that
once instead of per turn. `--no-warmup` skips the one-off warm-up synthesis that
initialises espeak-ng and the dictionary indexes.

From Python, `scripts/funasr_bridge.py` speaks this protocol already:

```bash
python3 scripts/funasr_bridge.py worker --exe out/ekho-worker "你好，世界。"
python3 scripts/funasr_bridge.py worker --exe out/ekho-worker --save out.wav "文本"
python3 scripts/funasr_bridge.py listen --worker out/ekho-worker   # no HTTP anywhere
```

```python
from funasr_bridge import WorkerClient
tts = WorkerClient("out/ekho-worker", ["--format", "wav"])  # pack loads once
for line in recognised:          # per turn
    payload = tts.speak(line)    # WAV bytes; play, save, or hand to a player
```

`speak_resident(text)` is the one-liner form: one worker per process, reused.

### 4. Choosing a front end at build time

Both front ends are built by default, and either can be dropped:

```bash
./configure                        # ekho, ekho-stream, ekho-server, ekho-worker
./configure --disable-http-server  # pipe only: no ekho-server
./configure --disable-worker       # HTTP only: no ekho-worker
```

Which to pick: HTTP when the caller is another machine or another language
runtime and opening a port is acceptable; the worker when the caller is a local
process that can hold a pipe open and wants the engine resident. The engine and
its data are identical either way -- `src/http_server.cpp` plus
`src/ekho_server_main.cpp` is one module, `src/ekho_worker_main.cpp` is the
other, and both link the same `libekho.a`.

### 5. Building for a board (aarch64)

`scripts/build-cross-aarch64.sh` builds the board binaries with **no root
anywhere** -- it downloads and unpacks a cross toolchain and a target
libsndfile under `$WORK` (default `~/ekho-cross-root`) and leaves the rest of
the machine alone:

```bash
scripts/build-cross-aarch64.sh                    # ekho-worker
TARGETS="ekho-worker ekho-server" scripts/build-cross-aarch64.sh
BOARD=root@192.168.1.113 scripts/build-cross-aarch64.sh
```

Three things that are easy to get wrong, and that the script exists to encode:

* **The toolchain has to match the board's glibc.** The board runs Ubuntu
  20.04 / glibc 2.31. A cross build with a current distro toolchain (gcc 15,
  glibc 2.43) links fine and then refuses to start there: `version
  'GLIBC_2.38' not found`. The script fetches focal's gcc-9 cross, whose
  sysroot tops out at GLIBC_2.30, so the binary loads.
* **libsndfile is not on the board in a usable form.** No headers, no static
  library -- only `libsndfile.so.1`. The script builds a static, WAV/PCM-only
  libsndfile for aarch64 and links it in, which also drops every codec
  dependency.
* **espeak-ng is only there as headers plus a shared library**, and that
  library itself needs `libpcaudio`/`libsonic`/`libasound`/`libpulse`, which
  the board has but a cross sysroot does not. The link is given
  `-Wl,--allow-shlib-undefined` and the board's loader resolves them at run
  time, so `ekho-worker` ends up needing exactly `libespeak-ng.so.1`,
  `libpthread`, `libm`, `libgcc_s` and `libc` -- all present there.

Deploying: `scp` on this setup does not carry the execute bit, so a fresh
binary lands non-executable:

```bash
scp ekho-worker root@192.168.1.113:/root/ekho-bench/bin/
ssh root@192.168.1.113 'chmod +x /root/ekho-bench/bin/ekho-worker'
```

Measured on the board (48 kHz stereo, the same one-line text each time):

| | time |
|---|---|
| one-shot `ekho`, warm page cache | 0.76 s per utterance |
| one-shot `ekho`, first run (cold cache) | 1.36 s |
| `ekho-worker` startup (pack load + warm-up) | 1.04 s |
| resident utterances afterwards | 0.55 s for 1.36 s of audio |

and the resident worker's WAV is **byte-identical** (same md5) to the one-shot
`ekho` output for the same text -- the pipe path changes how the engine is
driven, not what it says.

### 6. Feeding it from an ASR side

`scripts/funasr_bridge.py` wraps both ends of that HTTP service for an ASR
front end (FunASR or anything else):

```bash
python3 scripts/funasr_bridge.py "识别出的文本"                    # POST, then play
python3 scripts/funasr_bridge.py --save out.wav "文本"             # POST, then save
python3 scripts/funasr_bridge.py listen --bind 0.0.0.0 --port 2047 # the ASR side only POSTs
```

```python
import sys
sys.path.insert(0, "<this repo>/scripts")
from funasr_bridge import speak
speak("识别出的文本", server="http://127.0.0.1:2046")   # blocks until spoken
```

Two FunASR flavours exist, and the hook point differs:

* **The official package** (`pip install funasr`): `AutoModel.generate()`
  returns a list of dicts with a `"text"` field, and there is no per-result
  callback -- `progress_callback` reports progress only. The hook is
  therefore after the call:

  ```python
  from funasr import AutoModel
  res = model.generate(input=wav)        # list[dict]; res[0]["text"]
  speak(res[0]["text"])
  ```

  For a streaming model, keep the `cache` dict across calls and speak the
  text of the call that was made with `is_final=True`.

* **The ONNX runtime** (`funasr_onnx`, what the board runs): there is no
  `AutoModel` at all. The model object is called directly -- offline
  `m(wave)`, streaming `m(chunk, param_dict={"is_final": False, "cache":
  cache})` -- and the text comes back in several shapes, so normalise it
  first (the chain's own `_text_from()` already does: `str`,
  `{"preds": ...}`, `{"text": ...}`, and the offline `preds` pair whose
  first element is the whole sentence). The hook is wherever that text
  lands:

  ```python
  from funasr_onnx.paraformer_bin import Paraformer
  m = Paraformer(model_dir, batch_size=1, quantize=True, intra_op_num_threads=4)
  text = _text_from(m(wave))
  speak(text)
  ```

  The board has no `torch` and no `modelscope` -- the official package needs
  both, and its wheels are far too heavy for a 1 GB board -- so the ONNX
  runtime is the sane shape there.

  Nothing is lost by that: the graphs on the board are the same FunASR
  models, exported with `model.export(type="onnx", quantize=True)`
  (`model_quant.onnx`, plus `decoder_quant.onnx` for the online model).
  The export needs PyTorch, but it happens off the board -- the board runs
  the exported graphs through onnxruntime, whose C++ core
  (`libonnxruntime.so`) sits underneath a thin Python wrapper.

One limit to know about: the server serialises synthesis process-wide (the
engine keeps global callbacks, and espeak-ng keeps global voice state), so
one request at a time is all it really does. That is deliberate, and still
far faster than realtime.

#### Two processes, plain HTTP between them

The shape that works on a board is **two processes, not one** -- the ASR
side and the TTS side never share an address space:

```
  process 1 (Python)                      process 2 (C++)
  FunASR recognition --HTTP POST /tts-->  ekho-server :2046
        |                                       |
        |                               returns audio/wav
        v                                       |
  funasr_bridge.speak() --subprocess--> aplay --ALSA--> speaker
```

Both processes can live on the same board, and they talk plain HTTP over
loopback. That buys three things a single process cannot:

* **No binding, no ABI work.** The engine stays a C++ program; the ASR side
  stays Python. No ctypes or Cython layer, no shared library to build and
  ship, and no chance of the two runtimes disagreeing about who owns a
  callback.
* **No shared global state.** The engine keeps global callbacks and
  espeak-ng keeps global voice state; onnxruntime keeps its own sessions.
  Separate processes cannot corrupt each other.
* **No shared memory.** The 48 kHz voice pack is 136 MB of `.voice` files
  (140 MB including the shared dictionaries). It is read by the TTS process
  only, so the ASR process's memory footprint -- and the numbers measured
  for it -- stay as they are.

The TTS process is the one described in section 2 above, started once:

```bash
export EKHO_DATA_PATH=/root/ekho-bench/p48
export ESPEAK_DATA_PATH=/root/ekho-bench/espeak-ng-data
/root/ekho-bench/bin/ekho-server --host 127.0.0.1 --port 2046 --out-dir /tmp/ekho-out
```

and the ASR process only needs to POST a line of text to it, which is what
`funasr_bridge.speak()` does. `--host 127.0.0.1` keeps the service on
loopback, where it belongs: it has no authentication.

(Embedding the engine into the ASR process -- loading it in-process through
the C API in `src/ekho.h` -- is possible, but it needs a shared library
build, and it would put the engine's global state and the voice pack in
the same address space as the recogniser. Two processes and an HTTP call
are the cheaper arrangement.)

## Upstream ekho

What follows is the upstream README's own description of the project, inherited
uncut except where a pointer says otherwise:

Read <a href='https://github.com/hgneng/ekho/blob/master/INSTALL'>INSTALL</a> file if you have trouble in building.

Chinese text-to-speech engine. It's part of <a href='http://www.eguidedog.net'>eGuideDog project</a>. Here is <a href='http://www.eguidedog.net/ekho.php'>Ekho TTS's home page</a>.

This repository is forked from <a href='https://sourceforge.net/p/e-guidedog/code/HEAD/tree/eGuideDog_TTS/'>SourceForge</a> at version r2418 (and patch to r2478).

Voice files are not included. We can get them from a distribution package or <a href='https://sourceforge.net/projects/e-guidedog/files/Ekho-Voice-Data/0.2/'>Ekho Voice Data's download page</a>. To replace Mandarin voice files, we can replace the whole pinyin folder, then remove pinyin.index and pinyin.voice. Re-running ekho will generate new pinyin.index and pinyin.voice from new voice files. (In this fork, `scripts/fetch-voice.sh` supplies the 0.2 sources.)

In order to create your own voice, please refere to <a href='http://eguidedog.net/doc/doc_make_new_voice_cn.php'>如何为Ekho添加新的声音</a>.
