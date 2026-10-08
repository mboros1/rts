#include "map.h"

#include <raylib.h>

static const Color MAP_BG = {18, 22, 28, 255};
static const Color MAP_LINK = {70, 80, 96, 255};
static const Color MAP_TEXT = {225, 230, 236, 255};
static const Color MAP_DIM = {120, 128, 140, 255};

static Color side_color(int f) { return f == 0 ? (Color){70, 130, 230, 255} : (Color){220, 70, 60, 255}; }

// Laid out for a 900 px tall window; everything scales with the height.
static float unit(int h) { return (float)h / 900.0f; }

// Zones sit inside a margin; positions are 0..1 across it.
static Vector2 zone_at(const DirectoryZone *z, int w, int h)
{
	float mx = 190.0f * unit(h), my = 190.0f * unit(h);
	return (Vector2){mx + z->x * ((float)w - 2.0f * mx), my + z->y * ((float)h - 2.0f * my)};
}

static float zone_radius(int h) { return 100.0f * unit(h); }

int map_pick(const Directory *d, float x, float y, int w, int h)
{
	float r = zone_radius(h);
	for (int k = 0; k < d->zone_count; k++) {
		Vector2 c = zone_at(&d->zones[k], w, h);
		if ((c.x - x) * (c.x - x) + (c.y - y) * (c.y - y) <= r * r) return k;
	}
	return -1;
}

// How much of a zone each side holds: units count once, structures twice.
static float strength(const ZoneStatus *st, int f) { return (float)st->units[f] + 2.0f * (float)st->structures[f]; }

static void draw_zone(const DirectoryZone *z, Vector2 c, bool hover, int h)
{
	float u = unit(h), r = zone_radius(h);
	float blue = strength(&z->status, 0), red = strength(&z->status, 1), total = blue + red;
	// The disc shows the balance: blue's share fills from the left.
	Color fill = {44, 50, 60, 255};
	if (z->up && z->reported && total > 0.0f) {
		float b = blue / total;
		Color bc = side_color(0), rc = side_color(1);
		fill = (Color){(unsigned char)((float)bc.r * b + (float)rc.r * (1.0f - b)),
		               (unsigned char)((float)bc.g * b + (float)rc.g * (1.0f - b)),
		               (unsigned char)((float)bc.b * b + (float)rc.b * (1.0f - b)), 255};
		fill = ColorBrightness(fill, -0.35f);
	}
	DrawCircleV(c, r, fill);
	Color edge = z->home >= 0 ? side_color(z->home) : MAP_LINK;
	DrawRing(c, r - 3.0f * u, r + (z->home >= 0 ? 3.0f : 1.0f) * u, 0, 360, 48, edge);
	if (hover) DrawRing(c, r + 6.0f * u, r + 9.0f * u, 0, 360, 48, RAYWHITE);

	int fs = (int)(38.0f * u), small = (int)(26.0f * u), px = (int)(u + 0.5f);
	int nw = MeasureText(z->name, fs);
	DrawText(z->name, (int)c.x - nw / 2, (int)(c.y - r) - fs - 10 * px, fs, MAP_TEXT);
	if (z->home >= 0) {
		const char *home = z->home == 0 ? "blue's home" : "red's home";
		int hw = MeasureText(home, small);
		DrawText(home, (int)c.x - hw / 2, (int)(c.y - r) - fs - small - 14 * px, small, side_color(z->home));
	}
	if (!z->up) {
		int dw = MeasureText("down", fs);
		DrawText("down", (int)c.x - dw / 2, (int)c.y - fs / 2, fs, MAP_DIM);
		return;
	}
	if (!z->reported) {
		int dw = MeasureText("...", fs);
		DrawText("...", (int)c.x - dw / 2, (int)c.y - fs / 2, fs, MAP_DIM);
		return;
	}
	for (int f = 0; f < FACTION_COUNT; f++) {
		const char *line = TextFormat("%d units  %d bldg", z->status.units[f], z->status.structures[f]);
		int lw = MeasureText(line, small);
		DrawText(line, (int)c.x - lw / 2, (int)(c.y + r) + 8 * px + f * (small + 4 * px), small, side_color(f));
		// Transporters: a marker on the rim, blue's on the left, red's on the right.
		int n = z->status.transporters[f];
		if (n == 0) continue;
		Vector2 m = {c.x + (f == 0 ? -r : r), c.y};
		float k = 14.0f * u;
		DrawPoly(m, 4, k, 45.0f, side_color(f));
		DrawPolyLines(m, 4, k, 45.0f, RAYWHITE);
		if (n > 1) {
			const char *count = TextFormat("%d", n);
			DrawText(count, (int)m.x - MeasureText(count, small) / 2, (int)m.y - small / 2, small, RAYWHITE);
		}
	}
	// The zone's own clock: how long it has run.
	int64_t secs = z->status.tick / (int64_t)SIM_HZ;
	const char *clock = secs >= 3600 ? TextFormat("%lldh %02lldm", (long long)(secs / 3600), (long long)(secs / 60 % 60))
	                                 : TextFormat("%lldm %02llds", (long long)(secs / 60), (long long)(secs % 60));
	int cw = MeasureText(clock, small);
	DrawText(clock, (int)c.x - cw / 2, (int)c.y - small / 2, small, MAP_TEXT);
}

void map_draw(const Directory *d, int viewer, int hover, int w, int h)
{
	float u = unit(h);
	int px = (int)(u + 0.5f);
	DrawRectangle(0, 0, w, h, MAP_BG);
	for (int k = 0; k < d->link_count; k++) {
		Vector2 a = zone_at(&d->zones[d->links[k][0]], w, h), b = zone_at(&d->zones[d->links[k][1]], w, h);
		DrawLineEx(a, b, 4.0f * u, MAP_LINK);
	}
	// Units in transit: a marker on the way from source to destination.
	for (int k = 0; k < d->transit_count; k++) {
		const DirectoryTransit *t = &d->transit[k];
		Vector2 a = zone_at(&d->zones[t->src], w, h), b = zone_at(&d->zones[t->dst], w, h);
		float side = t->faction == 0 ? -1.0f : 1.0f; // blue above the line, red below
		Vector2 m = {a.x + (b.x - a.x) * 0.5f, a.y + (b.y - a.y) * 0.5f + side * 18.0f * u};
		DrawCircleV(m, 9.0f * u, side_color(t->faction));
		const char *n = TextFormat("%d %s %s", t->units, "->", d->zones[t->dst].name);
		DrawText(n, (int)m.x + 14 * px, (int)m.y - 8 * px, 16 * px, side_color(t->faction));
	}
	for (int k = 0; k < d->zone_count; k++) draw_zone(&d->zones[k], zone_at(&d->zones[k], w, h), k == hover, h);

	// Title centred at the top, help along the bottom.
	int ts = (int)(40.0f * u), hs = (int)(20.0f * u);
	const char *title = "strategic map";
	DrawText(title, (w - MeasureText(title, ts)) / 2, (int)(48.0f * u), ts, MAP_TEXT);
	const char *who = viewer == 0 ? "you are blue" : viewer == 1 ? "you are red" : "spectating";
	const char *help = TextFormat("%s   click a zone to launch in   esc: quit   (squares: transporters)", who);
	DrawText(help, (w - MeasureText(help, hs)) / 2, h - (int)(56.0f * u), hs, MAP_DIM);
	if (d->link_count == 0 && d->zone_count > 1) {
		const char *note = "(no world file: zones shown without links)";
		DrawText(note, (w - MeasureText(note, hs)) / 2, (int)(100.0f * u), hs, MAP_DIM);
	}
}
