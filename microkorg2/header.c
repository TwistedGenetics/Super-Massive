/*
 *  File: header.c
 *
 *  Horizon - microKORG 2 reverb (revfx) unit header.
 *  revfx units get 8 params max (common/unit_revfx.h): 3 on page 1, 5 on page 2.
 *  Names: max 8 characters.
 */

#include "unit.h"
#include "runtime.h"

// The load-meter build (tools/mk2-meter) reuses this header with its own
// name and unit ID, so both can be loaded side by side.
#ifndef HORIZON_UNIT_NAME
#define HORIZON_UNIT_NAME "Horizon"
#endif
#ifndef HORIZON_UNIT_ID
#define HORIZON_UNIT_ID 0x5U  // 1 = VectorFM, 2 = Formant Mutator, 3 = Rollers, 4 = Dusty Pads, 5 = Horizon
#endif

const __unit_header unit_header_t unit_header = {
    .header_size = sizeof(unit_header_t),
    .target = UNIT_TARGET_PLATFORM | k_unit_module_revfx,
    .api = UNIT_API_VERSION,
    .dev_id = 0x5477476EU,  // 'TwGn' (TwistedGenetics)
    .unit_id = HORIZON_UNIT_ID,
    .version = 0x00010000U,  // 1.0.0
    .name = HORIZON_UNIT_NAME,
    .num_presets = 0,
    .num_params = 8,
    .params = {
        // min, max, center, default, type, frac, frac mode, reserved, name

        // Page 1
        {10, 1250, 0, 400, k_unit_param_type_msec, 0, 0, 0, {"Time"}},
        {0, 100, 0, 70, k_unit_param_type_percent, 0, 0, 0, {"Feedback"}},
        {0, 100, 0, 60, k_unit_param_type_percent, 0, 0, 0, {"Warp"}},

        // Page 2
        {0, 100, 0, 70, k_unit_param_type_percent, 0, 0, 0, {"Density"}},
        {0, 100, 0, 30, k_unit_param_type_percent, 0, 0, 0, {"Mod"}},
        {-100, 100, 0, 0, k_unit_param_type_none, 0, 0, 0, {"Tone"}},
        {0, 9, 0, 1, k_unit_param_type_strings, 0, 0, 0, {"Mode"}},
        {-100, 100, 0, 35, k_unit_param_type_none, 0, 0, 0, {"Mix"}}}};
