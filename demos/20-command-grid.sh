#!/bin/sh
# Spike 17 demo, played: the command grid as a person uses it. A zone (the
# economy scenario, blue's bot off) and a client playing blue from an input
# script (demos/20-command-grid.input) through the real controls, recorded
# from its own window; its contact sheet is the script's shots.
#
# A played demo is a real run, reproducible in outcome (the script's
# expectations), not frame for frame, and always plays: there's nothing to
# re-render from. It takes as long as it plays (about 20 s).
#
# Environment: B, OUT, SIZE, CRF, LABEL, CLIPBOARD (see 04-server-and-client.sh)
set -eu
title="Spike 17: the command grid"
about="Played through the client: select, place from the build tabs, narrow with a tile, queue, repeat, move."
work="$B/demo-command-grid"
rm -rf "$work"; mkdir -p "$work"
port=$((38000 + $$ % 2000 * 3))
"$B/rts-server" --scenario economy --port "$port" --bot blue off --ticks 1800 > "$work/server.out" 2>&1 &
server=$!
trap 'kill $server 2>/dev/null || true' EXIT
sleep 0.3
clip=""
[ "${CLIPBOARD:-0}" = 1 ] && clip="--clipboard"
"$B/rts" --connect "127.0.0.1:$port" --faction blue --input demos/20-command-grid.input \
	--video "$OUT.mp4" --sheet "$B/demo-$(basename "$OUT").png" --size "$SIZE" --crf "$CRF" \
	--title "$title" --about "$about" --label "$LABEL" $clip > "$work/client.out" 2>&1 || {
	cat "$work/client.out"; exit 1; }
grep -E 'FAIL|expectations held|wrote' "$work/client.out"
