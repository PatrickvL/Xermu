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
    // --- Flat register file ---
    // Indexed by method/4.  Covers the NV097 method space (0x0000..0x1FFF).
    // Some slots hold integer data, others hold IEEE 754 floats (e.g. viewport).
    static constexpr uint32_t METHOD_SPACE = 0x2000;          // 8 KB method range
    static constexpr uint32_t REG_COUNT    = METHOD_SPACE / 4; // 2048 uint32_t slots
    uint32_t regs[REG_COUNT] = {};

    // Typed access helpers.
    uint32_t& reg(uint32_t method)       { return regs[method / 4]; }
    uint32_t  reg(uint32_t method) const { return regs[method / 4]; }

    float reg_float(uint32_t method) const {
        float f; memcpy(&f, &regs[method / 4], 4); return f;
    }
    void set_reg_float(uint32_t method, float f) {
        memcpy(&regs[method / 4], &f, 4);
    }

    // --- Vertex shader program (streaming upload, not direct registers) ---
    uint32_t vs_program_load  = 0;  // current load slot
    uint32_t vs_program[136 * 4] = {};  // 136 instruction slots × 4 dwords
    uint32_t vs_program_write_pos = 0;

    // --- Vertex shader constants (192 × 4 floats = 768 floats) ---
    uint32_t vs_const_load = 0;  // current load index
    float    vs_constants[192][4] = {};
    uint32_t vs_const_write_pos = 0;

    // --- Stats ---
    uint32_t draw_count = 0;  // number of SET_BEGIN_END transitions from 0→nonzero
    uint32_t clear_count = 0;
    uint32_t total_methods = 0;
    uint32_t clear_color_count = 0;  // clears that include color buffer
    uint32_t clear_depth_count = 0;  // clears that include depth buffer

    // --- Surface decode helpers ---
    // SET_SURFACE_FORMAT encoding:
    //   bits [3:0]   = color format (1=X1R5G5B5, 3=R5G6B5, 5=X8R8G8B8, 8=A8R8G8B8)
    //   bits [7:4]   = zeta format (1=Z16, 2=Z24S8)
    //   bits [11:8]  = type (1=pitch, 2=swizzle)
    //   bits [15:12] = anti-alias mode
    //   bits [19:16] = width log2  (swizzle mode)
    //   bits [23:20] = height log2 (swizzle mode)

    uint32_t surface_color_format() const { return reg(nv097::SET_SURFACE_FORMAT) & 0xF; }
    uint32_t surface_zeta_format()  const { return (reg(nv097::SET_SURFACE_FORMAT) >> 4) & 0xF; }
    uint32_t surface_type()         const { return (reg(nv097::SET_SURFACE_FORMAT) >> 8) & 0xF; }
    uint32_t surface_aa_mode()      const { return (reg(nv097::SET_SURFACE_FORMAT) >> 12) & 0xF; }
    uint32_t surface_width_log2()   const { return (reg(nv097::SET_SURFACE_FORMAT) >> 16) & 0xF; }
    uint32_t surface_height_log2()  const { return (reg(nv097::SET_SURFACE_FORMAT) >> 20) & 0xF; }
    uint32_t surface_color_pitch()  const { return reg(nv097::SET_SURFACE_PITCH) & 0xFFFF; }
    uint32_t surface_zeta_pitch()   const { return reg(nv097::SET_SURFACE_PITCH) >> 16; }
    uint32_t surface_clip_x()       const { return reg(nv097::SET_SURFACE_CLIP_HORIZONTAL) & 0xFFFF; }
    uint32_t surface_clip_width()   const { return reg(nv097::SET_SURFACE_CLIP_HORIZONTAL) >> 16; }
    uint32_t surface_clip_y()       const { return reg(nv097::SET_SURFACE_CLIP_VERTICAL) & 0xFFFF; }
    uint32_t surface_clip_height()  const { return reg(nv097::SET_SURFACE_CLIP_VERTICAL) >> 16; }

    // Process a single GPU method call.
    void handle_method(uint32_t subchannel, uint32_t method, uint32_t data) {
        (void)subchannel;
        total_methods++;

        // --- Methods that need special handling ---
        switch (method) {
        case nv097::SET_BEGIN_END:
            if (reg(nv097::SET_BEGIN_END) == 0 && data != 0)
                draw_count++;
            reg(method) = data;
            return;

        case nv097::CLEAR_SURFACE:
            reg(method) = data;
            clear_count++;
            // Bits: 0=Z, 1=stencil, 4=R, 5=G, 6=B, 7=A
            if (data & 0xF0) clear_color_count++;  // any color channel
            if (data & 0x03) clear_depth_count++;   // Z or stencil
            return;

        case nv097::SET_TRANSFORM_PROGRAM_LOAD:
            vs_program_load = data;
            vs_program_write_pos = data * 4;
            reg(method) = data;
            return;

        case nv097::SET_TRANSFORM_CONSTANT_LOAD:
            vs_const_load = data;
            vs_const_write_pos = data * 4;
            reg(method) = data;
            return;

        default:
            break;
        }

        // --- Vertex shader program upload (streaming) ---
        if (method >= nv097::SET_TRANSFORM_PROGRAM_START &&
            method <  nv097::SET_TRANSFORM_PROGRAM_START + 136 * 4 * 4) {
            uint32_t idx = vs_program_write_pos;
            if (idx < 136 * 4)
                vs_program[idx] = data;
            vs_program_write_pos++;
            return;
        }

        // --- Vertex shader constant upload (streaming, float data) ---
        if (method >= nv097::SET_TRANSFORM_CONSTANT_START &&
            method <  nv097::SET_TRANSFORM_CONSTANT_START + 192 * 4 * 4) {
            uint32_t idx = vs_const_write_pos;
            if (idx < 192 * 4) {
                float fval;
                memcpy(&fval, &data, 4);
                vs_constants[idx / 4][idx % 4] = fval;
            }
            vs_const_write_pos++;
            return;
        }

        // --- Default: store data directly into the register file ---
        if (method / 4 < REG_COUNT)
            regs[method / 4] = data;
    }
};

// Static method handler function for use with Nv2aState::method_handler.
static void pgraph_method_handler(void* user, uint32_t subchannel,
                                   uint32_t method, uint32_t data) {
    auto* pg = static_cast<PgraphState*>(user);
    pg->handle_method(subchannel, method, data);
}

} // namespace xbox
