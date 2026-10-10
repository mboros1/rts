#!/bin/sh
# Spike 18c demo: winning. worlds/teams.world, all bots, with one setup line:
# a red army starts beside blue's headquarters in west (a fixture: where
# things are at the start; red's bot does the rest). Red's bot sees the
# headquarters and its army goes for it; blue's commanders fight back. When
# it falls the zone tells the coordinator, which logs the end and tells
# every zone: they all stop, and every view says who won. A spectator
# watches west, then the map.
#
# A real run over sockets; its clip re-renders from the netlog (RENDER_ONLY=1).
#
# Environment: B, OUT, SIZE, CRF, LABEL, CLIPBOARD (see 04-server-and-client.sh)
set -eu
title="Spike 18c: winning"
about="A red army starts beside blue's headquarters. Red's bot goes for it; when it falls, every zone stops and says red won."
work="$B/demo-winning"
mkdir -p "$work"

if [ "${RENDER_ONLY:-0}" != 1 ] || [ ! -f "$work/journey.netlog" ]; then
	port=$((44000 + $$ % 250 * 4))
	sed "s/^port .*/port $port/" worlds/teams.world > "$work/endgame.world"
	echo "setup west at 0 spawn red tank 16 at 170,256 radius 10" >> "$work/endgame.world"
	echo "setup west at 0 spawn red artillery 6 at 182,256 radius 6" >> "$work/endgame.world"
	rm -rf "$work/w"
	world="$B/rts world --dir $work/w"
	$world --new "$work/endgame.world" --headless --speed 3 > "$work/start.txt"
	sleep 1
	"$B/rts" --coord "127.0.0.1:$port" --headless --faction spectator \
		--journey west:50,map:8 \
		--netlog "$work/journey.netlog" --stats "$work/journey.txt" > "$work/journey.out" 2>&1
	$world --status > "$work/status.txt"
	$world --stop > /dev/null
	# What it shows: the end logged once, and every zone stopped.
	grep -q '^ended 1 west$' "$work/w/live/coord.log" || { echo "demo: the coordinator logged no end"; exit 1; }
	echo "demo: $(grep -h "headquarters fell" "$work/w/live/coord.out")"
fi

clip="$B/rts clip --netlog $work/journey.netlog \
	--title \"$title\" --about \"$about\" --label \"$LABEL\" \
	--out $OUT.mp4 --sheet $B/demo-$(basename "$OUT").png --size $SIZE --crf $CRF"
[ "${CLIPBOARD:-0}" = 1 ] && clip="$clip --clipboard"
eval "$clip"
