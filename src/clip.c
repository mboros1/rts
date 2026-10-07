#include "clip.h"

#include <math.h>

#include "platform.h"
#include "replay.h"

static constexpr int SUPERSAMPLE = 2;   // render at 2x, encoder scales down
static constexpr int CLIP_FPS = 30;     // one frame per sim tick
// CRF 26 keeps ~10 s under 10 MB: thousands of tiny shells are costly to encode.

// ---------------------------------------------------------------- camera tracks

bool camera_track_write(const char *path, const CameraKey *keys, ptrdiff_t count)
{
	FILE *f = fopen(path, "w");
	if (!f) return false;
	for (ptrdiff_t i = 0; i < count; i++)
		fprintf(f, "%lld %.3f %.3f %.3f\n", (long long)keys[i].tick, (double)keys[i].focus_x,
		        (double)keys[i].focus_z, (double)keys[i].distance);
	return fclose(f) == 0;
}

bool camera_track_read(const char *path, Arena *arena, CameraTrack *out)
{
	Str text;
	if (!platform_read_file(arena, path, &text)) return false; // NUL-terminated
	ptrdiff_t lines = 0;
	for (ptrdiff_t i = 0; i < text.len; i++) lines += text.ptr[i] == '\n';
	CameraKey *keys = PUSH(arena, CameraKey, lines);
	ptrdiff_t n = 0;
	const char *p = text.ptr;
	while (n < lines && *p) {
		char *end;
		CameraKey k;
		k.tick = strtoll(p, &end, 10);
		if (end == p) break;
		p = end;
		k.focus_x = strtof(p, &end);
		k.focus_z = strtof(end, &end);
		k.distance = strtof(end, &end);
		p = end;
		while (*p == '\n' || *p == ' ' || *p == '\r') p++;
		keys[n++] = k;
	}
	*out = (CameraTrack){keys, n};
	return n > 0;
}

// The last key at or before tick; the first key before the track starts.
static CameraKey camera_track_at(const CameraTrack *t, int64_t tick)
{
	ptrdiff_t lo = 0, hi = t->count - 1;
	if (tick <= t->keys[0].tick) return t->keys[0];
	while (lo < hi) {
		ptrdiff_t mid = (lo + hi + 1) / 2;
		if (t->keys[mid].tick <= tick) lo = mid; else hi = mid - 1;
	}
	return t->keys[lo];
}

// ---------------------------------------------------------------- director

void director_update(Director *d, const Sim *sim)
{
	V2 sum = v2(0.0f, 0.0f);
	ptrdiff_t n = 0;
	for (int f = 0; f < FACTION_COUNT; f++) {
		const Projectiles *p = &sim->factions[f].incoming;
		for (ptrdiff_t i = 0; i < p->count; i++) sum = v2_add(sum, v2(p->px[i], p->pz[i]));
		n += p->count;
	}
	V2 focus;
	float spread;
	if (n >= 12) {
		focus = v2_scale(sum, 1.0f / (float)n);
		float total = 0.0f;
		for (int f = 0; f < FACTION_COUNT; f++) {
			const Projectiles *p = &sim->factions[f].incoming;
			for (ptrdiff_t i = 0; i < p->count; i++) total += v2_len(v2_sub(v2(p->px[i], p->pz[i]), focus));
		}
		spread = total / (float)n;
	} else {
		V2 centre[FACTION_COUNT];
		for (int f = 0; f < FACTION_COUNT; f++) {
			const Units *u = &sim->factions[f].units;
			V2 s = sim->factions[f].base;
			if (u->count) {
				s = v2(0.0f, 0.0f);
				for (ptrdiff_t i = 0; i < u->count; i++) s = v2_add(s, v2(u->px[i], u->pz[i]));
				s = v2_scale(s, 1.0f / (float)u->count);
			}
			centre[f] = s;
		}
		focus = v2_scale(v2_add(centre[0], centre[1]), 0.5f);
		spread = 0.5f * v2_len(v2_sub(centre[0], centre[1]));
	}
	float distance = f_clamp(spread * 2.4f + 70.0f, 80.0f, 360.0f);
	if (!d->primed) {
		*d = (Director){focus, distance, true};
		return;
	}
	d->focus = v2_add(d->focus, v2_scale(v2_sub(focus, d->focus), 0.05f));
	d->distance += (distance - d->distance) * 0.03f;
}

// ---------------------------------------------------------------- encoding

static FILE *encoder_open(const char *path, int src_w, int src_h, int out_w, int out_h)
{
	// Render textures read back bottom-up, hence vflip.
	const char *cmd = TextFormat(
		"ffmpeg -loglevel error -y -f rawvideo -pix_fmt rgba -s %dx%d -r %d -i - "
		"-vf vflip,scale=%d:%d:flags=area -c:v libx264 -preset veryfast -crf 26 "
		"-pix_fmt yuv420p -movflags +faststart '%s'",
		src_w, src_h, CLIP_FPS, out_w, out_h, path);
	if (strchr(path, '\'')) return nullptr;
	return popen(cmd, "w");
}

// ---------------------------------------------------------------- the run

typedef struct {
	int64_t tick;
	ptrdiff_t units[FACTION_COUNT];
	int64_t lost[FACTION_COUNT];
	ptrdiff_t shells;
	Hash hash;
} Sample;

static Sample sample_of(const Sim *sim)
{
	Sample s = {.tick = sim->tick.v, .hash = sim_hash_now(sim)};
	for (int f = 0; f < FACTION_COUNT; f++) {
		s.units[f] = sim->factions[f].units.count;
		s.lost[f] = sim->factions[f].stats.units_lost;
		s.shells += sim->factions[f].incoming.count;
	}
	return s;
}

static const char *caption(const char *name, const Sample *s)
{
	return TextFormat("%s   tick %lld   blue %td   red %td   shells %td", name, (long long)s->tick,
	                  s->units[0], s->units[1], s->shells);
}

static int fail(const char *msg, const char *detail)
{
	fprintf(stderr, "clip: %s%s%s\n", msg, detail ? ": " : "", detail ? detail : "");
	return 1;
}

static bool write_done(const char *path)
{
	return !path || platform_write_file(path, "", 0);
}

int clip_run(const ClipRequest *req)
{
	Arena work = arena_create("clip", (ptrdiff_t)1 << 30);
	Arena decode = arena_create("clip decode", (ptrdiff_t)64 << 20);
	Arena frame = arena_create("clip frame", (ptrdiff_t)256 << 20);
	Arena replay_out = arena_create("clip replay", (ptrdiff_t)1 << 30);

	// Where the commands come from: a replay file or bots playing a scenario.
	ReplayReader reader = {0};
	SimConfig config;
	Bot bots[FACTION_COUNT];
	const char *name;
	int64_t from = req->from, to = req->to;
	if (req->replay) {
		Str data;
		if (!platform_read_file(&work, req->replay, &data)) return fail("cannot read", req->replay);
		ReplayStatus st = replay_open(&reader, data);
		if (st != REPLAY_OK) return fail(replay_status_name(st), req->replay);
		config = reader.config;
		name = req->replay;
		if (to < 0) {
			// Scan for the last tick; the reader is cheap to rewind.
			ReplayReader scan = reader;
			int64_t last = 0;
			for (;;) {
				arena_reset(&decode);
				Tick t;
				Command *c;
				ptrdiff_t n;
				Hash h;
				st = replay_next(&scan, &decode, &t, &c, &n, &h);
				if (st != REPLAY_OK) break;
				last = t.v + 1;
			}
			if (st != REPLAY_END) return fail(replay_status_name(st), req->replay);
			to = last;
		}
	} else {
		if (!req->scenario) return fail("need --replay or --scenario", nullptr);
		config = scenario_config(req->scenario);
		scenario_bots(req->scenario, bots);
		name = req->scenario->name;
		if (from < 0) from = req->scenario->clip_from;
		if (to < 0) to = req->scenario->clip_to;
	}
	if (req->last_seconds > 0.0) from = to - (int64_t)(req->last_seconds * SIM_HZ);
	if (from < 0) from = 0;
	if (to <= from) return fail("empty tick window", nullptr);

	CameraTrack track = {0};
	if (req->camera && !camera_track_read(req->camera, &work, &track))
		return fail("cannot read camera track", req->camera);

	Sim sim;
	sim_init(&sim, config);
	Renderer renderer;
	renderer_init(&renderer, &sim, &work);
	int rw = req->width * SUPERSAMPLE, rh = req->height * SUPERSAMPLE;
	RenderTexture2D target = LoadRenderTexture(rw, rh);

	ReplayWriter writer;
	if (req->save_replay) replay_begin(&writer, &replay_out, config);

	FILE *video = nullptr;
	if (req->video && !(video = encoder_open(req->video, rw, rh, req->width, req->height)))
		return fail("cannot start ffmpeg for", req->video);

	// Contact sheet: evenly spaced ticks in (from, to], in a grid of cells.
	int frames = req->sheet ? (req->sheet_frames > 0 ? req->sheet_frames : 9) : 0;
	int cols = frames <= 4 ? 2 : frames <= 9 ? 3 : 4;
	int rows = frames ? (frames + cols - 1) / cols : 0;
	int cw = 640, ch = 640 * req->height / req->width;
	Image sheet = frames ? GenImageColor(cols * cw, rows * ch, BLACK) : (Image){0};
	Sample *samples = PUSH(&work, Sample, frames > 0 ? frames : 1);
	int taken = 0;

	Director director = {0};
	Command bot_cmds[256];
	while (sim.tick.v < to) {
		arena_reset(&decode);
		Tick tick = sim.tick;
		Command *cmds = bot_cmds;
		ptrdiff_t n = 0;
		Hash want = {0};
		if (req->replay) {
			Tick t;
			ReplayStatus st = replay_next(&reader, &decode, &t, &cmds, &n, &want);
			if (st != REPLAY_OK) return fail("replay ended early", replay_status_name(st));
			if (t.v != tick.v) return fail("replay out of order", req->replay);
		} else {
			for (int k = 0; k < FACTION_COUNT; k++)
				bot_think(&bots[k], &sim, &decode, bot_cmds, &n, countof(bot_cmds));
		}
		sim_step(&sim, cmds, n);
		Hash got = sim_hash_now(&sim);
		if (req->replay && got.v != want.v) {
			fprintf(stderr, "clip: DIVERGED after tick %lld (recorded %016llx, got %016llx); "
			        "was the replay made by a different build?\n", (long long)tick.v,
			        (unsigned long long)want.v, (unsigned long long)got.v);
			return 1;
		}
		if (req->save_replay) replay_tick(&writer, tick, cmds, n, got);

		// The director warms up for three seconds before the window opens.
		if (sim.tick.v > from - 90) director_update(&director, &sim);
		if (sim.tick.v <= from) continue;

		int64_t span = to - from;
		bool sample = taken < frames &&
		              sim.tick.v >= from + 1 + (frames > 1 ? (span - 1) * taken / (frames - 1) : span - 1);
		if (!video && !sample) continue;

		Camera3D cam;
		if (req->camera) {
			CameraKey k = camera_track_at(&track, sim.tick.v);
			cam = render_camera(&sim.terrain, v2(k.focus_x, k.focus_z), k.distance);
		} else {
			cam = render_camera(&sim.terrain, director.focus, director.distance);
		}
		Sample now = sample_of(&sim);
		arena_reset(&frame);
		BeginTextureMode(target);
		ClearBackground((Color){24, 28, 34, 255});
		BeginMode3D(cam);
		renderer_draw_world(&renderer, &sim, 0.0f, &frame, nullptr);
		EndMode3D();
		DrawText(caption(name, &now), 16 * SUPERSAMPLE, rh - 30 * SUPERSAMPLE, 18 * SUPERSAMPLE,
		         (Color){230, 230, 230, 200});
		EndTextureMode();

		Image img = LoadImageFromTexture(target.texture);
		if (video) fwrite(img.data, 4, (size_t)rw * (size_t)rh, video);
		if (sample) {
			ImageFlipVertical(&img);
			ImageResize(&img, cw, ch);
			int cx = (taken % cols) * cw, cy = (taken / cols) * ch;
			ImageDraw(&sheet, img, (Rectangle){0, 0, (float)cw, (float)ch},
			          (Rectangle){(float)cx, (float)cy, (float)cw, (float)ch}, WHITE);
			const char *label = TextFormat("t%lld  %td v %td  shells %td", (long long)now.tick,
			                               now.units[0], now.units[1], now.shells);
			ImageDrawRectangle(&sheet, cx, cy, MeasureText(label, 20) + 12, 26, (Color){0, 0, 0, 170});
			ImageDrawText(&sheet, label, cx + 6, cy + 3, 20, RAYWHITE);
			samples[taken++] = now;
		}
		UnloadImage(img);
	}

	int status = 0;
	if (video && pclose(video) != 0) status = fail("ffmpeg failed for", req->video);
	if (frames) {
		if (!ExportImage(sheet, req->sheet)) status = fail("cannot write", req->sheet);
		UnloadImage(sheet);
		const char *stats = TextFormat("%.*s.txt", (int)(strlen(req->sheet) - 4), req->sheet);
		FILE *f = fopen(stats, "w");
		if (f) {
			fprintf(f, "source %s\nwindow %lld..%lld ticks (%.1f s)\ncamera %s\n\n", name,
			        (long long)from, (long long)to, (double)(to - from) / SIM_HZ,
			        req->camera ? req->camera : "director");
			fprintf(f, "%8s %6s %6s %7s %7s %7s  %s\n", "tick", "blue", "red", "lost_b", "lost_r",
			        "shells", "hash");
			for (int i = 0; i < taken; i++)
				fprintf(f, "%8lld %6td %6td %7lld %7lld %7td  %016llx\n", (long long)samples[i].tick,
				        samples[i].units[0], samples[i].units[1], (long long)samples[i].lost[0],
				        (long long)samples[i].lost[1], samples[i].shells,
				        (unsigned long long)samples[i].hash.v);
			fclose(f);
		}
	}
	if (req->save_replay) {
		Str bytes = replay_bytes(&writer);
		if (!platform_write_file(req->save_replay, bytes.ptr, bytes.len))
			status = fail("cannot write", req->save_replay);
	}
	if (status == 0 && req->video && req->clipboard) {
		char abs[4096];
		if (!platform_absolute_path(req->video, abs, sizeof abs) || !platform_clipboard_copy_file(abs))
			status = fail("could not copy to the clipboard", req->video);
		else
			printf("clip: %s is on the clipboard\n", abs);
	}
	if (req->video && status == 0) printf("clip: wrote %s (%lld frames)\n", req->video, (long long)(to - from));
	if (req->sheet && status == 0) printf("clip: wrote %s (%d frames)\n", req->sheet, taken);
	write_done(req->done);

	UnloadRenderTexture(target);
	renderer_unload(&renderer);
	sim_destroy(&sim);
	return status;
}

// ---------------------------------------------------------------- command line

static void usage(void)
{
	fprintf(stderr,
	        "usage: rts clip (--scenario NAME | --replay PATH) [options]\n"
	        "  --from TICK --to TICK    window (scenario default: its clip window;\n"
	        "                           replay default: the whole replay)\n"
	        "  --last SECONDS           window ends at --to (or the end), this long\n"
	        "  --out PATH.mp4           video\n"
	        "  --sheet PATH.png         contact sheet, plus PATH.txt stats\n"
	        "  --frames N               frames in the sheet (default 9)\n"
	        "  --camera PATH            follow a recorded camera track (default: director)\n"
	        "  --save-replay PATH       keep the run's replay (scenarios)\n"
	        "  --clipboard              copy the video to the system clipboard\n"
	        "  --size WxH               output size (default 1280x720)\n"
	        "  --done PATH              touch PATH when finished\n"
	        "  --list                   list scenarios\n");
}

int clip_main(int argc, char **argv)
{
	ClipRequest req = {.from = -1, .to = -1, .width = 1280, .height = 720, .sheet_frames = 9};
	for (int i = 2; i < argc; i++) {
		const char *a = argv[i];
		const char *v = i + 1 < argc ? argv[i + 1] : nullptr;
		bool takes = true;
		if (!strcmp(a, "--list")) {
			for (ptrdiff_t k = 0; k < scenario_count; k++)
				printf("%-10s %s\n", scenarios[k].name, scenarios[k].about);
			return 0;
		} else if (!strcmp(a, "--clipboard")) {
			req.clipboard = true;
			takes = false;
		} else if (!v) {
			usage();
			return 2;
		} else if (!strcmp(a, "--scenario")) {
			if (!(req.scenario = scenario_find(v))) return fail("unknown scenario (try --list)", v);
		} else if (!strcmp(a, "--replay")) req.replay = v;
		else if (!strcmp(a, "--from")) req.from = atoll(v);
		else if (!strcmp(a, "--to")) req.to = atoll(v);
		else if (!strcmp(a, "--last")) req.last_seconds = atof(v);
		else if (!strcmp(a, "--out")) req.video = v;
		else if (!strcmp(a, "--sheet")) req.sheet = v;
		else if (!strcmp(a, "--frames")) req.sheet_frames = atoi(v);
		else if (!strcmp(a, "--camera")) req.camera = v;
		else if (!strcmp(a, "--save-replay")) req.save_replay = v;
		else if (!strcmp(a, "--done")) req.done = v;
		else if (!strcmp(a, "--size")) {
			if (sscanf(v, "%dx%d", &req.width, &req.height) != 2 || req.width < 64 || req.height < 64
			    || req.width > 3840 || req.height > 2160)
				return fail("bad --size", v);
		} else {
			usage();
			return 2;
		}
		if (takes) i++;
	}
	if (!req.video && !req.sheet) {
		usage();
		return 2;
	}
	if (req.clipboard && !req.video) return fail("--clipboard needs --out", nullptr);

	SetTraceLogLevel(LOG_WARNING);
	SetConfigFlags(FLAG_WINDOW_HIDDEN);
	InitWindow(64, 64, "rts clip");
	int status = clip_run(&req);
	CloseWindow();
	return status;
}
