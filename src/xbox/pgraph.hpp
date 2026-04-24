#pragma once
// ---------------------------------------------------------------------------
// pgraph.hpp — NV2A PGRAPH state shadow.
//
// Captures 3D pipeline state from NV097 (Kelvin) method calls dispatched
// by the PFIFO DMA pusher.  This is the CPU-side state mirror that will
// eventually feed the Vulkan rendering backend.
// ---------------------------------------------------------------------------

#include <cstdint>
#include <cstring>

namespace xbox {

// ============================= NV097 Method Offsets ========================
// Kelvin (NV20-class) 3D engine methods.  Method addresses are in bytes,
// matching the PFIFO push buffer encoding (bits [12:2] of the header).

namespace nv097 {

// Primitive assembly
static constexpr uint32_t SET_BEGIN_END           = 0x17FC;
static constexpr uint32_t DRAW_ARRAYS             = 0x1810;

// Render target / surface
static constexpr uint32_t SET_SURFACE_FORMAT       = 0x0208;
static constexpr uint32_t SET_SURFACE_PITCH        = 0x020C;
static constexpr uint32_t SET_SURFACE_COLOR_OFFSET = 0x0210;
static constexpr uint32_t SET_SURFACE_ZETA_OFFSET  = 0x0214;
static constexpr uint32_t SET_SURFACE_CLIP_HORIZONTAL = 0x0200;
static constexpr uint32_t SET_SURFACE_CLIP_VERTICAL   = 0x0204;

// Viewport
static constexpr uint32_t SET_VIEWPORT_OFFSET_X   = 0x0A20;  // float[4]
static constexpr uint32_t SET_VIEWPORT_SCALE_X    = 0x0AF0;  // float[4]

// Blend state
static constexpr uint32_t SET_BLEND_ENABLE         = 0x0304;
static constexpr uint32_t SET_BLEND_FUNC_SFACTOR   = 0x0344;
static constexpr uint32_t SET_BLEND_FUNC_DFACTOR   = 0x0348;
static constexpr uint32_t SET_BLEND_EQUATION        = 0x034C;
static constexpr uint32_t SET_BLEND_COLOR           = 0x0350;

// Depth / stencil
static constexpr uint32_t SET_DEPTH_TEST_ENABLE    = 0x0354;
static constexpr uint32_t SET_DEPTH_FUNC           = 0x0358;
static constexpr uint32_t SET_DEPTH_MASK           = 0x035C;
static constexpr uint32_t SET_STENCIL_TEST_ENABLE  = 0x1D0C;
static constexpr uint32_t SET_STENCIL_FUNC         = 0x0364;
static constexpr uint32_t SET_STENCIL_FUNC_REF     = 0x0368;
static constexpr uint32_t SET_STENCIL_FUNC_MASK    = 0x036C;
static constexpr uint32_t SET_STENCIL_OP_FAIL      = 0x0370;
static constexpr uint32_t SET_STENCIL_OP_ZFAIL     = 0x0374;
static constexpr uint32_t SET_STENCIL_OP_ZPASS     = 0x0378;

// Rasterizer
static constexpr uint32_t SET_CULL_FACE_ENABLE     = 0x039C;
static constexpr uint32_t SET_CULL_FACE            = 0x03A0;
static constexpr uint32_t SET_FRONT_FACE           = 0x03A4;
static constexpr uint32_t SET_SHADE_MODE           = 0x0338;
static constexpr uint32_t SET_COLOR_MASK           = 0x0340;

// Texture (4 stages × 64 bytes)
static constexpr uint32_t SET_TEXTURE_OFFSET       = 0x1B00;  // + stage*64
static constexpr uint32_t SET_TEXTURE_FORMAT        = 0x1B04;  // + stage*64
static constexpr uint32_t SET_TEXTURE_CONTROL0      = 0x1B0C;  // + stage*64
static constexpr uint32_t SET_TEXTURE_FILTER        = 0x1B14;  // + stage*64
static constexpr uint32_t SET_TEXTURE_IMAGE_RECT    = 0x1B1C;  // + stage*64

// Vertex shader
static constexpr uint32_t SET_TRANSFORM_PROGRAM_LOAD     = 0x1E9C;
static constexpr uint32_t SET_TRANSFORM_PROGRAM_START     = 0x1EA0;  // 4 dwords per slot
static constexpr uint32_t SET_TRANSFORM_CONSTANT_LOAD     = 0x1EA4;
static constexpr uint32_t SET_TRANSFORM_CONSTANT_START    = 0x0B80;  // 4 floats per const

// Register combiners (fragment shader)
static constexpr uint32_t SET_COMBINER_CONTROL            = 0x0288;
static constexpr uint32_t SET_COMBINER_COLOR_ICW_0        = 0x0260;  // + stage*4
static constexpr uint32_t SET_COMBINER_COLOR_OCW_0        = 0x0270;  // + stage*4
static constexpr uint32_t SET_COMBINER_ALPHA_ICW_0        = 0x0A60;  // + stage*4
static constexpr uint32_t SET_COMBINER_ALPHA_OCW_0        = 0x0A70;  // + stage*4
static constexpr uint32_t SET_COMBINER_SPECULAR_FOG_CW0   = 0x0288;
static constexpr uint32_t SET_COMBINER_SPECULAR_FOG_CW1   = 0x028C;

// Clear
static constexpr uint32_t CLEAR_SURFACE              = 0x1D94;

// NV object binding (subchannel 0 methods)
static constexpr uint32_t SET_OBJECT                  = 0x0000;
static constexpr uint32_t NO_OPERATION                = 0x0100;

} // namespace nv097

// ============================= PGRAPH State ================================

struct PgraphState {
    // --- Surface / render target ---
    uint32_t surface_format        = 0;
    uint32_t surface_pitch         = 0;
    uint32_t surface_color_offset  = 0;
    uint32_t surface_zeta_offset   = 0;
    uint32_t surface_clip_x        = 0;
    uint32_t surface_clip_y        = 0;

    // --- Blend ---
    uint32_t blend_enable          = 0;
    uint32_t blend_sfactor         = 1;   // ONE
    uint32_t blend_dfactor         = 0;   // ZERO
    uint32_t blend_equation        = 0x8006; // FUNC_ADD
    uint32_t blend_color           = 0;

    // --- Depth / stencil ---
    uint32_t depth_test_enable     = 0;
    uint32_t depth_func            = 0x0201; // LESS
    uint32_t depth_mask            = 1;
    uint32_t stencil_test_enable   = 0;
    uint32_t stencil_func          = 0x0207; // ALWAYS
    uint32_t stencil_ref           = 0;
    uint32_t stencil_mask          = 0xFFFFFFFF;
    uint32_t stencil_op_fail       = 0x1E00; // KEEP
    uint32_t stencil_op_zfail      = 0x1E00;
    uint32_t stencil_op_zpass      = 0x1E00;

    // --- Rasterizer ---
    uint32_t cull_face_enable      = 0;
    uint32_t cull_face             = 0x0405; // BACK
    uint32_t front_face            = 0x0901; // CCW
    uint32_t shade_mode            = 0x1D01; // SMOOTH
    uint32_t color_mask            = 0x01010101;

    // --- Texture (4 stages) ---
    struct TextureState {
        uint32_t offset   = 0;
        uint32_t format   = 0;
        uint32_t control0 = 0;
        uint32_t filter   = 0;
        uint32_t image_rect = 0;
    };
    TextureState textures[4] = {};

    // --- Vertex shader program ---
    uint32_t vs_program_load  = 0;  // current load slot
    uint32_t vs_program[136 * 4] = {};  // 136 instruction slots × 4 dwords
    uint32_t vs_program_write_pos = 0;

    // --- Vertex shader constants (192 × 4 floats = 768 floats) ---
    uint32_t vs_const_load = 0;  // current load index
    float    vs_constants[192][4] = {};
    uint32_t vs_const_write_pos = 0;

    // --- Register combiners ---
    uint32_t combiner_control = 0;
    uint32_t combiner_color_icw[8] = {};
    uint32_t combiner_color_ocw[8] = {};
    uint32_t combiner_alpha_icw[8] = {};
    uint32_t combiner_alpha_ocw[8] = {};
    uint32_t combiner_specular_fog_cw0 = 0;
    uint32_t combiner_specular_fog_cw1 = 0;

    // --- Primitive ---
    uint32_t begin_end_mode = 0;  // 0 = END, 1-10 = primitive type

    // --- Clear ---
    uint32_t clear_surface = 0;

    // --- Stats ---
    uint32_t draw_count = 0;  // number of SET_BEGIN_END transitions from 0→nonzero
    uint32_t clear_count = 0;
    uint32_t total_methods = 0;

    // Process a single GPU method call.
    void handle_method(uint32_t subchannel, uint32_t method, uint32_t data) {
        (void)subchannel;
        total_methods++;

        switch (method) {
        // Surface
        case nv097::SET_SURFACE_FORMAT:       surface_format = data; break;
        case nv097::SET_SURFACE_PITCH:        surface_pitch = data; break;
        case nv097::SET_SURFACE_COLOR_OFFSET: surface_color_offset = data; break;
        case nv097::SET_SURFACE_ZETA_OFFSET:  surface_zeta_offset = data; break;
        case nv097::SET_SURFACE_CLIP_HORIZONTAL: surface_clip_x = data; break;
        case nv097::SET_SURFACE_CLIP_VERTICAL:   surface_clip_y = data; break;

        // Blend
        case nv097::SET_BLEND_ENABLE:       blend_enable = data; break;
        case nv097::SET_BLEND_FUNC_SFACTOR: blend_sfactor = data; break;
        case nv097::SET_BLEND_FUNC_DFACTOR: blend_dfactor = data; break;
        case nv097::SET_BLEND_EQUATION:     blend_equation = data; break;
        case nv097::SET_BLEND_COLOR:        blend_color = data; break;

        // Depth
        case nv097::SET_DEPTH_TEST_ENABLE: depth_test_enable = data; break;
        case nv097::SET_DEPTH_FUNC:        depth_func = data; break;
        case nv097::SET_DEPTH_MASK:        depth_mask = data; break;

        // Stencil
        case nv097::SET_STENCIL_TEST_ENABLE: stencil_test_enable = data; break;
        case nv097::SET_STENCIL_FUNC:        stencil_func = data; break;
        case nv097::SET_STENCIL_FUNC_REF:    stencil_ref = data; break;
        case nv097::SET_STENCIL_FUNC_MASK:   stencil_mask = data; break;
        case nv097::SET_STENCIL_OP_FAIL:     stencil_op_fail = data; break;
        case nv097::SET_STENCIL_OP_ZFAIL:    stencil_op_zfail = data; break;
        case nv097::SET_STENCIL_OP_ZPASS:    stencil_op_zpass = data; break;

        // Rasterizer
        case nv097::SET_CULL_FACE_ENABLE: cull_face_enable = data; break;
        case nv097::SET_CULL_FACE:        cull_face = data; break;
        case nv097::SET_FRONT_FACE:       front_face = data; break;
        case nv097::SET_SHADE_MODE:       shade_mode = data; break;
        case nv097::SET_COLOR_MASK:       color_mask = data; break;

        // Textures (4 stages, 64-byte stride)
        case nv097::SET_TEXTURE_OFFSET + 0*64:
        case nv097::SET_TEXTURE_OFFSET + 1*64:
        case nv097::SET_TEXTURE_OFFSET + 2*64:
        case nv097::SET_TEXTURE_OFFSET + 3*64: {
            uint32_t stage = (method - nv097::SET_TEXTURE_OFFSET) / 64;
            textures[stage].offset = data;
            break;
        }
        case nv097::SET_TEXTURE_FORMAT + 0*64:
        case nv097::SET_TEXTURE_FORMAT + 1*64:
        case nv097::SET_TEXTURE_FORMAT + 2*64:
        case nv097::SET_TEXTURE_FORMAT + 3*64: {
            uint32_t stage = (method - nv097::SET_TEXTURE_FORMAT) / 64;
            textures[stage].format = data;
            break;
        }
        case nv097::SET_TEXTURE_CONTROL0 + 0*64:
        case nv097::SET_TEXTURE_CONTROL0 + 1*64:
        case nv097::SET_TEXTURE_CONTROL0 + 2*64:
        case nv097::SET_TEXTURE_CONTROL0 + 3*64: {
            uint32_t stage = (method - nv097::SET_TEXTURE_CONTROL0) / 64;
            textures[stage].control0 = data;
            break;
        }
        case nv097::SET_TEXTURE_FILTER + 0*64:
        case nv097::SET_TEXTURE_FILTER + 1*64:
        case nv097::SET_TEXTURE_FILTER + 2*64:
        case nv097::SET_TEXTURE_FILTER + 3*64: {
            uint32_t stage = (method - nv097::SET_TEXTURE_FILTER) / 64;
            textures[stage].filter = data;
            break;
        }
        case nv097::SET_TEXTURE_IMAGE_RECT + 0*64:
        case nv097::SET_TEXTURE_IMAGE_RECT + 1*64:
        case nv097::SET_TEXTURE_IMAGE_RECT + 2*64:
        case nv097::SET_TEXTURE_IMAGE_RECT + 3*64: {
            uint32_t stage = (method - nv097::SET_TEXTURE_IMAGE_RECT) / 64;
            textures[stage].image_rect = data;
            break;
        }

        // Vertex shader program upload
        case nv097::SET_TRANSFORM_PROGRAM_LOAD:
            vs_program_load = data;
            vs_program_write_pos = data * 4;
            break;
        default:
            // Vertex shader program data (4 dwords per instruction slot,
            // 136 slots at methods 0x0B00-0x0B7C repeated)
            if (method >= nv097::SET_TRANSFORM_PROGRAM_START &&
                method <  nv097::SET_TRANSFORM_PROGRAM_START + 136 * 4 * 4) {
                uint32_t idx = vs_program_write_pos;
                if (idx < 136 * 4)
                    vs_program[idx] = data;
                vs_program_write_pos++;
                break;
            }

            // Vertex shader constants (192 × 4 floats at methods 0x0B80+)
            if (method >= nv097::SET_TRANSFORM_CONSTANT_LOAD &&
                method == nv097::SET_TRANSFORM_CONSTANT_LOAD) {
                vs_const_load = data;
                vs_const_write_pos = data * 4;
                break;
            }
            if (method >= nv097::SET_TRANSFORM_CONSTANT_START &&
                method <  nv097::SET_TRANSFORM_CONSTANT_START + 192 * 4 * 4) {
                uint32_t idx = vs_const_write_pos;
                if (idx < 192 * 4) {
                    float fval;
                    memcpy(&fval, &data, 4);
                    vs_constants[idx / 4][idx % 4] = fval;
                }
                vs_const_write_pos++;
                break;
            }

            // Register combiners
            if (method >= nv097::SET_COMBINER_COLOR_ICW_0 &&
                method <  nv097::SET_COMBINER_COLOR_ICW_0 + 8 * 4) {
                combiner_color_icw[(method - nv097::SET_COMBINER_COLOR_ICW_0) / 4] = data;
                break;
            }
            if (method >= nv097::SET_COMBINER_COLOR_OCW_0 &&
                method <  nv097::SET_COMBINER_COLOR_OCW_0 + 8 * 4) {
                combiner_color_ocw[(method - nv097::SET_COMBINER_COLOR_OCW_0) / 4] = data;
                break;
            }
            if (method >= nv097::SET_COMBINER_ALPHA_ICW_0 &&
                method <  nv097::SET_COMBINER_ALPHA_ICW_0 + 8 * 4) {
                combiner_alpha_icw[(method - nv097::SET_COMBINER_ALPHA_ICW_0) / 4] = data;
                break;
            }
            if (method >= nv097::SET_COMBINER_ALPHA_OCW_0 &&
                method <  nv097::SET_COMBINER_ALPHA_OCW_0 + 8 * 4) {
                combiner_alpha_ocw[(method - nv097::SET_COMBINER_ALPHA_OCW_0) / 4] = data;
                break;
            }
            break;  // unhandled method — ignored

        // Primitive
        case nv097::SET_BEGIN_END:
            if (begin_end_mode == 0 && data != 0)
                draw_count++;
            begin_end_mode = data;
            break;

        // Clear
        case nv097::CLEAR_SURFACE:
            clear_surface = data;
            clear_count++;
            break;

        // Combiner control
        case nv097::SET_COMBINER_CONTROL:
            combiner_control = data;
            break;
        }
    }
};

// Static method handler function for use with Nv2aState::method_handler.
static void pgraph_method_handler(void* user, uint32_t subchannel,
                                   uint32_t method, uint32_t data) {
    auto* pg = static_cast<PgraphState*>(user);
    pg->handle_method(subchannel, method, data);
}

} // namespace xbox
