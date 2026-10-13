#!/bin/sh
# Spike 22g demo, played: unfinished buildings: select, queue, cancel. A zone (the
# economy scenario, blue's bot off) and a client playing blue from an input
# script (demos/33-unfinished.input) through the real controls, recorded
# from its own window; its contact sheet is the script's shots.
#
# A played demo is a real run, reproducible in outcome (the script's
# expectations), not frame for frame, and always plays: there's nothing to
# re-render from. It takes as long as it plays (about 20 s).
#
# Environment: B, OUT, SIZE, CRF, LABEL, CLIPBOARD (see 04-server-and-client.sh)
set -eu
title="Spike 22g: unfinished buildings"
about="Played through the client: a click selects an unfinished factory (its size is the target now); it takes a queue before it is done, and Cancel (N) takes it down with what was spent on it back."
work="$B/demo-unfinished"
rm -rf "$work"; mkdir -p "$work"
port=$((38000 + $$ % 2000 * 3))
"$B/rts-server" --script worlds/duel.script --port "$port" --ticks 3000 > "$work/server.out" 2>&1 &
server=$!
trap 'kill $server 2>/dev/null || true' EXIT
sleep 0.3
clip=""
[ "${CLIPBOARD:-0}" = 1 ] && clip="--clipboard"
"$B/rts" --connect "127.0.0.1:$port" --faction blue --input demos/33-unfinished.input \
	--video "$OUT.mp4" --sheet "$B/demo-$(basename "$OUT").png" --size "$SIZE" --crf "$CRF" \
	--title "$title" --about "$about" --label "$LABEL" $clip > "$work/client.out" 2>&1 || {
	cat "$work/client.out"; exit 1; }
grep -E 'FAIL|expectations held|wrote' "$work/client.out"
