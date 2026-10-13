#!/bin/sh
# Spike 22i demo, played: radar and jammers. A zone set up with blue's
# radar tower, red tanks out of blue's sight, and a red jammer with tanks
# around it; blue's client, from an input script, looks there.
#
# Environment: B, OUT, SIZE, CRF, LABEL, CLIPBOARD (see 04-server-and-client.sh)
set -eu
title="Spike 22i: radar and jammers"
about="Blue's radar shows red's tanks as blips, out of sight; around red's jammer there are none, and the ground there is static, not fog. A scout's sight shows what they are."
work="$B/demo-radar"
rm -rf "$work"; mkdir -p "$work"
cat > "$work/zone.script" <<'ZONE'
title Radar
scenario economy
bases blue
bot blue off
bot red off
at 0 spawn blue radar 1 at 120,256
at 0 spawn red tank 3 at 215,215 radius 6
at 0 spawn red jammer 1 at 225,300
at 0 spawn red tank 3 at 230,290 radius 6
ZONE
port=$((39000 + $$ % 2000 * 3))
"$B/rts-server" --script "$work/zone.script" --port "$port" --ticks 3000 > "$work/server.out" 2>&1 &
server=$!
trap 'kill $server 2>/dev/null || true' EXIT
sleep 0.3
clip=""
[ "${CLIPBOARD:-0}" = 1 ] && clip="--clipboard"
"$B/rts" --connect "127.0.0.1:$port" --faction blue --input demos/35-radar.input \
	--video "$OUT.mp4" --sheet "$B/demo-$(basename "$OUT").png" --size "$SIZE" --crf "$CRF" \
	--title "$title" --about "$about" --label "$LABEL" $clip > "$work/client.out" 2>&1 || {
	cat "$work/client.out"; exit 1; }
grep -E 'FAIL|expectations held|wrote' "$work/client.out"
