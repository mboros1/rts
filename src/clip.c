#include "clip.h"

#include <math.h>

#include "platform.h"
#include "replay.h"

static constexpr int SUPERSAMPLE = 2;   // render at 2x, encoder scales down
static constexpr int CLIP_FPS = 30;     // one frame per sim tick
// CRF 26 (the default) keeps ~10 s of 720p under 10 MB: thousands of tiny
// shells are costly to encode.

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

void director_update(Director *d, const Scene *scene)
{
	V2 sum = v2(0.0f, 0.0f);
	ptrdiff_t n = 0;
	for (int f = 0; f < FACTION_COUNT; f++) {
		const SceneShells *p = &scene->shells[f];
		for (ptrdiff_t i = 0; i < p->count; i++) sum = v2_add(sum, v2(p->px[i], p->pz[i]));
		n += p->count;
	}
	V2 focus;
	float spread;
	const SceneUnits *own = d->side >= 0 ? &scene->units[d->side] : nullptr;
	if (own && own->count) {
		// A player's view: its own units, spread as they are.
		V2 s = v2(0.0f, 0.0f);
		for (ptrdiff_t i = 0; i < own->count; i++) s = v2_add(s, v2(own->px[i], own->pz[i]));
		focus = v2_scale(s, 1.0f / (float)own->count);
		float total = 0.0f;
		for (ptrdiff_t i = 0; i < own->count; i++) total += v2_len(v2_sub(v2(own->px[i], own->pz[i]), focus));
		spread = total / (float)own->count;
	} else if (n >= 12) {
		focus = v2_scale(sum, 1.0f / (float)n);
		float total = 0.0f;
		for (int f = 0; f < FACTION_COUNT; f++) {
			const SceneShells *p = &scene->shells[f];
			for (ptrdiff_t i = 0; i < p->count; i++) total += v2_len(v2_sub(v2(p->px[i], p->pz[i]), focus));
		}
		spread = total / (float)n;
	} else {
		// Between the sides' centres; a side with nobody here doesn't count
		// (its base may be empty ground in a zone it has never reached).
		V2 centre[FACTION_COUNT];
		int present = 0;
		for (int f = 0; f < FACTION_COUNT; f++) {
			const SceneUnits *u = &scene->units[f];
			if (!u->count) continue;
			V2 s = v2(0.0f, 0.0f);
			for (ptrdiff_t i = 0; i < u->count; i++) s = v2_add(s, v2(u->px[i], u->pz[i]));
			centre[present++] = v2_scale(s, 1.0f / (float)u->count);
		}
		if (present == 0) {
			focus = v2_scale(v2_add(scene->base[0], scene->base[1]), 0.5f);
			spread = 0.5f * v2_len(v2_sub(scene->base[0], scene->base[1]));
		} else if (present == 1) {
			focus = centre[0];
			spread = 40.0f;
		} else {
			focus = v2_scale(v2_add(centre[0], centre[1]), 0.5f);
			spread = 0.5f * v2_len(v2_sub(centre[0], centre[1]));
		}
	}
	float distance = f_clamp(spread * 2.4f + 70.0f, 80.0f, 360.0f);
	if (!d->primed) {
		*d = (Director){focus, distance, true, d->side};
		return;
	}
	d->focus = v2_add(d->focus, v2_scale(v2_sub(focus, d->focus), 0.05f));
	d->distance += (distance - d->distance) * 0.03f;
}

// ---------------------------------------------------------------- output

// Where frames go: an x264 pipe, a contact sheet, or both. Frames render at
// SUPERSAMPLE x size for smooth edges, are box-filtered down on the GPU, and
// only the final size is read back: a quarter of the bytes across the bus.
typedef struct {
	const ClipRequest *req;
	int w, h;                        // output size
	RenderTexture2D big, small;
	FILE *video;
	int64_t video_frames;
	Image sheet;
	int frames, cols, cw, ch, taken;
} ClipOut;

static int fail(const char *msg, const char *detail);

static bool out_open(ClipOut *o, const ClipRequest *req)
{
	*o = (ClipOut){.req = req, .w = req->width, .h = req->height};
	o->big = LoadRenderTexture(o->w * SUPERSAMPLE, o->h * SUPERSAMPLE);
	o->small = LoadRenderTexture(o->w, o->h);
	// Bilinear at exactly half size averages each 2x2 block: a box filter.
	SetTextureFilter(o->big.texture, TEXTURE_FILTER_BILINEAR);
	if (req->video) {
		// Render textures read back bottom-up, hence vflip.
		if (strchr(req->video, '\'')) return false;
		const char *cmd = TextFormat(
			"ffmpeg -loglevel error -y -f rawvideo -pix_fmt rgba -s %dx%d -r %d -i - "
			"-vf vflip -c:v libx264 -preset veryfast -crf %d -pix_fmt yuv420p -movflags +faststart '%s'",
			o->w, o->h, CLIP_FPS, req->crf, req->video);
		if (!(o->video = popen(cmd, "w"))) return false;
	}
	o->frames = req->sheet ? (req->sheet_frames > 0 ? req->sheet_frames : 9) : 0;
	o->cols = o->frames <= 4 ? 2 : o->frames <= 9 ? 3 : 4;
	o->cw = 640;
	o->ch = 640 * o->h / o->w;
	if (o->frames) {
		int rows = (o->frames + o->cols - 1) / o->cols;
		o->sheet = GenImageColor(o->cols * o->cw, rows * o->ch, BLACK);
	}
	return true;
}

// Whether frame k of n should be in the sheet: evenly spaced, first to last.
static bool out_wants_sample(const ClipOut *o, int64_t k, int64_t n)
{
	if (o->taken >= o->frames) return false;
	int64_t at = o->frames > 1 ? (n - 1) * o->taken / (o->frames - 1) : n - 1;
	return k >= at;
}

static void out_begin(ClipOut *o)
{
	BeginTextureMode(o->big);
	ClearBackground((Color){24, 28, 34, 255});
}

// Finishes the frame drawn since out_begin; a sample also goes in the sheet.
static void out_end(ClipOut *o, bool sample, const char *label)
{
	EndTextureMode();
	if (!o->video && !sample) return;
	BeginTextureMode(o->small);
	DrawTexturePro(o->big.texture, (Rectangle){0, 0, (float)o->big.texture.width, (float)-o->big.texture.height},
	               (Rectangle){0, 0, (float)o->w, (float)o->h}, (Vector2){0, 0}, 0.0f, WHITE);
	EndTextureMode();
	Image img = LoadImageFromTexture(o->small.texture);
	if (o->video) {
		fwrite(img.data, 4, (size_t)o->w * (size_t)o->h, o->video);
		o->video_frames++;
	}
	if (sample) {
		ImageFlipVertical(&img);
		ImageResize(&img, o->cw, o->ch);
		int cx = (o->taken % o->cols) * o->cw, cy = (o->taken / o->cols) * o->ch;
		ImageDraw(&o->sheet, img, (Rectangle){0, 0, (float)o->cw, (float)o->ch},
		          (Rectangle){(float)cx, (float)cy, (float)o->cw, (float)o->ch}, WHITE);
		ImageDrawRectangle(&o->sheet, cx, cy, MeasureText(label, 20) + 12, 26, (Color){0, 0, 0, 170});
		ImageDrawText(&o->sheet, label, cx + 6, cy + 3, 20, RAYWHITE);
		o->taken++;
	}
	UnloadImage(img);
}

static bool write_done(const char *path)
{
	return !path || platform_write_file(path, "", 0);
}

// Closes the video, writes the sheet, fills the clipboard, touches done.
static int out_close(ClipOut *o, int status)
{
	const ClipRequest *req = o->req;
	if (o->video && pclose(o->video) != 0) status = fail("ffmpeg failed for", req->video);
	if (o->frames) {
		if (!ExportImage(o->sheet, req->sheet)) status = fail("cannot write", req->sheet);
		UnloadImage(o->sheet);
	}
	if (status == 0 && req->video && req->clipboard) {
		char abs[4096];
		if (!platform_absolute_path(req->video, abs, sizeof abs) || !platform_clipboard_copy_file(abs))
			status = fail("could not copy to the clipboard", req->video);
		else
			printf("clip: %s is on the clipboard\n", abs);
	}
	if (req->video && status == 0) printf("clip: wrote %s (%lld frames)\n", req->video, (long long)o->video_frames);
	if (req->sheet && status == 0) printf("clip: wrote %s (%d frames)\n", req->sheet, o->taken);
	write_done(req->done);
	UnloadRenderTexture(o->big);
	UnloadRenderTexture(o->small);
	return status;
}

// The stats file beside a contact sheet: PATH.png -> PATH.txt.
static FILE *open_stats(const ClipRequest *req)
{
	if (!req->sheet || strlen(req->sheet) < 4) return nullptr;
	return fopen(TextFormat("%.*s.txt", (int)(strlen(req->sheet) - 4), req->sheet), "w");
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

// The largest size up to `size` at which text fits in 92% of the width.
static int fit_text(const char *text, int size, int width)
{
	while (size > 8 && MeasureText(text, size) > width * 92 / 100) size--;
	return size;
}

// A title card for sharing: the first seconds of a clip say what it shows
// and which commit made it, then shrink to a corner label.
static void draw_title_card(const char *title, const char *about, const char *label,
                            int64_t frame, int w, int h)
{
	const int s = SUPERSAMPLE;
	const int64_t hold = 60, fade = 30;
	if (frame < hold + fade) {
		float k = frame < hold ? 1.0f : 1.0f - (float)(frame - hold) / (float)fade;
		unsigned char a = (unsigned char)(255.0f * k);
		DrawRectangle(0, 0, w, h, (Color){12, 14, 18, (unsigned char)(200.0f * k)});
		int ts = fit_text(title, 56 * s, w), as = fit_text(about, 24 * s, w), ls = 18 * s;
		DrawText(title, (w - MeasureText(title, ts)) / 2, h / 2 - ts, ts, (Color){245, 245, 245, a});
		if (about[0]) DrawText(about, (w - MeasureText(about, as)) / 2, h / 2 + 12 * s, as, (Color){200, 200, 200, a});
		if (label && label[0])
			DrawText(label, (w - MeasureText(label, ls)) / 2, h / 2 + 52 * s, ls, (Color){140, 160, 190, a});
		return;
	}
	const char *corner = label && label[0] ? TextFormat("%s   %s", title, label) : title;
	DrawRectangle(10 * s, 10 * s, MeasureText(corner, 20 * s) + 20 * s, 32 * s, (Color){0, 0, 0, 140});
	DrawText(corner, 20 * s, 16 * s, 20 * s, (Color){235, 235, 235, 230});
}

static int fail(const char *msg, const char *detail)
{
	fprintf(stderr, "clip: %s%s%s\n", msg, detail ? ": " : "", detail ? detail : "");
	return 1;
}

int clip_netlog(const ClipRequest *req);

int clip_run(const ClipRequest *req)
{
	if (req->netlog) return clip_netlog(req);
	Arena work = arena_create("clip", (ptrdiff_t)1 << 30);
	Arena decode = arena_create("clip decode", (ptrdiff_t)64 << 20);
	Arena frame = arena_create("clip frame", (ptrdiff_t)256 << 20);
	Arena replay_out = arena_create("clip replay", (ptrdiff_t)1 << 30);
	// Where the commands come from: a replay file, a script, or bots playing
	// a scenario.
	ReplayReader reader = {0};
	SimConfig config;
	Bot bots[FACTION_COUNT];
	Script script = {0};
	ScriptRun script_run = {0};
	const char *name;
	const char *title = "", *about = "";
	int64_t from = req->from, to = req->to;
	CameraTrack track = {0};
	if (req->script) {
		char err[512];
		if (!script_load(&script, &work, req->script, err, sizeof err)) return fail(err, nullptr);
		config = script.config;
		name = script.title[0] ? script.title : req->script;
		title = script.title;
		about = script.about;
		if (from < 0) from = script.clip_from;
		if (to < 0) to = script.clip_to;
		if (script.fixed_camera) {
			CameraKey *k = PUSH(&work, CameraKey, 1);
			*k = (CameraKey){0, script.camera_at.x, script.camera_at.z, script.camera_distance};
			track = (CameraTrack){k, 1};
		}
	} else if (req->replay) {
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
		if (!req->scenario) return fail("need --script, --replay or --scenario", nullptr);
		config = scenario_config(req->scenario);
		scenario_bots(req->scenario, bots);
		name = req->scenario->name;
		if (from < 0) from = req->scenario->clip_from;
		if (to < 0) to = req->scenario->clip_to;
	}
	if (req->last_seconds > 0.0) from = to - (int64_t)(req->last_seconds * SIM_HZ);
	if (from < 0) from = 0;
	if (to <= from) return fail("empty tick window", nullptr);

	if (req->camera && !camera_track_read(req->camera, &work, &track))
		return fail("cannot read camera track", req->camera);

	Sim sim;
	if (req->replay) {
		if (!replay_start(&reader, &sim)) return fail("the replay's snapshot doesn't load", req->replay);
		if (from < sim.tick.v) from = sim.tick.v; // a segment starts where it starts
	} else {
		sim_init(&sim, config);
	}
	if (req->script) script_run_init(&script_run, &script, &work, stdout);
	Renderer renderer;
	renderer_init(&renderer, &sim.terrain, sim.config.max_units, &work);
	ReplayWriter writer;
	if (req->save_replay) replay_begin(&writer, &replay_out, config);
	ClipOut out;
	if (!out_open(&out, req)) return fail("cannot start ffmpeg for", req->video);
	int rw = out.w * SUPERSAMPLE, rh = out.h * SUPERSAMPLE;
	Sample *samples = PUSH(&work, Sample, out.frames > 0 ? out.frames : 1);

	Director director = {.side = -1};
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
		} else if (req->script) {
			n = script_tick(&script_run, &sim, &decode, bot_cmds, countof(bot_cmds));
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
		Scene scene = scene_from_sim(&sim);
		if (sim.tick.v > from - 90) director_update(&director, &scene);
		if (sim.tick.v <= from) continue;

		int speed = req->speed > 1 ? req->speed : script.speed > 1 ? script.speed : 1;
		if ((sim.tick.v - from - 1) % speed != 0) continue;
		bool sample = out_wants_sample(&out, (sim.tick.v - from - 1) / speed, (to - from) / speed);
		if (!out.video && !sample) continue;

		Camera3D cam;
		if (track.count) {
			CameraKey k = camera_track_at(&track, sim.tick.v);
			cam = render_camera(&sim.terrain, v2(k.focus_x, k.focus_z), k.distance);
		} else {
			cam = render_camera(&sim.terrain, director.focus, director.distance);
		}
		Sample now = sample_of(&sim);
		arena_reset(&frame);
		out_begin(&out);
		BeginMode3D(cam);
		renderer_draw_scene(&renderer, &scene, 0.0f, &frame, nullptr, -1);
		EndMode3D();
		DrawText(caption(name, &now), 16 * SUPERSAMPLE, rh - 30 * SUPERSAMPLE, 18 * SUPERSAMPLE,
		         (Color){230, 230, 230, 200});
		if (title[0]) draw_title_card(title, about, req->label, (sim.tick.v - from) / speed, rw, rh);
		if (sample) samples[out.taken] = now;
		out_end(&out, sample, TextFormat("t%lld  %td v %td  shells %td", (long long)now.tick,
		                                 now.units[0], now.units[1], now.shells));
	}

	int status = 0;
	FILE *f = open_stats(req);
	if (f) {
		fprintf(f, "source %s\nwindow %lld..%lld ticks (%.1f s)\ncamera %s\n\n", name,
		        (long long)from, (long long)to, (double)(to - from) / SIM_HZ,
		        req->camera ? req->camera : track.count ? "fixed" : "director");
		fprintf(f, "%8s %6s %6s %7s %7s %7s  %s\n", "tick", "blue", "red", "lost_b", "lost_r", "shells", "hash");
		for (int i = 0; i < out.taken; i++)
			fprintf(f, "%8lld %6td %6td %7lld %7lld %7td  %016llx\n", (long long)samples[i].tick,
			        samples[i].units[0], samples[i].units[1], (long long)samples[i].lost[0],
			        (long long)samples[i].lost[1], samples[i].shells, (unsigned long long)samples[i].hash.v);
		fclose(f);
	}
	if (req->save_replay) {
		Str bytes = replay_bytes(&writer);
		if (!platform_write_file(req->save_replay, bytes.ptr, bytes.len))
			status = fail("cannot write", req->save_replay);
	}
	status = out_close(&out, status);
	renderer_unload(&renderer);
	sim_destroy(&sim);
	return status;
}


// ---------------------------------------------------------------- netlogs

typedef struct {
	double t;
	ptrdiff_t bytes;
} Arrival;

// One client's netlog, replayable to any moment: what that client knew then.
typedef struct {
	const char *path;
	NetlogReader nr;
	SimConfig config;
	V2 base[FACTION_COUNT];
	bool player;                 // a faction stream (simulated); else a mirror
	bool player_known;
	char info[256];
	double t_start, t_end;       // first snapshot, last record
	Terrain terrain;
	Mirror mirror;
	Remote remote;
	bool have_state, connected;
	double last_delta_t, rtt, status_since;
	char status[160];
	int64_t digests, mismatches, rejoins;
	Arrival *arrivals;
	ptrdiff_t arrived, window_start;
	// A journey (client.c --journey): the map between zones, zones changing.
	Arena *work;
	uint8_t faction;
	Directory dir;
	bool have_dir, on_map;
	bool launching;              // left the map: the next connection is a launch
	bool world_changed;          // a new zone: the renderer needs its terrain
	char zone[ZONE_NAME];
} Playback;

static bool playback_open(Playback *p, const char *path, Arena *work)
{
	*p = (Playback){.path = path, .connected = true, .t_start = -1.0, .work = work};
	Str file;
	if (!platform_read_file(work, path, &file) || !netlog_open(&p->nr, file)) return false;
	bool welcomed = false;
	ptrdiff_t records = 0;
	for (NetlogReader scan = p->nr;;) {
		NetlogRecord rec;
		if (!netlog_next(&scan, &rec)) break;
		records++;
		p->t_end = rec.t;
		if (rec.kind == MSG_WELCOME && !welcomed) {
			ByteReader r = byte_reader(rec.data, rec.len);
			if (get32(&r) != WIRE_VERSION) return false; // recorded by an older client
			get8(&r);
			if (!wire_get_config(&r, &p->config)) return false;
			for (int f = 0; f < FACTION_COUNT; f++) p->base[f] = v2(getf(&r), getf(&r));
			welcomed = !r.bad;
		} else if ((rec.kind == MSG_SNAPSHOT || rec.kind == MSG_FACTION_SNAPSHOT) && !p->player_known) {
			// What the log holds decides how to replay it, not the faction it
			// asked for: spike 2 logs are mirrors whoever recorded them.
			if (p->t_start < 0.0) p->t_start = rec.t;
			p->player = rec.kind == MSG_FACTION_SNAPSHOT;
			p->player_known = true;
		} else if (rec.kind == COORD_DIRECTORY && p->t_start < 0.0) {
			p->t_start = rec.t; // a journey can begin on the map
		} else if (rec.kind == NETLOG_INFO && !p->info[0]) {
			snprintf(p->info, sizeof p->info, "%.*s", (int)rec.len, (const char *)rec.data);
		}
	}
	if (!welcomed || !p->player_known) return false;
	terrain_generate(&p->terrain, work, p->config.seed);
	if (p->player) {
		// The faction is whichever the snapshot is for: read from WELCOME again.
		NetlogReader scan = p->nr;
		NetlogRecord rec;
		uint8_t faction = 0;
		while (netlog_next(&scan, &rec))
			if (rec.kind == MSG_WELCOME && rec.len > 4) {
				faction = rec.data[4];
				break;
			}
		if (faction >= FACTION_COUNT) return false;
		p->faction = faction;
		remote_init(&p->remote, &p->config, (FactionId){faction}, work);
	} else {
		mirror_init(&p->mirror, work, p->config.max_units, p->config.max_projectiles);
	}
	p->arrivals = PUSH(work, Arrival, records);
	return true;
}

static int64_t playback_tick(const Playback *p) { return p->player ? p->remote.sim.tick.v : p->mirror.tick; }
static uint16_t playback_dilation(const Playback *p) { return p->player ? p->remote.dilation : p->mirror.dilation; }

// Joining a zone: the same match carries on; another zone starts over on
// its own terrain.
static void playback_welcome(Playback *p, ByteReader *r)
{
	get32(r);
	get8(r);
	SimConfig config;
	V2 base[FACTION_COUNT];
	if (!wire_get_config(r, &config)) return;
	for (int f = 0; f < FACTION_COUNT; f++) base[f] = v2(getf(r), getf(r));
	if (r->bad) return;
	p->on_map = false;
	if (sim_config_equal(&config, &p->config)) return;
	p->config = config;
	memcpy(p->base, base, sizeof base);
	terrain_generate(&p->terrain, p->work, config.seed);
	if (p->player) {
		remote_destroy(&p->remote);
		remote_init(&p->remote, &config, (FactionId){p->faction}, p->work);
	} else {
		mirror_init(&p->mirror, p->work, config.max_units, config.max_projectiles);
	}
	p->have_state = false;
	p->world_changed = true;
}

// Apply every record up to time t.
static void playback_advance(Playback *p, double t)
{
	NetlogRecord rec;
	for (NetlogReader peek = p->nr; netlog_next(&peek, &rec) && rec.t <= t; p->nr = peek) {
		p->arrivals[p->arrived++] = (Arrival){rec.t, rec.len};
		ByteReader r = byte_reader(rec.data, rec.len);
		bool snapshot = rec.kind == MSG_SNAPSHOT || rec.kind == MSG_FACTION_SNAPSHOT;
		bool update = snapshot || rec.kind == MSG_DELTA || rec.kind == MSG_FACTION_TICK;
		if (update && p->player == (rec.kind == MSG_FACTION_SNAPSHOT || rec.kind == MSG_FACTION_TICK)) {
			bool ok, had_digest = false, matched = true;
			if (p->player) {
				RemoteResult res = snapshot ? (remote_snapshot(&p->remote, &r) ? REMOTE_OK : REMOTE_BAD)
				                 : p->remote.ready ? remote_tick(&p->remote, &r, &had_digest) : REMOTE_GAP;
				ok = res == REMOTE_OK || res == REMOTE_DIVERGED;
				matched = res != REMOTE_DIVERGED;
			} else {
				ApplyInfo ai;
				ok = wire_apply(&p->mirror, &r, snapshot, &ai) == APPLY_OK;
				had_digest = ai.had_digest;
				matched = !ai.had_digest || ai.digest_matched;
			}
			if (!ok) continue;
			p->have_state = true;
			p->last_delta_t = rec.t;
			p->digests += had_digest;
			p->mismatches += !matched;
			if (snapshot && p->launching) {
				snprintf(p->status, sizeof p->status, "launched into %s", p->zone[0] ? p->zone : "a zone");
				p->status_since = rec.t;
				p->launching = false;
			} else if (snapshot && rec.t > p->t_start) {
				snprintf(p->status, sizeof p->status, "rejoined: %ssnapshot of %.1f KB at tick %lld",
				         p->player ? "faction " : "", (double)rec.len / 1024.0, (long long)playback_tick(p));
				p->status_since = rec.t;
			}
		} else if (rec.kind == MSG_PONG && rec.len == 8) {
			double sent;
			memcpy(&sent, rec.data, sizeof sent);
			p->rtt = rec.t - sent;
		} else if (rec.kind == MSG_WELCOME) {
			playback_welcome(p, &r);
		} else if (rec.kind == COORD_DIRECTORY) {
			Directory d;
			if (get_directory(&r, &d)) {
				p->dir = d;
				p->have_dir = true;
			}
		} else if (rec.kind == NETLOG_MAP) {
			p->on_map = true;
			p->launching = true;
			p->connected = true;    // not lost: looking at the map
		} else if (rec.kind == NETLOG_INFO && rec.len > 5 && !memcmp(rec.data, "zone ", 5)) {
			snprintf(p->zone, sizeof p->zone, "%.*s", (int)(rec.len - 5), (const char *)rec.data + 5);
		} else if (rec.kind == NETLOG_DISCONNECTED) {
			p->connected = false;
			snprintf(p->status, sizeof p->status, "connection lost (%.*s)", (int)rec.len, (const char *)rec.data);
			p->status_since = rec.t;
		} else if (rec.kind == NETLOG_CONNECTED) {
			p->rejoins += !p->connected && !p->launching;
			p->connected = true;
		}
	}
	while (p->window_start < p->arrived && p->arrivals[p->window_start].t < t - 1.0) p->window_start++;
}

static double playback_rate(const Playback *p)
{
	ptrdiff_t bytes = 0;
	for (ptrdiff_t i = p->window_start; i < p->arrived; i++) bytes += p->arrivals[i].bytes;
	return (double)bytes;
}

static Scene playback_scene(const Playback *p, Arena *frame)
{
	return p->player ? remote_scene(&p->remote, p->base, frame) : scene_from_mirror(&p->mirror, &p->terrain, p->base, frame);
}

// Draws the view into the current render target, with its network panel:
// what this client saw at time t.
static void playback_draw(Playback *p, Renderer *renderer, const Scene *scene, Camera3D cam, double t,
                          Arena *frame, int w, int h, const char *heading)
{
	const int s = SUPERSAMPLE;
	uint16_t dilation = playback_dilation(p);
	float tick_s = SIM_DT * 1000.0f / (float)(dilation ? dilation : 1000);
	float alpha = p->connected ? f_clamp((float)(t - p->last_delta_t) / tick_s, 0.0f, 1.5f) : 0.0f;
	if (p->player) {
		uint8_t *visible = PUSH(frame, uint8_t, (ptrdiff_t)(p->terrain.cells_x + 1) * (p->terrain.cells_z + 1));
		remote_fog(&p->remote, visible);
		renderer_set_fog(renderer, visible);
	} else {
		renderer_set_fog(renderer, nullptr);
	}
	BeginMode3D(cam);
	renderer_draw_scene(renderer, scene, alpha, frame, nullptr, -1);
	EndMode3D();
	if (heading) {
		int hw = MeasureText(heading, 22 * s);
		DrawRectangle((w - hw) / 2 - 12 * s, 56 * s, hw + 24 * s, 34 * s, (Color){0, 0, 0, 160});
		DrawText(heading, (w - hw) / 2, 62 * s, 22 * s, RAYWHITE);
	}
	// The panel is laid out for 600 px and scales down to fit a half frame.
	float k = f_min(1.0f, (float)w / (float)(600 * s));
	int u = (int)((float)s * k * 100.0f); // hundredths of a layout pixel
#define P(x) ((x) * u / 100)
	DrawRectangle(P(10), h - P(92), P(560), P(82), (Color){0, 0, 0, 150});
	DrawText(TextFormat("client view: %s", p->info), P(20), h - P(84), P(18), RAYWHITE);
	DrawText(TextFormat("tick %lld   down %.1f KB/s   rtt %.0f ms   time %.0f%%%s", (long long)playback_tick(p),
	                    playback_rate(p) / 1024.0, p->rtt * 1e3, dilation / 10.0, p->player ? "   simulating locally" : ""),
	         P(20), h - P(60), P(18), RAYWHITE);
	DrawText(TextFormat("%s %lld matched, %lld mismatched   rejoins %lld", p->player ? "faction hashes" : "digests",
	                    (long long)(p->digests - p->mismatches), (long long)p->mismatches, (long long)p->rejoins),
	         P(20), h - P(36), P(18), p->mismatches ? RED : (Color){170, 230, 170, 255});
#undef P
	if (!p->connected) DrawRectangle(0, 0, w, h, (Color){0, 0, 0, 110});
	if (p->status[0] && (!p->connected || t - p->status_since < 3.0)) {
		int sw = MeasureText(p->status, 26 * s);
		DrawRectangle((w - sw) / 2 - 16 * s, h / 2 - 24 * s, sw + 32 * s, 48 * s, (Color){0, 0, 0, 170});
		DrawText(p->status, (w - sw) / 2, h / 2 - 13 * s, 26 * s, p->connected ? (Color){170, 230, 170, 255} : RAYWHITE);
	}
}

static const char *playback_heading(const Playback *p)
{
	if (!p->player) return "everything (spectator)";
	return p->remote.faction.v == 0 ? "what blue knows" : "what red knows";
}

// One netlog, or two side by side on a shared clock and camera (--compare):
// say, a player's fogged view beside a spectator's full one.
int clip_netlog(const ClipRequest *req)
{
	Arena work = arena_create("clip netlog", (ptrdiff_t)4 << 30);
	Arena frame = arena_create("clip frame", (ptrdiff_t)256 << 20);
	Playback views[2];
	int count = req->compare ? 2 : 1;
	const char *paths[2] = {req->netlog, req->compare};
	for (int k = 0; k < count; k++)
		if (!playback_open(&views[k], paths[k], &work)) return fail("cannot replay netlog", paths[k]);
	if (count == 2 && !sim_config_equal(&views[0].config, &views[1].config))
		return fail("netlogs are from different matches", req->compare);

	double t_start = views[0].t_start, t_end = views[0].t_end;
	for (int k = 1; k < count; k++) {
		t_start = views[k].t_start > t_start ? views[k].t_start : t_start;
		t_end = views[k].t_end < t_end ? views[k].t_end : t_end;
	}
	Renderer renderer;
	renderer_init(&renderer, &views[0].terrain, views[0].config.max_units, &work);
	ClipOut out;
	if (!out_open(&out, req)) return fail("cannot start ffmpeg for", req->video);
	int rw = out.w * SUPERSAMPLE, rh = out.h * SUPERSAMPLE;
	// Each half renders to its own target, then both are placed in the frame.
	RenderTexture2D half = count == 2 ? LoadRenderTexture(rw / 2, rh) : (RenderTexture2D){0};
	if (count == 2) SetTextureFilter(half.texture, TEXTURE_FILTER_BILINEAR);
	FILE *stats = open_stats(req);
	if (stats) {
		for (int k = 0; k < count; k++) fprintf(stats, "source %s: %s, %s\n", paths[k], views[k].info, playback_heading(&views[k]));
		fprintf(stats, "\n%7s %6s %6s %6s %7s %8s  %s\n", "time", "tick", "blue", "red", "shells", "KB/s", "status");
	}

	// Two halves may be one match seen two ways (shared camera, following the
	// fuller view) or two zones (--views: a fixed camera each).
	char headings[2][64] = {"", ""};
	if (req->headings) {
		const char *comma = strchr(req->headings, ',');
		if (!comma) return fail("--headings A,B", req->headings);
		snprintf(headings[0], sizeof headings[0], "%.*s", (int)(comma - req->headings), req->headings);
		snprintf(headings[1], sizeof headings[1], "%s", comma + 1);
	}
	bool fixed = req->views != nullptr;
	V2 view_at[2] = {0};
	float view_d[2] = {0};
	if (fixed && sscanf(req->views, "%f,%f,%f;%f,%f,%f", &view_at[0].x, &view_at[0].z, &view_d[0], &view_at[1].x,
	                    &view_at[1].z, &view_d[1]) != 6)
		return fail("--views X,Z,D;X,Z,D", req->views);

	Director director = {.side = count == 1 && views[0].player ? views[0].faction : -1};
	int64_t frames = (int64_t)((t_end - t_start) * CLIP_FPS) + 1;
	for (int64_t k = 0; k < frames; k++) {
		double t = t_start + (double)k / CLIP_FPS;
		for (int v = 0; v < count; v++) playback_advance(&views[v], t);
		if (views[0].world_changed) {
			// Into another zone: its terrain, and a camera that finds its way again.
			renderer_unload(&renderer);
			renderer_init(&renderer, &views[0].terrain, views[0].config.max_units, &work);
			views[0].world_changed = false;
			director = (Director){.side = director.side};
		}
		bool map = count == 1 && views[0].on_map && views[0].have_dir;
		if (!map && (!views[0].have_state || (count == 2 && !views[1].have_state))) continue;
		arena_reset(&frame);
		Scene scenes[2] = {0};
		if (!map) {
			for (int v = 0; v < count; v++) scenes[v] = playback_scene(&views[v], &frame);
			// Frame the whole picture when there is one to compare against.
			director_update(&director, &scenes[count - 1]);
		}
		bool sample = out_wants_sample(&out, k, frames);
		if (!out.video && !sample) continue;
		Camera3D cam = render_camera(&views[0].terrain, director.focus, director.distance);

		if (map) {
			out_begin(&out);
			map_draw(&views[0].dir, views[0].player ? views[0].faction : -1, -1, rw, rh);
		} else if (count == 1) {
			out_begin(&out);
			playback_draw(&views[0], &renderer, &scenes[0], cam, t, &frame, rw, rh, nullptr);
		} else {
			out_begin(&out);
			EndTextureMode();
			for (int v = 0; v < 2; v++) {
				BeginTextureMode(half);
				ClearBackground((Color){24, 28, 34, 255});
				Camera3D half_cam = fixed ? render_camera(&views[v].terrain, view_at[v], view_d[v]) : cam;
				playback_draw(&views[v], &renderer, &scenes[v], half_cam, t, &frame, rw / 2, rh,
				              headings[v][0] ? headings[v] : playback_heading(&views[v]));
				EndTextureMode();
				BeginTextureMode(out.big);
				DrawTexturePro(half.texture, (Rectangle){0, 0, (float)(rw / 2), (float)-rh},
				               (Rectangle){(float)(v * rw / 2), 0, (float)(rw / 2), (float)rh}, (Vector2){0, 0}, 0.0f, WHITE);
				EndTextureMode();
			}
			BeginTextureMode(out.big);
			DrawRectangle(rw / 2 - SUPERSAMPLE, 0, 2 * SUPERSAMPLE, rh, (Color){12, 14, 18, 255});
		}
		if (req->title && req->title[0])
			draw_title_card(req->title, req->about ? req->about : "", req->label, k, rw, rh);
		const Playback *p = &views[0];
		if (sample && stats)
			fprintf(stats, "%6.1fs %6lld %6td %6td %7td %8.1f  %s\n", t - t_start, (long long)playback_tick(p),
			        scenes[0].units[0].count, scenes[0].units[1].count,
			        scenes[0].shells[0].count + scenes[0].shells[1].count, playback_rate(p) / 1024.0,
			        p->connected ? "connected" : "disconnected");
		out_end(&out, sample, map ? TextFormat("%.1fs  strategic map", t - t_start)
		                          : TextFormat("%.1fs  %s%stick %lld  %s", t - t_start, p->zone[0] ? p->zone : "",
		                                       p->zone[0] ? "  " : "", (long long)playback_tick(p),
		                                       p->connected ? "" : "DISCONNECTED"));
	}
	int64_t mismatches = 0;
	if (stats) {
		for (int v = 0; v < count; v++)
			fprintf(stats, "\n%s: digests %lld, mismatches %lld, rejoins %lld", paths[v], (long long)views[v].digests,
			        (long long)views[v].mismatches, (long long)views[v].rejoins);
		fprintf(stats, "\n");
		fclose(stats);
	}
	for (int v = 0; v < count; v++) mismatches += views[v].mismatches;
	if (count == 2) UnloadRenderTexture(half);
	int status = out_close(&out, mismatches ? fail("a client's hashes mismatched", req->netlog) : 0);
	renderer_unload(&renderer);
	for (int v = 0; v < count; v++)
		if (views[v].player) remote_destroy(&views[v].remote);
	return status;
}

// ---------------------------------------------------------------- command line

static void usage(void)
{
	fprintf(stderr,
	        "usage: rts clip (--script PATH | --scenario NAME | --replay PATH | --netlog PATH) [options]\n"
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
	        "  --label TEXT             title card subtitle, e.g. the commit\n"
	        "  --title TEXT --about TEXT  title card for --netlog clips\n"
	        "  --compare NETLOG         a second netlog beside the first, same clock and camera\n"
	        "  --headings A,B           labels for the two halves\n"
	        "  --views X,Z,D;X,Z,D      a fixed camera for each half (two zones, say)\n"
	        "  --crf N                  video quality, 0..51, lower is bigger (default 26)\n"
	        "  --speed N                time-lapse: one frame every N ticks\n"
	        "  --list                   list scenarios\n");
}

int clip_main(int argc, char **argv)
{
	ClipRequest req = {.from = -1, .to = -1, .width = 1280, .height = 720, .sheet_frames = 9, .crf = 26};
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
		else if (!strcmp(a, "--script")) req.script = v;
		else if (!strcmp(a, "--netlog")) req.netlog = v;
		else if (!strcmp(a, "--compare")) req.compare = v;
		else if (!strcmp(a, "--headings")) req.headings = v;
		else if (!strcmp(a, "--views")) req.views = v;
		else if (!strcmp(a, "--title")) req.title = v;
		else if (!strcmp(a, "--about")) req.about = v;
		else if (!strcmp(a, "--label")) req.label = v;
		else if (!strcmp(a, "--speed")) req.speed = atoi(v);
		else if (!strcmp(a, "--crf")) {
			req.crf = atoi(v);
			if (req.crf < 0 || req.crf > 51) return fail("--crf is 0..51", v);
		}
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
