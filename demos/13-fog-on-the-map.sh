#!/bin/sh
# Spike 11 demo: fog on the strategic map. A self-playing world (bots on
# both sides, 15x) seen through blue's eyes: blue in full, red only as blue's
# units and scouts have seen it ("no intel", "CONTACT", "seen 4m ago", red
# transporters spotted, fading with age). Blue drops into centre for a
# moment (red's: fog everywhere but where blue's units are), then back to
# the map. The checks compare blue's map with a spectator's.
#
# A real run over sockets; its clip re-renders from the netlogs (RENDER_ONLY=1).
#
# Environment: B, OUT, SIZE, CRF, LABEL, CLIPBOARD (see 04-server-and-client.sh)
set -eu
title="Spike 11: fog on the map"
about="Blue's map: blue in full, red only as blue's units and scouts have seen it, sightings fading as they age."
work="$B/demo-fog-map"
mkdir -p "$work"

if [ "${RENDER_ONLY:-0}" != 1 ] || [ ! -f "$work/blue.netlog" ]; then
	port=$((44000 + $$ % 250 * 4))
	sed "s/^port .*/port $port/" worlds/five.world > "$work/five.world"
	world="$B/rts world --dir $work/w"
	$world --new "$work/five.world" --headless --speed 15 > "$work/start.txt"
	sleep 1
	journey="map:45,centre:8,map:15"
	"$B/rts" --coord "127.0.0.1:$port" --headless --faction blue --journey "$journey" \
		--netlog "$work/blue.netlog" --stats "$work/blue.txt" > "$work/blue.out" 2>&1
	$world --status --as blue > "$work/status-blue.txt"
	$world --status > "$work/status.txt"
	$world --stop > /dev/null
	# Blue's map has none of red's numbers, though red has plenty.
	grep '^east ' "$work/status-blue.txt" | grep -q 'red 0 units 0 bldg 0 transporters' || { echo "demo: no fog"; exit 1; }
	grep '^east ' "$work/status.txt" | grep -q 'red 0 units' && { echo "demo: red has nothing at home?"; exit 1; }
	true
fi

clip="$B/rts clip --netlog $work/blue.netlog \
	--title \"$title\" --about \"$about\" --label \"$LABEL\" \
	--out $OUT.mp4 --sheet $B/demo-$(basename "$OUT").png --size $SIZE --crf $CRF"
[ "${CLIPBOARD:-0}" = 1 ] && clip="$clip --clipboard"
eval "$clip"
