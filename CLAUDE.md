# rts

A persistent-world RTS in C: BAR-style simulation on a Foxhole-style
persistent map. Read these before changing anything:

- `docs/roadmap.md`: the spikes, the decisions behind them, results so far.
- `docs/conventions.md`: memory (regions, no malloc), determinism rules,
  thin types, boundaries, local CI.

## Checking your work

- `make check` is CI (the pre-push hook runs it). Keep it under ~30 s.
- Touched sim math? Also run `make determinism-x86`.
- Touched saving, zones or the coordinator? Also run `make soak MINUTES=3`
  (random SIGKILLs; checks no unit is lost and replays chain).
- Touched the bots or anything a whole world does? `make world-soak MINUTES=10`
  (a self-playing world under random kills; an hour for a release).
  Run soaks in the background on the commit and carry on; fix forward if
  one finds something. The hour-long soak runs at the end of a series of
  spikes, not after each one.
- Programs: `rts` (the game: `rts world`; also clips and the dev sandbox),
  `rts-headless` (bench, replays, scripts), `rts-server` (a zone),
  `rts-coord` (the coordinator).
- **The world is the game.** `make play` / `rts world` runs it. The sandbox
  (`rts --scenario`) is a dev tool: it shares `controls.c` with the client
  and gets no features of its own. Build gameplay for the world and test it
  there (`tools/world-smoke`, `rts --coord ... --bot --journey ...`).
- **Fixtures set up; they don't play.** Scripts, world files and soak tools
  may create a starting state. After that they act only through what a
  player can send (the client's `--bot` is the stand-in player). If a fixture
  starts making decisions, that logic belongs in the bot.
- Keep iteration fast: `make check` prints a time per stage (history in
  `build/timings.log`). Don't let a stage get slower without a reason.
- **Look at what you changed.** The game renders clips offline from the
  deterministic sim, so you can see behaviour without anyone playing:

  ```
  build/rts clip --list
  build/rts clip --scenario skirmish --sheet /tmp/s.png --frames 9
  ```

  The `.png` is a labelled grid of frames to read in one go; the `.txt`
  beside it has units, losses, shells in flight and the state hash per
  frame. Use `--from/--to` to pick the moment, `--out x.mp4` for video,
  `--replay` to render a recorded run (it refuses if the replay doesn't
  reproduce hash for hash).
- **Play the controls yourself.** The client takes an input script
  (`src/input.h`): keys, clicks and drags at times, in a hidden 1600 x 900
  window, with shots and expectations. `tools/ui-smoke` (in `make check`)
  plays `tools/ui/basics.input` against a zone; run the client with
  `--input FILE --shots DIR` and read the `.png`s to see what a player sees.
  Touched the controls or the panels? Play them before asking anyone to.

## Shipping a feature

Every feature commit includes a demo: `demos/NN-name.script` (see
`src/script.h` for the format), rendered with `make demos`, its contact sheet
read to confirm it shows the feature, and its `expect` lines passing in
`make check`. Details in `docs/conventions.md`, "Demos". Demo videos are in Git LFS and
are never pushed: don't run `git lfs install` in this repo.
