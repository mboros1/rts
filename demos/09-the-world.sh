#!/bin/sh
# Spike 7 demo: the game, through `rts world`. A new world from
# worlds/three.world: three zones on a line, blue's home in the west and red's
# in the east, each its own server process, with a coordinator between them.
# A player (the game's bot, playing through the client exactly as a person
# would) opens the strategic map, launches into west and builds, goes back to
# the map, then launches into the middle. The world runs at 4x so the
# economy moves.
#
# A real run over sockets (like 04, 05 and 07): reproducible in outcome (the
# checks below), and its clip re-renders from the netlog (RENDER_ONLY=1).
#
# Environment: B, OUT, SIZE, CRF, LABEL, CLIPBOARD (see 04-server-and-client.sh)
set -eu
title="Spike 7: the world"
about="Three zones, each a server; a coordinator; the strategic map. A player launches into west, builds, and moves on."
work="$B/demo-world"
mkdir -p "$work"

if [ "${RENDER_ONLY:-0}" != 1 ] || [ ! -f "$work/journey.netlog" ]; then
	port=$((20000 + $$ % 15000))
	sed "s/^port .*/port $port/" worlds/three.world > "$work/three.world"
	world="$B/rts world --dir $work/w"
	$world --new "$work/three.world" --headless --speed 4 > "$work/start.txt"
	sleep 2
	"$B/rts" --coord "127.0.0.1:$port" --headless --faction blue --bot \
		--journey map:3,west:22,map:4,middle:14,map:3 \
		--netlog "$work/journey.netlog" --stats "$work/journey.txt" > "$work/journey.out" 2>&1
	$world --status > "$work/status.txt"
	$world --stop > /dev/null

	fail() { echo "demo: $1"; cat "$work/journey.out"; exit 1; }
	grep -q '^zones_visited 2$' "$work/journey.txt" || fail "the player didn't visit two zones"
	grep -q '^digest_mismatches 0$' "$work/journey.txt" || fail "faction hash mismatch"
	[ "$(grep -c ' up ' "$work/status.txt")" = 3 ] || fail "not every zone was up: $(cat "$work/status.txt")"
fi

clip="$B/rts clip --netlog $work/journey.netlog \
	--title \"$title\" --about \"$about\" --label \"$LABEL\" \
	--out $OUT.mp4 --sheet $B/demo-$(basename "$OUT").png --size $SIZE --crf $CRF"
[ "${CLIPBOARD:-0}" = 1 ] && clip="$clip --clipboard"
eval "$clip"
