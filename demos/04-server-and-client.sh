#!/bin/sh
# Spike 2 demo: the 10,000-unit battle served over a real socket to a client
# playing blue with 150 ms of latency, which loses its connection halfway and
# rejoins from a snapshot. The clip is the client's view, replayed from its
# netlog, with its network panel.
#
# A network demo is a real run, so it is reproducible in outcome (every
# expectation below), not byte for byte like a .script demo. Its netlog is
# the source of the clip: RENDER_ONLY=1 re-renders from the last one.
#
# Environment (set by make demos / make share):
#   B      binaries          OUT    output path without extension
#   SIZE   e.g. 960x540      CRF    video quality
#   LABEL  title card text   CLIPBOARD=1 to copy the video
set -eu
title="Spike 2: server and client"
about="The battle streamed to a client at 150 ms latency; it drops at 15 s and rejoins from a snapshot."
work="$B/demo-net"
mkdir -p "$work"

if [ "${RENDER_ONLY:-0}" != 1 ] || [ ! -f "$work/client.netlog" ]; then
	port=$((20000 + $$ % 20000))
	"$B/rts-server" --scenario battle --port "$port" --ticks 960 --bot blue off \
		--latency 150 --jitter 40 --record "$work/server.replay" --stats "$work/server.txt" \
		> "$work/server.log" 2>&1 &
	server=$!
	"$B/rts" --connect "127.0.0.1:$port" --faction blue --headless --auto-orders \
		--latency 150 --jitter 40 --disconnect-at 450 --reconnect-after 3 --quit-at 930 \
		--netlog "$work/client.netlog" --stats "$work/client.txt" > "$work/client.log" 2>&1 || {
		kill "$server" 2>/dev/null; cat "$work/client.log"; exit 1; }
	wait "$server"

	# Expectations: the run held together.
	fail() { echo "demo: $1"; cat "$work/client.txt"; exit 1; }
	grep -q '^digest_mismatches 0$' "$work/client.txt" || fail "client digests mismatched"
	grep -q '^reconnects 1$' "$work/client.txt" || fail "client didn't rejoin"
	grep -q '^gaps 0$' "$work/client.txt" || fail "client missed ticks"
	"$B/rts-headless" verify "$work/server.replay" > /dev/null || fail "server replay didn't verify"
fi

clip="$B/rts clip --netlog $work/client.netlog --title \"$title\" --about \"$about\" --label \"$LABEL\" \
	--out $OUT.mp4 --sheet $B/demo-$(basename "$OUT").png --size $SIZE --crf $CRF"
[ "${CLIPBOARD:-0}" = 1 ] && clip="$clip --clipboard"
eval "$clip"
