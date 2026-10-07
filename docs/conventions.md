# Conventions

How code is written here. Short on purpose: each rule earns its place by
preventing a specific class of bug. Sources are the C notes (arenas, the FAF
review), browser-rts `AGENTS.md`, and what the Recoil survey showed.

## Language and build

- **C23**, portable. Develop and ship with clang; the code must also build
  with GCC (`gcc-15`) and never rely on extensions only one compiler has. Use
  `static_assert`, `typeof`, `constexpr`, `nullptr`, `[[nodiscard]]`, and
  `<stdckdint.h>` for size arithmetic. Not `<stdbit.h>`: macOS libc lacks it,
  so bit operations use compiler builtins.
- Feature macros: `base.h` defines `_DEFAULT_SOURCE` before any system
  header, because strict `-std=c23` hides POSIX in glibc and musl.
- **Unity build**: one translation unit per executable; a full rebuild stays
  under a second. If it stops doing that, fix the build before the next
  feature.
- **Debug**: `-O0 -g -fsanitize=address,undefined`, always. Sanitizers are
  not something you turn on to hunt a bug; they are how you develop.
- **Warnings**: `-Wall -Wextra -Wconversion -Wshadow -Wstrict-prototypes`;
  `-Werror` in `make check`.
- **Local first, no CI.** Everything runs from the Makefile on this machine:
  - `make check`: clang debug build with sanitizers, tests, replay
    determinism, then a `gcc-15 -Werror -fanalyzer` build (no sanitizers;
    GCC's don't run on Apple Silicon).
  - `make cross`: occasionally. x86-64 Linux via `zig cc` (run in Docker
    with `--platform linux/amd64`), Windows via `x86_64-w64-mingw32-gcc`.
  - `make determinism-x86`: builds the sim with `clang -arch x86_64`, runs
    it under Rosetta, and compares per-tick hashes against the native arm64
    run. This is the cross-architecture check without a second machine.
- **Sim flags, every build**: `-ffp-contract=off`, no `-ffast-math`. See
  Determinism.

## Local CI

There is no hosted CI. Local runs replace it, given three properties:

- **It always runs.** `tools/hooks/pre-push` runs `make check` and blocks the
  push on failure. Hooks are versioned in the repo; after cloning, run
  `git config core.hooksPath tools/hooks`.
- **It runs from clean.** `make check` builds into a fresh directory and
  never reuses stale objects.
- **It leaves a record.** Each run appends `time commit pass|fail` to
  `ci.log` (gitignored).

Keep `make check` under ~30 seconds, or the hook will start being skipped.
Slow checks (`make cross`, `make determinism-x86`, soak runs) stay out of the
hook. Run them by hand before merging sim math, or nightly from
`launchd`/`cron`.

## Demos: every feature ships with a clip

A feature commit adds `demos/NN-name.script`: a short script that shows the
feature working, with `expect` lines for what it should do. `make demos`
renders its small `.mp4` and `.jpg` contact sheet, and all three go in the
same commit.

- **The script is the source of truth.** The sim is deterministic, so any
  commit can re-render its demos exactly. The committed video exists so the
  history can be browsed without building anything. Keep it small: 960x540,
  CRF 30, and trim the window rather than lower the quality further.
- **Its expectations are tests.** `make check` runs every demo script.
  Set thresholds from what you observed in the sheet, not from guesses.
- **Look before committing.** Read the contact sheet and check that it
  shows what the title claims.
- **For posting**, `make share DEMO=NN-name` renders 720p with the commit on
  the title card and puts it on the clipboard.

## Modules and APIs

- One module = one `.h` (public API) + one `.c`. Everything not in the header
  is `static`.
- Prefix every public name with its module: `arena_push`, `units_spawn`,
  `losmap_update`.
- Types `PascalCase`, functions and fields `snake_case`, constants
  `UPPER_SNAKE`.
- **Transparent structs.** Fields are public and readable. Each struct's
  invariants are stated in a comment above it. Anything that must preserve an
  invariant goes through a function; reading never needs one.
- **No global mutable state.** Every function gets its context explicitly
  (`Sim *`, `Zone *`, `Arena *`). Recoil's sim has ~290 references to global
  singletons, and they are what make it hard to lift out. We don't start that.
- Fixed-width integers. **Sizes and counts are signed `ptrdiff_t`** — an
  underflow is a negative number UBSan can catch, not a huge unsigned one.
- `countof(a)` for array lengths; `sizeof` on a type only when allocating.

## Domain values are thin types

Same rule as browser-rts. A unit id, a faction, a tick, and a metal amount are
not interchangeable integers:

```c
typedef struct { uint32_t v; } FactionId;
typedef struct { int64_t v; }  Tick;
```

- Conversions live in one named function, never as arithmetic at call sites.
- **Every encoding of a fact is named.** If "no owner" is `-1` on the wire
  and an empty optional in the sim, both encodings get a name and one
  conversion.
- **The same fact at two moments is two facts.** `state_hash_published` and
  `state_hash_now` are different functions; a peer compares the second.

## Memory: regions, not malloc

`malloc`/`free`/`realloc`/`calloc` are banned outside `platform/`, enforced
with `#pragma GCC poison` in a common header. All memory comes from regions
reserved at startup.

### Three layers, never mixed

Most of the bugs in the FAF review came from mixing these layers:

1. **Backing memory**: where bytes come from. Reserve a large virtual
   range up front and commit pages as needed. Pointers are stable and
   `realloc` never comes up.
2. **Region lifetime**: acquire, reset, release. Each region has a
   **generation counter** bumped on every reset.
3. **Allocation within a region**: a plain bump. Nothing else.

### Lifetimes in this project

Every allocation names which of these it belongs to. If none fits, the
lifetime design is wrong; don't reach for a general allocator.

| Region | Lives for | Holds |
| --- | --- | --- |
| `program` | process | content tables, map data, config |
| `zone` | a zone's run | faction state, unit and projectile pools; sized at startup from limits |
| `tick` | one sim tick | spatial query results, effect lists before encoding, scratch |
| `frame` | one rendered frame (client) | draw lists, UI scratch |
| `message` | one decoded network message | decode output, before it is copied into the sim |
| `snapshot` | one save or load | serialization buffers |

Data that outlives its region is **copied out** to a longer-lived one. The
copy is the moment a lifetime changes, so it is easy to spot.

### Pools are sized up front

- Capacities come from declared limits (max units per zone, max projectiles,
  max commanders) and are allocated and checked at startup. A zone that
  starts has all the memory it will ever use.
- **Game limits are checked by game rules, not by the allocator.** A unit cap
  is enforced when a command is admitted. If an allocation inside a tick runs
  out of space, that's a bug: abort with the region name, the requested size,
  and the high-water mark.
- `arena_try_push` returns `nullptr` and is the only variant that may fail
  quietly. It's for untrusted input (network decode), where failure means
  rejecting the message.

### Designed to be debuggable

- **Every allocation is zeroed.** No uninitialized bytes anywhere. That also
  removes a source of nondeterminism and makes hashing structs safe.
- **Every region is named** and tracks capacity, used, high-water and
  allocation count. A debug overlay lists them all.
- **Reset poisons** the region (fill with `0xDD` and ASan-poison it). Use after
  reset crashes at the bad read instead of returning stale data.
- **Scratch marks carry the generation.** `arena_mark` / `arena_rewind` assert
  that the generation matches and marks are rewound in LIFO order.
- Free space past the bump pointer is ASan-poisoned, so an overrun past an
  allocation is caught.
- **Nothing is inferred.** A region is free because its lifetime state says
  so, never because `offset == 0`. Running out is an explicit path, never a
  silent grow.

### Objects that churn: handle pools

Units, projectiles and transfers use fixed-capacity pools with a free list,
addressed by handles:

```c
typedef struct { uint32_t index; uint32_t generation; } UnitHandle;
```

- Lookup asserts the generation, so a stale handle is caught at the first
  dereference.
- Data is **struct-of-arrays**: positions, velocities and HP in separate
  dense arrays. That's the data layout the SIMD and cache arguments depend on.
- Free-list order and iteration order are deterministic functions of the
  operation sequence. Document which order each pool iterates in.

### Ring buffers hold only derived data

A ring may only hold data that can be dropped or recomputed (render
interpolation, telemetry, history). The write position is a monotonically
increasing 64-bit offset. Data at `off` is valid iff
`off >= write_pos - cap`; the slot is `off & (cap - 1)`. Recheck after
copying if a write could race the read.

### Concurrency

Each region has exactly one owner. Allocating never synchronizes. Only
acquire and release touch shared state. Handing a region to another thread is
an ownership transfer with release/acquire ordering. This is how the faction
phases run in parallel.

### Policy at compile time

Variants (thread-local vs atomic, debug vs release checks) are chosen with
tiny `static inline` policy functions and one readable core, not runtime
flags.

## Strings and buffers

- `typedef struct { const char *ptr; ptrdiff_t len; } Str;` — slices, not
  NUL-terminated strings. NUL only at the boundary with C libraries.
- One `copy_bytes` routine. Length math uses `ckd_add` / `ckd_mul`.

## Determinism (the sim)

The sim is a pure function: state + commands (+ effects) → next state.

- No wall clock, no `rand()`, no I/O, no addresses in the sim. Randomness
  comes from a seeded PRNG stored *in* the state.
- Floats under these rules: `-ffp-contract=off`, no fast-math, our own
  `sin`/`cos`/`atan2`, no approximate SIMD (`rcp`/`rsqrt`); 4-wide SIMD only,
  with explicit reduction order; no NaNs, asserted in debug.
- No iteration in hash-table order. Iterate dense arrays or sorted keys.
- Hashes cover fields in a declared order, not raw struct bytes.
- **A divergence is a defect.** It's a bug against the build, never a
  conflict to resolve. Every divergence becomes a pinned replay fixture.
- **The server never reads client state.** A client's hash is telemetry,
  never an input.

## Boundaries

- **Sim never includes raylib.** Render reads sim state; it never writes it.
- **No shape described twice.** The wire format is one schema header; both
  ends use it.
- **Network input is untrusted.** The decoder is small, bounds-checked, uses
  `arena_try_push`, and is fuzzed.

## Testing

- Replay determinism: a recorded command log must produce identical hashes
  every tick. Part of `make check`.
- Cross-architecture hash comparison: `make determinism-x86` (arm64 vs x86-64
  under Rosetta), before merging anything that touches sim math.
- Differential tests against Recoil's headless build for anything ported.
- Fuzz every decoder.
