#!/bin/sh
# Spike 22e demo, played: pause with a note. A zone, an agent playing red
# through `rts --agent` (here a few lines of shell; in a game, tools/agent),
# and blue's client from an input script, recorded from its window. Red
# pauses with a note; blue sees why, and resumes with P.
#
# Environment: B, OUT, SIZE, CRF, LABEL, CLIPBOARD (see 04-server-and-client.sh)
set -eu
title="Spike 22e: pause with a note"
about="Red, played by an agent, pauses and says why; blue's screen shows it, and P resumes."
work="$B/demo-pause-notes"
rm -rf "$work"; mkdir -p "$work"
port=$((39000 + $$ % 2000 * 3))
"$B/rts-server" --scenario economy --port "$port" --bot red off --bot blue off --ticks 3000 > "$work/server.out" 2>&1 &
server=$!
trap 'kill $server 2>/dev/null || true' EXIT
sleep 0.3
{ sleep 4; echo "note pause Pausing to rethink: you have twice my metal income, so I'm switching to a fast raider push before you tech up. Take a minute if you like."; sleep 12; } |
	"$B/rts" --connect "127.0.0.1:$port" --faction red --agent > "$work/agent.out" 2>&1 &
clip=""
[ "${CLIPBOARD:-0}" = 1 ] && clip="--clipboard"
"$B/rts" --connect "127.0.0.1:$port" --faction blue --input demos/31-pause-notes.input \
	--video "$OUT.mp4" --sheet "$B/demo-$(basename "$OUT").png" --size "$SIZE" --crf "$CRF" \
	--title "$title" --about "$about" --label "$LABEL" $clip > "$work/client.out" 2>&1 || {
	cat "$work/client.out"; exit 1; }
grep -E 'FAIL|expectations held|wrote' "$work/client.out"
grep -q 'red.0 paused' "$work/server.out" || { echo "demo: red never paused"; exit 1; }
grep -q 'blue.0 resumed' "$work/server.out" || { echo "demo: blue never resumed"; exit 1; }
