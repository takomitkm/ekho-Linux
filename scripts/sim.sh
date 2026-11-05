#!/bin/bash
# ekho-linux simulation harness.
#
# Measures the properties the streaming design is supposed to have, using the
# same definitions as the board-side TTS benchmark:
#
#   first_ms  append() -> first PCM a reader could take
#   gen_ms    total time inside synth4()
#   rtf       audio seconds per synthesis second
#   dropped   16-bit frames the PCM ring had to discard
#
# Sections 1-2 are correctness (nothing lost, nothing varying), sections 3-5 are
# the streaming properties, 6-8 the other back ends. Measurement tables are
# printed; anything that is a claim about correctness also asserts, and the
# script exits non-zero if any assertion fails.
set -u

SRC=${SRC:-/mnt/s/work/WCIS/repos/ekho-linux}
OUT=${OUT:-$SRC/out}
SIM=${SIM:-/tmp/ekho-sim}
mkdir -p "$SIM"

# Overridable so a trimmed copy of the data can be regression-tested against
# the same assertions; the default is unchanged.
export EKHO_DATA_PATH="${EKHO_DATA_PATH:-$SRC/ekho-data}"

cat > "$SIM/zh_long.txt" <<'EOF'
你好，这是在学校机器上用 MinGW 编译的 Ekho 十一点零版本测试。语音合成正常，这是普通话。
EOF
cat > "$SIM/zh_bytes.txt" <<'EOF'
你好，这是在学校机器上编译的测试文本。语音合成正常，这是普通话。
EOF
cat > "$SIM/zh_stream.txt" <<'EOF'
系统启动完成。第一项，电源状态正常。第二项，存储设备已挂载，剩余空间百分之八十七。第三项，网络链路已建立，延迟二十三毫秒。现在开始播放欢迎词。任务结束。
EOF
cat > "$SIM/en_mixed.txt" <<'EOF'
你好，这是用 MinGW 编译的 Ekho 十一点零版本测试。Windows 和 MSYS2 都可以正常工作。
EOF
cat > "$SIM/nums.txt" <<'EOF'
输出电压 12.5V，电流 2.4A，效率提升到 93.7%。CPU 频率 2.4GHz，内存 16GB。
EOF

hr() { printf '\n%s\n' "=== $* ==="; }
ok()  { printf '  ok    %s\n' "$*"; }
bad() { printf '  FAIL  %s\n' "$*"; FAILED=$((FAILED + 1)); }
FAILED=0

# Pull a field out of the driver's JSON without needing jq.
field() { sed -n "s/.*\"$2\":\([^,}]*\).*/\1/p" <<<"$1"; }

# Duration of a WAV in seconds, from the file's own header. The byte rate at
# offset 28 already folds in the sample rate, the channel count and the sample
# width, so it stays right now that the default output is 48 kHz / 2ch rather
# than the voice pack's 44.1 kHz / mono. `$2` overrides the byte rate.
dur() { awk -v b="$(stat -c%s "$1" 2>/dev/null || echo 0)" \
             -v r="${2:-$(od -An -tu4 -j 28 -N 4 "$1" 2>/dev/null | tr -d ' ')}" \
             'BEGIN{printf "%.3f", (b-44)/(r ? r : 176400)}'; }

# |a-b| as a fraction of a, tested against a limit.
within() { awk -v a="$1" -v b="$2" -v lim="$3" \
  'BEGIN{d=(a>b?a-b:b-a); exit !(d <= a*lim)}'; }

md5of() { md5sum "$1" 2>/dev/null | cut -c1-16; }

hr "1. correctness: streaming must not lose audio"
# Same text, same voice, three ways. The one-shot CLI is the reference; the
# streaming path splits at clause boundaries, which shifts the pauses at each
# join but must not drop syllables, so the durations should be within a few
# percent. `--no-segment` isolates the synth4() callback path from the
# segmentation on top of it.
#
# The reference text has ASCII in it ("MinGW"), so its byte count wobbles by a
# few bytes from run to run: espeak-ng is not reproducible (see the note under
# section 7). That is why the check below is a tolerance and not an equality --
# pure-Chinese text round-trips byte-identically, which section 2 covers.
#
# Mandarin only, since 2026-11-02: this tree ships the Mandarin voice data
# alone (see README, "This fork: Linux only" -- the other voices were removed
# on request). The loop stays a loop so a data directory that still carries
# other voices can be put through the same assertions.
for v in Mandarin; do
  echo "  --- $v ---"
  "$OUT/ekho" -v "$v" -f "$SIM/zh_long.txt" -o "$SIM/k_oneshot.wav" >/dev/null 2>&1
  printf '    %-38s %9s B  %8s s\n' "one-shot CLI (reference)" \
    "$(stat -c%s "$SIM/k_oneshot.wav")" "$(dur "$SIM/k_oneshot.wav")"
  "$OUT/ekho-stream" -v "$v" -f "$SIM/zh_long.txt" --no-segment --json \
    -o "$SIM/k_whole.wav" >/dev/null 2>&1
  printf '    %-38s %9s B  %8s s\n' "streaming, one synth4 of everything" \
    "$(stat -c%s "$SIM/k_whole.wav")" "$(dur "$SIM/k_whole.wav")"
  "$OUT/ekho-stream" -v "$v" -f "$SIM/zh_long.txt" --json \
    -o "$SIM/k_seg.wav" >/dev/null 2>&1
  printf '    %-38s %9s B  %8s s\n' "streaming, clause-segmented" \
    "$(stat -c%s "$SIM/k_seg.wav")" "$(dur "$SIM/k_seg.wav")"
  ref=$(stat -c%s "$SIM/k_oneshot.wav")
  for f in k_whole k_seg; do
    if within "$ref" "$(stat -c%s "$SIM/$f.wav")" 0.15; then
      ok "$v: $f within 15% of the one-shot CLI"
    else
      bad "$v: $f is $(stat -c%s "$SIM/$f.wav") B against $ref B"
    fi
  done
done

hr "1b. byte-chunked input must not lose characters"
# A byte-chunked caller splits multi-byte characters; the session holds the
# partial sequence over to the next call. Chunk sizes 1/2/3/4/6/7 all land
# inside a 3-byte CJK character at different phases, and every one of them must
# produce exactly the same audio.
#
# CJK-only on purpose. Run against the mixed zh_long.txt this used to pass, but
# only because the old default voice (en+f1) happened to be byte-reproducible:
# espeak-ng's output for text with ASCII in it varies run to run for the
# current default voice (en-us+f3), so comparing bytes across two invocations
# would be measuring espeak, not the hold-back. Section 7 has the same caveat.
printf '  %-8s %10s  %s\n' chunk_B bytes md5
ref11=""
chunkbad=0
for c in 1 2 3 4 6 7; do
  "$OUT/ekho-stream" -v Mandarin -f "$SIM/zh_bytes.txt" --chunk "$c" --json \
    -o "$SIM/k_c$c.wav" >/dev/null 2>&1
  b=$(stat -c%s "$SIM/k_c$c.wav")
  h=$(md5of "$SIM/k_c$c.wav")
  printf '  %-8s %10s  %s\n' "$c" "$b" "$h"
  if [ -z "$ref11" ]; then
    ref11="$h"; refb="$b"
  elif [ "$h" != "$ref11" ]; then
    bad "chunk size $c gave different audio ($h vs $ref11)"
    chunkbad=1
  fi
done
if [ "$chunkbad" = 0 ]; then
  ok "all chunk sizes produced byte-identical audio ($refb B)"
fi

hr "2. determinism (same input three times)"
# Pure Chinese, so this is a real determinism claim and not a tolerance. See the
# note under section 7 about why text with ASCII in it cannot be held to this.
det_ref=""
for i in 1 2 3; do
  "$OUT/ekho-stream" -v Mandarin -f "$SIM/zh_stream.txt" --chunk 6 --chunk-delay 5 \
    --json -o "$SIM/det_$i.wav" >/dev/null 2>&1
  h=$(md5of "$SIM/det_$i.wav")
  printf '  run %d: %s  %s B\n' "$i" "$h" "$(stat -c%s "$SIM/det_$i.wav")"
  if [ -z "$det_ref" ]; then det_ref="$h"; elif [ "$h" != "$det_ref" ]; then det_bad=1; fi
done
if [ "${det_bad:-0}" = 0 ]; then
  ok "three runs byte-identical"
else
  bad "runs differed"
fi

hr "3. the streaming property: text arriving one token at a time"
# This is the case the design exists for. `--chunk-delay` stands in for an LLM
# emitting tokens; first_ms is then bounded by how long the first clause takes
# to arrive, and gen_ms shows how much redundant work each strategy does.
echo "  arrival: 12 bytes every 30 ms (about four Chinese characters)"
printf '  %-18s %10s %10s %10s %9s %8s\n' mode first_ms gen_ms total_ms rtf dropped
for mode in "--no-segment" ""; do
  label=$([ -z "$mode" ] && echo "clause-segmented" || echo "whole-buffer")
  r=$("$OUT/ekho-stream" -v Mandarin -f "$SIM/zh_stream.txt" \
      --chunk 12 --chunk-delay 30 --json $mode -o "$SIM/arr_$label.wav" 2>/dev/null)
  printf '  %-18s %10s %10s %10s %9s %8s\n' "$label" \
    "$(field "$r" first_ms)" "$(field "$r" gen_ms)" "$(field "$r" total_ms)" \
    "$(field "$r" rtf)" "$(field "$r" dropped_frames)"
  d=$(field "$r" dropped_frames)
  [ "$d" = 0 ] && ok "$label: dropped nothing" || bad "$label: dropped $d frames"
done

hr "4. first-chunk latency against chunk size (no arrival delay)"
printf '  %-8s %10s %10s %10s %8s\n' chunk_B first_ms gen_ms segments dropped
for c in 6 12 24 48 0; do
  lbl=$([ "$c" = 0 ] && echo all || echo "$c")
  r=$("$OUT/ekho-stream" -v Mandarin -f "$SIM/zh_stream.txt" --chunk "$c" --json \
      -o "$SIM/cs.wav" 2>/dev/null)
  printf '  %-8s %10s %10s %10s %8s\n' "$lbl" "$(field "$r" first_ms)" \
    "$(field "$r" gen_ms)" "$(field "$r" segments)" "$(field "$r" dropped_frames)"
done

hr "5. backpressure: the ring size must not change the audio"
# The producer blocks instead of discarding audio, so a ring smaller than one
# segment must still lose zero frames -- it just makes synthesis slower. The
# segment cap is pinned: left to itself the driver derives it from the ring
# size, which would change the clause cuts and with them the audio, and then
# this test could not tell a backpressure bug from a segmentation change.
printf '  %-10s %9s %10s %8s %8s  %s\n' ring_ms segments gen_ms rtf dropped md5
bp_ref=""
bp_failed_before=$FAILED
for ms in 100 250 1000 4000 60000; do
  r=$("$OUT/ekho-stream" -v Mandarin -f "$SIM/zh_stream.txt" --ring-ms "$ms" \
      --max-segment-chars 24 --json -o "$SIM/bp_$ms.wav" 2>/dev/null)
  h=$(md5of "$SIM/bp_$ms.wav")
  printf '  %-10s %9s %10s %8s %8s  %s\n' "$ms" "$(field "$r" segments)" \
    "$(field "$r" gen_ms)" "$(field "$r" rtf)" "$(field "$r" dropped_frames)" "$h"
  d=$(field "$r" dropped_frames)
  [ "$d" = 0 ] || bad "ring ${ms}ms dropped $d frames"
  if [ -z "$bp_ref" ]; then
    bp_ref="$h"
  elif [ "$h" != "$bp_ref" ]; then
    bad "ring ${ms}ms produced different audio ($h vs $bp_ref)"
  fi
done
if [ "$FAILED" = "$bp_failed_before" ]; then
  ok "nothing dropped and identical audio at every ring size (100 ms .. 60 s)"
fi

hr "6. text normalisation"
# Numbers and units are spelled out. That is strictly more syllables, so the
# normalised reading must be longer than the raw one.
r=$("$OUT/ekho-stream" -v Mandarin -f "$SIM/nums.txt" --json \
    -o "$SIM/nums.wav" 2>/dev/null)
echo "  input : $(cat "$SIM/nums.txt")"
n=$(stat -c%s "$SIM/nums.wav")
"$OUT/ekho-stream" -v Mandarin -f "$SIM/nums.txt" --no-normalize --json \
  -o "$SIM/nums_raw.wav" >/dev/null 2>&1
raw=$(stat -c%s "$SIM/nums_raw.wav")
printf '  audio : normalised %s B  vs  raw %s B  (+%s%%)\n' "$n" "$raw" \
  "$(( (n - raw) * 100 / raw ))"
if [ "$n" -gt "$raw" ]; then
  ok "normalisation lengthened the reading (numbers spoken out)"
else
  bad "normalised output ($n B) is not longer than raw ($raw B)"
fi

hr "7. English via espeak-ng"
r=$("$OUT/ekho-stream" -v Mandarin -f "$SIM/en_mixed.txt" --json \
    -o "$SIM/en.wav" 2>&1)
echo "  $r"
if [ -s "$SIM/en.wav" ]; then
  ok "mixed Chinese/English produced $(stat -c%s "$SIM/en.wav") B"
else
  bad "mixed Chinese/English produced no audio"
fi
# Note: this section deliberately does not assert byte-identity. espeak-ng is
# not reproducible on its own -- a C program calling espeak_Synth() five times
# with the same sentence gets five different lengths and five different
# waveforms, and re-initialising espeak between calls does not help, because it
# draws on a continuing internal pseudo-random sequence while building prosody.
# (Observed 2026-10-17: with the current default voice en-us+f3, no two
# invocations of the same command produce the same bytes; with the old default
# en+f1 they did, which is why some earlier byte comparisons passed.) Chinese
# never touches espeak and is exact (section 2); text containing ASCII
# inherits espeak's variance.

hr "8. peak RSS"
for spec in "ekho|-v Mandarin -f $SIM/zh_long.txt -o $SIM/rss.wav" \
            "ekho-stream|-v Mandarin -f $SIM/zh_stream.txt --json -o $SIM/rss2.wav" \
            "ekho-stream|-v Mandarin -f $SIM/zh_stream.txt --chunk 12 --chunk-delay 30 -o $SIM/rss3.wav"; do
  prog=${spec%%|*}; args=${spec#*|}
  kb=$(/usr/bin/time -f '%M' -o /tmp/rss.txt "$OUT/$prog" $args >/dev/null 2>&1; cat /tmp/rss.txt)
  printf '  %-14s %8s KB\n' "$prog" "$kb"
done

hr "9. a lone letter keeps its duration"
# The 26 alphabet/<letter>.wav recordings are 16 kHz upstream and get spliced
# into a stream running at the *pack* rate. Copied verbatim they played 3x too
# fast on the 48 kHz pack (2.76x on the 44.1 kHz one); PhoneticSymbol::
# readSndfile() now resamples them to the pack rate, and the shipped files are
# converted to 48 kHz on top of that. k.wav is 0.292 s of audio and the Mandarin
# tempo divides by 2.39, so a lone K belongs at ~0.110 s at any output rate --
# it used to be 0.041 s here, which is what this section pins down.
secs_of() {
  awk -v b="$(stat -c%s "$1")" \
      -v r="$(od -An -tu4 -j 28 -N 4 "$1" | tr -d ' ')" \
      'BEGIN{printf "%.4f", (b-44)/(r ? r : 176400)}'
}
for spec in "48000|2" "44100|1"; do
  r=${spec%%|*}; c=${spec#*|}
  "$OUT/ekho" -v Mandarin --samplerate="$r" --channels="$c" K \
    -o "$SIM/letter_$r.wav" >/dev/null 2>&1
  d=$(secs_of "$SIM/letter_$r.wav")
  printf '  %5s Hz / %sch   K = %s s\n' "$r" "$c" "$d"
  if awk -v d="$d" 'BEGIN{exit !(d > 0.095 && d < 0.125)}'; then
    ok "K at $r Hz is $d s"
  else
    bad "K at $r Hz is $d s, expected ~0.110 s (resampled away?)"
  fi
done

printf '\n'
if [ "$FAILED" = 0 ]; then
  echo "sim: all assertions passed"
else
  echo "sim: $FAILED FAILED"
fi
exit "$FAILED"
