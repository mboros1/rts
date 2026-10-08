#!/bin/sh
# Spike 8 demo: transporters. Four zones on a line, west - middle - far -
# east. Blue's home is west; blue also holds a small outpost in far (a
# transporter). In west, a crew of engineers and raiders is sent unpaired to
# middle, where blue has nothing: a long, costly charge, and they land in
# blue's strip. They build a transporter there. Then twelve tanks go from
# west to far, two hops, middle relaying them.
#
# The orders come from the zones' scripts, as a player's would; the clip is a
# blue player's journey watching it: west, the map, middle, the map, far.
#
# A real run over sockets (like 04, 05 and 09): reproducible in outcome (the
# checks below), and its clip re-renders from the netlog (RENDER_ONLY=1).
#
# Environment: B, OUT, SIZE, CRF, LABEL, CLIPBOARD (see 04-server-and-client.sh)
set -eu
title="Spike 8: transporters"
about="An unpaired send opens a zone where blue had nothing; the crew builds a transporter; tanks then go two hops through it."
work="$B/demo-transporters"
mkdir -p "$work"

if [ "${RENDER_ONLY:-0}" != 1 ] || [ ! -f "$work/journey.netlog" ]; then
	rm -f "$work"/*.txt "$work"/*.out "$work"/*.log 2>/dev/null || true
	port=$((20000 + $$ % 15000)); speed=4
	cat > "$work/line.world" <<WORLD
port $port
zone west   0.1 0.5 home blue
zone middle 0.37 0.5
zone far    0.63 0.5
zone east   0.9 0.5 home red
link west middle
link middle far
link far east
WORLD
	cat > "$work/west.script" <<SCRIPT
scenario economy
bases blue
bot blue off
bot red off
zones west middle far east
at 0  spawn blue transporter 1 at 110,300 radius 0
at 0  spawn blue generator 30 at 30,120 radius 24
at 0  spawn blue tank 12 at 150,320 radius 6
at 0  spawn blue engineer 4 at 140,280 radius 3
at 0  spawn blue raider 2 at 140,280 radius 3
at 1  select blue any within 10 of 140,280 as crew
at 1  select blue tank as army
at 60 send crew to middle at 60,200
at 2300 send army to far
SCRIPT
	cat > "$work/middle.script" <<SCRIPT
scenario economy
bases none
bot blue off
bot red off
zones west middle far east
at 0    spawn blue generator 30 at 30,400 radius 24
at 1500 select blue engineer as crew
at 1501 build crew transporter at 80,220
SCRIPT
	cat > "$work/far.script" <<SCRIPT
scenario economy
bases none
bot blue off
bot red off
zones west middle far east
at 0 spawn blue transporter 1 at 100,256 radius 0
at 0 spawn blue generator 10 at 40,150 radius 15
SCRIPT
	printf 'scenario economy\nbases red\nbot blue off\nzones west middle far east\n' > "$work/east.script"

	coord=$port; pids=""
	"$B/rts-coord" --port "$coord" --log "$work/coord.log" --retry 0.3 --world "$work/line.world" > "$work/coord.out" 2>&1 &
	pids="$pids $!"
	sleep 0.3
	k=1
	for z in west middle far east; do
		"$B/rts-server" --script "$work/$z.script" --zone "$z" --world "$work/line.world" --coord "127.0.0.1:$coord" \
			--port $((port + k)) --ticks 4800 --speed $speed --stats "$work/$z.txt" > "$work/$z.out" 2>&1 &
		pids="$pids $!"; k=$((k + 1))
	done
	sleep 0.5
	"$B/rts" --coord "127.0.0.1:$coord" --headless --faction blue \
		--journey west:12,map:3,middle:13,map:3,far:8 \
		--netlog "$work/journey.netlog" --stats "$work/journey.txt" > "$work/journey.out" 2>&1
	kill $pids 2>/dev/null || true
	wait 2>/dev/null || true

	fail() { echo "demo: $1"; tail -5 "$work"/*.out; exit 1; }
	get() { awk -v k="$2" '$1 == k { print $2 }' "$work/$1.txt"; }
	[ "$(get middle units_in)" = 18 ] || fail "middle took in $(get middle units_in), not 6 + 12"
	[ "$(get middle units_out)" = 12 ] || fail "middle relayed $(get middle units_out), not 12"
	[ "$(get far units_in)" = 12 ] || fail "far took in $(get far units_in), not 12"
	[ "$(get middle transporters_blue)" = 1 ] || fail "the crew didn't build a transporter in middle"
	grep -q '^digest_mismatches 0$' "$work/journey.txt" || fail "faction hash mismatch"
fi

clip="$B/rts clip --netlog $work/journey.netlog \
	--title \"$title\" --about \"$about\" --label \"$LABEL\" \
	--out $OUT.mp4 --sheet $B/demo-$(basename "$OUT").png --size $SIZE --crf $CRF"
[ "${CLIPBOARD:-0}" = 1 ] && clip="$clip --clipboard"
eval "$clip"
