# Roadmap

Six spikes from a single-process combat sandbox to multiple zones and multiple
processes on one machine: AI on both sides plus one human player, every process
talking over sockets. Each spike is defined by what it proves, not by features.

## Decisions so far

- **C engine, raylib for rendering.** Recoil (`reference/RecoilEngine`) is a
  reference, not a fork: its sim assumes one shared world and its renderer and
  Lua layer are bound to OpenGL.
- **Floats, not fixed point**, with determinism held by rules: no fast-math,
  `-ffp-contract=off`, our own trig, no approximate SIMD (`rcp`/`rsqrt`),
  4-wide SIMD only with explicit reduction order, no NaNs in the sim. A
  cross-architecture hash check is what actually guarantees it.
- **Faction-split simulation.** The server owns truth. Each tick runs an
  interaction phase (server only: LOS, radar, hits, captures) that emits one
  effect stream per faction, then a faction phase per faction that depends only
  on that faction's commands and effects. A faction's clients run their own
  faction phase and match the server's hash. Clients never hold enemy state
  they cannot see.
- **Projectiles cross factions as launch events.** Unguided and homing shots
  are fully determined at launch, so the target faction simulates flight,
  dodging, and hits on its own units. No friendly fire, at least to start.
- **Zones connect only through warp gates.** Each zone is an independent
  simulation process. Every gate belongs to one faction; destroying the enemy's
  gate cuts their reinforcement into that zone, so the gate camping seen in EVE
  cannot happen.
- **Beachheads are gate seeds.** A high-energy shot fires a gate seed into a
  zone with no gate of yours; it is vulnerable while it unfolds. To start with,
  only the seed can travel this way.
- **Bots run inside the server tick** and enter the same command stream as
  players.

## Spikes

| # | Spike | Proves | Done when |
| --- | --- | --- | --- |
| 1 | Combat sandbox: one process, raylib | Combat feel and scale | 10k units at 30 Hz with headroom; dodgeable ballistic shells; state hash every tick; a recorded run replays to identical hashes |
| 2 | Server and client over sockets | The sim runs headless; the client only displays | Server process with bots on both sides plus the player's commands; client gets a snapshot plus deltas; disconnect and rejoin mid-game; simulated latency and jitter; time-dilation hook in the tick loop |
| 3 | Faction split | The two-phase tick works | Server runs the interaction phase and per-faction state; the client runs its faction phase from commands plus effects and matches the server's faction hash; no unseen enemy state reaches the client |
| 4 | Economy | The production loop | Metal and energy, factories, units produced and fielded by the player and by bots |
| 5 | Zones, gates, and the pool | The world spans processes | 2–3 zone processes plus a coordinator holding the transfer log and the faction unit pool; gate transfer survives a zone process being killed mid-transfer; gate seed opens a new zone; the player can switch zones |
| 6 | Persistence and soak test | It survives as a persistent world | Logout converts units to defenses or an AI doctrine; zones snapshot to disk and restart cleanly; bots on both sides run for hours without divergence, memory growth, or crashes |
| 7 | The world | The spikes are one game | `rts world` starts a persistent world of zones in the background and opens the strategic map; launch into a zone and play the full economy through the client; the world continues across restarts; saves and loads for testing; the sandbox is only a dev tool |
| 8 | Transporters | Expansion across the zone graph | Only home zones start with bases; engineers build transporters; unpaired sends (adjacent only, raiders and engineers, landing in your strip at a point you pick) and paired sends (any unit, relayed hop by hop, each zone paying its hop, units waiting visibly at relays); landing areas queue; pool, gate seeds and logout policies gone; fixtures only set up state |
| 9 | Entrench and away | Leaving the keyboard | Entrench is a paid order that transforms combat units into defenses and back; away, the bot plays your side |
| 10 | Strategic bots | The world plays itself | A cross-zone layer for both sides expands, reinforces beachheads and relays; from a fresh world both sides push into the middle; an hour-long soak passes (with coordinator log compaction) |
| 11 | Fog on the map | A side knows only what it has seen | The strategic map shows a side its own presence and nothing of the enemy's numbers: contact, last seen (with its age, fading), enemy transporters spotted; bots play by the same fog; a scout unit to see with |
| 12 | Doctrines and units | Worlds that don't stall | Configurable bot doctrines (per side, world or zone) and new units, both sides with the same roster; fronts move in the hour soak |
| 13 | Players and ownership | A side is a team | Players join a side by name into one of 8 slots; every unit has an owner and only the owner commands it; one key gives a selection to a teammate; a factory's output goes to the owner it names; a send names who the units arrive owned by; ownership survives sends, saves and kills. The economy stays shared |
| 14 | Separate economies | Each player runs their own | Each player has their own economy in each zone; a builder's owner pays for its work; resources are given to a teammate instantly up to their free storage; what's past a player's storage spills to teammates with room |
| 15 | AI slots | Teams with gaps still play | A slot nobody plays, or whose player left, is AI-held: one bot per side drives every AI-held slot (units and economy) alongside the humans; a teammate can take its units; a returning player gets the slot back; an 8v8 bot benchmark records its cost |
| 16 | Containers | Resources cross zones | A mobile container holds part of its owner's resources and carries them through transporters; in the zone it reaches, they join its owner's economy there |
| 17 | Command grid | Everything a selection can do is on screen | BAR's layout: the build grid (items on QWER ASDF, tabs on ZXCV) over the info pane, the orders grid beside them, resources at the top, the team bottom right; every button clickable and keyed; placeholders where later spikes go; played by hand |
| 18 | Commanders | A player is somewhere | Each player starts with only a commander (it builds the basics; engineers come from the first factory); no free income or storage in a zone without yours; a headquarters per side, no player's, where commanders respawn every 60 s; warp home (energy streamed as building is, cast, cooldown), automatic 30 s after logging out |
| 18b | Bots with commanders | Bots open and play as players now do | The commander-only opening; bots keep their commanders useful and alive, and warp home |
| 18c | Winning | A game ends | Destroying a side's headquarters ends the game |

Then the UI spikes: the command grid and selection panel, the strategic map
as the logistics console with pings and chat, minimap, control groups and
overlays, and a team playtest. Through spikes 13-16 a soak runs in the
background on each spike's commit (3 minutes of `make soak` when saving or
the coordinator changes, 10 of `make world-soak`), and the hour-long soak
runs once, at the end of the series.

## Spike 1 results (2026-10-07)

Done. `make bench`, `make determinism-x86`, `make cross` reproduce these.

- **Scale**: 10k units (5k a side) at 1.2–1.5 ms mean per tick, worst 2.4 ms;
  35k units at 4.3 ms mean, one 21 ms worst tick. Budget is 33.3 ms.
  Single-threaded, no SIMD.
- **Combat**: ballistic shells with lead; artillery lobs high arcs that stay
  up long enough to dodge. Shells cross factions as launches and are flown
  by the target faction, as in the spike 3 design.
- **Determinism**: replays verify tick for tick; arm64 and x86-64 (Rosetta)
  agree both ways over 1800 ticks of a 10k-unit battle; a Linux x86-64 musl
  build in Docker produces the same hashes. The game's own recorded session
  verifies headlessly.
- **Not yet**: pathfinding (units steer straight with separation), fog of
  war, the Windows build is compiled but never run, and the 21 ms spike at
  35k is unexplained.

## Spike 2 results (2026-10-07)

Done. `rts-server` runs the sim in real time; `rts --connect` plays on it;
`tools/net-smoke` (in `make check`) and `demos/04-server-and-client.sh`
exercise it end to end over real sockets.

- **Protocol** (`src/wire.h`): snapshot, then one delta per tick. Units are
  dead-reckoned: resent only when the client's prediction drifts 0.5 units or
  velocity changes 0.5 u/s. Shells are start and end events; clients fly them
  with the sim's own `shell_advance` and match bit for bit.
- **Digest**: every second the server sends the hash of what it has
  broadcast; the client compares its own. Zero mismatches across every run
  so far, including through rejoins and with 6000 shells in the air.
- **Bandwidth** (`rts-headless wire`): 10k-unit battle averages 305 KB/s,
  peak about 450 KB/s; a 400-a-side skirmish 8 to 30 KB/s. Snapshot of the
  full battle: 105 to 210 KB. Encoding costs 0.12 ms a tick.
- **Rejoin**: a dropped client reconnects and resumes from a fresh snapshot.
- **Latency and jitter** are emulated in the real send path on both ends.
- **Time dilation**: the tick rate slows when steps exceed 80% of the budget;
  `--dilation` forces it, `--speed` runs tests faster than real time.
- **Not yet**: per-faction visibility (spike 3 filters what a client gets),
  windowed client play has not been exercised by a person, Windows sockets.

## Spike 3 results (2026-10-07)

Done, in one spike. The faction split holds at full scale.

- **Each faction's phase depends only on its own state, commands and
  effects.** Effects are the shells arriving at it and what it perceives of
  its enemies. `faction_tick` runs a faction from exactly those, and
  `faction_hash` must match the server's.
- **Units perceive enemies through the wire's own encoding.** Visible enemies
  arrive as quantized, dead-reckoned entries (resent on 0.5 units of drift),
  and the server's units aim through that same perception. Server and client
  build identical views from identical bytes; aim is at most half a unit off.
- **Fog of war**: sight per type (raider 48, tank 32, artillery 26).
  Artillery outranges its sight (78) and needs spotters. A faction's
  perception equals exactly the enemies in sight, checked in tests.
- **Evidence**: `rts-headless split` rebuilds both factions of the 10k battle
  from their encoded streams: 3600 hash checks, 0 mismatches (`make check`
  runs a shorter one). Mid-game joins load a faction snapshot and match.
  Corrupted snapshots are rejected.
- **Cost**: a faction's stream averages 69 KB/s in the 10k battle (spectator
  stream: 305 KB/s), with a 40 KB burst when armies first see each other.
  Faction snapshot 290 KB. Client faction step 0.6 ms; server step rose from
  1.5 to 2.0 ms for sensing.
- **Clients**: players simulate their faction and draw fog; spectators keep
  the full mirror. `rts clip --netlog A --compare B` shows two clients side by
  side (demo 05: blue's view beside the spectator's).
- **Not yet**: a player's own orders wait a round trip before their units
  react (no local prediction); own shells are drawn approximately after they
  leave the outbox; the raider sight change rebalanced fights slightly.

## Spike 4 results (2026-10-08)

Done. The economy is faction state, so the faction split carries it unchanged.

- **Units and structures**: engineers build extractors (metal, only on metal
  spots), generators (energy) and factories; factories queue units, with
  repeat. Structures are units with no speed: seen, targeted and destroyed by
  the existing code. A scenario switch (`economy`) keeps old scenarios as
  they were.
- **Streaming economy**: builders ask for what their work costs each tick; a
  short resource slows everyone using it by one factor, and only jobs that
  use it (a generator still builds in an energy stall).
- **Metal spots** are shared map data, mirrored per side. One cross-faction
  rule: a spot belongs to whoever has a finished extractor on it, decided in
  the interaction phase and sent as an effect (a 64-bit mask).
- **Players**: in the local game, engineers place structures (E, G, F, with a
  ghost) and factories queue (1-4, shift to repeat). Scripts gain `build`,
  `produce` and `queue` (a group works down a build list). Network clients
  can send build and produce orders; their UI for it isn't built yet.
- **Bots** open on economy (engineers only until metal flows), keep power
  ahead of demand, take nearby metal, add factories with surplus, and never
  send two engineers to the same site. Bot against bot is symmetric and
  grows to 15-20 extractors and 2-5 factories in four minutes.
- **Tests**: economy invariants every tick (resources in bounds, spots owned
  once, ownership earned), the faction split under the economy, scripted
  build orders. Demo 06 is a four-minute bot game at 8x.
- **Not yet**: engineer build queues in the sim (scripts queue instead), an
  economy UI for network clients, balance beyond "both sides grow". A
  hand-scripted build order loses to the bot: bots react, scripts don't.
- **CI**: `make check` is 9.3 s, up from 6.5: the economy test (3 s under
  ASan) and a bigger sanitized build. Next to win back.

## Spike 5 results (2026-10-08)

Done. Zones are separate `rts-server` processes; `rts-coord` sits between.

- **Gates** are structures that charge on energy (10 s, 2000 energy). Units
  ordered through one walk to it and leave when it fires; arrivals come in
  around the faction's gate. Departures and arrivals are recorded commands,
  so every zone's replay still verifies alone, and both are faction state,
  so the faction split holds (tested).
- **Seeds**: a charged gate plus 5000 energy fires a gate seed into another
  zone. It lands near the faction's base there and unfolds over 20 s,
  vulnerable until it's up.
- **The coordinator** keeps a text log, one fsynced line per event
  (`transfer`, `delivered`, `draw`), and rebuilds pools and pending
  transfers from it on restart. It holds a transfer while its zone is down
  or has no gate yet for that faction, and retries; zones recognise repeats
  by transfer id. The pool is a destination like a zone; drawing from it is
  a transfer back.
- **Players** find zones through the coordinator (`rts --coord`) and switch
  with Tab, rejoining each zone from its faction snapshot.
- **Evidence**: `tools/zone-smoke` (in `make check`, 7 s): units leave alpha
  for beta while beta is down; the coordinator is killed and restarted from
  its log; beta comes up and receives all ten; a seed opens gamma; a player
  switches zones with every faction hash matching. Unit tests cover the
  ledger (replay, pool, partial draws, bad log lines) and gates under the
  faction split. Demo 07 shows alpha and beta side by side.
- **Not yet**: a zone's own crash loses its state, including units that left
  but weren't yet reported (spike 6: zone snapshots). Zones are local
  processes on one machine; the directory hands out 127.0.0.1.

## Spike 6 results (2026-10-08)

Done. A zone can be killed at any moment and comes back where it was.

- **Zone state**: `--state PATH` saves the whole sim (both factions, spot
  owners, the tick) plus the zone's own bookkeeping to one file, written
  atomically (temp file, fsync, rename). It saves every `--save-every`
  seconds, on SIGTERM, and after any tick where units left or arrived.
  About 140–470 KB and 2 ms for a few hundred units.
- **No unit lost or duplicated**: a zone saves before it tells the
  coordinator anything. Departures wait in an outbox inside the saved state,
  each with the zone's own sequence number, until the coordinator
  acknowledges them; after a restart they are re-sent and the coordinator
  drops ones it already logged. Arrivals are confirmed only once a save
  includes them, and the zone remembers recent transfer ids, so a
  redelivery after a crash is ignored.
- **Replays in segments**: a replay can begin from an embedded snapshot, so
  each run of a zone writes a segment that starts from its saved state.
  Segments stream to disk at every save; `rts-headless verify --partial`
  checks one cut short by a crash, and prints where it starts and ends so
  segments can be chained.
- **Logout**: `--logout entrench` turns a faction's combat units into
  turrets when its last player leaves (more hp, longer reach, can't move)
  and back when a player returns; `--logout bot` hands the faction to its
  bot; `hold` does nothing. Demo 08 shows a line of tanks entrenching and
  holding off two waves, then mobilizing.
- **Soak** (`make soak MINUTES=N`, `tools/soak.py`): a coordinator, zones
  alpha and beta with an economy and bots on both sides, blue sending
  raider waves through gates both ways, a player logging in and out of alpha
  (entrench and mobilize), and SIGKILLs: alpha every 15–40 s, the
  coordinator every 45–90 s, at 8x speed. It then checks unit conservation
  against the coordinator's log, that every replay segment verifies and
  chains to the last, that the player never saw a faction hash mismatch,
  that beta's memory stayed flat, and that nothing died on its own.
  A 10-minute run (80 simulated minutes per zone): 21 kills of alpha, 8 of
  the coordinator, 95 player sessions; 1838 units crossed with none lost or
  duplicated (7 still in transit at the end, pending in the coordinator);
  alpha's 22 replay segments all verify and chain; beta held 67 MB
  throughout; no hash mismatch; 13 minutes wall time.
- **Found by the soak**: a charged gate (`work == 1.0`) failed the faction
  state's validation on load, so a zone killed while a gate was full could
  not resume. Fixed, and `test_gates` now saves and reloads a charged gate.
  And three ways the soak itself could pass without proving anything, each
  now a failure: orphans from an aborted run writing into the next run's
  directory (it now kills its children however it exits); the laptop
  sleeping through the run (it holds `caffeinate` and fails if the wall
  clock jumped past the uptime clock); and one side losing its gates early,
  so nothing crossed (both sides now send waves, and fewer than 10 units
  crossing per minute fails). An outbox entry whose ack was lost is already
  in the coordinator's log; zones report outbox sequence numbers so it's
  counted once.
- **Not yet**: the coordinator's log grows forever and it holds at most
  4096 transfers (it needs compaction: a snapshot line plus the tail).
  Replay segments are never pruned. Hours-long runs and real players are
  still ahead; the soak runs minutes at 8x.

## Spike 7 results (2026-10-08)

Done. `make play` (or `build/rts world`) is the game.

- **The world** is a world file (`worlds/three.world`, `worlds/five.world`):
  zones, their places on the strategic map, home zones, and links. `rts
  world --new FILE` starts it: a coordinator and one `rts-server` per zone,
  detached, so the world keeps running when the window closes. `rts world`
  continues it (starting whatever isn't running), `--stop` saves every
  zone and stops, `--save NAME` / `--load NAME` keep situations to come back
  to, `--status` prints the map as text. Everything lives in `world/live`.
- **The strategic map** comes from the coordinator's directory: each zone
  reports its strength every second, and the coordinator adds links and
  units in transit. Click a zone to launch in; Tab or Esc goes back.
- **One set of controls.** Selection, orders, building, factory queues and
  the HUD moved from the sandbox into `controls.c`, which the client and the
  sandbox share. The sandbox (`rts --scenario`) is a dev tool and gets no
  features of its own.
- **A stand-in player**: `rts --coord ... --bot` runs the game's bot on the
  client's own view of its faction and sends its orders as a player's.
  `--journey map:3,west:20,...` scripts the trip around the map. Clips
  follow a journey across zones and draw the map screens.
- **Evidence**: `tools/world-smoke` (in `make check`, 12 s, in parallel):
  a new world, a journey through two zones with the bot as the player and
  every faction hash matching, stop and continue, save and load, the
  journey's clip. Demo 09 is a journey through a running world.
- **Not yet**: every zone still starts with both bases and the bots play
  them all the same, so the map has no shape yet (spike 8). The map shows
  both sides' strength everywhere, ignoring fog. Units only cross zones
  through spike 5's gates, which the client can't order yet.

## Spike 8 results (2026-10-08)

Done. Expansion goes through transporters, along the zone graph.

- **Home-only bases**: a world gives each side a base in its home zone
  only (`bases blue|red|none` in a zone's setup); everywhere else it has to
  get in.
- **Transporters** (the old gate, renamed) are built by engineers (R). A
  send leaves from our transporter nearest the units, and the transporter
  charges only while units wait, for the batch then waiting:
  - *paired* (our transporter at the destination): any units, up to 60, a
    10 s charge at 30 energy a unit; they land around that transporter;
  - *unpaired* (none there): to a neighbouring zone only, raiders and
    engineers only (others are turned away), up to 8, a 40 s charge at
    4000 + 400 a unit; they land in our strip of the zone (west or east
    quarter) at the point picked.
- **Relays**: a send to a zone further away goes hop by hop through zones
  with our transporters, the route the sender gives (`send army to far via
  middle`) or the shortest one. Each hop is its own send, charged and paid
  by the zone it leaves; the units wait at the relay's transporter, in the
  open, until it fires.
- **Push-back**: a zone takes a batch only while its landing area (16 around
  the transporter or landing point) has room for it (60 of our units);
  otherwise the coordinator holds it and retries. A transporter is one
  queue, so sends through it go in turn.
- **The server routes every send** before the step, whoever ordered it: it
  knows the links from the world file and where our transporters are from
  the coordinator's directory, and turns "to ZONE" into next hop, route and
  paired or not, or refuses it. Players can't mark a send unpaired; the
  server decides.
- **In the client**: T, then a zone's number. Unpaired, a click then picks
  the landing point, with our strip traced on the ground.
- **Gone**: the pool, gate seeds and logout policies. Away from the
  keyboard, a side's bot plays it, unless the zone's setup turned that bot
  off. In a world, a setup script can only spawn at tick 0 (fixtures set up;
  they don't play).
- **Evidence**: `test_transporters` (paired cost and timing, unpaired
  filtering, the batch caps, strip landing, a relay, one queue per
  transporter, the faction split and save/load); `tools/zone-smoke` (in
  `make check`, 12 s): an unpaired crew through a coordinator restart, a
  refused send, a transporter built, tanks relayed two hops and held until
  the landing area clears, a player switching zones; a 2-minute soak (527
  units across, none lost or duplicated). Demo 10.
- **Formats**: replay v6, wire v3, zone state v2 (a command carries its
  route; the config which sides start with a base). Demo 07 (gate seeds,
  the pool) is retired: its video stays, its script no longer runs. Demo
  08's title describes the old logout policy; spike 9 replaces it.
- **Not yet**: the bots don't use transporters (spike 10), so only players
  and scripts expand. The client's send keys are only tried by hand. The map
  still shows both sides' strength everywhere.

## Spike 9 results (2026-10-08)

Done. Entrenching is an order, not something logging out does.

- **Entrench** (V in the client, `entrench GROUP` in scripts) turns the
  selected combat units into turrets where they stand: 6 s, 40 metal and
  300 energy a unit, paid as it goes from the economy at its shared rate
  like building. **Mobilize** (V on entrenched turrets) turns them back: 4 s,
  150 energy. While transforming a unit can't move or fire, and is drawn
  pale, sinking as it digs in. Without an economy (the battle scenarios) it
  only takes the time.
- **Away**: since spike 8, a side's bot plays it while no player is in the
  zone (unless the zone's setup turned that bot off), and stands down when
  one joins.
- **Evidence**: `test_entrench` (only the units ordered transform, nothing
  until 6 s, silent meanwhile, the metal paid, turrets holding a line, and
  mobilizing; energy runs out without generators, and it slows to what the
  economy can pay); the transporter test entrenches and mobilizes under the
  faction split. Demo 11 is blue's view of a line digging in, breaking an
  assault, and pushing.
- **Formats**: replay v7, wire v4 (entrench and mobilize carry units). Demo
  08 (logout entrench) is retired.
- **Not yet**: the bot never entrenches; nor does anything decay, so an
  entrenched line holds forever (later: upkeep). The prices are first
  guesses.

## Spike 10 results (2026-10-08)

Done. A fresh world plays itself, and keeps going for hours.

- **The bots play the map** (`bot.c`, every 10 s, through ordinary orders
  the zone server routes like a player's). In each zone each side's bot
  knows the world from the directory: links, where its side has
  transporters, both sides' strength everywhere. It builds a transporter
  where its side has none (every idle engineer on it); opens neighbouring
  zones where it has no transporter with an unpaired crew (4 engineers, 4
  raiders): empty ones first, enemy-held ones only from a zone with an army
  to follow; and sends what it can spare (keeping 10) to the zone of its
  side that's furthest forward: one where the enemy is, then one next to
  them. Within a zone it attacks what it sees, or hunts an enemy it knows
  of after a minute if it's the stronger, so a landing nobody has spotted
  gets time to raise a transporter. A zone's bot stops building army at 300.
- **Getting there took four tries**, each run watched as text on the map:
  crews thrown at enemy zones and lost; armies piling up at home because
  reinforcements only went to zones that were losing; a frozen standoff in
  a zone where neither side could see the other; and defenders who always
  knew where a landing was. The rules above are what came out.
- **Coordinator log compaction**: at startup and every 2000 lines the log is
  rewritten (atomically) as the next id, each zone's highest departure
  number, and the pending transfers; delivered ones leave memory too.
- **Evidence**: `make world-soak` (`tools/world-soak.py`): five zones, bots
  everywhere, a zone SIGKILLed every 20-60 s and the coordinator every
  60-120 s, players (the client's `--bot`) dropping in. An hour at 8x (about
  8 game hours a zone): 88 zone kills, 40 coordinator kills, 61 player
  sessions; 7449 units crossed, none lost or duplicated; 93 replay segments
  verified and chained; memory flat within every process's life (a zone
  restored from its state sits higher, around 350 MB, and stays there); the
  log compacted 41 times; both sides held zones beyond home (centre changed
  hands). The ledger test covers compaction. Demo 12 is a time-lapse of a
  fresh world filling in.
- **Not yet**: the fronts settle into a standoff once every zone reaches the
  cap: crews keep trying, and sometimes a landing takes, but neither side
  wins a zone for long. Making a war that moves is game design (costs,
  caps, what attackers and defenders can see), not plumbing. The map shows
  both sides' strength everywhere, which the bots use too.

## Spike 11 results (2026-10-08)

Done. A side's map shows the enemy only as it has seen them.

- **Intel, from what units see**: each zone tracks, per side, the last tick
  it perceived enemy units and enemy transporters (how many), from the same
  perception it aims with; it's kept in the zone's saved state and reported
  with the zone's status as ages in game seconds.
- **The coordinator fogs the directory** for whoever asks as a side: its own
  units, buildings, transporters and transits, its intel about the enemy,
  and none of the enemy's numbers. Spectators, zones and tools get all of it
  (nothing checks who's asking yet: there's no authentication).
- **On the map**: "no intel", "CONTACT", or "seen 4m ago" fading with age;
  enemy transporters spotted as markers, faded with age and labelled with
  it. A zone's disc is your colour where you are, theirs (dark) where you
  believe they are. `rts world --status --as blue` prints a side's view.
- **The bots play by the same fog**: they believe a zone is the enemy's on
  contact, a sighting within 10 minutes, or a transporter seen within 30;
  the front is where they see the enemy, then where they saw a transporter,
  then next to a zone they believe is theirs. They hunt only an enemy seen
  lately; an unspotted landing goes unhunted.
- **Scouts**: fast (14), far-sighted (70), fragile, unarmed, cheap; allowed in
  unpaired sends; factory key 5. Each zone's bot keeps two, patrols with
  them (the enemy's side of the zone most), and sends one to a neighbour it
  has no fresh intel on.
- **Evidence**: `test_fog` (the directory with intel round trips; a side's
  view loses the enemy's numbers and transits and keeps its own; a bot's
  beliefs follow); the world smoke test checks blue's map shows nothing of
  red's home; a 10-minute world soak with the fogged bots (15 zone kills,
  6 coordinator kills, 2591 units across, none lost or duplicated; blue held
  north and south, red centre). Demo 13 is blue's map of a self-playing
  world.
- **Found by the soak**: a zone flushes its replay before writing its state,
  so one killed between the two restarts a little behind its replay's end,
  and the next segment overlaps the last. The soaks now accept an overlap
  when the earlier segment was in the same state at that tick
  (`rts-headless verify --hash-at T`).
- **Formats**: replay v8, wire v5 (scouts renumber the unit types), zone
  state v3 (intel).
- **Not yet**: no authentication, so a client could ask for the whole map.
  Sightings say when, not how many or where in the zone. Compare clips of
  two maps side by side are too small to read, so demo 13 shows one.

## Spike 12 results (2026-10-08)

Done. Doctrines and two new units; the same roster for both sides.

- **Units**: the *siege* gun (range 140, but nothing closer than 35; slow,
  big splash, sees only 30, so it needs spotters) for lines that have dug in;
  the *sapper* (armoured, lightly armed, builds at twice an engineer's
  pace, allowed unpaired) so a beachhead's transporter goes up in seconds.
  Factory keys 6 and 7. Builders are now any mobile unit with build power.
- **Doctrines** (`bot.h`): a preset, then any field by name, e.g.
  `turtle army_cap=500 siege=3`. Presets: balanced, rush, turtle, raider,
  siege. Fields: the factory cycle per combat type, army cap, garrison,
  smallest send, the army it takes to assault, crew make-up (engineers or
  sappers, and escort), scouts, transporters per zone, and whether it digs
  in at the front, raids enemy zones with unpaired raider packs, or hunts.
  Set per side in a zone's setup script (`doctrine red siege`), per side or
  per side and zone in a world file (`doctrine blue in north raider`), or
  for the client's `--bot` (`--doctrine`). `worlds/doctrines.world` is an
  example.
- **Sends spread out**: a send leaves from our nearest transporter with
  nobody waiting at it, so a zone with several sends side by side.
- **Matchups**, 10 minutes each in the five-zone world, counting zones
  changing hands: balanced v balanced 5 (the land grab, then the old
  standoff); rush v turtle 2, a rout: rush takes everything including red's
  home while turtle never leaves it; raider v siege 9, with centre swinging
  between red and contested over and over. Doctrines move fronts.
- **Found by a test**: units' sight was capped at 48 by the interaction
  phase's search radius, so scouts (70) saw no further than raiders. The cap
  is now 70, and `sim_init` checks no unit sees further.
- **Evidence**: `test_doctrine` (presets, overrides, nonsense refused,
  scripts and world files carry them), `test_siege` (fires far with a
  spotter, never close), the matchups above, and an hour's soak of
  `worlds/doctrines.world` (blue raids and rushes, red brings siege and digs
  in at the centre): 90 zone kills, 39 coordinator kills, 4557 units across,
  none lost or duplicated, 95 replay segments chained; centre changed hands
  7 times (9 front changes in all, short of the 10 asked of it beforehand).
  Demo 14 is the doctrines world.
- **Found by the soak**: a zone took deliveries it had no room for. A side's
  units in a zone are capped (8192); past that the sim spawned no more, and
  arrivals already confirmed were lost (24,000 in one run, as raider-doctrine
  homes sent everything they made forward and a front zone filled up). A zone
  now holds a delivery unless it has room for all of it, and bots stop
  making army once their side is strong enough everywhere, and stop feeding
  a zone that's full.
- **Formats**: replay v9, wire v6 (four combat types).
- **Not yet**: fronts move but settle: after the land grab the fighting
  concentrates on one contested zone. No doctrine adapts (a rush keeps
  rushing against a wall); turtle at home is too passive to win anything; the
  presets' numbers are first guesses, to be tuned by running matchups.

## Spike 13 results (2026-10-09)

Done. A side is a team of players, each with units of their own.

- **Players are slots**: up to 8 a side (`PlayerSlot`, a byte; widening it is
  a typedef). A world or script says how many (`players blue 3`, default 1).
  Each player starts a base of their own in their side's home zone, above
  and below the side's base in turn. Who is in a slot is the coordinator's
  to say: a client joins a side by name (`--name`, default `$USER`) and gets
  the first free slot, logged (`player NAME FACTION SLOT`), so the same name
  has the same slot in every zone and across restarts; a full side turns
  the name away. Blue's map lists blue's players, nothing of red's. No
  authentication.
- **Every unit has an owner, and only the owner's orders move it.** The sim
  checks it like a stale handle: a command carries its player, and units
  that aren't theirs are ignored. A structure belongs to the builder who
  started it; a send leaves only through one of the player's own
  transporters.
- **Giving**: a give order hands units over as they are, mid-order. A
  factory has an output owner (who what it makes belongs to). A send names
  who the units arrive owned by, and the transfer carries it through the
  coordinator, saves and kills. The economy is still one per side (spike 14).
- **Bots play a slot.** One per player the side has, each ordering only its
  own units, keeping clear of sites and metal teammates are taking, and
  planning with an even share of the side's economy (three bots reading the
  whole store as theirs stalled it: 11 engineers, 13 generators, no metal).
  A factory pointed at a teammate makes them army only. All of a side's
  bots stand down while one of its people is in the zone (per slot is spike
  15). The client's `--bot` plays its own slot.
- **In the client**: you select only your own units; each player is drawn in
  their own shade of the side's colour (player 0 in the side's own, so
  older clips are unchanged pixel for pixel); a team line above the HUD.
  H then a player's number gives the selection (only factories: points their
  output; shift+H gives them); T, a zone, then a player's number sends for
  them (Enter: yourself). Stopgap keys until the UI spikes.
- **Evidence**: `test_ownership` (another player's orders ignored, the
  owner's obeyed; give, output, a send through someone else's transporter
  refused, a send for a teammate leaving and arriving theirs; a base each;
  owners in the snapshot and the hash); the world smoke test (alice and bob
  join blue as 0 and 1, a third name is turned away, bob keeps his place
  across a stop and continue, blue's map lists them); arm64 and x86-64 agree.
  Demo 15: blue is three bots, red one; blue.0's factory makes blue.1's
  army; blue grows to 60 units against red's 35.
- **Soaks**: 3 minutes of `make soak` pass (once the soak counted transfers
  from the coordinator's output: since compaction, spike 10, its log drops
  delivered ones). 10 minutes of `worlds/teams.world` (blue three players,
  red two): 14 zone kills, 5 coordinator kills, every unit accounted for,
  nothing died on its own; but neither side ever left its home zone (blue
  sat on 903 units). A side of several bots doesn't expand: spike 14 fixes it.
- **Formats**: replay v10, wire v7, zone state v4, coordinator log (transfer
  lines carry the player). Worlds from before need `rts world --new`.
- **Not yet**: the H and T keys have only been tried headlessly (scripts and
  the stand-in player send the same orders), never by hand. A bot doesn't
  ask for or answer anything: teammates' bots only share a map and stay out
  of each other's way.

## Spike 14 results (2026-10-09)

Done. Each player has their own economy in each zone.

- **Per player**: metal, energy, storage, income and stalls. Extractors and
  generators earn for their owner (metal spots stay the side's). Each player
  pays for their own: a structure by whoever's builder works on it (helping
  a teammate's build costs you), a factory's units by the factory's owner
  (whoever they're for), a transporter's charge by its owner, a transform by
  the unit's owner. A broke player stalls alone. For now every player gets
  the start resources, the base trickle and the same storage in every zone
  (a commander unit replaces that later).
- **Overflow**: income past a player's storage spills in equal parts to
  teammates in the zone with room; past everyone's room it's lost.
- **Giving resources**: a share moves metal and energy to a teammate at once,
  as much as the giver has and the taker has room for. Scripts:
  `give blue.0 metal 400 to 1`; the client: H with nothing selected, then a
  player's number, gives a quarter of your metal and energy (a stopgap).
- **Bots** play their own economy; spike 13's even share of the side's is
  gone. That was what kept team sides at home: with an economy each, the
  team world's bots take the middle again within six game minutes.
- **Clips** of a side with more than one player show each player's metal and
  energy, in their colour (one-player clips are unchanged).
- **Evidence**: `test_economies` (overflow to a teammate, shares capped by
  what's had and the room, a broke player stalling alone while a teammate's
  factory runs, the factory's owner paying). Demo 16: two against two, blue.0
  feeding blue.1 400 metal every 20 s (406 -> 7 and 430 -> 830 at the
  first); blue.1 ends with 46 units to blue.0's 30, and red, stalled on metal
  and not sharing, is down to 9. Demo 15 is re-rendered: three economies
  against one now make 158 units to 37. arm64 and x86-64 agree.
- **Soaks**: 3 minutes of `make soak` pass; 10 minutes of `worlds/teams.world`
  pass: 14 zone kills, 5 coordinator kills, 1283 units across with none lost,
  every replay segment chained; blue held north and south, red centre.
- **Formats**: replay v11, wire v8, zone state v5.
- **Not yet**: the share key gives a fixed quarter; nobody has played it by
  hand. Bots never share. After the land grab the team world settles into
  the old standoff (fronts of 300 against 460 that never attack).

## Spike 15 results (2026-10-09)

Done. A player nobody is playing has their bot play them.

- **Presence is per world.** A logged-in client asks the coordinator for the
  directory every few seconds, with its name: that's a heartbeat, and the
  directory says which players are online (10 s without one, or a client
  logging out, and they aren't). While you're logged in anywhere, on the map
  or in any zone, your units in every zone are yours and hold where they
  are. 30 game seconds after you log out (a dropped connection or a restart
  doesn't hand anything over) you're away, and your bot plays your units in
  every zone, unless the zone's setup turned bots off. A slot nobody has
  taken is away from the start. A zone records who's away as a command, so
  replays and the faction split hold. A zone of its own (no world) counts
  the clients connected to it.
- **Take** (BAR's /take): a player who's here takes every unit, in this
  zone, of teammates who are away. Resources stay with the slot. K in the
  client; `take blue.1` in scripts (a script's player is away while its bot
  is on). A player coming back gets their slot and whatever wasn't taken.
- **The 8v8 benchmark** (`make bench-teams`, `rts-headless bench-teams`): one
  zone, eight bots a side from their own bases, ten game minutes. It first
  showed the bots never leaving their opening: with 32 metal spots split
  sixteen ways nobody reaches 12 metal a second, the bot's sign the opening
  is over; now the opening also ends when there's no free metal left. Then:
  peak 495 units, step mean 0.14 ms, worst 1.52 ms; bots 0.14 ms at worst
  (budget 33 ms). Blue wipes red out by minute 4 and builds on. The map,
  not the engine, is the limit at 8v8: a zone's metal is shared by sixteen
  players, so armies stay small.
- **Evidence**: `test_take` (only an away player's units are taken, only by
  one who's here; coming back to nothing taken back); the world smoke test
  (east, which alice never visits, counts her as here while she plays
  elsewhere, and her bot takes over once she logs out); demo 17 (two against
  two, all bots; blue.0's player logs in at 2 minutes, takes blue.1's 23
  units onto its own 27 and sends them at red). The script demos re-render
  byte for byte. arm64 and x86-64 agree.
- **Soaks**: 3 minutes of `make soak` pass; 10 minutes of `worlds/teams.world`
  pass: 14 zone kills, 5 coordinator kills, 11 player sessions (each zone
  handed away players to their bots 9 times), 1519 units across with none
  lost, every replay segment chained; centre changed hands three times.
- **Formats**: replay v12, wire v9, zone state v6; the directory carries who's
  online.
- **Not yet**: K and the away marks have only been tried headlessly. Taking
  is all or nothing. The grace is game time, so at 8x it's under 4 seconds
  of wall time. An 8v8 needs a bigger zone (more metal) to be a big battle.

## Spike 16 results (2026-10-09)

Done. Resources cross zones in containers. (Spike 16b replaced the rules
below: containers are now metal and energy trucks, storage is room, and a
truck carries a load only when it leaves or changes hands.)

- **Containers** are trucks of their owner's metal and energy (1000 and 5000
  at most): slow (5), fragile, unarmed, 100 metal and 1000 energy at a
  factory (key 8), paired sends only. In whatever zone a container is, its
  cargo is part of its owner's store and its room adds to their storage.
  Income past the held store fills containers; spending empties them first;
  and a container unloads into the held store whenever it has room, so a
  truck arriving where its owner is short empties, and one where the store
  is full stays full. Giving resources fills a teammate's containers too.
  Each container keeps its own cargo. A container that dies loses it.
- **Vaults**: entrenching a container makes it a vault (hardened storage,
  900 hp, can't move), cargo and all; mobilizing makes it a truck again.
- **Across zones**: a send takes the cargo with it. Departures, the
  coordinator's transfers (`cargo:M,E` in its log) and arrivals carry it, so
  it survives restarts and kills like the units do; arriving containers
  share it.
- **Bots, minimally**: a bot whose held store is nearly full keeps up to two
  containers waiting to fill; a full one goes to the zone of ours furthest
  forward where that same player is short (each zone reports which of a
  side's players are short of metal or energy, parked cargo included); in
  any zone but home, empties beyond two go home, like trucks. Bots budget
  only their held store. Getting there took four tries, each a team world
  watched by type: containers piling at the front (124 in one zone) because
  fronts float full stores too; a hundred generators because the bot judged
  "enough power" against the containers' room; trucks never unloading
  because spending came out of the held store first; and a side shipping to
  a zone where another of its players was short. Zones' stats files now list
  each side's units by type.
- **Evidence**: `test_containers` (room adds; income past the held store
  fills a container, not the teammate; spending empties containers first; a
  vault keeps its cargo; a send carries it, owned by its recipient; arriving
  containers bring it; a destroyed vault loses it) and the ledger test
  (cargo through the coordinator's log). Demo 18: blue.0's overflow fills
  three trucks (25,000 energy), it gives them to blue.1 (10,000 -> 25,000),
  who drives them forward and digs them in as vaults. Demo 19: a team world
  at 15x, 46 loaded containers crossing from blue's home to its fronts.
  arm64 and x86-64 agree.
- **Found by make check**: the client's command buffer was sized for the old
  message, and the two new floats overflowed it silently; the server
  refused every command. It's sized from the fields now, and an overflow is
  fatal.
- **Soaks**: 3 minutes of `make soak` pass. The hour-long soak at the end of
  spikes 13-16 (`worlds/teams.world`, 60 minutes at 8x) passes: 92 zone
  kills, 39 coordinator kills, 58 player sessions, 5600 units across with
  none lost or duplicated, every replay segment chained, the log compacted
  40 times; centre changed hands three times.
- **Formats**: replay v13, wire v10, zone state v7, coordinator log (cargo).
- **Not yet**: no explicit load or unload; which containers fill is their
  order in the zone. Trucks carry energy almost only (metal rarely
  overflows). Demo 19's spectator view can't tell a player's units apart.

## Spike 16b results (2026-10-09)

Done. Storage is room; trucks carry it, split by resource like BAR.

- **Metal trucks and energy trucks** (1000 metal, 5000 energy of room; the
  same cost, speed and factory as containers were: keys 8 and 9), and their
  vaults when dug in. A player's storage in a zone is the base plus their
  trucks' and vaults' room there; there's no cargo in a truck at rest, just
  one pool per resource. Storage lost (a truck killed, or leaving) loses
  what's past the new cap.
- **A load when it moves**: leaving through a transporter, or changing hands
  (give, take), a truck takes a full load of its resource, or what the
  player has, even from below the base (they chose to send it). It brings
  that, and its room, to where it arrives, or to its new owner.
- **Bots**: a bot whose base storage of a resource is nearly full keeps up to
  two trucks of it; with a full load to take, one goes to a zone of ours
  further forward where that player is short of that resource; one too many
  in a zone that isn't home goes home when there's little to take. Zones
  report shortages per player and resource. Bots budget against base
  storage. In a team world it's rare: fronts float full energy and metal
  seldom overflows (a balance question for later, not the trucks').
- **Evidence**: `test_trucks` (room adds and keeps income from spilling; a
  given truck takes a full load and its room; a vault keeps the room; a sent
  truck takes a full load from below the base; arriving trucks bring room
  and load; a destroyed vault loses what's past the cap). Demo 18 (renamed
  18-trucks): blue.0's three energy trucks add 15,000 of room, which fills;
  given to blue.1, they take 15,000 with them (25,000 -> 10,000, and 10,000
  -> 25,000), and become vaults at the front. Demo 19 (19-truck-logistics):
  a team world where bots ship a few loaded trucks. The other demos
  re-render byte for byte. arm64 and x86-64 agree.
- **Soaks**: 3 minutes of `make soak` pass; 10 minutes of `worlds/teams.world`
  pass (14 zone kills, 5 coordinator kills, 11 player sessions, 1322 units
  across with none lost; centre changed hands three times).
- **Formats**: replay v14, wire v11, zone state v8.

## Spike 17 results (2026-10-09)

Built; the hands-on playtest is next. The controls are on screen, laid out
as BAR's (from a screenshot of it).

- **The build grid** (bottom left): two rows of items on QWER and ASDF, each
  with its metal and energy cost, and tabs on ZXCV. For builders the tabs
  are categories (economy, combat, utility, build; combat is empty until
  there's something to build there); for factories, pages (army,
  logistics), with what's queued counted on each item; shift queues five.
- **The info pane** (below it): with several types selected, a tile per
  type with its count (click: only those; shift-click: not those); with
  one, its details: health, what it's doing, range, damage, sight, speed,
  storage, a factory's queue and progress, construction.
- **The orders grid** (beside them, five wide): repeat (a toggle, with BAR's
  green or red bar) and clear queue for factories; stop, move, fight,
  entrench or mobilize; send, give, take. Each order has its own key, live
  whatever's selected (see the keymap below). Move and fight wait for a
  click (shift: more); right-click is still fight and ctrl+right-click move.
- **Resources** top centre (have, storage, income and spending), **your
  team** bottom right (names, you, who's away), and boxes marked UNDER
  CONSTRUCTION where the minimap, control groups and idle builders go.
- **menu.c** works out the grids from the selection, without raylib, so it's
  tested (`test_menu`); controls.c draws them and binds keys and clicks from
  one layout, so a click lands on what was drawn. A factory's repeat is now
  its own command (it was set by whatever was queued last).
- **Clips draw the controls**: `at T show GROUP [tab N]` in a script draws
  them as that group's player would see them with it selected. Demo 20
  walks through builders, a factory's two pages, a mixed group's tiles, one
  factory's details and a raider's orders.
- **`make play`** refuses, with a message, a world an older build made
  (`rts world` stamps the formats it was made with) rather than failing in
  each zone.
- **The playtest** (the user, by hand): it plays well, but the order keys
  didn't work and clashed: the camera panned on WASD (the grid's W A S D,
  and S was stop), Esc both cancelled a prompt and left for the map, the
  sandbox's B and C were entrench and a tab, and the order keys went idle
  whenever a builder was selected (a drag around the base nearly always
  catches one). Now:
  - **A keymap** (`keymap.c`, `config/keys.json`): a JSON object of action
    names to key names, over the defaults; a file with an unknown action or
    key, a fixed key (digits, Enter) or a key bound twice is refused whole,
    and the defaults stand. `--keys PATH` for another file. `test_keymap`
    checks the file names every action as the defaults do.
  - **The defaults**: the build grid on QWER ASDF and ZXCV (live while
    builders or factories are selected); orders, always live: move M, fight
    G, stop Space, entrench B, repeat Y, clear queue J, send T, give H, take
    K; the camera on the arrows (and middle-drag); Tab the map; Esc cancels
    what's in progress, and only with nothing to cancel leaves for the map;
    F1 the overlay; the sandbox's pause, bot and clip on P, F2 and F3.
- **Not yet**: the text is small in a 960 x 540 clip (as in a window that
  size). No screen to rebind keys yet: edit the file.

## Spike 17b results (2026-10-09)

Done. The client can be played without a person, so an agent (or a test)
can see and use the controls as a player does.

- **Input is a record** (`input.h`): a frame's keys pressed and down, the
  mouse, its buttons, the wheel, the window's size. The controls, the camera
  and the client read only that; raylib fills it for a person.
- **Input scripts**: `rts --connect ... --input FILE --shots DIR` plays a
  script into the record instead, in a hidden 1600 x 900 window drawing into
  a texture: keys (the keymap's names, with shift or ctrl), clicks, drags and
  mouse moves at times after the zone's first snapshot; shots of the frame;
  and expectations of what the client did (orders sent by kind, units
  selected, a placement or prompt in progress), which fail the run.
- **`make check` plays the controls** (`tools/ui-smoke`, 6 s, in parallel): a
  zone, blue's bot off, and a client playing `tools/ui/basics.input`: select
  the base by dragging, place an extractor and cancel it, place a factory
  from the build tab, stop, move with a click, cancel a fight, narrow to the
  factory with its tile, queue one raider and five tanks, switch repeat on,
  open the logistics page, and leave with Esc. 14 expectations.
- **Found by it, first run**: the click that placed a structure also started
  a selection drag, which on release emptied the selection, so every order
  key after a placement did nothing (probably most of what the playtest hit).
  And from its shots: the bottom-right placeholders' text overlapped, the
  order keys overlapped their labels, and the zone banner covered the
  minimap's place. All fixed: the banner is top right now.
- **Played demos**: the client records what it plays (`--video`, `--sheet`
  of the script's shots, `--title`/`--about`/`--label` for the title card,
  `--clipboard`); a demo is a `.sh` running a zone and the client with an
  input script beside it. Demo 20 is now one (it was a clip with `show`):
  select the base, the builders' tabs, a factory's ghost and placing it,
  narrowing to the old factory, queueing five tanks, repeat, the logistics
  page, the engineers moving. It plays in 17 s and generates in 23 s (the
  clip took 5 s); `make check` doesn't run demos, and `make demos` only
  re-makes a stale one.
- **Found by demo 20's sheet**: builders' categories and factories' pages
  shared one open tab, so after a factory's logistics page the engineers'
  grid opened on combat, empty. Each keeps its own now; the UI smoke test
  checks it.
- **Not yet**: a script's positions are pixels, so it depends on where the
  camera starts and units stand; good enough for the base at the start.

## Spike 18 results (2026-10-09)

Done, with bots' expansion left for 18b. A player is where their commander is.

- **Commanders on** (a game setting: `commanders on` in a script, and every
  new world): each player starts with only a commander, the side with only
  a headquarters too. The commander builds what engineers build, at twice
  the pace, and fights (2400 hp, range 30); engineers come from the first
  factory. Older scripts keep the older start (a factory and two
  engineers): every existing demo re-renders byte for byte.
- **Presence**: a player has the base storage and the free income only in a
  zone their commander is in. Elsewhere, what they've built or shipped.
- **The headquarters**: one per side in its home zone, no player's (it
  can't be given or taken, nor can commanders). A commander lost anywhere
  reappears beside it 60 s later; one lost in another zone is reported home
  through the coordinator (a `respawn` transfer), and the home zone keeps
  the time.
- **Warp home**: an order for commanders. 5000 energy, paid as building is
  (10 s at full rate, slower in a stall), still and silent meanwhile; a hit
  stops it; then 5 minutes before it can warp again. At home it steps out
  beside the headquarters; elsewhere it leaves (a `warp` transfer) and lands
  there. A player gone 30 s has their commanders warp home, free, so their
  bot never plays a commander away from home. U in the client.
- **Bots, as far as this spike goes**: a bot's commander builds at home (the
  first factory before anything, then the transporter); a bot with no
  transporter in a zone starts one at once, not at its next strategy turn.
- **Evidence**: `test_commanders` (the start; presence; give refused; the
  respawn in 60 s; a warp's price, time, recharge, and a hit stopping one;
  a warp and a loss elsewhere leaving for home). Demo 21, played: the
  commander-only start, the first factory, an engineer, warp home (its
  zone gives blue generators to pay for the warp in the demo's time).
  The world smoke test's bob now plays 900 ticks (with the commander's
  start, his bot's first orders come later). A 3-minute zone soak passes.
- **Formats**: replay v15, wire v12, zone state v9; the coordinator's log
  has `warp` and `respawn` transfers.
- **Not yet** (18b): bots' crews land where they have no commander and so
  no economy, and stand there (28 idle engineers in north after two and a
  half minutes); home armies pile up. They need to bring trucks or their
  commander. The world soak would fail its expansion check until then.

If spike 3 runs long, split it: first the faction split on the server only
(a faction's state can be rebuilt from its effect stream), then the client
copy over the socket.

## Cross-cutting rules

- Lay out the simulation by faction from spike 1, even before it is enforced.
- Keep the sim free of raylib; the renderer only reads sim state.
- Socket layer has injectable latency, jitter, and dropped connections from
  spike 2 on. Localhost hides timing bugs.
- One schema header for the wire format.
- Unity build; ASan and UBSan on in debug.

## Gate transfer protocol (spike 5)

1. **Charging (source zone):** the gate starts charging; the source asks the
   coordinator to reserve the transfer; the coordinator checks the
   destination.
2. **Commit:** the batch leaves the source sim and becomes an in-transit record
   in the coordinator's log, with a transfer id and full unit state.
3. **Arrival (destination zone):** when ready, units spawn from the record and
   the record is marked delivered.

The charge and arrival animations cover the round trip. Transfer ids make
delivery idempotent, so units are never duplicated or lost across crashes. A
gate seed is the same transfer with a map position instead of a gate as its
destination.

Failure cases: destination gate destroyed in transit (hold, return, or
redirect — a gameplay rule); destination zone process down (delivered on
restart).

## Later

- A balance spike: energy is plentiful everywhere and metal scarce (spike
  16b: fronts float full energy, metal seldom overflows), so shipping energy
  means little. With it, weapons costing energy per shot, from the firing
  unit's owner, scaled by damage and range: cheap units nearly free,
  artillery more, and heavy drainers when there are any; a unit that can't
  pay holds its fire. Bots keep a reserve at the front and count being short
  of energy as a reason for trucks. And the zones' size and metal: 32 spots
  shared by sixteen players keeps an 8v8 small (spike 15).
- Commander economics: a respawn's cost, and the headquarters' own (spike 18
  makes them free).
- The war's length: a game ends when a headquarters falls (spike 18c); how a
  persistent world is balanced over a long war is open (perhaps games
  shorter than a season), as is what carries over between games.
- The transporter metagame: distance on the zone graph, units that travel
  without one (drop pods, commandos).
- Logging out to escape a fight: units are handed to the bot 30 s after,
  and commanders warp home (spike 18); whether that's exploitable.
- Replay segment pruning (each zone's replays grow forever; the
  coordinator's log is compacted since spike 10).
- Defenses piling up: upkeep so entrenched units and vaults decay unless
  supplied.
- Use-based skills and XP: farming with alt accounts, the experience for new
  players.
- Per-zone player caps, with time dilation for overloaded zones.
- Real pathfinding (QTPFS port or our own), graphics, anti-cheat,
  authentication (a client can ask for the whole map today), real internet
  networking, the Windows build run (it's compiled, never run).
- GPL: porting Recoil code makes the engine GPL; a clean-room spec avoids it.
