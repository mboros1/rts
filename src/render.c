#include "render.h"

#include <math.h>
#include <rlgl.h>

static const char *UNIT_VS =
	"#version 330\n"
	"in vec3 vertexPosition;\n"
	"in vec3 vertexNormal;\n"
	"in vec4 vertexColor;\n"
	"in mat4 instanceTransform;\n"
	"uniform mat4 mvp;\n"
	"out float shade;\n"
	"out vec4 tone;\n"
	"void main() {\n"
	"    vec3 n = normalize(mat3(instanceTransform) * vertexNormal);\n"
	"    shade = 0.45 + 0.55 * max(dot(n, normalize(vec3(0.4, 1.0, 0.3))), 0.0);\n"
	"    tone = vertexColor;\n"
	"    gl_Position = mvp * instanceTransform * vec4(vertexPosition, 1.0);\n"
	"}\n";

static const char *UNIT_FS =
	"#version 330\n"
	"in float shade;\n"
	"in vec4 tone;\n"
	"uniform vec4 colDiffuse;\n"
	"out vec4 finalColor;\n"
	"// tone.a: how much of the side's colour (1: the body; 0: metal, lights).\n"
	"void main() { finalColor = vec4(mix(tone.rgb, colDiffuse.rgb * tone.rgb, tone.a) * shade, colDiffuse.a); }\n";

static const Color FACTION_COLOR[FACTION_COUNT] = {{70, 140, 235, 255}, {225, 80, 60, 255}};

// Each player in a shade of their side's colour: the first in the side's own,
// the rest turned toward neighbouring hues, lighter and darker in turn.
Color render_player_color(int faction, PlayerSlot p)
{
	static const Color shades[FACTION_COUNT][MAX_PLAYERS] = {
		{{70, 140, 235, 255}, {60, 205, 215, 255}, {135, 110, 240, 255}, {40, 95, 175, 255},
		 {120, 200, 255, 255}, {95, 160, 150, 255}, {175, 150, 255, 255}, {30, 150, 205, 255}},
		{{225, 80, 60, 255}, {240, 160, 50, 255}, {215, 60, 140, 255}, {165, 50, 40, 255},
		 {255, 130, 110, 255}, {200, 120, 70, 255}, {245, 110, 190, 255}, {230, 205, 80, 255}},
	};
	return shades[faction][p.v];
}
static const Color SELECTED_COLOR = {120, 255, 140, 255};
static const Color SHELL_COLOR = {255, 220, 120, 255};

// The terrain mesh is built from the sim's own heightfield, so units sit
// exactly on what they're drawn on. Lighting is baked into vertex colours.
enum { TERRAIN_CHUNK = 128 };

// One sample's lit colour, from its height and the slope around it.
static void terrain_sample_color(const Terrain *t, int32_t i, int32_t j, unsigned char out[4], float normal[3])
{
	int32_t sx = t->cells_x + 1, sz = t->cells_z + 1, v = j * sx + i;
	float h = t->heights[v];
	float hl = t->heights[j * sx + (i > 0 ? i - 1 : i)];
	float hr = t->heights[j * sx + (i < sx - 1 ? i + 1 : i)];
	float hd = t->heights[(j > 0 ? j - 1 : j) * sx + i];
	float hu = t->heights[(j < sz - 1 ? j + 1 : j) * sx + i];
	float nx = hl - hr, ny = 2.0f * t->cell, nz = hd - hu;
	float nl = sqrtf(nx * nx + ny * ny + nz * nz);
	nx /= nl, ny /= nl, nz /= nl;
	normal[0] = nx, normal[1] = ny, normal[2] = nz;
	float k = h / (t->max_height > 0.0f ? t->max_height : 1.0f);
	float light = 0.5f + 0.5f * fmaxf(0.0f, nx * 0.37f + ny * 0.86f + nz * 0.33f);
	float r = (60.0f + 70.0f * k) * light, g = (95.0f + 60.0f * k) * light, b = (55.0f + 40.0f * k) * light;
	out[0] = (unsigned char)fminf(r, 255.0f);
	out[1] = (unsigned char)fminf(g, 255.0f);
	out[2] = (unsigned char)fminf(b, 255.0f);
	out[3] = 255;
}

// The cells [i0, i0 + w) x [j0, j0 + h) as a mesh.
static Model build_terrain_chunk(const Terrain *t, int32_t i0, int32_t j0, int32_t w, int32_t h)
{
	int32_t sx = w + 1, sz = h + 1;
	Mesh m = {0};
	m.vertexCount = sx * sz;
	m.triangleCount = 2 * w * h;
	m.vertices = MemAlloc((unsigned)(m.vertexCount * 3) * sizeof(float));
	m.normals = MemAlloc((unsigned)(m.vertexCount * 3) * sizeof(float));
	m.colors = MemAlloc((unsigned)(m.vertexCount * 4));
	m.indices = MemAlloc((unsigned)(m.triangleCount * 3) * sizeof(unsigned short));
	ASSERT(m.vertexCount <= 65535);
	for (int32_t j = 0; j < sz; j++)
		for (int32_t i = 0; i < sx; i++) {
			int32_t v = j * sx + i, gi = i0 + i, gj = j0 + j;
			m.vertices[v * 3 + 0] = (float)gi * t->cell;
			m.vertices[v * 3 + 1] = t->heights[gj * (t->cells_x + 1) + gi];
			m.vertices[v * 3 + 2] = (float)gj * t->cell;
			terrain_sample_color(t, gi, gj, m.colors + v * 4, m.normals + v * 3);
		}
	int32_t k = 0;
	for (int32_t j = 0; j < h; j++)
		for (int32_t i = 0; i < w; i++) {
			unsigned short a = (unsigned short)(j * sx + i), b = (unsigned short)(a + 1);
			unsigned short c = (unsigned short)(a + sx), d = (unsigned short)(c + 1);
			unsigned short tri[6] = {a, c, b, b, c, d};
			for (int q = 0; q < 6; q++) m.indices[k++] = tri[q];
		}
	UploadMesh(&m, false);
	return LoadModelFromMesh(m);
}

static void build_terrain(Renderer *r, const Terrain *t, Arena *program)
{
	r->terrain_chunks = 0;
	r->samples_x = t->cells_x + 1;
	for (int32_t j0 = 0; j0 < t->cells_z; j0 += TERRAIN_CHUNK)
		for (int32_t i0 = 0; i0 < t->cells_x; i0 += TERRAIN_CHUNK) {
			int32_t c = r->terrain_chunks++;
			ASSERT(c < (int32_t)countof(r->terrain));
			r->chunk_i[c] = i0;
			r->chunk_j[c] = j0;
			r->chunk_w[c] = t->cells_x - i0 < TERRAIN_CHUNK ? t->cells_x - i0 : TERRAIN_CHUNK;
			r->chunk_h[c] = t->cells_z - j0 < TERRAIN_CHUNK ? t->cells_z - j0 : TERRAIN_CHUNK;
			r->terrain[c] = build_terrain_chunk(t, i0, j0, r->chunk_w[c], r->chunk_h[c]);
		}
	r->terrain_vertices = (ptrdiff_t)(t->cells_x + 1) * (t->cells_z + 1);
	r->terrain_colors = PUSH(program, unsigned char, r->terrain_vertices * 4);
	float normal[3];
	for (int32_t j = 0; j <= t->cells_z; j++)
		for (int32_t i = 0; i <= t->cells_x; i++)
			terrain_sample_color(t, i, j, r->terrain_colors + ((ptrdiff_t)j * (t->cells_x + 1) + i) * 4, normal);
	r->fogged_colors = PUSH(program, unsigned char, r->terrain_vertices * 4);
	r->chunk_colors = PUSH(program, unsigned char, (TERRAIN_CHUNK + 1) * (TERRAIN_CHUNK + 1) * 4);
}

// Models: each type a few primitives merged into one mesh, so a type is
// still one instanced draw. A model fills x, z in [-0.5, 0.5] and y in
// [0, 1] (its base at y = 0, its front toward +z), and is scaled per
// instance from the UnitDef. Each part's vertex colour is a tone: alpha 255
// takes the side's colour times its grey (the body, darker trim), alpha 0
// keeps its own colour (metal, tracks, lights): cheap depth.
typedef enum { PART_BOX, PART_CYLINDER, PART_CONE, PART_SPHERE } Shape;
typedef struct {
	Shape shape;
	int slices;          // round shapes' sides (0: 12)
	float x, y, z;       // a box's or sphere's centre; a round shape's base centre
	float sx, sy, sz;    // a box's size; a round shape's diameter (sx = sz) and length (sy)
	float tilt;          // degrees from upright toward +z (90: lying along +z, a barrel)
	Color tone;
} Part;

static const Color TONE_BODY = {255, 255, 255, 255};
static const Color TONE_TRIM = {150, 150, 150, 255};
static const Color TONE_METAL = {72, 74, 80, 0};
static const Color TONE_DARK = {34, 34, 38, 0};
static const Color TONE_PALE = {200, 200, 190, 0};
static const Color TONE_GLOW = {255, 205, 100, 0};

#define MODEL(...) ((const Part[]){__VA_ARGS__}), (int)(sizeof((const Part[]){__VA_ARGS__}) / sizeof(Part))
#define TRACKS(w, h, l) {PART_BOX, 0, -0.5f + 0.5f * (w), 0.5f * (h), 0, w, h, l, 0, TONE_METAL}, {PART_BOX, 0, 0.5f - 0.5f * (w), 0.5f * (h), 0, w, h, l, 0, TONE_METAL}

static Mesh model_mesh(const Part *parts, int n)
{
	// Unindexed triangles: raylib's round shapes come without indices.
	Mesh gen[16];
	ASSERT(n <= (int)countof(gen));
	int total = 0;
	for (int k = 0; k < n; k++) {
		const Part *p = &parts[k];
		int slices = p->slices ? p->slices : 12;
		gen[k] = p->shape == PART_BOX        ? GenMeshCube(1.0f, 1.0f, 1.0f)
		       : p->shape == PART_CYLINDER ? GenMeshCylinder(0.5f, 1.0f, slices)
		       : p->shape == PART_CONE     ? GenMeshCone(0.5f, 1.0f, slices)
		                              : GenMeshSphere(0.5f, 8, slices);
		total += gen[k].indices ? gen[k].triangleCount * 3 : gen[k].vertexCount;
	}
	Mesh m = {0};
	m.vertexCount = total;
	m.triangleCount = total / 3;
	m.vertices = MemAlloc((unsigned)(total * 3) * sizeof(float));
	m.normals = MemAlloc((unsigned)(total * 3) * sizeof(float));
	m.colors = MemAlloc((unsigned)(total * 4));
	int v = 0;
	for (int k = 0; k < n; k++) {
		const Part *p = &parts[k];
		const Mesh *g = &gen[k];
		float a = p->tilt * (PI / 180.0f), ca = cosf(a), sa = sinf(a);
		int count = g->indices ? g->triangleCount * 3 : g->vertexCount;
		for (int q = 0; q < count; q++, v++) {
			int s = g->indices ? g->indices[q] : q;
			float x = g->vertices[s * 3] * p->sx, y = g->vertices[s * 3 + 1] * p->sy, z = g->vertices[s * 3 + 2] * p->sz;
			// Normals under a non-uniform scale: divide by it, then renormalise.
			float nx = g->normals[s * 3] / p->sx, ny = g->normals[s * 3 + 1] / p->sy, nz = g->normals[s * 3 + 2] / p->sz;
			float nl = sqrtf(nx * nx + ny * ny + nz * nz);
			nx /= nl, ny /= nl, nz /= nl;
			// Tilt about x, from +y toward +z.
			float ty = y * ca - z * sa, tz = y * sa + z * ca;
			float tny = ny * ca - nz * sa, tnz = ny * sa + nz * ca;
			m.vertices[v * 3] = p->x + x;
			m.vertices[v * 3 + 1] = p->y + ty;
			m.vertices[v * 3 + 2] = p->z + tz;
			m.normals[v * 3] = nx;
			m.normals[v * 3 + 1] = tny;
			m.normals[v * 3 + 2] = tnz;
			m.colors[v * 4] = p->tone.r;
			m.colors[v * 4 + 1] = p->tone.g;
			m.colors[v * 4 + 2] = p->tone.b;
			m.colors[v * 4 + 3] = p->tone.a;
		}
		UnloadMesh(gen[k]);
	}
	UploadMesh(&m, false);
	return m;
}

static void build_models(Renderer *r)
{
	Mesh *mesh = r->unit_mesh;
	mesh[UNIT_RAIDER] = model_mesh(MODEL(TRACKS(0.18f, 0.3f, 0.95f),
		{PART_BOX, 0, 0, 0.3f, -0.05f, 0.64f, 0.38f, 0.85f, 0, TONE_BODY},
		{PART_BOX, 0, 0, 0.22f, 0.42f, 0.5f, 0.22f, 0.16f, 0, TONE_TRIM},
		{PART_CYLINDER, 8, 0, 0.55f, 0.0f, 0.12f, 0.5f, 0.12f, 90, TONE_METAL}));
	mesh[UNIT_TANK] = model_mesh(MODEL(TRACKS(0.24f, 0.36f, 1.0f),
		{PART_BOX, 0, 0, 0.38f, 0, 0.56f, 0.36f, 0.85f, 0, TONE_BODY},
		{PART_BOX, 0, 0, 0.7f, -0.08f, 0.44f, 0.3f, 0.44f, 0, TONE_TRIM},
		{PART_CYLINDER, 8, 0, 0.72f, 0.12f, 0.13f, 0.6f, 0.13f, 90, TONE_METAL}));
	mesh[UNIT_ARTILLERY] = model_mesh(MODEL(TRACKS(0.2f, 0.3f, 0.9f),
		{PART_BOX, 0, 0, 0.32f, 0, 0.62f, 0.3f, 0.8f, 0, TONE_BODY},
		{PART_CYLINDER, 10, 0, 0.45f, -0.05f, 0.5f, 0.15f, 0.5f, 0, TONE_METAL},
		{PART_CYLINDER, 8, 0, 0.55f, -0.1f, 0.16f, 0.85f, 0.16f, 50, TONE_TRIM}));
	mesh[UNIT_SIEGE] = model_mesh(MODEL(TRACKS(0.22f, 0.32f, 1.0f),
		{PART_BOX, 0, 0, 0.34f, 0, 0.58f, 0.34f, 0.9f, 0, TONE_BODY},
		{PART_BOX, 0, 0, 0.6f, -0.32f, 0.5f, 0.24f, 0.26f, 0, TONE_METAL},
		{PART_CYLINDER, 8, 0, 0.55f, -0.15f, 0.2f, 1.15f, 0.2f, 62, TONE_TRIM},
		{PART_CYLINDER, 8, 0, 0.55f, -0.15f, 0.32f, 0.12f, 0.32f, 62, TONE_DARK}));
	mesh[UNIT_HEAVY_TANK] = model_mesh(MODEL(TRACKS(0.26f, 0.4f, 1.0f),
		{PART_BOX, 0, 0, 0.38f, 0, 0.5f, 0.4f, 0.9f, 0, TONE_BODY},
		{PART_BOX, 0, 0, 0.72f, -0.1f, 0.56f, 0.32f, 0.56f, 0, TONE_TRIM},
		{PART_CYLINDER, 8, -0.1f, 0.74f, 0.15f, 0.11f, 0.6f, 0.11f, 90, TONE_METAL},
		{PART_CYLINDER, 8, 0.1f, 0.74f, 0.15f, 0.11f, 0.6f, 0.11f, 90, TONE_METAL}));
	mesh[UNIT_HOWITZER] = model_mesh(MODEL(TRACKS(0.22f, 0.32f, 1.0f),
		{PART_BOX, 0, 0, 0.34f, 0, 0.56f, 0.34f, 0.9f, 0, TONE_BODY},
		{PART_BOX, 0, 0, 0.62f, -0.05f, 0.44f, 0.24f, 0.5f, 0, TONE_TRIM},
		{PART_CYLINDER, 8, 0, 0.62f, 0.0f, 0.2f, 0.85f, 0.2f, 55, TONE_METAL}));
	mesh[UNIT_ENGINEER] = model_mesh(MODEL(TRACKS(0.16f, 0.25f, 0.9f),
		{PART_BOX, 0, 0, 0.32f, -0.05f, 0.66f, 0.4f, 0.75f, 0, TONE_BODY},
		{PART_BOX, 0, 0, 0.62f, 0.22f, 0.42f, 0.22f, 0.3f, 0, TONE_TRIM},
		{PART_CYLINDER, 6, 0.15f, 0.52f, -0.25f, 0.1f, 0.75f, 0.1f, 60, TONE_PALE},
		{PART_BOX, 0, -0.18f, 0.6f, -0.2f, 0.12f, 0.12f, 0.12f, 0, TONE_GLOW}));
	mesh[UNIT_ADVANCED_ENGINEER] = model_mesh(MODEL(TRACKS(0.18f, 0.28f, 0.95f),
		{PART_BOX, 0, 0, 0.34f, -0.05f, 0.64f, 0.42f, 0.8f, 0, TONE_BODY},
		{PART_BOX, 0, 0, 0.66f, 0.22f, 0.42f, 0.24f, 0.3f, 0, TONE_TRIM},
		{PART_CYLINDER, 6, 0.18f, 0.55f, -0.25f, 0.1f, 0.75f, 0.1f, 60, TONE_PALE},
		{PART_CYLINDER, 6, -0.18f, 0.55f, -0.25f, 0.1f, 0.75f, 0.1f, 60, TONE_PALE},
		{PART_BOX, 0, 0, 0.78f, 0.22f, 0.14f, 0.1f, 0.14f, 0, TONE_GLOW}));
	mesh[UNIT_SAPPER] = model_mesh(MODEL(TRACKS(0.18f, 0.28f, 0.85f),
		{PART_BOX, 0, 0, 0.34f, -0.08f, 0.62f, 0.42f, 0.7f, 0, TONE_BODY},
		{PART_BOX, 0, 0, 0.62f, -0.15f, 0.36f, 0.18f, 0.34f, 0, TONE_TRIM},
		{PART_BOX, 0, 0, 0.22f, 0.44f, 0.95f, 0.36f, 0.1f, 0, TONE_METAL}));
	mesh[UNIT_SCOUT] = model_mesh(MODEL(
		{PART_SPHERE, 10, 0, 0.45f, 0, 0.8f, 0.7f, 0.8f, 0, TONE_BODY},
		{PART_CYLINDER, 10, 0, 0.0f, 0, 0.5f, 0.16f, 0.5f, 0, TONE_METAL},
		{PART_CYLINDER, 4, 0.12f, 0.7f, -0.1f, 0.05f, 0.4f, 0.05f, 0, TONE_PALE},
		{PART_SPHERE, 6, 0, 0.5f, 0.36f, 0.22f, 0.22f, 0.12f, 0, TONE_GLOW}));
	mesh[UNIT_COMMANDER] = model_mesh(MODEL(
		{PART_CYLINDER, 5, 0, 0, 0, 0.95f, 0.7f, 0.95f, 0, TONE_BODY},
		{PART_CYLINDER, 5, 0, 0.7f, 0, 0.6f, 0.16f, 0.6f, 0, TONE_TRIM},
		{PART_CYLINDER, 10, 0, 0, 0, 1.0f, 0.1f, 1.0f, 0, TONE_METAL},
		{PART_SPHERE, 8, 0, 0.92f, 0, 0.22f, 0.22f, 0.22f, 0, TONE_GLOW}));
	mesh[UNIT_METAL_TRUCK] = model_mesh(MODEL(TRACKS(0.12f, 0.2f, 0.95f),
		{PART_BOX, 0, 0, 0.32f, 0.33f, 0.72f, 0.5f, 0.3f, 0, TONE_TRIM},
		{PART_BOX, 0, 0, 0.25f, -0.15f, 0.78f, 0.3f, 0.66f, 0, TONE_BODY},
		{PART_BOX, 0, 0, 0.5f, -0.15f, 0.6f, 0.22f, 0.5f, 0, TONE_PALE}));
	mesh[UNIT_ENERGY_TRUCK] = model_mesh(MODEL(TRACKS(0.12f, 0.2f, 0.95f),
		{PART_BOX, 0, 0, 0.32f, 0.33f, 0.72f, 0.5f, 0.3f, 0, TONE_TRIM},
		{PART_CYLINDER, 12, 0, 0.45f, -0.48f, 0.7f, 0.66f, 0.7f, 90, TONE_BODY},
		{PART_CYLINDER, 12, 0, 0.45f, -0.2f, 0.74f, 0.06f, 0.74f, 90, TONE_GLOW}));
	mesh[UNIT_EXTRACTOR] = model_mesh(MODEL(
		{PART_CYLINDER, 10, 0, 0, 0, 1.0f, 0.25f, 1.0f, 0, TONE_METAL},
		{PART_CYLINDER, 10, 0, 0.25f, 0, 0.7f, 0.55f, 0.7f, 0, TONE_BODY},
		{PART_CYLINDER, 10, 0, 0.8f, 0, 0.84f, 0.12f, 0.84f, 0, TONE_TRIM},
		{PART_CYLINDER, 6, 0, 0.92f, 0, 0.2f, 0.08f, 0.2f, 0, TONE_GLOW}));
	mesh[UNIT_ADVANCED_EXTRACTOR] = model_mesh(MODEL(
		{PART_CYLINDER, 5, 0, 0, 0, 1.0f, 0.2f, 1.0f, 0, TONE_METAL},
		{PART_CYLINDER, 5, 0, 0.2f, 0, 0.75f, 0.6f, 0.75f, 0, TONE_BODY},
		{PART_CYLINDER, 5, 0, 0.8f, 0, 0.9f, 0.1f, 0.9f, 0, TONE_TRIM},
		{PART_CYLINDER, 8, 0, 0.9f, 0, 0.35f, 0.1f, 0.35f, 0, TONE_GLOW}));
	mesh[UNIT_GENERATOR] = model_mesh(MODEL(
		{PART_BOX, 0, 0, 0.07f, 0, 0.9f, 0.14f, 0.9f, 0, TONE_METAL},
		{PART_CYLINDER, 6, 0, 0.14f, 0, 0.3f, 0.68f, 0.3f, 0, TONE_BODY},
		{PART_CYLINDER, 12, 0, 0.8f, 0, 0.9f, 0.06f, 0.9f, 0, TONE_TRIM},
		{PART_SPHERE, 8, 0, 0.92f, 0, 0.2f, 0.16f, 0.2f, 0, TONE_GLOW}));
	mesh[UNIT_FACTORY] = model_mesh(MODEL(
		{PART_BOX, 0, 0, 0.34f, -0.04f, 1.0f, 0.68f, 0.92f, 0, TONE_BODY},
		{PART_BOX, 0, 0, 0.71f, -0.04f, 1.02f, 0.06f, 0.94f, 0, TONE_TRIM},
		{PART_BOX, 0, 0, 0.22f, 0.45f, 0.5f, 0.44f, 0.06f, 0, TONE_DARK},
		{PART_BOX, 0, 0.33f, 0.48f, 0.45f, 0.08f, 0.08f, 0.04f, 0, TONE_GLOW},
		{PART_CYLINDER, 8, 0.3f, 0.7f, -0.25f, 0.14f, 0.3f, 0.14f, 0, TONE_METAL}));
	mesh[UNIT_ADVANCED_FACTORY] = model_mesh(MODEL(
		{PART_BOX, 0, 0, 0.36f, -0.04f, 1.0f, 0.72f, 0.92f, 0, TONE_BODY},
		{PART_BOX, 0, 0, 0.75f, -0.04f, 1.02f, 0.06f, 0.94f, 0, TONE_TRIM},
		{PART_BOX, 0, 0, 0.25f, 0.45f, 0.6f, 0.5f, 0.06f, 0, TONE_DARK},
		{PART_BOX, 0, 0.36f, 0.55f, 0.45f, 0.08f, 0.08f, 0.04f, 0, TONE_GLOW},
		{PART_BOX, 0, -0.36f, 0.55f, 0.45f, 0.08f, 0.08f, 0.04f, 0, TONE_GLOW},
		{PART_CYLINDER, 8, 0.3f, 0.75f, -0.25f, 0.14f, 0.25f, 0.14f, 0, TONE_METAL},
		{PART_CYLINDER, 8, -0.3f, 0.75f, -0.25f, 0.14f, 0.25f, 0.14f, 0, TONE_METAL}));
	mesh[UNIT_TRANSPORTER] = model_mesh(MODEL(
		{PART_CYLINDER, 20, 0, 0, 0, 1.0f, 0.06f, 1.0f, 0, TONE_METAL},
		{PART_CYLINDER, 20, 0, 0.06f, 0, 0.76f, 0.04f, 0.76f, 0, TONE_BODY},
		{PART_CYLINDER, 20, 0, 0.1f, 0, 0.5f, 0.03f, 0.5f, 0, TONE_GLOW},
		{PART_BOX, 0, 0.36f, 0.25f, 0.36f, 0.08f, 0.5f, 0.08f, 0, TONE_TRIM},
		{PART_BOX, 0, -0.36f, 0.25f, 0.36f, 0.08f, 0.5f, 0.08f, 0, TONE_TRIM},
		{PART_BOX, 0, 0.36f, 0.25f, -0.36f, 0.08f, 0.5f, 0.08f, 0, TONE_TRIM},
		{PART_BOX, 0, -0.36f, 0.25f, -0.36f, 0.08f, 0.5f, 0.08f, 0, TONE_TRIM}));
	mesh[UNIT_TURRET] = model_mesh(MODEL(
		{PART_CYLINDER, 8, 0, 0, 0, 1.0f, 0.3f, 1.0f, 0, TONE_METAL},
		{PART_CYLINDER, 8, 0, 0.3f, 0, 0.7f, 0.35f, 0.7f, 0, TONE_BODY},
		{PART_CYLINDER, 8, 0, 0.5f, 0.1f, 0.16f, 0.55f, 0.16f, 90, TONE_TRIM}));
	mesh[UNIT_METAL_VAULT] = model_mesh(MODEL(
		{PART_BOX, 0, 0, 0.42f, 0, 0.9f, 0.84f, 0.9f, 0, TONE_BODY},
		{PART_BOX, 0, 0, 0.1f, 0, 0.96f, 0.12f, 0.96f, 0, TONE_METAL},
		{PART_BOX, 0, 0, 0.88f, 0, 0.96f, 0.12f, 0.96f, 0, TONE_METAL},
		{PART_BOX, 0, 0, 0.45f, 0.46f, 0.4f, 0.5f, 0.04f, 0, TONE_PALE}));
	mesh[UNIT_ENERGY_VAULT] = model_mesh(MODEL(
		{PART_CYLINDER, 12, 0, 0, 0, 0.9f, 1.0f, 0.9f, 0, TONE_BODY},
		{PART_CYLINDER, 12, 0, 0, 0, 0.96f, 0.12f, 0.96f, 0, TONE_METAL},
		{PART_CYLINDER, 12, 0, 0.88f, 0, 0.96f, 0.12f, 0.96f, 0, TONE_METAL},
		{PART_CYLINDER, 12, 0, 0.46f, 0, 0.94f, 0.08f, 0.94f, 0, TONE_GLOW}));
	mesh[UNIT_HEADQUARTERS] = model_mesh(MODEL(
		{PART_CYLINDER, 6, 0, 0, 0, 1.0f, 0.55f, 1.0f, 0, TONE_BODY},
		{PART_CYLINDER, 6, 0, 0.55f, 0, 0.66f, 0.25f, 0.66f, 0, TONE_TRIM},
		{PART_CYLINDER, 6, 0, 0, 0, 1.06f, 0.08f, 1.06f, 0, TONE_METAL},
		{PART_CYLINDER, 4, 0, 0.8f, 0, 0.08f, 0.2f, 0.08f, 0, TONE_METAL},
		{PART_SPHERE, 8, 0, 0.92f, 0, 0.16f, 0.16f, 0.16f, 0, TONE_GLOW}));
	for (int k = 0; k < UNIT_TYPE_COUNT; k++) ASSERT(mesh[k].vertexCount > 0); // a model for every type
}

Camera3D render_camera(const Terrain *t, V2 focus, float distance)
{
	Vector3 target = {focus.x, terrain_height(t, focus.x, focus.z), focus.z};
	return (Camera3D){
		.target = target,
		.position = {target.x, target.y + distance * 0.85f, target.z + distance * 0.55f},
		.up = {0, 1, 0},
		.fovy = 45.0f,
		.projection = CAMERA_PERSPECTIVE,
	};
}

void renderer_init(Renderer *r, const Terrain *terrain, ptrdiff_t max_units, Arena *program)
{
	*r = (Renderer){0};
	r->shader = LoadShaderFromMemory(UNIT_VS, UNIT_FS);
	r->shader.locs[SHADER_LOC_MATRIX_MVP] = GetShaderLocation(r->shader, "mvp");
	r->shader.locs[SHADER_LOC_COLOR_DIFFUSE] = GetShaderLocation(r->shader, "colDiffuse");
	// raylib 5.5 binds the per-instance matrix attribute through this slot.
	r->shader.locs[SHADER_LOC_MATRIX_MODEL] = GetShaderLocationAttrib(r->shader, "instanceTransform");
	r->material = LoadMaterialDefault();
	r->material.shader = r->shader;

	build_models(r);
	r->shell_mesh = GenMeshSphere(1.0f, 6, 8);
	build_terrain(r, terrain, program);
	// Far enough to see a whole zone from above it (a standard zone keeps
	// raylib's default).
	rlSetClipPlanes(RL_CULL_DISTANCE_NEAR, terrain->size_x <= 512.0f ? RL_CULL_DISTANCE_FAR : 3.0 * terrain->size_x);

	r->heading_cap = max_units;
	for (int k = 0; k < FACTION_COUNT; k++) r->heading[k] = PUSH(program, float, r->heading_cap);
}

void renderer_set_fog(Renderer *r, const uint8_t *visible)
{
	const unsigned char *src = r->terrain_colors;
	if (visible) {
		for (ptrdiff_t v = 0; v < r->terrain_vertices; v++) {
			// Out of sight: darker and cooler, so it reads as unknown ground.
			bool seen = visible[v];
			unsigned char *c = r->fogged_colors + v * 4;
			c[0] = seen ? src[v * 4 + 0] : (unsigned char)(src[v * 4 + 0] * 2 / 5);
			c[1] = seen ? src[v * 4 + 1] : (unsigned char)(src[v * 4 + 1] * 2 / 5);
			c[2] = seen ? src[v * 4 + 2] : (unsigned char)(src[v * 4 + 2] * 1 / 2 + 10);
			c[3] = 255;
		}
		src = r->fogged_colors;
	}
	// Each chunk's colours from the samples it covers. Buffer 3 of a raylib
	// mesh holds vertex colours.
	for (int32_t c = 0; c < r->terrain_chunks; c++) {
		int32_t w = r->chunk_w[c] + 1, h = r->chunk_h[c] + 1;
		for (int32_t j = 0; j < h; j++)
			memcpy(r->chunk_colors + (ptrdiff_t)j * w * 4,
			       src + ((ptrdiff_t)(r->chunk_j[c] + j) * r->samples_x + r->chunk_i[c]) * 4, (size_t)w * 4);
		UpdateMeshBuffer(r->terrain[c].meshes[0], 3, r->chunk_colors, w * h * 4, 0);
	}
}

void renderer_unload(Renderer *r)
{
	for (int k = 0; k < UNIT_TYPE_COUNT; k++) UnloadMesh(r->unit_mesh[k]);
	UnloadMesh(r->shell_mesh);
	for (int32_t c = 0; c < r->terrain_chunks; c++) UnloadModel(r->terrain[c]);
	r->material.shader = (Shader){0};
	UnloadShader(r->shader);
}

// Column-major, as raylib expects: scale, then turn about y, then move.
static Matrix transform(float x, float y, float z, float sx, float sy, float sz, float yaw)
{
	float c = cosf(yaw), s = sinf(yaw);
	return (Matrix){
		.m0 = c * sx, .m4 = 0.0f, .m8 = s * sz, .m12 = x,
		.m1 = 0.0f, .m5 = sy, .m9 = 0.0f, .m13 = y,
		.m2 = -s * sx, .m6 = 0.0f, .m10 = c * sz, .m14 = z,
		.m3 = 0.0f, .m7 = 0.0f, .m11 = 0.0f, .m15 = 1.0f,
	};
}

static void draw_bucket(Renderer *r, Mesh mesh, Color color, const Matrix *m, ptrdiff_t n)
{
	if (n == 0) return;
	r->material.maps[MATERIAL_MAP_DIFFUSE].color = color;
	DrawMeshInstanced(mesh, r->material, m, (int)n);
}

void renderer_draw_scene(Renderer *r, const Scene *scene, float alpha, Arena *frame,
                         const uint8_t *selected, int selected_faction)
{
	ASSERT(scene->max_units <= r->heading_cap);
	for (int32_t c = 0; c < r->terrain_chunks; c++) DrawModel(r->terrain[c], (Vector3){0, 0, 0}, 1.0f, WHITE);
	float ahead = alpha * SIM_DT;

	for (int f = 0; f < FACTION_COUNT; f++) {
		const SceneUnits *u = &scene->units[f];
		float *heading = r->heading[f];
		// Buckets per type: selected, under construction, and then one per
		// player (each in its own shade of the side's colour). Counted first,
		// so each holds exactly what goes in it.
		enum { SELECTED = 0, BUILDING = 1, PLAYER0 = 2, BUCKETS = PLAYER0 + MAX_PLAYERS };
		Matrix *bucket[UNIT_TYPE_COUNT][BUCKETS];
		ptrdiff_t count[UNIT_TYPE_COUNT][BUCKETS] = {0}, size[UNIT_TYPE_COUNT][BUCKETS] = {0};
		uint8_t *which = PUSH(frame, uint8_t, u->count > 0 ? u->count : 1);
		for (ptrdiff_t i = 0; i < u->count; i++) {
			int s = PLAYER0 + (u->owner ? u->owner[i].v : 0);
			if ((u->progress && u->progress[i] < 1.0f) || (u->order && u->order[i] == ORDER_TRANSFORM)) s = BUILDING;
			if (f == selected_faction && selected && selected[i]) s = SELECTED; // shows even while it's being built
			which[i] = (uint8_t)s;
			size[u->type[i]][s]++;
		}
		for (int t = 0; t < UNIT_TYPE_COUNT; t++)
			for (int s = 0; s < BUCKETS; s++) bucket[t][s] = size[t][s] ? PUSH(frame, Matrix, size[t][s]) : nullptr;

		for (ptrdiff_t i = 0; i < u->count; i++) {
			const UnitDef *def = &unit_defs[u->type[i]];
			int32_t slot = u->slot[i];
			if (u->vx[i] * u->vx[i] + u->vz[i] * u->vz[i] > 0.09f)
				heading[slot] = atan2f(u->vx[i], u->vz[i]);
			float x = u->px[i] + u->vx[i] * ahead, z = u->pz[i] + u->vz[i] * ahead;
			float d = 2.0f * def->radius;
			int s = which[i];
			int t = u->type[i];
			// Structures rise as they're built.
			float built = u->progress ? u->progress[i] : 1.0f;
			float h = def->height * f_max(0.15f, built);
			// Transforming: pale, and sinking as it digs in (or rising as it packs up).
			if (u->order && u->order[i] == ORDER_TRANSFORM) {
				float w = u->work[i];
				h *= unit_digs_in(u->type[i]) ? 1.0f - 0.6f * w : 0.4f + 0.6f * w;
			}
			// Models sit on their base; a structure on the highest ground under
			// it, so a wide one (a transporter's pad) doesn't sink into a slope.
			float y = u->py[i];
			if (unit_is_structure(u->type[i]) && scene->terrain)
				for (int k = 0; k < 8; k++) {
					float a = (float)k * (PI / 4.0f);
					y = fmaxf(y, terrain_height(scene->terrain, x + def->radius * cosf(a), z + def->radius * sinf(a)));
				}
			float yaw = unit_is_structure(u->type[i]) ? 0.0f : heading[slot];
			if (t == UNIT_ENGINEER || t == UNIT_ADVANCED_ENGINEER || t == UNIT_SAPPER) h *= 0.7f;
			bucket[t][s][count[t][s]++] = transform(x, y, z, d, h, d, yaw);
		}
		Color building = ColorLerp(FACTION_COLOR[f], WHITE, 0.55f);
		for (int t = 0; t < UNIT_TYPE_COUNT; t++) {
			for (int p = 0; p < MAX_PLAYERS; p++)
				draw_bucket(r, r->unit_mesh[t], render_player_color(f, (PlayerSlot){(uint8_t)p}), bucket[t][PLAYER0 + p],
				            count[t][PLAYER0 + p]);
			draw_bucket(r, r->unit_mesh[t], SELECTED_COLOR, bucket[t][SELECTED], count[t][SELECTED]);
			draw_bucket(r, r->unit_mesh[t], building, bucket[t][BUILDING], count[t][BUILDING]);
		}
	}

	ptrdiff_t shells = 0;
	for (int f = 0; f < FACTION_COUNT; f++) shells += scene->shells[f].count;
	Matrix *m = PUSH(frame, Matrix, shells);
	ptrdiff_t n = 0;
	for (int f = 0; f < FACTION_COUNT; f++) {
		const SceneShells *p = &scene->shells[f];
		for (ptrdiff_t i = 0; i < p->count; i++) {
			float size = p->weapon[i] == UNIT_ARTILLERY ? 0.55f : p->weapon[i] == UNIT_TANK ? 0.35f : 0.2f;
			m[n++] = transform(p->px[i] + p->vx[i] * ahead, p->py[i] + p->vy[i] * ahead,
			                   p->pz[i] + p->vz[i] * ahead, size, size, size, 0.0f);
		}
	}
	draw_bucket(r, r->shell_mesh, SHELL_COLOR, m, n);

	// Metal spots: dark discs, ringed in the owner's colour where known.
	if (scene->economy && scene->spots) {
		for (int32_t k = 0; k < scene->spots->count; k++) {
			V2 at = scene->spots->at[k];
			Vector3 p = {at.x, terrain_height(scene->terrain, at.x, at.z) + 0.05f, at.z};
			DrawCylinder(p, SPOT_RADIUS, SPOT_RADIUS, 0.12f, 16, (Color){40, 44, 52, 255});
			int8_t o = scene->spot_owner[k];
			if (o >= 0) DrawCylinderWires(p, SPOT_RADIUS + 0.6f, SPOT_RADIUS + 0.6f, 0.2f, 16, FACTION_COLOR[o]);
		}
	}

	for (int f = 0; f < FACTION_COUNT; f++) {
		V2 b = scene->base[f];
		Vector3 at = {b.x, terrain_height(scene->terrain, b.x, b.z), b.z};
		DrawCylinderWires(at, 20.0f, 20.0f, 0.5f, 24, FACTION_COLOR[f]);
	}
}
