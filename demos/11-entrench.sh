#!/bin/sh
# Spike 9 demo: entrenching, as blue's player sees it (fog of war and all).
# Blue's line of tanks is ordered to entrench: it pays metal and energy as
# it goes and takes six seconds, sinking into the ground, unable to move or
# fire meanwhile. Red's assault comes in against the turrets and breaks.
# Then blue mobilizes (cheaper, four seconds) and pushes.
#
# Everything is set up at tick 0; after that the script only gives orders a
# player could. A real run over sockets (like 04): its clip re-renders from
# the netlog (RENDER_ONLY=1).
#
# Environment: B, OUT, SIZE, CRF, LABEL, CLIPBOARD (see 04-server-and-client.sh)
set -eu
title="Spike 9: entrench"
about="Blue's tanks dig in as turrets (paid, six seconds, helpless meanwhile), break red's assault, then mobilize and push."
work="$B/demo-entrench"
mkdir -p "$work"

if [ "${RENDER_ONLY:-0}" != 1 ] || [ ! -f "$work/blue.netlog" ]; then
	rm -f "$work"/*.txt "$work"/*.out 2>/dev/null || true
	port=$((20000 + $$ % 15000))
	cat > "$work/line.script" <<SCRIPT
scenario economy
bases none
bot blue off
bot red off
at 0    spawn blue tank 14 at 210,256 radius 9
at 0    spawn blue generator 12 at 175,256 radius 7
at 0    spawn red raider 26 at 330,236 radius 10
at 0    spawn red tank 8 at 340,280 radius 8
at 1    select blue tank as line
at 1    select red any as assault
at 60   entrench line
at 260  attack-move assault 200,256
at 800  mobilize line
at 940  attack-move line 340,256
at 290  expect blue turrets == 14
at 790  expect blue lost <= 4
at 790  expect red lost >= 20
SCRIPT
	"$B/rts-server" --script "$work/line.script" --port "$port" --ticks 1250 --speed 1.5 \
		--stats "$work/server.txt" > "$work/server.out" 2>&1 &
	server=$!
	sleep 0.3
	"$B/rts" --connect "127.0.0.1:$port" --faction blue --headless --quit-at 1230 \
		--netlog "$work/blue.netlog" --stats "$work/blue.txt" > "$work/blue.out" 2>&1
	wait "$server"
	grep -q '^digest_mismatches 0$' "$work/blue.txt" || { echo "demo: faction hash mismatch"; exit 1; }
	if grep -q 'FAIL' "$work/server.out"; then grep 'FAIL\|ok ' "$work/server.out"; echo "demo: expectations failed"; exit 1; fi
fi

clip="$B/rts clip --netlog $work/blue.netlog \
	--title \"$title\" --about \"$about\" --label \"$LABEL\" \
	--out $OUT.mp4 --sheet $B/demo-$(basename "$OUT").png --size $SIZE --crf $CRF"
[ "${CLIPBOARD:-0}" = 1 ] && clip="$clip --clipboard"
eval "$clip"
