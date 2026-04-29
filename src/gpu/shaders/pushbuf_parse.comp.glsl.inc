R"GLSL(
// ---------------------------------------------------------------------------
// pushbuf_parse.comp.glsl — NV2A DMA push buffer parser (compute shader).
//
// Single-threaded (1,1,1 workgroup): NV2A push buffer processing is serial.
// Reads DMA commands from Xbox RAM, decodes method/data pairs, writes to
// the PGRAPH state buffer, and emits indirect draw commands + per-draw
// state snapshots on draw triggers.
// ---------------------------------------------------------------------------
#version 450
#extension GL_EXT_shader_explicit_arithmetic_types : enable

layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;

// Push constants from the CPU.
layout(push_constant) uniform PushConstants {
    uint max_dwords;   // budget per dispatch
    uint frame_id;
    uint pad0;
    uint pad1;
};

// Single SSBO covering the entire GPU allocation.
// Accessed as uint32 array.  Offsets match GpuBufferLayout.
layout(set = 0, binding = 0) buffer GpuBuffer {
    uint data[];
} gpu;

// ===================== Memory layout constants =============================
// Must match GpuBufferLayout in nv2a_vk_renderer.hpp.

const uint XBOX_RAM_OFFSET      = 0;                                // 0x00000000
const uint XBOX_RAM_SIZE        = 64 * 1024 * 1024;                 // 0x04000000
const uint PGRAPH_STATE_OFFSET  = XBOX_RAM_SIZE;                    // 0x04000000
const uint PGRAPH_STATE_SIZE    = 32 * 1024;                        // 0x00008000
const uint PFIFO_CTL_OFFSET     = PGRAPH_STATE_OFFSET + PGRAPH_STATE_SIZE; // 0x04008000
const uint PFIFO_CTL_SIZE       = 32 * 1024;
const uint DRAW_CMD_OFFSET      = PFIFO_CTL_OFFSET + PFIFO_CTL_SIZE;       // 0x04010000
const uint DRAW_CMD_SIZE        = 64 * 1024;
const uint DRAW_STATE_OFFSET    = DRAW_CMD_OFFSET + DRAW_CMD_SIZE;         // 0x04020000
const uint DRAW_STATE_SIZE      = 128 * 1024;
const uint VS_PROGRAM_OFFSET    = DRAW_STATE_OFFSET + DRAW_STATE_SIZE;     // 0x04040000
const uint VS_PROGRAM_SIZE      = 128 * 1024;

// All offsets in uint32 (dword) units.
const uint XBOX_RAM_DW      = XBOX_RAM_OFFSET / 4;
const uint PGRAPH_STATE_DW  = PGRAPH_STATE_OFFSET / 4;
const uint PFIFO_CTL_DW     = PFIFO_CTL_OFFSET / 4;
const uint DRAW_CMD_DW      = DRAW_CMD_OFFSET / 4;
const uint DRAW_STATE_DW    = DRAW_STATE_OFFSET / 4;
const uint VS_PROGRAM_DW    = VS_PROGRAM_OFFSET / 4;

// ===================== PfifoControlBlock fields ============================
// Must match struct PfifoControlBlock layout.
const uint CTL_DMA_PUT        = PFIFO_CTL_DW + 0;
const uint CTL_DMA_GET        = PFIFO_CTL_DW + 1;
const uint CTL_PUSH_ENABLED   = PFIFO_CTL_DW + 2;
const uint CTL_DRAW_COUNT     = PFIFO_CTL_DW + 3;
const uint CTL_FRAME_ID       = PFIFO_CTL_DW + 4;
const uint CTL_FLAGS          = PFIFO_CTL_DW + 5;
const uint CTL_SUBR_RETURN    = PFIFO_CTL_DW + 6;
const uint CTL_SUBR_ACTIVE    = PFIFO_CTL_DW + 7;
const uint CTL_PCRTC_START    = PFIFO_CTL_DW + 8;
const uint CTL_DMA_BASE       = PFIFO_CTL_DW + 9;

// ===================== PGRAPH register helpers =============================
const uint PGRAPH_REG_COUNT = 2048;  // 0x2000 bytes / 4

// NV097 method offsets (byte addresses, matching pgraph.hpp).
const uint NV097_SET_BEGIN_END                 = 0x17FC;
const uint NV097_DRAW_ARRAYS                   = 0x1810;
const uint NV097_SET_SURFACE_FORMAT            = 0x0208;
const uint NV097_SET_SURFACE_PITCH             = 0x020C;
const uint NV097_SET_SURFACE_COLOR_OFFSET      = 0x0210;
const uint NV097_SET_SURFACE_ZETA_OFFSET       = 0x0214;
const uint NV097_SET_VIEWPORT_OFFSET_X         = 0x0A20;
const uint NV097_SET_VIEWPORT_SCALE_X          = 0x0AF0;
const uint NV097_SET_FOG_COLOR                 = 0x02A8;
const uint NV097_SET_BLEND_ENABLE              = 0x0304;
const uint NV097_SET_DEPTH_TEST_ENABLE         = 0x030C;
const uint NV097_SET_BLEND_FUNC_SFACTOR        = 0x0344;
const uint NV097_SET_BLEND_FUNC_DFACTOR        = 0x0348;
const uint NV097_SET_BLEND_EQUATION            = 0x0350;
const uint NV097_SET_DEPTH_FUNC                = 0x0354;
const uint NV097_SET_COLOR_MASK                = 0x0358;
const uint NV097_SET_DEPTH_MASK                = 0x035C;
const uint NV097_SET_COMBINER_ALPHA_ICW_0      = 0x0260;
const uint NV097_SET_COMBINER_SPECULAR_FOG_CW0 = 0x0288;
const uint NV097_SET_COMBINER_SPECULAR_FOG_CW1 = 0x028C;
const uint NV097_SET_COMBINER_FACTOR0_0        = 0x0A60;
const uint NV097_SET_COMBINER_FACTOR1_0        = 0x0A80;
const uint NV097_SET_COMBINER_ALPHA_OCW_0      = 0x0AA0;
const uint NV097_SET_COMBINER_COLOR_ICW_0      = 0x0AC0;
const uint NV097_SET_COMBINER_COLOR_OCW_0      = 0x1E40;
const uint NV097_SET_COMBINER_CONTROL          = 0x1E60;
const uint NV097_SET_TEXTURE_OFFSET            = 0x1B00;
const uint NV097_SET_TEXTURE_FORMAT            = 0x1B04;
const uint NV097_SET_TEXTURE_CONTROL0          = 0x1B0C;
const uint NV097_SET_TEXTURE_FILTER            = 0x1B14;
const uint NV097_SET_TEXTURE_IMAGE_RECT        = 0x1B1C;
const uint NV097_SET_VERTEX_DATA_ARRAY_FORMAT  = 0x1760;
const uint NV097_SET_VERTEX_DATA_ARRAY_OFFSET  = 0x1720;
const uint NV097_SET_TRANSFORM_PROGRAM_LOAD    = 0x1E9C;
const uint NV097_SET_TRANSFORM_PROGRAM_START   = 0x1EA0;
const uint NV097_SET_TRANSFORM_CONSTANT_LOAD   = 0x1EA4;
const uint NV097_SET_TRANSFORM_CONSTANT_START  = 0x0B80;
const uint NV097_CLEAR_SURFACE                 = 0x1D94;

// DrawStateSnapshot size in dwords (must match C++ struct, 512B = 128 dwords).
const uint DRAW_STATE_DWORDS = 128;

// ===================== Local state for streaming uploads ===================
// VS program/constant upload state (mirrors CPU-side PgraphState members).
uint vs_program_write_pos = 0;
uint vs_const_write_pos   = 0;

// ===================== Helper: read PGRAPH register ========================
uint pg_read(uint method_byte_offset) {
    return gpu.data[PGRAPH_STATE_DW + method_byte_offset / 4];
}

// ===================== Helper: write PGRAPH register =======================
void pg_write(uint method_byte_offset, uint value) {
    gpu.data[PGRAPH_STATE_DW + method_byte_offset / 4] = value;
}

// ===================== Helper: read dword from Xbox RAM ====================
uint ram_read(uint byte_offset) {
    if (byte_offset + 4 > XBOX_RAM_SIZE) return 0;
    return gpu.data[XBOX_RAM_DW + byte_offset / 4];
}

// ===================== Emit a draw command ==================================
void emit_draw(uint vertex_count, uint primitive_mode) {
    uint draw_idx = atomicAdd(gpu.data[CTL_DRAW_COUNT], 1);
    if (draw_idx >= 1024) return;  // overflow guard

    // Write VkDrawIndirectCommand at DRAW_CMD region.
    uint cmd_dw = DRAW_CMD_DW + draw_idx * 4;
    gpu.data[cmd_dw + 0] = vertex_count;       // vertexCount
    gpu.data[cmd_dw + 1] = 1;                  // instanceCount
    gpu.data[cmd_dw + 2] = 0;                  // firstVertex
    gpu.data[cmd_dw + 3] = draw_idx;           // firstInstance (= draw index)

    // Snapshot per-draw state from PGRAPH registers.
    uint snap_dw = DRAW_STATE_DW + draw_idx * DRAW_STATE_DWORDS;

    // Surface state
    gpu.data[snap_dw +  0] = pg_read(NV097_SET_SURFACE_FORMAT);
    gpu.data[snap_dw +  1] = pg_read(NV097_SET_SURFACE_PITCH);
    gpu.data[snap_dw +  2] = pg_read(NV097_SET_SURFACE_COLOR_OFFSET);
    gpu.data[snap_dw +  3] = pg_read(NV097_SET_SURFACE_ZETA_OFFSET);

    // Viewport offset (4 floats stored as uint bits)
    for (uint i = 0; i < 4; i++)
        gpu.data[snap_dw + 4 + i] = pg_read(NV097_SET_VIEWPORT_OFFSET_X + i * 4);
    // Viewport scale (4 floats)
    for (uint i = 0; i < 4; i++)
        gpu.data[snap_dw + 8 + i] = pg_read(NV097_SET_VIEWPORT_SCALE_X + i * 4);

    // Blend/depth state
    gpu.data[snap_dw + 12] = pg_read(NV097_SET_BLEND_ENABLE);
    gpu.data[snap_dw + 13] = pg_read(NV097_SET_BLEND_FUNC_SFACTOR);
    gpu.data[snap_dw + 14] = pg_read(NV097_SET_BLEND_FUNC_DFACTOR);
    gpu.data[snap_dw + 15] = pg_read(NV097_SET_BLEND_EQUATION);
    gpu.data[snap_dw + 16] = pg_read(NV097_SET_DEPTH_TEST_ENABLE);
    gpu.data[snap_dw + 17] = pg_read(NV097_SET_DEPTH_FUNC);
    gpu.data[snap_dw + 18] = pg_read(NV097_SET_DEPTH_MASK);
    gpu.data[snap_dw + 19] = pg_read(NV097_SET_COLOR_MASK);

    // Combiner control
    gpu.data[snap_dw + 20] = pg_read(NV097_SET_COMBINER_CONTROL);
    // Color ICW [8]
    for (uint i = 0; i < 8; i++)
        gpu.data[snap_dw + 21 + i] = pg_read(NV097_SET_COMBINER_COLOR_ICW_0 + i * 4);
    // Color OCW [8]
    for (uint i = 0; i < 8; i++)
        gpu.data[snap_dw + 29 + i] = pg_read(NV097_SET_COMBINER_COLOR_OCW_0 + i * 4);
    // Alpha ICW [8]
    for (uint i = 0; i < 8; i++)
        gpu.data[snap_dw + 37 + i] = pg_read(NV097_SET_COMBINER_ALPHA_ICW_0 + i * 4);
    // Alpha OCW [8]
    for (uint i = 0; i < 8; i++)
        gpu.data[snap_dw + 45 + i] = pg_read(NV097_SET_COMBINER_ALPHA_OCW_0 + i * 4);

    // Texture state (4 stages, 5 regs each, stride 64 bytes = 16 dwords per stage)
    for (uint s = 0; s < 4; s++) {
        uint tex_base = s * 64;  // byte stride between texture stages
        gpu.data[snap_dw + 53 + s]      = pg_read(NV097_SET_TEXTURE_OFFSET + tex_base);
        gpu.data[snap_dw + 57 + s]      = pg_read(NV097_SET_TEXTURE_FORMAT + tex_base);
        gpu.data[snap_dw + 61 + s]      = pg_read(NV097_SET_TEXTURE_CONTROL0 + tex_base);
        gpu.data[snap_dw + 65 + s]      = pg_read(NV097_SET_TEXTURE_FILTER + tex_base);
        gpu.data[snap_dw + 69 + s]      = pg_read(NV097_SET_TEXTURE_IMAGE_RECT + tex_base);
    }

    // Vertex attribute format descriptors [16]
    for (uint i = 0; i < 16; i++)
        gpu.data[snap_dw + 73 + i] = pg_read(NV097_SET_VERTEX_DATA_ARRAY_FORMAT + i * 4);

    // Vertex data offset (attribute 0 offset as base)
    gpu.data[snap_dw + 89] = pg_read(NV097_SET_VERTEX_DATA_ARRAY_OFFSET);

    // Primitive mode
    gpu.data[snap_dw + 90] = primitive_mode;

    // Combiner factor colors C0 per-stage [8]
    for (uint i = 0; i < 8; i++)
        gpu.data[snap_dw + 91 + i] = pg_read(NV097_SET_COMBINER_FACTOR0_0 + i * 4);
    // Combiner factor colors C1 per-stage [8]
    for (uint i = 0; i < 8; i++)
        gpu.data[snap_dw + 99 + i] = pg_read(NV097_SET_COMBINER_FACTOR1_0 + i * 4);

    // Fog color
    gpu.data[snap_dw + 107] = pg_read(NV097_SET_FOG_COLOR);

    // Final combiner specular/fog config words
    gpu.data[snap_dw + 108] = pg_read(NV097_SET_COMBINER_SPECULAR_FOG_CW0);
    gpu.data[snap_dw + 109] = pg_read(NV097_SET_COMBINER_SPECULAR_FOG_CW1);
}

// ===================== Process a single method write ========================
void handle_method(uint method, uint data_val) {
    // Vertex shader program streaming upload
    if (method >= NV097_SET_TRANSFORM_PROGRAM_START &&
        method <  NV097_SET_TRANSFORM_PROGRAM_START + 136 * 4 * 4) {
        if (vs_program_write_pos < 136 * 4)
            gpu.data[VS_PROGRAM_DW + vs_program_write_pos] = data_val;
        vs_program_write_pos++;
        return;
    }

    // Vertex shader constant streaming upload
    // Constants stored at VS_PROGRAM_DW + 136*4 (after program slots).
    if (method >= NV097_SET_TRANSFORM_CONSTANT_START &&
        method <  NV097_SET_TRANSFORM_CONSTANT_START + 192 * 4 * 4) {
        uint const_base = 136 * 4;  // offset past program data
        if (vs_const_write_pos < 192 * 4)
            gpu.data[VS_PROGRAM_DW + const_base + vs_const_write_pos] = data_val;
        vs_const_write_pos++;
        return;
    }

    // Special methods
    switch (method) {
    case NV097_SET_TRANSFORM_PROGRAM_LOAD:
        vs_program_write_pos = data_val * 4;
        pg_write(method, data_val);
        return;

    case NV097_SET_TRANSFORM_CONSTANT_LOAD:
        vs_const_write_pos = data_val * 4;
        pg_write(method, data_val);
        return;

    case NV097_SET_BEGIN_END: {
        uint prev = pg_read(NV097_SET_BEGIN_END);
        pg_write(method, data_val);

        if (prev != 0 && data_val == 0) {
            // END: emit the draw with accumulated vertex count.
            // For now, vertex count = 0 (populated by DRAW_ARRAYS or inline data).
            // The draw was already emitted at BEGIN with vertex count 0;
            // a later DRAW_ARRAYS overrides it.
        }
        if (prev == 0 && data_val != 0) {
            // BEGIN: record primitive mode, draw will be emitted at END or DRAW_ARRAYS.
        }
        return;
    }

    case NV097_DRAW_ARRAYS: {
        // data = (start << 0) | (count << 24)  — NV2A encoding
        // Actually: bits[23:0] = start_index, bits[31:24] = count - 1
        uint start = data_val & 0x00FFFFFFu;
        uint count = ((data_val >> 24) & 0xFF) + 1;
        uint prim  = pg_read(NV097_SET_BEGIN_END);
        emit_draw(count, prim);
        return;
    }

    case NV097_CLEAR_SURFACE:
        pg_write(method, data_val);
        // Emit a fullscreen draw with clear flag.
        // The clear color/depth values are already in PGRAPH state;
        // the draw pipeline reads them from the per-draw snapshot.
        // Use primitive_mode = 0xFFFF as a sentinel for "clear operation".
        emit_draw(3, 0xFFFF);
        return;

    default:
        break;
    }

    // Default: write method data into the PGRAPH state register file.
    uint reg_idx = method / 4;
    if (reg_idx < PGRAPH_REG_COUNT)
        pg_write(method, data_val);
}

// ===================== Main entry point ====================================
void main() {
    // Check if push is enabled.
    if (gpu.data[CTL_PUSH_ENABLED] == 0) return;

    uint get = gpu.data[CTL_DMA_GET];
    uint put = gpu.data[CTL_DMA_PUT];
    if (get == put) return;

    uint dma_base = gpu.data[CTL_DMA_BASE];

    // Restore VS upload positions from PGRAPH state.
    vs_program_write_pos = pg_read(NV097_SET_TRANSFORM_PROGRAM_LOAD) * 4;
    vs_const_write_pos   = pg_read(NV097_SET_TRANSFORM_CONSTANT_LOAD) * 4;

    uint consumed = 0;

    while (get != put && consumed < max_dwords) {
        uint phys = dma_base + get;
        if (phys + 4 > XBOX_RAM_SIZE) break;

        uint hdr = ram_read(phys);
        get += 4;
        consumed++;

        if (hdr == 0) continue;  // NOP

        // OLD_JUMP: bits[31:29]=001, bits[1:0]=00
        if ((hdr & 0xE0000003u) == 0x20000000u) {
            get = hdr & 0x1FFFFFFFu;
            continue;
        }

        // JUMP (new style): bits[1:0]=01
        if ((hdr & 3u) == 1u) {
            get = hdr & 0xFFFFFFFCu;
            continue;
        }

        // CALL (bits[1:0] == 2)
        if ((hdr & 3u) == 2u) {
            if (gpu.data[CTL_SUBR_ACTIVE] == 0) {
                gpu.data[CTL_SUBR_RETURN] = get;
                gpu.data[CTL_SUBR_ACTIVE] = 1;
                get = hdr & 0xFFFFFFFCu;
            }
            continue;
        }

        // RETURN
        if (hdr == 0x00020000u) {
            if (gpu.data[CTL_SUBR_ACTIVE] != 0) {
                get = gpu.data[CTL_SUBR_RETURN];
                gpu.data[CTL_SUBR_ACTIVE] = 0;
            }
            continue;
        }

        // Method command detection (matching xemu/NV2A hardware):
        // Increasing: (hdr & 0xE0030003) == 0
        // Non-increasing: (hdr & 0xE0030003) == 0x40000000
        uint type;
        if ((hdr & 0xE0030003u) == 0u)
            type = 0;  // increasing
        else if ((hdr & 0xE0030003u) == 0x40000000u)
            type = 4;  // non-increasing
        else
            continue;  // unknown, skip

        {
            uint method     = hdr & 0x1FFCu;
            uint subchannel = (hdr >> 13) & 0x7;
            uint count      = (hdr >> 18) & 0x7FFu;

            for (uint i = 0; i < count && consumed < max_dwords; i++) {
                uint dphys = dma_base + get;
                if (dphys + 4 > XBOX_RAM_SIZE) { get = put; break; }

                uint data_val = ram_read(dphys);
                get += 4;
                consumed++;

                uint m = (type == 0) ? (method + i * 4) : method;

                // Only process subchannel 0 (3D engine / Kelvin) methods >= 0x100.
                // Methods < 0x100 are PFIFO internals (SET_OBJECT, etc.).
                if (m >= 0x0100)
                    handle_method(m, data_val);
            }
        }
    }

    // Update DMA_GET.
    gpu.data[CTL_DMA_GET] = get;
}
)GLSL"
