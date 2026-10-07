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
	if (n >= 12) {
		focus = v2_scale(sum, 1.0f / (float)n);
		float total = 0.0f;
		for (int f = 0; f < FACTION_COUNT; f++) {
			const SceneShells *p = &scene->shells[f];
			for (ptrdiff_t i = 0; i < p->count; i++) total += v2_len(v2_sub(v2(p->px[i], p->pz[i]), focus));
		}
		spread = total / (float)n;
	} else {
		V2 centre[FACTION_COUNT];
		for (int f = 0; f < FACTION_COUNT; f++) {
			const SceneUnits *u = &scene->units[f];
			V2 s = scene->base[f];
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
	sim_init(&sim, config);
	if (req->script) script_run_init(&script_run, &script, &work, stdout);
	Renderer renderer;
	renderer_init(&renderer, &sim.terrain, sim.config.max_units, &work);
	ReplayWriter writer;
	if (req->save_replay) replay_begin(&writer, &replay_out, config);
	ClipOut out;
	if (!out_open(&out, req)) return fail("cannot start ffmpeg for", req->video);
	int rw = out.w * SUPERSAMPLE, rh = out.h * SUPERSAMPLE;
	Sample *samples = PUSH(&work, Sample, out.frames > 0 ? out.frames : 1);

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

		bool sample = out_wants_sample(&out, sim.tick.v - from - 1, to - from);
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
		if (title[0]) draw_title_card(title, about, req->label, sim.tick.v - from, rw, rh);
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

int clip_netlog(const ClipRequest *req)
{
	Arena work = arena_create("clip netlog", (ptrdiff_t)2 << 30);
	Arena frame = arena_create("clip frame", (ptrdiff_t)256 << 20);
	Str file;
	if (!platform_read_file(&work, req->netlog, &file)) return fail("cannot read", req->netlog);
	NetlogReader nr;
	if (!netlog_open(&nr, file)) return fail("not a netlog", req->netlog);

	// The world comes from the first WELCOME; the window runs from the first
	// snapshot to the last record.
	SimConfig config = {0};
	V2 base[FACTION_COUNT] = {0};
	bool welcomed = false;
	double t_start = -1.0, t_end = 0.0;
	char info[256] = "";
	ptrdiff_t records = 0;
	for (NetlogReader scan = nr;;) {
		NetlogRecord rec;
		if (!netlog_next(&scan, &rec)) break;
		records++;
		t_end = rec.t;
		if (rec.kind == MSG_WELCOME && !welcomed) {
			ByteReader r = byte_reader(rec.data, rec.len);
			get32(&r);
			get8(&r);
			if (!wire_get_config(&r, &config)) return fail("bad welcome in", req->netlog);
			for (int f = 0; f < FACTION_COUNT; f++) base[f] = v2(getf(&r), getf(&r));
			welcomed = !r.bad;
		} else if (rec.kind == MSG_SNAPSHOT && t_start < 0.0) {
			t_start = rec.t;
		} else if (rec.kind == NETLOG_INFO && !info[0]) {
			snprintf(info, sizeof info, "%.*s", (int)rec.len, (const char *)rec.data);
		}
	}
	if (!welcomed || t_start < 0.0) return fail("netlog has no welcome and snapshot", req->netlog);

	Terrain terrain;
	terrain_generate(&terrain, &work, config.seed);
	Mirror mirror;
	mirror_init(&mirror, &work, config.max_units, config.max_projectiles);
	Renderer renderer;
	renderer_init(&renderer, &terrain, config.max_units, &work);
	ClipOut out;
	if (!out_open(&out, req)) return fail("cannot start ffmpeg for", req->video);
	int rw = out.w * SUPERSAMPLE, rh = out.h * SUPERSAMPLE, s = SUPERSAMPLE;
	Arrival *arrivals = PUSH(&work, Arrival, records);
	ptrdiff_t arrived = 0, window_start = 0;
	FILE *stats = open_stats(req);
	if (stats) fprintf(stats, "source %s\nclient %s\n\n%7s %6s %6s %6s %7s %8s  %s\n", req->netlog, info,
	                   "time", "tick", "blue", "red", "shells", "KB/s", "status");

	bool connected = true, have_mirror = false;
	double last_delta_t = 0.0, rtt = 0.0, status_since = t_start;
	char status[160] = "";
	int64_t digests = 0, mismatches = 0, rejoins = 0;
	Director director = {0};
	int64_t frames = (int64_t)((t_end - t_start) * CLIP_FPS) + 1;
	for (int64_t k = 0; k < frames; k++) {
		double t = t_start + (double)k / CLIP_FPS;
		NetlogRecord rec;
		for (NetlogReader peek = nr; netlog_next(&peek, &rec) && rec.t <= t; nr = peek) {
			arrivals[arrived++] = (Arrival){rec.t, rec.len};
			ByteReader r = byte_reader(rec.data, rec.len);
			if (rec.kind == MSG_SNAPSHOT || rec.kind == MSG_DELTA) {
				ApplyInfo ai;
				ApplyResult res = wire_apply(&mirror, &r, rec.kind == MSG_SNAPSHOT, &ai);
				if (res != APPLY_OK) {
					snprintf(status, sizeof status, "the client dropped a bad update");
					continue;
				}
				have_mirror = true;
				last_delta_t = rec.t;
				digests += ai.had_digest;
				mismatches += ai.had_digest && !ai.digest_matched;
				if (rec.kind == MSG_SNAPSHOT && rec.t > t_start) {
					snprintf(status, sizeof status, "rejoined: snapshot of %.1f KB at tick %lld",
					         (double)rec.len / 1024.0, (long long)mirror.tick);
					status_since = rec.t;
				}
			} else if (rec.kind == MSG_PONG && rec.len == 8) {
				double sent;
				memcpy(&sent, rec.data, sizeof sent);
				rtt = rec.t - sent;
			} else if (rec.kind == NETLOG_DISCONNECTED) {
				connected = false;
				snprintf(status, sizeof status, "connection lost (%.*s)", (int)rec.len, (const char *)rec.data);
				status_since = rec.t;
			} else if (rec.kind == NETLOG_CONNECTED) {
				rejoins += !connected;
				connected = true;
			}
		}
		while (window_start < arrived && arrivals[window_start].t < t - 1.0) window_start++;
		ptrdiff_t bytes = 0;
		for (ptrdiff_t i = window_start; i < arrived; i++) bytes += arrivals[i].bytes;
		if (!have_mirror) continue;

		arena_reset(&frame);
		Scene scene = scene_from_mirror(&mirror, &terrain, base, &frame);
		director_update(&director, &scene);
		float tick_s = SIM_DT * 1000.0f / (float)(mirror.dilation ? mirror.dilation : 1000);
		float alpha = connected ? f_clamp((float)(t - last_delta_t) / tick_s, 0.0f, 1.5f) : 0.0f;
		bool sample = out_wants_sample(&out, k, frames);
		if (!out.video && !sample) continue;

		out_begin(&out);
		BeginMode3D(render_camera(&terrain, director.focus, director.distance));
		renderer_draw_scene(&renderer, &scene, alpha, &frame, nullptr, -1);
		EndMode3D();
		// The client's network panel, as it saw it.
		DrawRectangle(10 * s, rh - 92 * s, 560 * s, 82 * s, (Color){0, 0, 0, 150});
		DrawText(TextFormat("client view: %s", info), 20 * s, rh - 84 * s, 18 * s, RAYWHITE);
		DrawText(TextFormat("tick %lld   down %.1f KB/s   rtt %.0f ms   time %.0f%%", (long long)mirror.tick,
		                    (double)bytes / 1024.0, rtt * 1e3, mirror.dilation / 10.0),
		         20 * s, rh - 60 * s, 18 * s, RAYWHITE);
		DrawText(TextFormat("digests %lld matched, %lld mismatched   rejoins %lld", (long long)(digests - mismatches),
		                    (long long)mismatches, (long long)rejoins),
		         20 * s, rh - 36 * s, 18 * s, mismatches ? RED : (Color){170, 230, 170, 255});
		if (!connected) DrawRectangle(0, 0, rw, rh, (Color){0, 0, 0, 110});
		if (status[0] && (!connected || t - status_since < 3.0)) {
			int w = MeasureText(status, 30 * s);
			DrawRectangle((rw - w) / 2 - 16 * s, rh / 2 - 26 * s, w + 32 * s, 52 * s, (Color){0, 0, 0, 170});
			DrawText(status, (rw - w) / 2, rh / 2 - 15 * s, 30 * s, connected ? (Color){170, 230, 170, 255} : RAYWHITE);
		}
		if (req->title && req->title[0])
			draw_title_card(req->title, req->about ? req->about : "", req->label, k, rw, rh);
		if (sample && stats)
			fprintf(stats, "%6.1fs %6lld %6td %6td %7td %8.1f  %s\n", t - t_start, (long long)mirror.tick,
			        scene.units[0].count, scene.units[1].count, scene.shells[0].count + scene.shells[1].count,
			        (double)bytes / 1024.0, connected ? "connected" : "disconnected");
		out_end(&out, sample, TextFormat("%.1fs  tick %lld  %s", t - t_start, (long long)mirror.tick,
		                                 connected ? "" : "DISCONNECTED"));
	}
	if (stats) {
		fprintf(stats, "\ndigests %lld\ndigest_mismatches %lld\nrejoins %lld\n", (long long)digests,
		        (long long)mismatches, (long long)rejoins);
		fclose(stats);
	}
	int status_code = out_close(&out, mismatches ? fail("the client's digests mismatched", req->netlog) : 0);
	renderer_unload(&renderer);
	return status_code;
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
	        "  --crf N                  video quality, 0..51, lower is bigger (default 26)\n"
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
		else if (!strcmp(a, "--title")) req.title = v;
		else if (!strcmp(a, "--about")) req.about = v;
		else if (!strcmp(a, "--label")) req.label = v;
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
