#!/bin/bash
# Listening samples: render a set of texts that a TTS engine is normally judged
# on to WAV files on disk, so a human can decide whether it actually sounds
# acceptable. The harnesses in this directory assert numbers; none of them can
# tell you that something is unpleasant to listen to.
#
#   scripts/make-samples.sh [outdir]        default outdir: $HOME/ekho-samples
#
# Binaries are taken from out/ (scripts/build-native.sh); override with
#   EKHO=/path/to/ekho  EKHO_STREAM=/path/to/ekho-stream
#
# Each .wav gets a matching .txt holding exactly the text that was spoken.
#
# One thing to know before reading the output: `-v English` / `-v en` used to
# reach a stub that returned silence; it synthesises through espeak-ng now (see
# README.md, "Added on top of upstream"). English words inside Chinese text go
# through the same espeak-ng path, so the group-02 files use Mandarin on purpose
# -- that is the mixed-language case a caller actually hits.
set -u

here=$(cd "$(dirname "$0")" && pwd)
src=$(cd "$here/.." && pwd)
OUT=${1:-$HOME/ekho-samples}
EKHO=${EKHO:-$src/out/ekho}
EKHO_STREAM=${EKHO_STREAM:-$src/out/ekho-stream}
: "${EKHO_DATA_PATH:=$src/ekho-data}"
export EKHO_DATA_PATH

for p in "$EKHO" "$EKHO_STREAM"; do
    [ -x "$p" ] || { echo "not executable: $p" >&2
                     echo "build first (scripts/build-native.sh) or set EKHO=/ EKHO_STREAM=/" >&2
                     exit 1; }
done

Z1="$OUT/01-zh-mandarin"      # numbers, money, units, polyphones, erhua, mixed script, prose
E="$OUT/02-en-english"        # espeak-ng through a Chinese voice (see header)
S="$OUT/03-streaming"         # one-shot vs segmented vs "as an LLM emits it"
L="$OUT/04-en-speed"          # the same sentence at six espeak rates, to pick a default
T="$OUT/05-en-tone"           # 100 wpm: six voices, plus four nearby rates
# The Cantonese group that used to sit at 02 was removed 2026-11-02 with the
# Cantonese voice data (this tree now ships the Mandarin data alone).
mkdir -p "$Z1" "$E" "$S" "$L" "$T"

# text, wav, voice, the text
gen() {
    printf '%s' "$4" > "$1"
    "$EKHO" -v "$3" -f "$1" -o "$2" >/dev/null 2>&1
    printf '  %-34s %s\n' "$(basename "$2")" "$( [ -s "$2" ] && echo ok || echo FAILED)"
}

echo "=== 01-zh-mandarin (44.1 kHz) ==="
gen "$Z1/01-numbers-and-dates.txt"  "$Z1/01-numbers-and-dates.wav"  Mandarin \
    '2026年10月16日，会议从上午9点30分开到下午3点05分，参会的有128人。'
gen "$Z1/02-money-and-percent.txt"  "$Z1/02-money-and-percent.wav"  Mandarin \
    '订单金额是1280.5元，打了8.5折，折后1088.43元，相当于省了17.2%。'
gen "$Z1/03-units-and-signs.txt"    "$Z1/03-units-and-signs.wav"    Mandarin \
    '今天气温-3.5℃，明天回升到12℃，风速每秒3.2米，文件大小是25MB。'
gen "$Z1/04-polyphone-shan-chang.txt" "$Z1/04-polyphone-shan-chang.wav" Mandarin \
    '单老师说，单于会骑马，不会骑自行车；老李的头发长得很长。'
gen "$Z1/05-polyphone-hai-de.txt"   "$Z1/05-polyphone-hai-de.wav"   Mandarin \
    '他还了钱，还想再借；这件事我可得好好想想，得空再谈。'
gen "$Z1/06-tongue-twister-sishi.txt" "$Z1/06-tongue-twister-sishi.wav" Mandarin \
    '四是四，十是十，十四是十四，四十是四十；谁能说准四十、十四、四十四，谁来试一试。'
gen "$Z1/07-erhua-and-neutral-tone.txt" "$Z1/07-erhua-and-neutral-tone.wav" Mandarin \
    '这儿有只小花鸟儿，就在那家小店儿门口，你看它多好玩儿。'
gen "$Z1/08-mixed-cn-en.txt"        "$Z1/08-mixed-cn-en.wav"        Mandarin \
    '请把报告存成 PDF，再发一份到 service@example.com，主题写 Q3 Report。'
gen "$Z1/09-service-register.txt"   "$Z1/09-service-register.wav"   Mandarin \
    '好的，正在为您查询，请稍等。已经找到三趟列车，最早的一班9点20分出发。'
gen "$Z1/10-prose-spring.txt"       "$Z1/10-prose-spring.wav"       Mandarin \
    '盼望着，盼望着，东风来了，春天的脚步近了。一切都像刚睡醒的样子，欣欣然张开了眼。山朗润起来了，水涨起来了，太阳的脸红起来了。'

echo "=== 02-en-english (44.1 kHz, Mandarin voice on ASCII text) ==="
# At the compiled defaults: 100 wpm and the eSpeak voice 'en-us+f3' (both
# picked by ear 2026-10-17; see groups 04 and 05).
gen "$E/14-harvard-sentences.txt"   "$E/14-harvard-sentences.wav"   Mandarin \
    'The birch canoe slid on the smooth planks. Glue the sheet to the dark blue background. It is easy to tell the depth of a well.'
gen "$E/15-pangram-and-twister.txt" "$E/15-pangram-and-twister.wav" Mandarin \
    'The quick brown fox jumps over the lazy dog. Peter Piper picked a peck of pickled peppers.'
gen "$E/16-english-numbers.txt"     "$E/16-english-numbers.wav"     Mandarin \
    'The meeting is on October 16th, 2026, at 9:30 in the morning. The total is one thousand two hundred eighty dollars and fifty cents.'

echo "=== 04-en-speed (the same sentence at six espeak rates) ==="
# English is read by espeak-ng, and its rate is independent of -s, which only
# moves the Chinese tempo. This fork used to land on 140 wpm by accident -- an
# explicit --english-speed was dropped before espeak existed, and initEnglish()
# then applied a delta 20 points off what anyone asked for. It is explicit now:
# EKHO_ESPEAK_RATE picks the base (words per minute, 80..450) and
# --english-speed is a percentage on top. This group is how the default was
# chosen, by ear rather than by argument: 100 wpm (2026-10-17).
HARVARD='The birch canoe slid on the smooth planks. Glue the sheet to the dark blue background. It is easy to tell the depth of a well.'
printf '%s' "$HARVARD" > "$L/text.txt"
for wpm in 80 100 120 140 160 175; do
    out="$L/en-$(printf '%03d' "$wpm")-wpm.wav"
    EKHO_ESPEAK_RATE=$wpm "$EKHO" -v Mandarin -f "$L/text.txt" -o "$out" >/dev/null 2>&1
    printf '  %-34s %s (%d wpm)\n' "$(basename "$out")" "$( [ -s "$out" ] && echo ok || echo FAILED)" "$wpm"
done

echo "=== 05-en-tone (six voices at 100 wpm + four nearby rates) ==="
# Round two of the same choice. At a slow rate eSpeak stretches every segment
# instead of resampling, so a slower voice is not simply "the same voice,
# slower" -- the timbre smears. The voice is the lever that does not touch the
# rate; EKHO_ESPEAK_RATE the other way round. The winner (2026-10-17) is now
# the compiled default: 'en-us+f3' at 100 wpm (see libekho_impl.cpp).
printf '%s' "$HARVARD" > "$T/text.txt"
i=0
for voice in en+f1 en en+f3 en-us en-us+f3 en-us+m3; do
    i=$((i+1))
    out="$T/$(printf '%02d' "$i")-voice-$voice.wav"
    "$EKHO" -v Mandarin -f "$T/text.txt" -o "$out" --english-voice="$voice" >/dev/null 2>&1
    printf '  %-34s %s (voice %s)\n' "$(basename "$out")" "$( [ -s "$out" ] && echo ok || echo FAILED)" "$voice"
done
for wpm in 110 120 130 140; do
    i=$((i+1))
    out="$T/$(printf '%02d' "$i")-rate-$wpm.wav"
    EKHO_ESPEAK_RATE=$wpm "$EKHO" -v Mandarin -f "$T/text.txt" -o "$out" >/dev/null 2>&1
    printf '  %-34s %s (%d wpm)\n' "$(basename "$out")" "$( [ -s "$out" ] && echo ok || echo FAILED)" "$wpm"
done

echo "=== 03-streaming (same text, three ways) ==="
LONG='市气象台今天上午发布消息，未来三天本市以晴到多云为主，最高气温22度，最低气温9度，早晚温差较大，请市民注意增减衣物。另外，明天早高峰期间，地铁一号线和四号线将缩短发车间隔。'
printf '%s' "$LONG" > "$S/17-long-news.txt"
"$EKHO"        -v Mandarin -f "$S/17-long-news.txt" -o "$S/17-oneshot.wav"             >/dev/null 2>&1
"$EKHO_STREAM" -v Mandarin -f "$S/17-long-news.txt" -o "$S/18-stream-segmented.wav"    >/dev/null 2>&1
"$EKHO_STREAM" -v Mandarin -f "$S/17-long-news.txt" --chunk 6 --chunk-delay 40 \
               -o "$S/19-stream-as-llm-emits.wav" >/dev/null 2>&1
"$EKHO_STREAM" -v Mandarin -f "$S/17-long-news.txt" --json > "$S/19-stats.json" 2>/dev/null

for f in 17-oneshot 18-stream-segmented 19-stream-as-llm-emits; do
    printf '  %-34s %s\n' "$f.wav" "$( [ -s "$S/$f.wav" ] && echo ok || echo FAILED)"
done
echo
echo "--- streaming stats (the numbers the ear cannot hear) ---"
sed 's/^/  /' "$S/19-stats.json" 2>/dev/null | head -20
echo
echo "--- what was written ---"
if command -v python3 >/dev/null 2>&1; then
    python3 - "$OUT" <<'PY'
import os, sys, wave
root = sys.argv[1]
for dirpath, dirnames, filenames in os.walk(root):
    dirnames.sort()
    wavs = sorted(f for f in filenames if f.endswith('.wav'))
    if not wavs:
        continue
    print(os.path.basename(dirpath) + "/")
    for f in wavs:
        p = os.path.join(dirpath, f)
        try:
            w = wave.open(p, 'rb')
            r, n, c = w.getframerate(), w.getnframes(), w.getnchannels()
            print("  %-34s %9d B  %6.3f s  %5d Hz  %dch" % (f, os.path.getsize(p), n / r, r, c))
        except Exception as e:
            print("  %-34s FAILED: %s" % (f, e))
PY
else
    find "$OUT" -name '*.wav' -printf '  %-60p %9s B\n' | sort
fi
echo
echo "samples in $OUT"
