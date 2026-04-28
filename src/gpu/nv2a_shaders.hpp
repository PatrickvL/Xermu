#pragma once
// ---------------------------------------------------------------------------
// nv2a_shaders.hpp — Runtime GLSL → SPIR-V compilation for NV2A shaders.
//
// GLSL source is embedded as C++ raw string literals.  At renderer init,
// compile_shader() uses the glslang C API to produce SPIR-V.  This avoids
// requiring the Vulkan SDK or glslc in the build environment.
// ---------------------------------------------------------------------------

#include <glslang/Include/glslang_c_interface.h>
#include <glslang/Public/resource_limits_c.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace xbox {

// ===================== Embedded GLSL sources ===============================
// Raw string literals — each corresponds to a .glsl file in src/gpu/shaders/.

static const char g_pushbuf_parse_comp_glsl[] =
#include "shaders/pushbuf_parse.comp.glsl.inc"
;

static const char g_scanout_vert_glsl[] =
#include "shaders/scanout.vert.glsl.inc"
;

static const char g_scanout_frag_glsl[] =
#include "shaders/scanout.frag.glsl.inc"
;

static const char g_nv2a_draw_vert_glsl[] =
#include "shaders/nv2a_draw.vert.glsl.inc"
;

static const char g_nv2a_draw_frag_glsl[] =
#include "shaders/nv2a_draw.frag.glsl.inc"
;

// ===================== Runtime compilation =================================

enum class ShaderStage { Vertex, Fragment, Compute };

inline bool compile_glsl_to_spirv(const char* source, ShaderStage stage,
                                  std::vector<uint32_t>& spirv_out,
                                  const char* name = "shader") {
    glslang_stage_t glslang_stage;
    switch (stage) {
    case ShaderStage::Vertex:   glslang_stage = GLSLANG_STAGE_VERTEX;   break;
    case ShaderStage::Fragment: glslang_stage = GLSLANG_STAGE_FRAGMENT; break;
    case ShaderStage::Compute:  glslang_stage = GLSLANG_STAGE_COMPUTE;  break;
    default: return false;
    }

    glslang_input_t input = {};
    input.language                          = GLSLANG_SOURCE_GLSL;
    input.stage                             = glslang_stage;
    input.client                            = GLSLANG_CLIENT_VULKAN;
    input.client_version                    = GLSLANG_TARGET_VULKAN_1_2;
    input.target_language                   = GLSLANG_TARGET_SPV;
    input.target_language_version           = GLSLANG_TARGET_SPV_1_5;
    input.code                              = source;
    input.default_version                   = 450;
    input.default_profile                   = GLSLANG_NO_PROFILE;
    input.force_default_version_and_profile = false;
    input.forward_compatible                = false;
    input.messages                          = GLSLANG_MSG_DEFAULT_BIT;
    input.resource                          = glslang_default_resource();

    glslang_shader_t* shader = glslang_shader_create(&input);
    if (!shader) {
        fprintf(stderr, "[nv2a_shaders] Failed to create shader object for %s\n", name);
        return false;
    }

    if (!glslang_shader_preprocess(shader, &input)) {
        fprintf(stderr, "[nv2a_shaders] Preprocessing failed for %s:\n%s\n",
                name, glslang_shader_get_info_log(shader));
        glslang_shader_delete(shader);
        return false;
    }

    if (!glslang_shader_parse(shader, &input)) {
        fprintf(stderr, "[nv2a_shaders] Parse failed for %s:\n%s\n",
                name, glslang_shader_get_info_log(shader));
        glslang_shader_delete(shader);
        return false;
    }

    glslang_program_t* program = glslang_program_create();
    glslang_program_add_shader(program, shader);

    if (!glslang_program_link(program,
            GLSLANG_MSG_SPV_RULES_BIT | GLSLANG_MSG_VULKAN_RULES_BIT)) {
        fprintf(stderr, "[nv2a_shaders] Link failed for %s:\n%s\n",
                name, glslang_program_get_info_log(program));
        glslang_program_delete(program);
        glslang_shader_delete(shader);
        return false;
    }

    glslang_program_SPIRV_generate(program, glslang_stage);

    size_t word_count = glslang_program_SPIRV_get_size(program);
    spirv_out.resize(word_count);
    glslang_program_SPIRV_get(program, spirv_out.data());

    const char* spirv_msgs = glslang_program_SPIRV_get_messages(program);
    if (spirv_msgs && spirv_msgs[0])
        fprintf(stderr, "[nv2a_shaders] SPIR-V messages for %s: %s\n", name, spirv_msgs);

    glslang_program_delete(program);
    glslang_shader_delete(shader);

    fprintf(stderr, "[nv2a_shaders] Compiled %s: %zu SPIR-V words\n", name, word_count);
    return word_count > 0;
}

// Convenience: compile all NV2A shaders.  Call once at startup after
// glslang_initialize_process().
struct Nv2aShaders {
    std::vector<uint32_t> pushbuf_comp;
    std::vector<uint32_t> scanout_vert;
    std::vector<uint32_t> scanout_frag;
    std::vector<uint32_t> draw_vert;
    std::vector<uint32_t> draw_frag;

    bool compile_all() {
        bool ok = true;
        ok &= compile_glsl_to_spirv(g_pushbuf_parse_comp_glsl, ShaderStage::Compute,
                                    pushbuf_comp, "pushbuf_parse.comp");
        ok &= compile_glsl_to_spirv(g_scanout_vert_glsl, ShaderStage::Vertex,
                                    scanout_vert, "scanout.vert");
        ok &= compile_glsl_to_spirv(g_scanout_frag_glsl, ShaderStage::Fragment,
                                    scanout_frag, "scanout.frag");
        ok &= compile_glsl_to_spirv(g_nv2a_draw_vert_glsl, ShaderStage::Vertex,
                                    draw_vert, "nv2a_draw.vert");
        ok &= compile_glsl_to_spirv(g_nv2a_draw_frag_glsl, ShaderStage::Fragment,
                                    draw_frag, "nv2a_draw.frag");
        return ok;
    }
};

} // namespace xbox
