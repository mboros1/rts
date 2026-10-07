#!/bin/sh
# Spike 3 demo: the faction split. Blue's client simulates its own faction
# from nothing but blue's commands and effects, and only ever learns of red
# units blue can see. A spectator connected to the same server sees it all.
# The clip shows both, side by side, on one clock and one camera.
#
# Like 04, a real run over sockets: reproducible in outcome (the checks
# below), and its clip re-renders from the netlogs (RENDER_ONLY=1).
#
# Environment: B, OUT, SIZE, CRF, LABEL, CLIPBOARD (see 04-server-and-client.sh)
set -eu
title="Spike 3: fog of war"
about="Left: blue's client simulating its own faction, seeing only what blue sees. Right: everything."
work="$B/demo-fog"
mkdir -p "$work"

if [ "${RENDER_ONLY:-0}" != 1 ] || [ ! -f "$work/blue.netlog" ]; then
	port=$((20000 + $$ % 20000))
	"$B/rts-server" --scenario battle --port "$port" --ticks 1000 --bot blue off \
		--latency 80 --jitter 20 --record "$work/server.replay" --stats "$work/server.txt" \
		> "$work/server.log" 2>&1 &
	server=$!
	"$B/rts" --connect "127.0.0.1:$port" --faction spectator --headless --quit-at 960 \
		--netlog "$work/spectator.netlog" --stats "$work/spectator.txt" > "$work/spectator.log" 2>&1 &
	spectator=$!
	"$B/rts" --connect "127.0.0.1:$port" --faction blue --headless --auto-orders \
		--latency 80 --jitter 20 --quit-at 960 \
		--netlog "$work/blue.netlog" --stats "$work/blue.txt" > "$work/blue.log" 2>&1 || {
		kill "$server" "$spectator" 2>/dev/null; cat "$work/blue.log"; exit 1; }
	wait "$spectator"
	wait "$server"

	fail() { echo "demo: $1"; cat "$work/blue.txt" "$work/server.txt"; exit 1; }
	grep -q '^digest_mismatches 0$' "$work/blue.txt" || fail "blue's faction hashes mismatched"
	grep -q '^digest_mismatches 0$' "$work/spectator.txt" || fail "spectator digests mismatched"
	grep -q '^gaps 0$' "$work/blue.txt" || fail "blue missed ticks"
	[ "$(sed -n 's/^digests //p' "$work/blue.txt")" -ge 25 ] || fail "too few faction hashes checked"
	"$B/rts-headless" verify "$work/server.replay" > /dev/null || fail "server replay didn't verify"
fi

clip="$B/rts clip --netlog $work/blue.netlog --compare $work/spectator.netlog \
	--title \"$title\" --about \"$about\" --label \"$LABEL\" \
	--out $OUT.mp4 --sheet $B/demo-$(basename "$OUT").png --size $SIZE --crf $CRF"
[ "${CLIPBOARD:-0}" = 1 ] && clip="$clip --clipboard"
eval "$clip"
