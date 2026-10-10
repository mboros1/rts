// Quantized motion: what crosses the network and what a faction perceives.
//
// Positions span the zone's width (`map`) in 16 bits: 1/128 unit on a
// 512-unit zone, 1/32 on a 2048 one; velocities are
// 1/256 unit/s. Prediction is dead reckoning from the tick a value was sent.
// The sim's perception of enemies and the network's mirror both use these,
// so a client can rebuild exactly what the server's units perceive.
#pragma once

#include "vmath.h"

static inline uint16_t q_pos(float x, float map) { return (uint16_t)(f_clamp(x, 0.0f, map) * (65535.0f / map) + 0.5f); }
static inline float dq_pos(uint16_t q, float map) { return (float)q * (map / 65535.0f); }
static inline int16_t q_vel(float v)
{
	float s = f_clamp(v * 256.0f, -32767.0f, 32767.0f);
	return (int16_t)(s < 0.0f ? s - 0.5f : s + 0.5f);
}
static inline float dq_vel(int16_t q) { return (float)q * (1.0f / 256.0f); }

// Where a dead-reckoned unit is `age` seconds after it was sent.
static inline V2 q_predict(uint16_t qx, uint16_t qz, int16_t qvx, int16_t qvz, float age, float map)
{
	return v2(dq_pos(qx, map) + dq_vel(qvx) * age, dq_pos(qz, map) + dq_vel(qvz) * age);
}

// Resend a unit's motion when the prediction is this far off, or its
// velocity changed by more than this (in 1/256 unit/s). The trade at 10k
// units (rts-headless wire): 0.35 and 0.125 u/s cost 492 KB/s; these, half a
// raider's width and 0.5 u/s, cost 305 KB/s; 0.75 and 1 u/s would be 186 KB/s
// but let units visibly overlap in tight formations.
static constexpr float RESEND_DISTANCE = 0.5f;
static constexpr int32_t RESEND_VELOCITY = 128;
// Ages travel in 16 bits: refresh anything older than this.
static constexpr int64_t RESEND_AGE = 60000;
