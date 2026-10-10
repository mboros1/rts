#!/bin/sh
# Spike 18 demo, played: commanders. A zone where each side starts with only
# a commander and a headquarters (commanders on, blue's bot off), and a
# client playing blue from demos/21-commanders.input: the commander builds
# the first factory, the factory an engineer, and the commander warps home
# beside the headquarters. Recorded from the client's window.
#
# Played demos always play (see 20-command-grid.sh): about 40 s.
#
# Environment: B, OUT, SIZE, CRF, LABEL, CLIPBOARD (see 04-server-and-client.sh)
set -eu
title="Spike 18: commanders"
about="Each player starts with only a commander: it builds the first factory, then warps home to the headquarters (blue starts with some generators here, so it can pay for the warp)."

work="$B/demo-commanders"
rm -rf "$work"; mkdir -p "$work"
# Setup: blue's generators, so the warp's 5000 energy comes in the time the demo has.
printf 'scenario economy\ncommanders on\neconomy home\nbot blue off\nat 0 spawn blue generator 12 at 50,200 radius 12\n' > "$work/zone.script"
port=$((39000 + $$ % 2000 * 3))
"$B/rts-server" --script "$work/zone.script" --port "$port" --ticks 3000 > "$work/server.out" 2>&1 &
server=$!
trap 'kill $server 2>/dev/null || true' EXIT
sleep 0.3
clip=""
[ "${CLIPBOARD:-0}" = 1 ] && clip="--clipboard"
"$B/rts" --connect "127.0.0.1:$port" --faction blue --input demos/21-commanders.input \
	--video "$OUT.mp4" --sheet "$B/demo-$(basename "$OUT").png" --size "$SIZE" --crf "$CRF" \
	--title "$title" --about "$about" --label "$LABEL" $clip > "$work/client.out" 2>&1 || {
	cat "$work/client.out"; exit 1; }
grep -E 'FAIL|expectations held|wrote' "$work/client.out"
