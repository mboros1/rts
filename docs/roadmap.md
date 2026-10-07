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

- Gate expansion metagame: seed energy cost, distance scaling on the zone
  graph, which units can travel without a gate (drop pods, commandos).
- Logout exploits: logging out to escape a fight (needs a logout timer), and
  converting units to defenses only in friendly territory.
- Offline defenses piling up: upkeep so static defenses decay unless supplied.
- Economy growth over a war that never ends: war resets or hard resource sinks.
- Use-based skills and XP: farming with alt accounts, the experience for new
  players.
- Unit ownership rules: in the pool, in play, persisted, destroyed.
- Per-zone commander caps plus time dilation for overloaded zones.
- Real pathfinding (QTPFS port or our own), graphics, anti-cheat, real
  internet networking.
- GPL: porting Recoil code makes the engine GPL; a clean-room spec avoids it.
