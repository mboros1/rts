#!/bin/sh
# Spike 16 demo: truck logistics. worlds/teams.world (blue three players,
# red two), all bots: a bot whose base storage of a resource is full keeps a
# truck or two of it, and sends one with a full load to a zone of its own
# further forward where it's short of that resource. In practice that's rare:
# fronts float full energy and metal seldom overflows. A spectator watches the
# map at 15x, then blue's home, then the north front.
#
# A real run over sockets; its clip re-renders from the netlog (RENDER_ONLY=1).
#
# Environment: B, OUT, SIZE, CRF, LABEL, CLIPBOARD (see 04-server-and-client.sh)
set -eu
title="Spike 16: truck logistics"
about="Teams of bots keep trucks of what they have too much of, and ship full loads to a front that's short. 15x."
work="$B/demo-truck-logistics"
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
	# What it shows: trucks built where a store is full, and a loaded one crossing.
	loaded=$(grep -c 'truck:[0-9]* cargo:' "$work/w/live/coord.log" || true)
	[ "$loaded" -ge 1 ] || { echo "demo: no loaded truck crossed"; exit 1; }
	grep -q 'units_blue.*-truck:' "$work/w/live/west.txt" || { echo "demo: no trucks at blue's home"; exit 1; }
	echo "demo: $loaded loaded trucks crossed"
fi

clip="$B/rts clip --netlog $work/journey.netlog \
	--title \"$title\" --about \"$about\" --label \"$LABEL\" \
	--out $OUT.mp4 --sheet $B/demo-$(basename "$OUT").png --size $SIZE --crf $CRF"
[ "${CLIPBOARD:-0}" = 1 ] && clip="$clip --clipboard"
eval "$clip"
