#!/bin/sh
# Spike 18b demo: crews. worlds/teams.world with commanders, all bots. Home
# is rich (the headquarters' income) and keeps a small army, so its metal
# overflows: bots dig trucks in as vaults and send the surplus army forward.
# A commander stays home; a zone it isn't in gives its player no storage or
# income, so a bot's crew to open a new zone brings a metal truck and an
# energy truck with its engineers and escort: the metal for a transporter,
# and room to bank what it builds there. It lands, puts up power, then the
# transporter, then the rest. A spectator watches the map at 15x, then the
# fronts north and south where crews land.
#
# A real run over sockets; its clip re-renders from the netlog (RENDER_ONLY=1).
#
# Environment: B, OUT, SIZE, CRF, LABEL, CLIPBOARD (see 04-server-and-client.sh)
set -eu
title="Spike 18b: crews"
about="Home keeps a small army; its metal overflows into vaults and trucks. A crew opening a zone brings a truck of metal and one of energy, builds power, then a transporter. 15x."
work="$B/demo-crews"
mkdir -p "$work"

if [ "${RENDER_ONLY:-0}" != 1 ] || [ ! -f "$work/journey.netlog" ]; then
	port=$((44000 + $$ % 250 * 4))
	sed "s/^port .*/port $port/" worlds/teams.world > "$work/teams.world"
	rm -rf "$work/w"
	world="$B/rts world --dir $work/w"
	$world --new "$work/teams.world" --headless --speed 15 > "$work/start.txt"
	sleep 1
	"$B/rts" --coord "127.0.0.1:$port" --headless --faction spectator \
		--journey map:20,north:20,south:20,map:5 \
		--netlog "$work/journey.netlog" --stats "$work/journey.txt" > "$work/journey.out" 2>&1
	$world --status > "$work/status.txt"
	$world --stop > /dev/null
	# What it shows: crews crossing with trucks, and transporters raised away from home.
	crews=$(grep 'unpaired' "$work/w/live/coord.log" | grep -c 'metal-truck:' || true)
	[ "$crews" -ge 1 ] || { echo "demo: no crew crossed with a truck"; exit 1; }
	away=$(cat "$work/w/live/north.txt" "$work/w/live/south.txt" "$work/w/live/centre.txt" | grep -c 'units_.*transporter:' || true)
	[ "$away" -ge 1 ] || { echo "demo: no transporter raised away from home"; exit 1; }
	echo "demo: $crews crews crossed with trucks; transporters in $away zones away from home"
fi

clip="$B/rts clip --netlog $work/journey.netlog \
	--title \"$title\" --about \"$about\" --label \"$LABEL\" \
	--out $OUT.mp4 --sheet $B/demo-$(basename "$OUT").png --size $SIZE --crf $CRF"
[ "${CLIPBOARD:-0}" = 1 ] && clip="$clip --clipboard"
eval "$clip"
