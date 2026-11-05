#!/bin/bash
# ekho-linux: exercise ekho-server, the replacement for `ekho --server`.
#
# The properties worth testing are the ones the old protocol lacked:
#
#   * a response can start before the request body is complete
#   * the body can arrive in pieces (chunked), as an LLM emits tokens
#   * the reply is length-framed, so a client never has to guess where it ends
#   * the synthesised file is kept on the server and named in a header
#   * malformed requests get an HTTP error rather than a hang or a kill
set -u

SRC=${SRC:-/mnt/s/work/WCIS/repos/ekho-linux}
OUT=${OUT:-$SRC/out}
SIM=${SIM:-/tmp/ekho-server-test}
PORT=${PORT:-12046}
HOST=127.0.0.1
BASE="http://$HOST:$PORT"

# Overridable so a trimmed copy of the data can be regression-tested against
# the same assertions; the default is unchanged.
export EKHO_DATA_PATH="${EKHO_DATA_PATH:-$SRC/ekho-data}"
mkdir -p "$SIM"
rm -f "$SIM"/*

cat > "$SIM/zh.txt" <<'EOF'
系统启动完成。第一项，电源状态正常。第二项，存储设备已挂载，剩余空间百分之八十七。第三项，网络链路已建立，延迟二十三毫秒。现在开始播放欢迎词。任务结束。
EOF
cat > "$SIM/nums.txt" <<'EOF'
输出电压 12.5V，电流 2.4A。CPU 频率 2.4GHz。
EOF

SERVER_PID=""
cleanup() {
  if [ -n "$SERVER_PID" ] && kill -0 "$SERVER_PID" 2>/dev/null; then
    kill "$SERVER_PID" 2>/dev/null
    wait "$SERVER_PID" 2>/dev/null
  fi
}
trap cleanup EXIT

hr() { printf '\n%s\n' "=== $* ==="; }
ok()   { printf '  ok    %s\n' "$*"; }
bad()  { printf '  FAIL  %s\n' "$*"; FAILED=$((FAILED + 1)); }
FAILED=0

# ---------------------------------------------------------------------------
hr "0. start the server"
# ---------------------------------------------------------------------------
"$OUT/ekho-server" --host "$HOST" --port "$PORT" --out-dir "$SIM" --verbose \
  > "$SIM/server.log" 2>&1 &
SERVER_PID=$!
sed 's/^/  /' "$SIM/server.log"

for i in $(seq 1 40); do
  if curl -sf -o /dev/null "$BASE/health" 2>/dev/null; then break; fi
  if ! kill -0 "$SERVER_PID" 2>/dev/null; then
    echo "  server died:"; cat "$SIM/server.log" | sed 's/^/    /'; exit 1
  fi
  sleep 0.25
done
if curl -sf -o /dev/null "$BASE/health"; then ok "listening on $BASE"; else bad "no /health after 10s"; fi

# ---------------------------------------------------------------------------
hr "1. GET /health and /voices"
# ---------------------------------------------------------------------------
printf '  health: %s\n' "$(curl -s "$BASE/health")"
printf '  voices: %s\n' "$(curl -s "$BASE/voices")"

# ---------------------------------------------------------------------------
hr "2. POST /tts?wait=1 vs the one-shot CLI"
# ---------------------------------------------------------------------------
# The reference is the plain CLI, which is the old behaviour. The two are not
# sample-identical and should not be: the streaming path cuts at clause
# boundaries, so each clause is synthesised as its own utterance and the pause
# between clauses is whatever the overlap logic inserts at the cut, not what one
# long utterance would have produced. What must hold is that no audio is lost
# and the tempo is the same -- a duration in the same ballpark. Byte-identical
# output across repeated requests is checked separately in section 7.
"$OUT/ekho" -v Mandarin -f "$SIM/zh.txt" -o "$SIM/ref.wav" >/dev/null 2>&1
curl -s -X POST --data-binary "@$SIM/zh.txt" "$BASE/tts?wait=1" -o "$SIM/wait.wav"
refb=$(stat -c%s "$SIM/ref.wav"); waitb=$(stat -c%s "$SIM/wait.wav")
# Duration from the WAV byte rate (offset 28), which already folds in the sample
# rate, the channel count and the sample width. Assuming "2 bytes per frame,
# 44.1 kHz" here is what made the old numbers disagree with the driver's JSON as
# soon as the default format moved to the board's 48 kHz / 2ch.
wavsec() { awk -v b="$(stat -c%s "$1")" \
                -v r="$(od -An -tu4 -j 28 -N 4 "$1" | tr -d ' ')" \
                'BEGIN{printf "%.3f", (b-44)/(r ? r : 176400)}'; }
wavrate() { od -An -tu4 -j 24 -N 4 "$1" | tr -d ' '; }
wavch()   { od -An -tu2 -j 22 -N 2 "$1" | tr -d ' '; }
refs=$(wavsec "$SIM/ref.wav"); waits=$(wavsec "$SIM/wait.wav")
printf '  one-shot CLI  %9s B  %8.3f s\n  wait=1        %9s B  %8.3f s\n  format        %9s Hz  %s ch\n' \
       "$refb" "$refs" "$waitb" "$waits" \
       "$(wavrate "$SIM/ref.wav")" "$(wavch "$SIM/ref.wav")"
if awk -v a="$refs" -v b="$waits" 'BEGIN{d=(a>b?a-b:b-a); exit !(d <= a*0.10)}'; then
  ok "duration within 10% of the one-shot CLI"
else
  bad "duration differs from the one-shot CLI by more than 10%"
fi

# ---------------------------------------------------------------------------
hr "3. streaming POST /tts: chunked PCM, length-framed by the transfer coding"
# ---------------------------------------------------------------------------
curl -s -D "$SIM/hdr.txt" -X POST --data-binary "@$SIM/zh.txt" "$BASE/tts" -o "$SIM/stream.pcm"
grep -iE '^(HTTP/|content-type|transfer-encoding|x-ekho)' "$SIM/hdr.txt" | sed 's/^/  /'
# The streaming format must describe the same audio the WAV contains; a stale
# ";channels=1" here is how a stereo stream gets played as half-speed noise.
refr=$(wavrate "$SIM/ref.wav"); refc=$(wavch "$SIM/ref.wav")
ct=$(sed -n 's/^[Cc]ontent-[Tt]ype: *//p' "$SIM/hdr.txt" | tr -d '\r')
case "$ct" in
  *"rate=$refr"*"channels=$refc"*)
    ok "Content-Type advertises the WAV format ($refr Hz / $refc ch)" ;;
  *)
    bad "Content-Type '$ct' does not match ${refr} Hz / ${refc} ch" ;;
esac
pcmb=$(stat -c%s "$SIM/stream.pcm")
# same audio as ?wait=1, minus its 44-byte WAV header
wavb=$((waitb - 44))
printf '  streamed   %9s B\n  expected   %9s B (wait=1 minus the WAV header)\n' "$pcmb" "$wavb"
if [ "$pcmb" = "$wavb" ]; then ok "streamed PCM length matches"; else bad "streamed PCM length differs"; fi

srvfile=$(sed -n 's/^[Xx]-[Ee]kho-[Ff]ile: *//p' "$SIM/hdr.txt" | tr -d '\r')
if [ -n "$srvfile" ] && [ -f "$srvfile" ]; then
  ok "server kept the audio: $srvfile ($(stat -c%s "$srvfile") B)"
else
  bad "X-Ekho-File missing or not on disk: '$srvfile'"
fi

# ---------------------------------------------------------------------------
hr "4. chunked request body, as an LLM emits tokens"
# ---------------------------------------------------------------------------
# No Content-Length: the client streams. `-T` uploads stdin with chunked framing
# as the bytes arrive; --data-binary would buffer the whole file first.
curl -s -X POST -T "$SIM/zh.txt" "$BASE/tts" -o "$SIM/chunked.pcm"
cb=$(stat -c%s "$SIM/chunked.pcm")
printf '  chunked   %9s B\n' "$cb"
if [ "$cb" = "$pcmb" ]; then
  ok "chunked body produces the same audio as Content-Length"
else
  bad "chunked body differs ($cb vs $pcmb)"
fi

# ---------------------------------------------------------------------------
hr "5. the whole point: audio before the text has finished arriving"
# ---------------------------------------------------------------------------
# Feed the text slowly through a pipe. If the server were one-shot, the first
# response byte could not precede the last request byte. curl has to *stream*
# the body for this to mean anything: `--data-binary @-` buffers all of stdin
# first (it needs the length), so this uses `-T -`, which uploads with chunked
# framing as bytes arrive.
slowfeed() {
  # nine bytes at a time (three CJK characters) every 250 ms
  local total off
  total=$(stat -c%s "$SIM/zh.txt")
  off=0
  while [ "$off" -lt "$total" ]; do
    dd if="$SIM/zh.txt" bs=1 skip="$off" count=9 status=none
    off=$((off + 9))
    sleep 0.25
  done
}

start=$(date +%s.%N)
slowfeed | curl -s -X POST -H 'Transfer-Encoding: chunked' -T - "$BASE/tts" \
    -o "$SIM/slow.pcm" -w '%{time_starttransfer} %{time_total}\n' \
    > "$SIM/timing.txt"
end=$(date +%s.%N)
fb=$(awk '{print $1}' "$SIM/timing.txt")
tt=$(awk '{print $2}' "$SIM/timing.txt")
wall=$(awk -v a="$start" -v b="$end" 'BEGIN{printf "%.3f", b-a}')
printf '  feeding took %ss, first response byte at %ss, response finished at %ss\n' \
       "$wall" "$fb" "$tt"
if [ -n "$fb" ] && awk -v a="$fb" -v b="$wall" 'BEGIN{exit !(a < b*0.6)}'; then
  ok "first byte at ${fb}s while the body was still arriving (${wall}s total) -> streamed"
else
  bad "response did not start early (first=$fb feed=$wall)"
fi
slowb=$(stat -c%s "$SIM/slow.pcm")
if awk -v a="$slowb" -v b="$pcmb" 'BEGIN{d=(a>b?a-b:b-a); exit !(d <= b*0.10)}'; then
  ok "same duration within 10% ($slowb vs $pcmb bytes)"
else
  # Not byte-identical, and should not be: the text is cut at whatever
  # boundaries have arrived by the time a clause completes, so the pauses
  # between clauses land in different places. What must not happen is audio
  # going missing.
  bad "slow arrival duration differs by more than 10% ($slowb vs $pcmb)"
fi

# ---------------------------------------------------------------------------
hr "6. error handling"
# ---------------------------------------------------------------------------
# Raw request helper: some malformed requests cannot be expressed with curl,
# which always adds framing of its own.
raw_request() {
  local line
  exec 4<>"/dev/tcp/$HOST/$PORT" || { echo "no-connect"; return; }
  printf '%b' "$1" >&4
  read -r -t 5 line <&4
  exec 4<&- 2>/dev/null
  exec 4>&- 2>/dev/null
  printf '%s' "$line" | tr -d '\r'
}
code() { curl -s -o /dev/null -w '%{http_code}' "$@"; }

c=$(code -X POST --data-binary "@$SIM/zh.txt" "$BASE/tts?voice=Klingon")
printf '  unknown voice          -> %s\n' "$c"
[ "$c" = 400 ] && ok "unknown voice rejected (no silent fallback)" || bad "unknown voice gave $c"

# Content-Length: 0 is framing, so this must reach the handler and be rejected as
# empty text rather than refused as unframed.
c=$(code -X POST -H 'Content-Length: 0' "$BASE/tts?wait=1")
printf '  empty body (wait=1)    -> %s\n' "$c"
[ "$c" = 400 ] && ok "empty body refused with 400" || bad "empty body gave $c"

c=$(raw_request "POST /tts HTTP/1.1\r\nHost: $HOST\r\nConnection: close\r\n\r\n")
printf '  no body framing        -> %s\n' "$c"
case "$c" in *411*) ok "unframed POST refused with 411" ;; *) bad "unframed POST gave '$c'" ;; esac

c=$(code "$BASE/nope")
printf '  unknown path           -> %s\n' "$c"
[ "$c" = 404 ] && ok "unknown path 404" || bad "unknown path gave $c"

c=$(code "$BASE/health")
[ "$c" = 200 ] && ok "server still healthy after the errors" || bad "server unhealthy: $c"

# ---------------------------------------------------------------------------
hr "7. several requests in a row (serialised synthesis)"
# ---------------------------------------------------------------------------
# Pure Chinese must be byte-identical request after request. This is a real
# property with real failure modes, not a nicety: the engine is shared across
# connections, the resampler keeps the samples it could not convert yet, and any
# of that leaking across requests makes the same text produce different audio
# depending on what was asked for before it. That is exactly the bug
# Ekho::drainAudio() exists to prevent, so this is the test that would catch a
# regression in it.
for i in 1 2 3; do
  curl -s -X POST --data-binary "@$SIM/zh.txt" "$BASE/tts?wait=1" -o "$SIM/seq$i.wav"
done
printf '  zh: %s %s %s bytes\n' "$(stat -c%s "$SIM/seq1.wav")" \
       "$(stat -c%s "$SIM/seq2.wav")" "$(stat -c%s "$SIM/seq3.wav")"
if [ "$(md5sum < "$SIM/seq1.wav")" = "$(md5sum < "$SIM/seq2.wav")" ] &&
   [ "$(md5sum < "$SIM/seq2.wav")" = "$(md5sum < "$SIM/seq3.wav")" ]; then
  ok "Chinese is byte-identical across requests"
else
  bad "Chinese differed between identical requests"
fi

# Text containing ASCII is a different story, and the reason is worth writing
# down: espeak-ng is not reproducible by itself. A C program that calls
# espeak_Initialize() once and then espeak_Synth() with the same sentence five
# times gets five different lengths and five different waveforms, and
# espeak_Terminate() + a fresh espeak_Initialize() before every call does not
# help -- espeak consumes a continuing internal pseudo-random sequence while
# building prosody, so its output depends on how many sentences it has already
# spoken in this process. Nothing in this fork can make that deterministic; all
# it can do is keep it from spreading, so the check here is a tolerance.
#
# This is why section 7 above uses Chinese: it is the part of the pipeline that
# can be, and now is, exact.
for i in 1 2 3; do
  curl -s -X POST --data-binary "@$SIM/nums.txt" "$BASE/tts?wait=1" -o "$SIM/mix$i.wav"
done
mix1=$(stat -c%s "$SIM/mix1.wav"); mix2=$(stat -c%s "$SIM/mix2.wav")
mix3=$(stat -c%s "$SIM/mix3.wav")
printf '  zh+English: %s %s %s bytes\n' "$mix1" "$mix2" "$mix3"
if awk -v a="$mix1" -v b="$mix2" -v c="$mix3" \
     'BEGIN{exit !((a-b<0?b-a:a-b) < a*0.02 && (a-c<0?c-a:a-c) < a*0.02)}'; then
  ok "Chinese/English text stays within 2% (espeak-ng's own variance)"
else
  bad "Chinese/English text varies by more than 2%"
fi

# ---------------------------------------------------------------------------
hr "8. server log"
# ---------------------------------------------------------------------------
tail -14 "$SIM/server.log" | sed 's/^/  /'

printf '\n'
if [ "$FAILED" = 0 ]; then
  echo "server tests: all passed"
else
  echo "server tests: $FAILED FAILED"
fi
exit "$FAILED"
