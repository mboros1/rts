#!/bin/sh
# Spike 10 demo: a fresh world plays itself. Five zones, both sides' bots
# everywhere, nobody playing. Each side builds a transporter at home, opens
# the empty zones next to it with unpaired crews, raises transporters there,
# and sends its armies forward to wherever the enemy is; crews try to land
# in enemy-held zones unseen. The world runs at 15x; a spectator watches the
# strategic map, drops into the front, and goes back to the map.
#
# A real run over sockets: reproducible in kind, not in detail (the zones
# run in parallel); its clip re-renders from the netlog (RENDER_ONLY=1).
#
# Environment: B, OUT, SIZE, CRF, LABEL, CLIPBOARD (see 04-server-and-client.sh)
set -eu
title="Spike 10: the world plays itself"
about="Bots on both sides, nobody playing: they expand through transporters, hold fronts, and try to land behind them. 15x speed."
work="$B/demo-world-plays"
mkdir -p "$work"

if [ "${RENDER_ONLY:-0}" != 1 ] || [ ! -f "$work/journey.netlog" ]; then
	port=$((45000 + $$ % 1000 * 4))
	sed "s/^port .*/port $port/" worlds/five.world > "$work/five.world"
	world="$B/rts world --dir $work/w"
	$world --new "$work/five.world" --headless --speed 15 > "$work/start.txt"
	sleep 1
	"$B/rts" --coord "127.0.0.1:$port" --headless --faction spectator \
		--journey map:36,centre:12,map:5,north:12,map:5 \
		--netlog "$work/journey.netlog" --stats "$work/journey.txt" > "$work/journey.out" 2>&1
	$world --status > "$work/status.txt"
	$world --stop > /dev/null
	fail() { echo "demo: $1"; cat "$work/status.txt"; exit 1; }
	# Both sides got beyond their home zones.
	awk '$3 != "blue'"'"'s" && / blue [1-9]/ { b = 1 } $3 != "red'"'"'s" && /red [1-9]/ { r = 1 } END { exit !(b && r) }' \
		"$work/status.txt" || fail "a side never left home"
fi

clip="$B/rts clip --netlog $work/journey.netlog \
	--title \"$title\" --about \"$about\" --label \"$LABEL\" \
	--out $OUT.mp4 --sheet $B/demo-$(basename "$OUT").png --size $SIZE --crf $CRF"
[ "${CLIPBOARD:-0}" = 1 ] && clip="$clip --clipboard"
eval "$clip"
