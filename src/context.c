#include "context.h"

typedef struct {
	const char *name;
	float *(*field)(ZoneContext *);
} Field;

static float *f_start_metal(ZoneContext *c) { return &c->start_metal; }
static float *f_start_energy(ZoneContext *c) { return &c->start_energy; }
static float *f_base_metal(ZoneContext *c) { return &c->base_metal; }
static float *f_base_energy(ZoneContext *c) { return &c->base_energy; }
static float *f_metal_cap(ZoneContext *c) { return &c->metal_cap; }
static float *f_energy_cap(ZoneContext *c) { return &c->energy_cap; }
static float *f_truck_metal(ZoneContext *c) { return &c->truck_metal; }
static float *f_truck_energy(ZoneContext *c) { return &c->truck_energy; }
static float *f_cost(ZoneContext *c) { return &c->cost; }
static float *f_build_time(ZoneContext *c) { return &c->build_time; }
static float *f_metal(ZoneContext *c) { return &c->yield[SOURCE_METAL]; }
static float *f_generator(ZoneContext *c) { return &c->yield[SOURCE_GENERATOR]; }

static const Field economy_fields[] = {
	{"start_metal", f_start_metal}, {"start_energy", f_start_energy}, {"base_metal", f_base_metal},
	{"base_energy", f_base_energy}, {"metal_cap", f_metal_cap},       {"energy_cap", f_energy_cap},
	{"truck_metal", f_truck_metal}, {"truck_energy", f_truck_energy}, {"cost", f_cost},
	{"build_time", f_build_time},
};
// One per income source: a new source is a new field here.
static const Field environment_fields[] = {{"metal", f_metal}, {"generator", f_generator}};
static_assert(countof(environment_fields) == SOURCE_COUNT - 1, "a field per income source");

// Presets: a part's fields, from the standard context.
static void economy_preset(ZoneContext *c, int k)
{
	ZoneContext d = zone_context_default();
	float yield[SOURCE_COUNT];
	memcpy(yield, c->yield, sizeof yield);
	*c = d;
	memcpy(c->yield, yield, sizeof yield);
	switch (k) {
	case 0: break;                                                   // standard
	case 1: c->base_metal = 8.0f; break;                             // home: where the surplus army comes from
	case 2: c->base_metal = 3.0f, c->base_energy = 60.0f; break;     // rich
	case 3: c->base_metal = 0.5f, c->base_energy = 15.0f, c->cost = 1.25f; break; // lean
	}
}
static const char *const economy_presets[] = {"standard", "home", "rich", "lean"};

static void environment_preset(ZoneContext *c, int k)
{
	for (int s = 0; s < SOURCE_COUNT; s++) c->yield[s] = 1.0f;     // standard
	if (k == 1) c->yield[SOURCE_METAL] = c->yield[SOURCE_GENERATOR] = 0.5f; // barren
	if (k == 2) c->yield[SOURCE_METAL] = 1.5f;                             // abundant
}
static const char *const environment_presets[] = {"standard", "barren", "abundant"};

static bool context_refuse(char *err, size_t err_len, const char *why, const char *what)
{
	snprintf(err, err_len, "%s: %s", why, what);
	return false;
}

bool context_parse(ZoneContext *c, ContextPart part, const char *spec, char *err, size_t err_len)
{
	bool economy = part == CONTEXT_ECONOMY;
	const Field *table = economy ? economy_fields : environment_fields;
	int field_count = economy ? (int)countof(economy_fields) : (int)countof(environment_fields);
	const char *const *names = economy ? economy_presets : environment_presets;
	int preset_count = economy ? (int)countof(economy_presets) : (int)countof(environment_presets);
	char buf[256];
	if (strlen(spec) >= sizeof buf) return context_refuse(err, err_len, "too long", spec);
	snprintf(buf, sizeof buf, "%s", spec);
	ZoneContext out = *c;
	char *save;
	bool first = true;
	for (char *t = strtok_r(buf, " \t", &save); t; t = strtok_r(nullptr, " \t", &save), first = false) {
		char *eq = strchr(t, '=');
		if (!eq) {
			int k = 0;
			while (k < preset_count && strcmp(names[k], t)) k++;
			if (!first || k == preset_count)
				return context_refuse(err, err_len,
				              economy ? "a preset (standard home rich lean) first, then FIELD=VALUE"
				                      : "a preset (standard barren abundant) first, then FIELD=VALUE",
				              t);
			if (economy) economy_preset(&out, k);
			else environment_preset(&out, k);
			continue;
		}
		*eq = 0;
		int k = 0;
		while (k < field_count && strcmp(table[k].name, t)) k++;
		if (k == field_count) return context_refuse(err, err_len, economy ? "no such economy field" : "no such income source", t);
		char *end;
		float v = strtof(eq + 1, &end);
		if (eq[1] == 0 || *end || !(v >= 0.0f && v <= 1e7f)) return context_refuse(err, err_len, "expected a number >= 0 for", t);
		*table[k].field(&out) = v;
	}
	if (!zone_context_ok(&out))
		return context_refuse(err, err_len, "out of range (start within storage, cost up to 100, build_time 0.01..100)", spec);
	*c = out;
	return true;
}
