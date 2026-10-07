// Clips: short videos and contact sheets rendered from a run.
//
// Nothing is captured from the screen. The sim is deterministic, so a clip
// re-runs a replay (or plays a scenario with bots) and renders the ticks it
// wants offscreen, at any resolution. Replays carry a hash per tick, so a
// clip proves it shows exactly what happened, or refuses.
//
// Outputs, any combination:
//   video   .mp4 for people; can go straight onto the system clipboard
//   sheet   one .png grid of labelled frames, for an agent to read at once
//   stats   .txt beside the sheet: per sampled tick, units, shells, hash
//   replay  .replay of a scenario run, so the clip can be reproduced
//   done    an empty marker file written last, for whoever is waiting
#pragma once

#include "arena.h"
#include "render.h"
#include "scenario.h"
#include "script.h"
#include "sim.h"

// The camera at one tick. Recorded by the game, one line per tick:
// "tick focus_x focus_z distance".
typedef struct {
	int64_t tick;
	float focus_x, focus_z, distance;
} CameraKey;

typedef struct {
	CameraKey *keys;
	ptrdiff_t count;
} CameraTrack;

bool camera_track_write(const char *path, const CameraKey *keys, ptrdiff_t count);
bool camera_track_read(const char *path, Arena *arena, CameraTrack *out);

// Follows the action: in-flight shells if there are enough, otherwise the
// ground between the armies. Smoothed so it drifts rather than jumps.
typedef struct {
	V2 focus;
	float distance;
	bool primed;
} Director;

void director_update(Director *d, const Sim *sim);

typedef struct {
	const char *replay;         // run this replay ...
	const Scenario *scenario;   // ... or play this scenario with bots ...
	const char *script;         // ... or run this script (with its bots)
	const char *label;          // shown on the title card, e.g. the commit
	int64_t from, to;           // tick window; to < 0 means the end
	double last_seconds;        // if > 0, from = to - this many seconds
	const char *camera;         // camera track path; nullptr for the director
	const char *video;          // .mp4 output or nullptr
	const char *sheet;          // .png contact sheet or nullptr
	int sheet_frames;           // frames in the sheet
	const char *save_replay;    // write the run's replay here (scenarios)
	const char *done;           // marker written when everything is finished
	bool clipboard;             // put the video on the system clipboard
	int width, height;          // output size
	int crf;                    // x264 quality: lower is better and bigger
} ClipRequest;

// Expects a window (hidden is fine) for the GL context. Returns an exit code.
int clip_run(const ClipRequest *req);

// `rts clip ...`: parse arguments, open a hidden window, run.
int clip_main(int argc, char **argv);
