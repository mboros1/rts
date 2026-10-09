#!/bin/sh
# Spike 12 demo: doctrines. worlds/doctrines.world: blue raids (raiders and
# scouts, three transporters, packs landing in red's zones; it rushes from
# the south); red brings siege guns behind tanks and digs in at the centre.
# Same units for both, different bots. A spectator watches the strategic map at 15x, drops into the
# centre, and goes back to the map.
#
# A real run over sockets; its clip re-renders from the netlog (RENDER_ONLY=1).
#
# Environment: B, OUT, SIZE, CRF, LABEL, CLIPBOARD (see 04-server-and-client.sh)
set -eu
title="Spike 12: doctrines"
about="Same units, different bots: blue raids and rushes, red brings siege guns and digs in at the centre. 15x."
work="$B/demo-doctrines"
mkdir -p "$work"

if [ "${RENDER_ONLY:-0}" != 1 ] || [ ! -f "$work/journey.netlog" ]; then
	port=$((43000 + $$ % 250 * 4))
	sed "s/^port .*/port $port/" worlds/doctrines.world > "$work/doctrines.world"
	world="$B/rts world --dir $work/w"
	$world --new "$work/doctrines.world" --headless --speed 15 > "$work/start.txt"
	grep -q 'doctrine blue rush' "$work/w/live/south.script" || { echo "demo: south isn't rushing"; exit 1; }
	grep -q 'doctrine red turtle' "$work/w/live/centre.script" || { echo "demo: centre isn't digging in"; exit 1; }
	sleep 1
	"$B/rts" --coord "127.0.0.1:$port" --headless --faction spectator \
		--journey map:40,centre:12,map:20 \
		--netlog "$work/journey.netlog" --stats "$work/journey.txt" > "$work/journey.out" 2>&1
	$world --status > "$work/status.txt"
	$world --stop > /dev/null
fi

clip="$B/rts clip --netlog $work/journey.netlog \
	--title \"$title\" --about \"$about\" --label \"$LABEL\" \
	--out $OUT.mp4 --sheet $B/demo-$(basename "$OUT").png --size $SIZE --crf $CRF"
[ "${CLIPBOARD:-0}" = 1 ] && clip="$clip --clipboard"
eval "$clip"
