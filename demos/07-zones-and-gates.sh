#!/bin/sh
# RETIRED in spike 8: gate seeds and the pool are gone, and this run's
# netlogs are an older wire version. The video and sheet stay as the record
# of spike 5; this script no longer runs.
echo "demos/07-zones-and-gates.sh is retired (spike 8)"; exit 0
# Spike 5 demo: two zones as separate processes, a coordinator between them.
# In alpha, blue fires a gate seed into beta, where blue has no gate; it
# lands by blue's base there and unfolds. Blue's army walks into alpha's gate and leaves; the
# coordinator holds it until beta's new gate is up, then it comes through
# and attacks red. Left: alpha. Right: beta. Both seen by spectators.
#
# A real run over sockets (like 04 and 05): reproducible in outcome (the
# checks below), and its clip re-renders from the netlogs (RENDER_ONLY=1).
#
# Environment: B, OUT, SIZE, CRF, LABEL, CLIPBOARD (see 04-server-and-client.sh)
set -eu
title="Spike 5: zones and gates"
about="Left: zone alpha. Right: zone beta. Blue seeds a gate into beta, where it had none, then sends its army through."
work="$B/demo-zones"
mkdir -p "$work"

if [ "${RENDER_ONLY:-0}" != 1 ] || [ ! -f "$work/alpha.netlog" ]; then
	rm -f "$work"/*.log "$work"/*.txt 2>/dev/null || true
	cat > "$work/alpha.script" <<SCRIPT
scenario economy
bot blue off
bot red off
zones alpha beta
at 0   spawn blue gate 1 at 130,256 radius 0
at 0   spawn blue generator 30 at 40,140 radius 26
at 0   spawn blue tank 16 at 180,256 radius 12
at 0   spawn blue raider 30 at 175,215 radius 12
at 1   select blue gate as gates
at 1   select blue tank as tanks
at 1   select blue raider as raiders
at 320 seed gates to beta
at 340 gate tanks to beta via 130,256
at 340 gate raiders to beta via 130,256
SCRIPT
	cat > "$work/beta.script" <<SCRIPT
scenario economy
bot blue off
bot red off
zones alpha beta
at 0    spawn red tank 14 at 330,256 radius 14
at 0    spawn red artillery 6 at 390,256 radius 8
at 1080 select blue tank as tanks
at 1080 select blue raider as raiders
at 1081 attack-move tanks 330,256
at 1081 attack-move raiders 330,240
SCRIPT
	port=$((20000 + $$ % 15000)); coord=$port; alpha=$((port + 1)); beta=$((port + 2))
	zones="--zones alpha,beta --coord 127.0.0.1:$coord"
	"$B/rts-coord" --port "$coord" --log "$work/coord.log" --retry 0.3 > "$work/coord.out" 2>&1 &
	cpid=$!
	sleep 0.3
	"$B/rts-server" --script "$work/alpha.script" --zone alpha $zones --port "$alpha" --ticks 1500 --speed 2 \
		--stats "$work/alpha.txt" > "$work/alpha.out" 2>&1 &
	apid=$!
	"$B/rts-server" --script "$work/beta.script" --zone beta $zones --port "$beta" --ticks 1500 --speed 2 \
		--stats "$work/beta.txt" > "$work/beta.out" 2>&1 &
	bpid=$!
	sleep 0.2
	"$B/rts" --connect "127.0.0.1:$alpha" --faction spectator --headless --quit-at 1490 \
		--netlog "$work/alpha.netlog" > "$work/alpha-view.out" 2>&1 &
	"$B/rts" --connect "127.0.0.1:$beta" --faction spectator --headless --quit-at 1490 \
		--netlog "$work/beta.netlog" > "$work/beta-view.out" 2>&1
	wait "$apid" "$bpid"
	kill "$cpid" 2>/dev/null || true

	fail() { echo "demo: $1"; cat "$work/coord.out"; exit 1; }
	grep -q 'seed' "$work/coord.log" || fail "no seed"
	grep -q '^units_in 46$' "$work/beta.txt" || fail "the army didn't all arrive in beta: $(grep units_in "$work/beta.txt")"
	grep -q '^gates_blue 1$' "$work/beta.txt" || fail "beta has no blue gate"
fi

clip="$B/rts clip --netlog $work/alpha.netlog --compare $work/beta.netlog \
	--headings \"zone alpha,zone beta\" --views \"150,256,150;175,256,390\" \
	--title \"$title\" --about \"$about\" --label \"$LABEL\" \
	--out $OUT.mp4 --sheet $B/demo-$(basename "$OUT").png --size $SIZE --crf $CRF"
[ "${CLIPBOARD:-0}" = 1 ] && clip="$clip --clipboard"
eval "$clip"
