#include "view.h"

#include <math.h>

void view_update(View *v, const Terrain *t, const Keymap *keys)
{
	float dt = GetFrameTime();
	float pan = v->distance * 1.2f * dt;
	if (IsKeyDown(keys->key[BIND_CAMERA_UP])) v->focus.z -= pan;
	if (IsKeyDown(keys->key[BIND_CAMERA_DOWN])) v->focus.z += pan;
	if (IsKeyDown(keys->key[BIND_CAMERA_LEFT])) v->focus.x -= pan;
	if (IsKeyDown(keys->key[BIND_CAMERA_RIGHT])) v->focus.x += pan;
	if (IsMouseButtonDown(MOUSE_BUTTON_MIDDLE)) {
		Vector2 d = GetMouseDelta();
		float k = v->distance / (float)GetScreenHeight();
		v->focus.x -= d.x * k;
		v->focus.z -= d.y * k;
	}
	float wheel = GetMouseWheelMove();
	if (wheel != 0.0f) v->distance = f_clamp(v->distance * powf(0.88f, wheel), 25.0f, 700.0f);
	v->focus = v2(f_clamp(v->focus.x, 0.0f, t->size_x), f_clamp(v->focus.z, 0.0f, t->size_z));

	v->camera = render_camera(t, v->focus, v->distance);
}

// March the mouse ray down to the sim's terrain, then bisect.
bool pick_ground(const View *v, const Terrain *t, Vector2 mouse, V2 *out)
{
	Ray ray = GetScreenToWorldRay(mouse, v->camera);
	float step = 1.0f, prev = 0.0f;
	for (float s = 0.0f; s < 3000.0f; s += step) {
		Vector3 p = {ray.position.x + ray.direction.x * s, ray.position.y + ray.direction.y * s,
		             ray.position.z + ray.direction.z * s};
		if (p.y <= terrain_height(t, p.x, p.z)) {
			float lo = prev, hi = s;
			for (int k = 0; k < 12; k++) {
				float mid = 0.5f * (lo + hi);
				Vector3 q = {ray.position.x + ray.direction.x * mid, ray.position.y + ray.direction.y * mid,
				             ray.position.z + ray.direction.z * mid};
				if (q.y <= terrain_height(t, q.x, q.z)) hi = mid; else lo = mid;
			}
			*out = v2(ray.position.x + ray.direction.x * hi, ray.position.z + ray.direction.z * hi);
			return true;
		}
		prev = s;
	}
	return false;
}
