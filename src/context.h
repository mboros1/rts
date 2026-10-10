// A zone's context as text: its economy's rules and its environment, as a
// preset and fields over it, the way doctrines are written (bot.h):
//
//   economy home                       a preset: standard home rich lean
//   economy lean start_metal=300       ... with fields over it
//   economy cost=1.2                   or fields alone, over what's there
//   environment barren generator=1.5   presets: standard barren abundant
//
// Economy fields: start_metal start_energy base_metal base_energy metal_cap
// energy_cap truck_metal truck_energy cost build_time (cost and build_time
// multiply every unit's). Environment fields: one per income source
// (metal generator), multiplying what units of that source earn.
//
// A spec with an unknown preset or field, or a value out of range, is
// refused whole (the context untouched). Pure: no sim state.
#pragma once

#include "base.h"
#include "sim.h"

typedef enum : uint8_t { CONTEXT_ECONOMY, CONTEXT_ENVIRONMENT } ContextPart;

// Apply spec to c: a preset sets its part's fields (only its part's), then
// each FIELD=VALUE. False, with why in err, on anything wrong.
bool context_parse(ZoneContext *c, ContextPart part, const char *spec, char *err, size_t err_len);
