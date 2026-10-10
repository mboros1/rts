#!/bin/sh
# Spike 19 demo: zone contexts. worlds/teams.world, all bots, with two
# fronts made different: north rich (more free income) on abundant metal,
# south lean with units at twice the price, and barren: extractors and
# generators give 0.3 of what they do elsewhere. Homes are `home` (rich in metal) by
# default. Blue's crews open both; two spectators watch at 15x, one north
# (left) and one south (right), the same moments side by side: north grows
# into a base and an army, south crawls.
#
# A real run over sockets; its clip re-renders from the netlog (RENDER_ONLY=1).
#
# Environment: B, OUT, SIZE, CRF, LABEL, CLIPBOARD (see 04-server-and-client.sh)
set -eu
title="Spike 19: zone contexts"
about="Each zone has its own economy and environment. North: rich and abundant. South: lean, units cost double, and barren. Blue opens both. 15x."
work="$B/demo-zone-contexts"
mkdir -p "$work"

if [ "${RENDER_ONLY:-0}" != 1 ] || [ ! -f "$work/journey.netlog" ]; then
	port=$((44000 + $$ % 250 * 4))
	{
		sed "s/^port .*/port $port/" worlds/teams.world
		echo "economy in north rich"
		echo "environment in north abundant"
		echo "economy in south lean cost=2"
		echo "environment in south barren metal=0.3 generator=0.3"
	} > "$work/contexts.world"
	rm -rf "$work/w"
	world="$B/rts world --dir $work/w"
	$world --new "$work/contexts.world" --headless --speed 15 > "$work/start.txt"
	sleep 1
	"$B/rts" --coord "127.0.0.1:$port" --headless --faction spectator --journey map:15,south:40 \
		--netlog "$work/south.netlog" --stats "$work/south.txt" > "$work/south.out" 2>&1 &
	south=$!
	"$B/rts" --coord "127.0.0.1:$port" --headless --faction spectator --journey map:15,north:40 \
		--netlog "$work/journey.netlog" --stats "$work/journey.txt" > "$work/journey.out" 2>&1
	wait $south
	$world --status > "$work/status.txt"
	$world --stop > /dev/null
	grep -q 'economy rich' "$work/w/live/north.script" && grep -q 'environment barren' "$work/w/live/south.script" \
		|| { echo "demo: the zones weren't given their contexts"; exit 1; }
	# What it shows: blue's army grows faster in rich north than lean south.
	count() { awk -v z="$1" '$1 == z { for (i = 1; i <= NF; i++) if ($i == "units") { print $(i - 1); exit } }' "$work/status.txt"; }
	north=$(count north); south=$(count south)
	echo "demo: blue's army: north (rich) $north, south (lean) $south"
	[ "$north" -gt $((2 * south)) ] || { echo "demo: lean south kept up with rich north"; exit 1; }
fi

clip="$B/rts clip --netlog $work/journey.netlog --compare $work/south.netlog \
	--headings \"north: rich and abundant,south: lean (cost x2) and barren (x0.3)\" --views \"80,256,190;80,256,190\" \
	--title \"$title\" --about \"$about\" --label \"$LABEL\" \
	--out $OUT.mp4 --sheet $B/demo-$(basename "$OUT").png --size $SIZE --crf $CRF"
[ "${CLIPBOARD:-0}" = 1 ] && clip="$clip --clipboard"
eval "$clip"
