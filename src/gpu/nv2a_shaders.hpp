#pragma once
// ---------------------------------------------------------------------------
// nv2a_shaders.hpp — Precompiled SPIR-V shaders for NV2A rendering.
//
// SPIR-V is compiled offline at build time (GLSL → glslangValidator → .spv
// → C uint32_t array header).  No runtime shader compilation needed.
// ---------------------------------------------------------------------------

#include <cstdint>
#include <vector>

// Generated headers (in ${CMAKE_BINARY_DIR}/shaders/ — added to include path)
#include "pushbuf_parse.comp.glsl.h"
#include "scanout.vert.glsl.h"
#include "scanout.frag.glsl.h"
#include "nv2a_draw.vert.glsl.h"
#include "nv2a_draw.frag.glsl.h"

namespace xbox {

// Precompiled SPIR-V shader collection.
struct Nv2aShaders {
    std::vector<uint32_t> pushbuf_comp;
    std::vector<uint32_t> scanout_vert;
    std::vector<uint32_t> scanout_frag;
    std::vector<uint32_t> draw_vert;
    std::vector<uint32_t> draw_frag;

    bool compile_all() {
        pushbuf_comp.assign(spv_pushbuf_parse_comp,
                            spv_pushbuf_parse_comp + spv_pushbuf_parse_comp_size);
        scanout_vert.assign(spv_scanout_vert,
                            spv_scanout_vert + spv_scanout_vert_size);
        scanout_frag.assign(spv_scanout_frag,
                            spv_scanout_frag + spv_scanout_frag_size);
        draw_vert.assign(spv_nv2a_draw_vert,
                         spv_nv2a_draw_vert + spv_nv2a_draw_vert_size);
        draw_frag.assign(spv_nv2a_draw_frag,
                         spv_nv2a_draw_frag + spv_nv2a_draw_frag_size);
        return true;
    }
};

} // namespace xbox
