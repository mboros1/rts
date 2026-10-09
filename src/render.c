#include "render.h"

#include <math.h>

static const char *UNIT_VS =
	"#version 330\n"
	"in vec3 vertexPosition;\n"
	"in vec3 vertexNormal;\n"
	"in mat4 instanceTransform;\n"
	"uniform mat4 mvp;\n"
	"out float shade;\n"
	"void main() {\n"
	"    vec3 n = normalize(mat3(instanceTransform) * vertexNormal);\n"
	"    shade = 0.45 + 0.55 * max(dot(n, normalize(vec3(0.4, 1.0, 0.3))), 0.0);\n"
	"    gl_Position = mvp * instanceTransform * vec4(vertexPosition, 1.0);\n"
	"}\n";

static const char *UNIT_FS =
	"#version 330\n"
	"in float shade;\n"
	"uniform vec4 colDiffuse;\n"
	"out vec4 finalColor;\n"
	"void main() { finalColor = vec4(colDiffuse.rgb * shade, colDiffuse.a); }\n";

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
static Model build_terrain(const Terrain *t)
{
	int32_t sx = t->cells_x + 1, sz = t->cells_z + 1;
	Mesh m = {0};
	m.vertexCount = sx * sz;
	m.triangleCount = 2 * t->cells_x * t->cells_z;
	m.vertices = MemAlloc((unsigned)(m.vertexCount * 3) * sizeof(float));
	m.normals = MemAlloc((unsigned)(m.vertexCount * 3) * sizeof(float));
	m.colors = MemAlloc((unsigned)(m.vertexCount * 4));
	m.indices = MemAlloc((unsigned)(m.triangleCount * 3) * sizeof(unsigned short));
	ASSERT(m.vertexCount <= 65535);

	for (int32_t j = 0; j < sz; j++) {
		for (int32_t i = 0; i < sx; i++) {
			int32_t v = j * sx + i;
			float x = (float)i * t->cell, z = (float)j * t->cell;
			float h = t->heights[v];
			float hl = t->heights[j * sx + (i > 0 ? i - 1 : i)];
			float hr = t->heights[j * sx + (i < sx - 1 ? i + 1 : i)];
			float hd = t->heights[(j > 0 ? j - 1 : j) * sx + i];
			float hu = t->heights[(j < sz - 1 ? j + 1 : j) * sx + i];
			float nx = hl - hr, ny = 2.0f * t->cell, nz = hd - hu;
			float nl = sqrtf(nx * nx + ny * ny + nz * nz);
			nx /= nl, ny /= nl, nz /= nl;
			m.vertices[v * 3 + 0] = x;
			m.vertices[v * 3 + 1] = h;
			m.vertices[v * 3 + 2] = z;
			m.normals[v * 3 + 0] = nx;
			m.normals[v * 3 + 1] = ny;
			m.normals[v * 3 + 2] = nz;
			float k = h / (t->max_height > 0.0f ? t->max_height : 1.0f);
			float light = 0.5f + 0.5f * fmaxf(0.0f, nx * 0.37f + ny * 0.86f + nz * 0.33f);
			float r = (60.0f + 70.0f * k) * light, g = (95.0f + 60.0f * k) * light,
			      b = (55.0f + 40.0f * k) * light;
			m.colors[v * 4 + 0] = (unsigned char)fminf(r, 255.0f);
			m.colors[v * 4 + 1] = (unsigned char)fminf(g, 255.0f);
			m.colors[v * 4 + 2] = (unsigned char)fminf(b, 255.0f);
			m.colors[v * 4 + 3] = 255;
		}
	}
	int32_t k = 0;
	for (int32_t j = 0; j < t->cells_z; j++) {
		for (int32_t i = 0; i < t->cells_x; i++) {
			unsigned short a = (unsigned short)(j * sx + i), b = (unsigned short)(a + 1);
			unsigned short c = (unsigned short)(a + sx), d = (unsigned short)(c + 1);
			unsigned short tri[6] = {a, c, b, b, c, d};
			for (int q = 0; q < 6; q++) m.indices[k++] = tri[q];
		}
	}
	UploadMesh(&m, false);
	return LoadModelFromMesh(m);
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

	// Unit meshes are unit-sized and scaled per instance from the UnitDef.
	r->unit_mesh[UNIT_RAIDER] = GenMeshCube(1.0f, 1.0f, 1.4f);
	r->unit_mesh[UNIT_TANK] = GenMeshCube(1.0f, 1.0f, 1.0f);
	r->unit_mesh[UNIT_ARTILLERY] = GenMeshCone(0.5f, 1.0f, 8);
	r->unit_mesh[UNIT_SIEGE] = GenMeshCone(0.5f, 1.0f, 4);
	r->unit_mesh[UNIT_ENGINEER] = GenMeshCube(1.0f, 1.0f, 1.0f);
	r->unit_mesh[UNIT_SAPPER] = GenMeshCube(1.0f, 1.0f, 1.0f);
	r->unit_mesh[UNIT_SCOUT] = GenMeshSphere(0.5f, 6, 8);
	r->unit_mesh[UNIT_COMMANDER] = GenMeshCylinder(0.5f, 1.0f, 5);   // a pentagonal tower
	r->unit_mesh[UNIT_METAL_TRUCK] = GenMeshCube(1.0f, 1.0f, 1.6f);
	r->unit_mesh[UNIT_ENERGY_TRUCK] = GenMeshCylinder(0.5f, 1.0f, 8); // a tanker
	r->unit_mesh[UNIT_EXTRACTOR] = GenMeshCylinder(0.5f, 1.0f, 10);
	r->unit_mesh[UNIT_GENERATOR] = GenMeshCylinder(0.3f, 1.0f, 6);
	r->unit_mesh[UNIT_FACTORY] = GenMeshCube(1.0f, 1.0f, 1.0f);
	r->unit_mesh[UNIT_TRANSPORTER] = GenMeshCylinder(0.5f, 1.0f, 20);
	r->unit_mesh[UNIT_TURRET] = GenMeshCone(0.5f, 1.0f, 6);
	r->unit_mesh[UNIT_METAL_VAULT] = GenMeshCube(1.0f, 1.0f, 1.0f);
	r->unit_mesh[UNIT_ENERGY_VAULT] = GenMeshCylinder(0.5f, 1.0f, 12);
	r->unit_mesh[UNIT_HEADQUARTERS] = GenMeshCylinder(0.5f, 1.0f, 6);
	for (int k = 0; k < UNIT_TYPE_COUNT; k++) ASSERT(r->unit_mesh[k].vertexCount > 0); // a mesh for every type
	r->shell_mesh = GenMeshSphere(1.0f, 6, 8);
	r->terrain = build_terrain(terrain);
	r->terrain_vertices = r->terrain.meshes[0].vertexCount;
	r->terrain_colors = arena_copy(program, r->terrain.meshes[0].colors, r->terrain_vertices * 4, 1);
	r->fogged_colors = PUSH(program, unsigned char, r->terrain_vertices * 4);

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
	// Buffer 3 of a raylib mesh holds vertex colours.
	UpdateMeshBuffer(r->terrain.meshes[0], 3, src, (int)(r->terrain_vertices * 4), 0);
}

void renderer_unload(Renderer *r)
{
	for (int k = 0; k < UNIT_TYPE_COUNT; k++) UnloadMesh(r->unit_mesh[k]);
	UnloadMesh(r->shell_mesh);
	UnloadModel(r->terrain);
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
	DrawModel(r->terrain, (Vector3){0, 0, 0}, 1.0f, WHITE);
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
			if (f == selected_faction && selected && selected[i]) s = SELECTED;
			if ((u->progress && u->progress[i] < 1.0f) || (u->order && u->order[i] == ORDER_TRANSFORM)) s = BUILDING;
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
			bool from_base = t == UNIT_ARTILLERY || t == UNIT_SIEGE || t == UNIT_EXTRACTOR || t == UNIT_GENERATOR || t == UNIT_TRANSPORTER
			              || t == UNIT_TURRET || t == UNIT_METAL_VAULT || t == UNIT_ENERGY_VAULT || t == UNIT_ENERGY_TRUCK
			              || t == UNIT_COMMANDER || t == UNIT_HEADQUARTERS;
			float y = u->py[i] + (from_base ? 0.0f : 0.5f * h);
			float yaw = unit_is_structure(u->type[i]) ? 0.0f : heading[slot];
			if (t == UNIT_ENGINEER || t == UNIT_SAPPER) h *= 0.7f;
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
