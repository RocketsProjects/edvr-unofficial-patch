#pragma once
// Build-only registry: the shield ripple's door pass and its coverage instrument's marking pass (src/d3d11/shield_ripple_shaders.h). New, so held to their own
// contract (symbol, source name, entry, profile, the text each compiles); tools/shield_ripple_test runs the compiled bytes.
#include "../../src/d3d11/shield_ripple_shaders.h"
static std::vector<Variant> shieldRippleVariants() { return {
    {"kShieldRippleDoorBytecode","shield_ripple_door_cs","main",nullptr,{},false,edvr::kShieldRippleDoorCsHlsl,"cs_5_0"},
    {"kShieldRippleMarkVsBytecode","shield_ripple_mark_vs","main",nullptr,{},false,edvr::kShieldRippleMarkVsHlsl,"vs_5_0"},
    {"kShieldRippleMarkPsBytecode","shield_ripple_mark_ps","main",nullptr,{},false,edvr::kShieldRippleMarkPsHlsl,"ps_5_0"},
}; }
