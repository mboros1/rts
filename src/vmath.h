// Vector math for the sim.
//
// Determinism rule (docs/conventions.md): only + - * / and sqrt, which IEEE
// 754 rounds identically everywhere. No libm transcendentals, no approximate
// reciprocals. Build with -ffp-contract=off so a*b+c is never fused.
#pragma once

#include "base.h"

// The ground plane is x/z; y is up.
typedef struct {
	float x, z;
} V2;

typedef struct {
	float x, y, z;
} V3;

static inline float f_sqrt(float x) { return __builtin_sqrtf(x); }
static inline float f_min(float a, float b) { return a < b ? a : b; }
static inline float f_max(float a, float b) { return a > b ? a : b; }
static inline float f_clamp(float x, float lo, float hi) { return f_min(f_max(x, lo), hi); }

static inline V2 v2(float x, float z) { return (V2){x, z}; }
static inline V2 v2_add(V2 a, V2 b) { return (V2){a.x + b.x, a.z + b.z}; }
static inline V2 v2_sub(V2 a, V2 b) { return (V2){a.x - b.x, a.z - b.z}; }
static inline V2 v2_scale(V2 a, float s) { return (V2){a.x * s, a.z * s}; }
static inline float v2_dot(V2 a, V2 b) { return a.x * b.x + a.z * b.z; }
static inline float v2_len2(V2 a) { return v2_dot(a, a); }
static inline float v2_len(V2 a) { return f_sqrt(v2_len2(a)); }

// Length clamped to at most max; a zero vector stays zero.
static inline V2 v2_limit(V2 a, float max)
{
	float l2 = v2_len2(a);
	if (l2 <= max * max) return a;
	return v2_scale(a, max / f_sqrt(l2));
}

static inline V3 v3(float x, float y, float z) { return (V3){x, y, z}; }
static inline V3 v3_add(V3 a, V3 b) { return (V3){a.x + b.x, a.y + b.y, a.z + b.z}; }
static inline V3 v3_sub(V3 a, V3 b) { return (V3){a.x - b.x, a.y - b.y, a.z - b.z}; }
static inline V3 v3_scale(V3 a, float s) { return (V3){a.x * s, a.y * s, a.z * s}; }
static inline float v3_dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
