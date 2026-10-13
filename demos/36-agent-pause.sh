#!/bin/sh
# Spike 22j demo, played: a duel with red played through the MCP server
# (tools/agent/mcp_server.py, driven by tools/agent/harness.py as Claude
# Code would drive it). Blue's client, from an input script: placing an
# extractor lights up the metal spots; then red pauses with a note through
# the MCP's note tool; blue reads it and resumes with P.
#
# Environment: B, OUT, SIZE, CRF, LABEL, CLIPBOARD (see 04-server-and-client.sh)
set -eu
title="Spike 22j: the agent pauses"
about="Red is played through the MCP server. Blue places an extractor (the metal spots light up); red pauses with a note through the MCP; blue reads it and resumes."
work="$B/demo-agent-pause"
rm -rf "$work"; mkdir -p "$work"
port=$((39000 + $$ % 2000 * 3))
"$B/rts-server" --script worlds/duel.script --port "$port" --ticks 6000 > "$work/server.out" 2>&1 &
server=$!
trap 'kill $server 2>/dev/null || true' EXIT
sleep 0.3
python3 tools/agent/harness.py "$port" join strategies wait:7 \
	"pause:Red here, pausing through the MCP: my note shows on your screen and the zone stops until you press P." \
	wait:8 > "$work/agent.out" 2>&1 &
clip=""
[ "${CLIPBOARD:-0}" = 1 ] && clip="--clipboard"
"$B/rts" --connect "127.0.0.1:$port" --faction blue --input demos/36-agent-pause.input \
	--video "$OUT.mp4" --sheet "$B/demo-$(basename "$OUT").png" --size "$SIZE" --crf "$CRF" \
	--title "$title" --about "$about" --label "$LABEL" $clip > "$work/client.out" 2>&1 || {
	cat "$work/client.out"; exit 1; }
grep -E 'FAIL|expectations held|wrote' "$work/client.out"
grep -q 'red.0 paused' "$work/server.out" || { echo "demo: red never paused"; cat "$work/agent.out"; exit 1; }
grep -q 'blue.0 resumed' "$work/server.out" || { echo "demo: blue never resumed"; exit 1; }
