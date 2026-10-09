#!/bin/sh
# Spike 16 demo: containers. worlds/teams.world (blue three players, red
# two), all bots: a bot whose home store is full builds containers (trucks
# of metal and energy) and ships the full ones to a front zone where it's
# short; there they unload into its store, and empties come home. A
# spectator watches the map at 15x, then blue's home, where trucks fill and
# leave, then the north front, where they arrive.
#
# A real run over sockets; its clip re-renders from the netlog (RENDER_ONLY=1).
#
# Environment: B, OUT, SIZE, CRF, LABEL, CLIPBOARD (see 04-server-and-client.sh)
set -eu
title="Spike 16: container logistics"
about="Storage that moves: trucks fill at blue's home and cross to the fronts, where they unload. Teams of bots, 15x."
work="$B/demo-container-logistics"
mkdir -p "$work"

if [ "${RENDER_ONLY:-0}" != 1 ] || [ ! -f "$work/journey.netlog" ]; then
	port=$((44000 + $$ % 250 * 4))
	sed "s/^port .*/port $port/" worlds/teams.world > "$work/teams.world"
	world="$B/rts world --dir $work/w"
	$world --new "$work/teams.world" --headless --speed 15 > "$work/start.txt"
	sleep 1
	"$B/rts" --coord "127.0.0.1:$port" --headless --faction spectator \
		--journey map:30,west:15,north:15,map:5 \
		--netlog "$work/journey.netlog" --stats "$work/journey.txt" > "$work/journey.out" 2>&1
	$world --status > "$work/status.txt"
	$world --stop > /dev/null
	# What it shows: loaded containers crossed between zones, and reached a front.
	loaded=$(grep -c ' container:[0-9]* cargo:' "$work/w/live/coord.log" || true)
	[ "$loaded" -ge 5 ] || { echo "demo: only $loaded loaded containers crossed"; exit 1; }
	grep -q 'units_blue.* container:' "$work/w/live/north.txt" || { echo "demo: no blue containers at the north front"; exit 1; }
	echo "demo: $loaded loaded containers crossed"
fi

clip="$B/rts clip --netlog $work/journey.netlog \
	--title \"$title\" --about \"$about\" --label \"$LABEL\" \
	--out $OUT.mp4 --sheet $B/demo-$(basename "$OUT").png --size $SIZE --crf $CRF"
[ "${CLIPBOARD:-0}" = 1 ] && clip="$clip --clipboard"
eval "$clip"
