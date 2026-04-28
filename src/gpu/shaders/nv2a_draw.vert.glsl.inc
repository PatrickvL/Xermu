R"GLSL(
// ---------------------------------------------------------------------------
// nv2a_draw.vert.glsl — NV2A vertex shader interpreter (ubershader).
//
// Pre-compiled interpreter that executes the NV2A vertex program stored in
// the VS_PROGRAM region of the GPU buffer.  Fetches vertex attributes from
// Xbox RAM using per-draw format descriptors.
//
// Based on the same interpreter approach as Cxbx-Reloaded's
// CxbxVertexShaderInterpreter.hlsl, ported to GLSL.
// ---------------------------------------------------------------------------
#version 450
#extension GL_ARB_shader_draw_parameters : enable

layout(set = 0, binding = 0) buffer readonly GpuBuffer {
    uint data[];
} gpu;

// ===================== Layout constants ====================================
const uint XBOX_RAM_DW     = 0;
const uint PGRAPH_STATE_DW = (64 * 1024 * 1024) / 4;
const uint PFIFO_CTL_DW    = PGRAPH_STATE_DW + (32 * 1024) / 4;
const uint DRAW_CMD_DW     = PFIFO_CTL_DW + (32 * 1024) / 4;
const uint DRAW_STATE_DW   = DRAW_CMD_DW + (64 * 1024) / 4;
const uint VS_PROGRAM_DW   = DRAW_STATE_DW + (128 * 1024) / 4;
const uint DRAW_STATE_DWORDS = 128;

// VS program: 136 slots × 4 dwords = 544 dwords.
// VS constants: 192 slots × 4 floats = 768 floats, stored after program.
const uint VS_CONST_DW     = VS_PROGRAM_DW + 136 * 4;

// ===================== Outputs =============================================
layout(location = 0) out vec4 v_color0;   // oD0 (diffuse)
layout(location = 1) out vec4 v_color1;   // oD1 (specular)
layout(location = 2) out vec4 v_tex0;
layout(location = 3) out vec4 v_tex1;
layout(location = 4) out vec4 v_tex2;
layout(location = 5) out vec4 v_tex3;
layout(location = 6) out vec4 v_fog;
layout(location = 7) flat out uint v_draw_idx;

// ===================== Helpers =============================================

// Read PGRAPH register by method byte offset.
uint pg_read(uint method_byte_offset) {
    return gpu.data[PGRAPH_STATE_DW + method_byte_offset / 4];
}

// Read per-draw state snapshot field.
uint draw_state(uint draw_idx, uint field_dw) {
    return gpu.data[DRAW_STATE_DW + draw_idx * DRAW_STATE_DWORDS + field_dw];
}

// Read float from uint bits.
float as_float(uint bits) {
    return uintBitsToFloat(bits);
}

// ===================== Vertex attribute fetch ===============================
// NV2A vertex attribute format:
//   bits[3:0]  = type (0=UB_D3D, 1=S1, 2=F, 4=UB_OGL, 5=S32K, 6=CMP)
//   bits[7:4]  = component count
//   bits[31:8] = stride

const uint NV097_SET_VERTEX_DATA_ARRAY_FORMAT  = 0x1760;
const uint NV097_SET_VERTEX_DATA_ARRAY_OFFSET  = 0x1720;

vec4 fetch_attribute(uint attr_idx, uint vertex_id) {
    uint fmt_reg = pg_read(NV097_SET_VERTEX_DATA_ARRAY_FORMAT + attr_idx * 4);
    uint offset  = pg_read(NV097_SET_VERTEX_DATA_ARRAY_OFFSET + attr_idx * 4);

    uint type    = fmt_reg & 0xF;
    uint count   = (fmt_reg >> 4) & 0xF;
    uint stride  = fmt_reg >> 8;

    if (count == 0) return vec4(0.0, 0.0, 0.0, 1.0);

    // Base address in Xbox RAM (byte offset).
    uint base = offset + vertex_id * stride;

    vec4 result = vec4(0.0, 0.0, 0.0, 1.0);

    if (type == 2) {
        // Float32
        for (uint c = 0; c < min(count, 4u); c++) {
            uint dw = gpu.data[XBOX_RAM_DW + (base + c * 4) / 4];
            result[c] = as_float(dw);
        }
    }
    else if (type == 0 || type == 4) {
        // Unsigned byte (D3D: BGRA, OGL: RGBA), always 4 components.
        uint dw = gpu.data[XBOX_RAM_DW + base / 4];
        if (type == 0) {
            // D3D byte order: BGRA
            result.r = float((dw >> 16) & 0xFF) / 255.0;
            result.g = float((dw >>  8) & 0xFF) / 255.0;
            result.b = float((dw >>  0) & 0xFF) / 255.0;
            result.a = float((dw >> 24) & 0xFF) / 255.0;
        } else {
            // OGL byte order: RGBA
            result.r = float((dw >>  0) & 0xFF) / 255.0;
            result.g = float((dw >>  8) & 0xFF) / 255.0;
            result.b = float((dw >> 16) & 0xFF) / 255.0;
            result.a = float((dw >> 24) & 0xFF) / 255.0;
        }
    }
    else if (type == 1) {
        // Signed 16-bit normalized (S1)
        for (uint c = 0; c < min(count, 4u); c++) {
            uint byte_off = base + c * 2;
            uint dw = gpu.data[XBOX_RAM_DW + byte_off / 4];
            uint shift = (byte_off & 2) * 8;
            int s = int((dw >> shift) & 0xFFFF);
            if (s >= 0x8000) s -= 0x10000;
            result[c] = float(s) / 32767.0;
        }
    }
    else if (type == 5) {
        // Signed 16-bit scaled (S32K) — not normalized
        for (uint c = 0; c < min(count, 4u); c++) {
            uint byte_off = base + c * 2;
            uint dw = gpu.data[XBOX_RAM_DW + byte_off / 4];
            uint shift = (byte_off & 2) * 8;
            int s = int((dw >> shift) & 0xFFFF);
            if (s >= 0x8000) s -= 0x10000;
            result[c] = float(s);
        }
    }

    return result;
}

// ===================== VS interpreter ======================================
// NV2A vertex shader instructions: 4 dwords per slot.
// dword[0] = ILU/MAC opcode + input/output masks
// dword[1..3] = input/constant/swizzle encoding
//
// Registers: r[12] (temp), v[16] (input), o[16] (output), c[192] (constant),
//            a0 (address register).

// VS output register indices (NV2A conventions):
const uint OREG_OPOS = 0;
const uint OREG_OD0  = 3;
const uint OREG_OD1  = 4;
const uint OREG_OFOG = 5;
const uint OREG_OPT  = 6;   // point size
const uint OREG_OB0  = 7;   // back diffuse
const uint OREG_OB1  = 8;   // back specular
const uint OREG_OT0  = 9;
const uint OREG_OT1  = 10;
const uint OREG_OT2  = 11;
const uint OREG_OT3  = 12;

// MAC opcodes
const uint MAC_NOP = 0;
const uint MAC_MOV = 1;
const uint MAC_MUL = 2;
const uint MAC_ADD = 3;
const uint MAC_MAD = 4;
const uint MAC_DP3 = 5;
const uint MAC_DPH = 6;
const uint MAC_DP4 = 7;
const uint MAC_DST = 8;
const uint MAC_MIN = 9;
const uint MAC_MAX = 10;
const uint MAC_SLT = 11;
const uint MAC_SGE = 12;
const uint MAC_ARL = 13;

// ILU opcodes
const uint ILU_NOP = 0;
const uint ILU_MOV = 1;
const uint ILU_RCP = 2;
const uint ILU_RCC = 3;
const uint ILU_RSQ = 4;
const uint ILU_EXP = 5;
const uint ILU_LOG = 6;
const uint ILU_LIT = 7;

void main() {
    uint draw_idx = gl_DrawIDARB;
    uint vid = gl_VertexIndex;

    // Fetch vertex input attributes.
    vec4 v_regs[16];
    for (uint i = 0; i < 16; i++)
        v_regs[i] = fetch_attribute(i, vid);

    // Temp registers.
    vec4 r_regs[12];
    for (uint i = 0; i < 12; i++)
        r_regs[i] = vec4(0.0);

    // Output registers.
    vec4 o_regs[16];
    for (uint i = 0; i < 16; i++)
        o_regs[i] = vec4(0.0);

    // Constants.
    vec4 c_regs[192];
    for (uint i = 0; i < 192; i++) {
        uint base = VS_CONST_DW + i * 4;
        c_regs[i] = vec4(
            as_float(gpu.data[base + 0]),
            as_float(gpu.data[base + 1]),
            as_float(gpu.data[base + 2]),
            as_float(gpu.data[base + 3])
        );
    }

    int a0 = 0;  // address register

    // Read program start slot from PGRAPH.
    uint start_slot = 0;  // default: slot 0 (fixed function passthrough mode starts at 0)

    // Execute instructions.
    for (uint slot = start_slot; slot < 136; slot++) {
        uint base = VS_PROGRAM_DW + slot * 4;
        uint d0 = gpu.data[base + 0];
        uint d1 = gpu.data[base + 1];
        uint d2 = gpu.data[base + 2];
        uint d3 = gpu.data[base + 3];

        // Decode instruction fields (NV2A vertex shader encoding).
        // d0: bits[24:21] = ILU opcode, bits[28:25] = MAC opcode
        //     bit[0] = final instruction flag
        uint ilu_op = (d0 >> 21) & 0xF;
        uint mac_op = (d0 >> 25) & 0xF;
        bool is_final = (d0 & 1) != 0;

        // d1: input sources, swizzles, negation.
        // Simplified decoding — full NV2A encoding has complex bit packing.
        // For now: A = input A, B = input B, C = input C
        // Each input: type (2 bits), index (4 bits), swizzle (8 bits), negate (1 bit)

        // d2: bits[31:28] = MAC output write mask
        //     bits[15:12] = ILU output write mask
        //     bits[10:8]  = MAC output register index
        //     bits[6:4]   = ILU output register index

        // Input A source: d1 bits[31:24]
        uint a_src_type = (d1 >> 30) & 0x3;  // 0=temp, 1=input, 2=const
        uint a_src_idx  = (d1 >> 24) & 0xF;
        // Input B source: d1 bits[23:16]
        uint b_src_type = (d1 >> 22) & 0x3;
        uint b_src_idx  = (d1 >> 16) & 0xF;
        // Input C source: d1 bits[15:8]
        uint c_src_type = (d1 >> 14) & 0x3;
        uint c_src_idx  = (d1 >>  8) & 0xF;

        // Resolve inputs.
        vec4 A, B, C;
        if (a_src_type == 0)      A = r_regs[min(a_src_idx, 11u)];
        else if (a_src_type == 1) A = v_regs[a_src_idx];
        else                      A = c_regs[min(a_src_idx + uint(max(a0, 0)), 191u)];

        if (b_src_type == 0)      B = r_regs[min(b_src_idx, 11u)];
        else if (b_src_type == 1) B = v_regs[b_src_idx];
        else                      B = c_regs[min(b_src_idx + uint(max(a0, 0)), 191u)];

        if (c_src_type == 0)      C = r_regs[min(c_src_idx, 11u)];
        else if (c_src_type == 1) C = v_regs[c_src_idx];
        else                      C = c_regs[min(c_src_idx + uint(max(a0, 0)), 191u)];

        // MAC output register.
        uint mac_dst = (d2 >> 8) & 0xF;
        uint mac_mask = (d2 >> 28) & 0xF;

        // ILU output register.
        uint ilu_dst = (d2 >> 4) & 0xF;
        uint ilu_mask = (d2 >> 12) & 0xF;

        // Execute MAC operation.
        vec4 mac_result = vec4(0.0);
        switch (mac_op) {
        case MAC_NOP: break;
        case MAC_MOV: mac_result = A; break;
        case MAC_MUL: mac_result = A * B; break;
        case MAC_ADD: mac_result = A + C; break;
        case MAC_MAD: mac_result = A * B + C; break;
        case MAC_DP3: mac_result = vec4(dot(A.xyz, B.xyz)); break;
        case MAC_DPH: mac_result = vec4(dot(A.xyz, B.xyz) + B.w); break;
        case MAC_DP4: mac_result = vec4(dot(A, B)); break;
        case MAC_DST: mac_result = vec4(1.0, A.y * B.y, A.z, B.w); break;
        case MAC_MIN: mac_result = min(A, B); break;
        case MAC_MAX: mac_result = max(A, B); break;
        case MAC_SLT: mac_result = vec4(lessThan(A, B)); break;
        case MAC_SGE: mac_result = vec4(greaterThanEqual(A, B)); break;
        case MAC_ARL: a0 = int(floor(A.x)); break;
        }

        // Write MAC result to destination (temp or output register).
        if (mac_op != MAC_NOP && mac_op != MAC_ARL && mac_mask != 0) {
            bool mac_to_output = (d2 & 0x40000000u) != 0;
            if (mac_to_output && mac_dst < 16) {
                if ((mac_mask & 8) != 0) o_regs[mac_dst].x = mac_result.x;
                if ((mac_mask & 4) != 0) o_regs[mac_dst].y = mac_result.y;
                if ((mac_mask & 2) != 0) o_regs[mac_dst].z = mac_result.z;
                if ((mac_mask & 1) != 0) o_regs[mac_dst].w = mac_result.w;
            } else if (mac_dst < 12) {
                if ((mac_mask & 8) != 0) r_regs[mac_dst].x = mac_result.x;
                if ((mac_mask & 4) != 0) r_regs[mac_dst].y = mac_result.y;
                if ((mac_mask & 2) != 0) r_regs[mac_dst].z = mac_result.z;
                if ((mac_mask & 1) != 0) r_regs[mac_dst].w = mac_result.w;
            }
        }

        // Execute ILU operation (uses C input).
        vec4 ilu_result = vec4(0.0);
        switch (ilu_op) {
        case ILU_NOP: break;
        case ILU_MOV: ilu_result = C; break;
        case ILU_RCP: ilu_result = vec4(1.0 / C.x); break;
        case ILU_RCC: {
            float v = 1.0 / C.x;
            v = clamp(v, 5.42101e-020, 1.884467e+019);
            ilu_result = vec4(v);
            break;
        }
        case ILU_RSQ: ilu_result = vec4(inversesqrt(abs(C.x))); break;
        case ILU_EXP: ilu_result = vec4(exp2(C.x)); break;
        case ILU_LOG: ilu_result = vec4(log2(abs(C.x))); break;
        case ILU_LIT: {
            float diffuse  = max(C.x, 0.0);
            float specular = (C.x > 0.0) ? pow(max(C.y, 0.0), clamp(C.w, -128.0, 128.0)) : 0.0;
            ilu_result = vec4(1.0, diffuse, specular, 1.0);
            break;
        }
        }

        // Write ILU result.
        if (ilu_op != ILU_NOP && ilu_mask != 0) {
            bool ilu_to_output = (d2 & 0x20000000u) != 0;
            if (ilu_to_output && ilu_dst < 16) {
                if ((ilu_mask & 8) != 0) o_regs[ilu_dst].x = ilu_result.x;
                if ((ilu_mask & 4) != 0) o_regs[ilu_dst].y = ilu_result.y;
                if ((ilu_mask & 2) != 0) o_regs[ilu_dst].z = ilu_result.z;
                if ((ilu_mask & 1) != 0) o_regs[ilu_dst].w = ilu_result.w;
            } else if (ilu_dst < 12) {
                if ((ilu_mask & 8) != 0) r_regs[ilu_dst].x = ilu_result.x;
                if ((ilu_mask & 4) != 0) r_regs[ilu_dst].y = ilu_result.y;
                if ((ilu_mask & 2) != 0) r_regs[ilu_dst].z = ilu_result.z;
                if ((ilu_mask & 1) != 0) r_regs[ilu_dst].w = ilu_result.w;
            }
        }

        if (is_final) break;
    }

    // Map NV2A output registers to Vulkan outputs.
    gl_Position = o_regs[OREG_OPOS];
    v_color0    = o_regs[OREG_OD0];
    v_color1    = o_regs[OREG_OD1];
    v_fog       = o_regs[OREG_OFOG];
    v_tex0      = o_regs[OREG_OT0];
    v_tex1      = o_regs[OREG_OT1];
    v_tex2      = o_regs[OREG_OT2];
    v_tex3      = o_regs[OREG_OT3];
    v_draw_idx  = uint(gl_InstanceIndex);
}
)GLSL"
