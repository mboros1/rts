# rts

A persistent-world RTS in C: BAR-style simulation on a Foxhole-style
persistent map. Read these before changing anything:

- `docs/roadmap.md`: the spikes, the decisions behind them, results so far.
- `docs/conventions.md`: memory (regions, no malloc), determinism rules,
  thin types, boundaries, local CI.

## Checking your work

- `make check` is CI (the pre-push hook runs it). Keep it under ~30 s.
- Touched sim math? Also run `make determinism-x86`.
- Programs: `rts` (game, client, clips), `rts-headless` (bench, replays,
  scripts), `rts-server` (a zone), `rts-coord` (the coordinator).
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

## Shipping a feature

Every feature commit includes a demo: `demos/NN-name.script` (see
`src/script.h` for the format), rendered with `make demos`, its contact sheet
read to confirm it shows the feature, and its `expect` lines passing in
`make check`. Details in `docs/conventions.md`, "Demos". Demo videos are in Git LFS and
are never pushed: don't run `git lfs install` in this repo.
