// ---------------------------------------------------------------------------
// nv2a_draw.frag.glsl — NV2A register combiner interpreter (ubershader).
//
// Pre-compiled interpreter that evaluates up to 8 general combiner stages
// plus a final combiner, reading configuration from per-draw state snapshots.
//
// Based on Cxbx-Reloaded's CxbxRegisterCombinerInterpreter.hlsl approach,
// ported to GLSL.
// ---------------------------------------------------------------------------
#version 450

layout(location = 0) in  vec4 v_color0;   // oD0 (diffuse)
layout(location = 1) in  vec4 v_color1;   // oD1 (specular)
layout(location = 2) in  vec4 v_tex0;
layout(location = 3) in  vec4 v_tex1;
layout(location = 4) in  vec4 v_tex2;
layout(location = 5) in  vec4 v_tex3;
layout(location = 6) in  vec4 v_fog;
layout(location = 7) flat in  uint v_draw_idx;

layout(location = 0) out vec4 o_color;

layout(set = 0, binding = 0) buffer readonly GpuBuffer {
    uint data[];
} gpu;

// ===================== Layout constants ====================================
const uint DRAW_CMD_DW     = ((64 * 1024 * 1024) + (32 * 1024) + (32 * 1024)) / 4;
const uint DRAW_STATE_DW   = DRAW_CMD_DW + (64 * 1024) / 4;
const uint DRAW_STATE_DWORDS = 128;

// Per-draw state field offsets (dword index within a DrawStateSnapshot).
const uint DS_COMBINER_CONTROL = 20;
const uint DS_COLOR_ICW_0      = 21;  // [8]
const uint DS_COLOR_OCW_0      = 29;  // [8]
const uint DS_ALPHA_ICW_0      = 37;  // [8]
const uint DS_ALPHA_OCW_0      = 45;  // [8]
const uint DS_FACTOR0_0        = 91;  // [8] combiner constant C0 per stage
const uint DS_FACTOR1_0        = 99;  // [8] combiner constant C1 per stage
const uint DS_FOG_COLOR        = 107;
const uint DS_SPEC_FOG_CW0     = 108;
const uint DS_SPEC_FOG_CW1     = 109;

// ===================== Combiner register file ==============================
// NV2A register combiner input registers (accessible by index 0..15):
//   0 = ZERO
//   1 = CONSTANT_COLOR0 (C0)
//   2 = CONSTANT_COLOR1 (C1)
//   3 = FOG_COLOR
//   4 = PRIMARY_COLOR (v_color0 / diffuse)
//   5 = SECONDARY_COLOR (v_color1 / specular)
//   8 = TEXTURE0
//   9 = TEXTURE1
//  10 = TEXTURE2 (NV2A extension)
//  11 = TEXTURE3 (NV2A extension)
//  12 = SPARE0
//  13 = SPARE1
//  14 = SPARE0_PLUS_SECONDARY
//  15 = EF_PROD (E times F in final combiner)

vec4 comb_regs[16];

// ===================== Combiner helpers ====================================

// Map a 4-bit input register index to combiner_regs[].
vec4 resolve_input(uint reg_idx) {
    return comb_regs[reg_idx & 0xF];
}

// Apply input mapping (bits [5:4] of each ICW byte):
//   0 = UNSIGNED_IDENTITY  → max(0, x)
//   1 = UNSIGNED_INVERT    → 1 - max(0, x) = 1 - clamp(x, 0, 1)
//   2 = EXPAND_NORMAL      → 2*max(0, x) - 1
//   3 = EXPAND_NEGATE       → 1 - 2*max(0, x) = -(2*clamp(x,0,1) - 1)
//   4 = HALFBIAS_NORMAL    → max(0, x) - 0.5
//   5 = HALFBIAS_NEGATE    → 0.5 - max(0, x)
//   6 = SIGNED_IDENTITY    → x
//   7 = SIGNED_NEGATE      → -x
vec4 apply_mapping(vec4 v, uint mapping) {
    switch (mapping) {
    case 0: return max(v, vec4(0.0));
    case 1: return vec4(1.0) - clamp(v, vec4(0.0), vec4(1.0));
    case 2: return 2.0 * max(v, vec4(0.0)) - vec4(1.0);
    case 3: return vec4(1.0) - 2.0 * max(v, vec4(0.0));
    case 4: return max(v, vec4(0.0)) - vec4(0.5);
    case 5: return vec4(0.5) - max(v, vec4(0.0));
    case 6: return v;
    case 7: return -v;
    default: return v;
    }
}

// Decode one ICW byte: bits[7:4] = register, bits[3:0] = mapping | alpha_flag.
// Actually NV2A ICW encoding per input (8 bits):
//   bits[7:4] = input register index
//   bits[3:1] = mapping (see above)
//   bit[0]    = alpha replicate (if 1, use .aaaa)
vec4 decode_input(uint icw_byte) {
    uint reg_idx = (icw_byte >> 4) & 0xF;
    uint mapping = (icw_byte >> 1) & 0x7;
    bool alpha_rep = (icw_byte & 1) != 0;

    vec4 v = resolve_input(reg_idx);
    if (alpha_rep) v = vec4(v.a);
    return apply_mapping(v, mapping);
}

// Unpack NV2A color register (0xAARRGGBB packed dword) to vec4.
vec4 unpack_color(uint packed) {
    float r = float((packed >> 16) & 0xFF) / 255.0;
    float g = float((packed >>  8) & 0xFF) / 255.0;
    float b = float((packed >>  0) & 0xFF) / 255.0;
    float a = float((packed >> 24) & 0xFF) / 255.0;
    return vec4(r, g, b, a);
}

// ===================== Main ================================================

void main() {
    // Use draw index passed from vertex shader (= firstInstance from emit_draw).
    uint draw_idx = v_draw_idx;
    uint snap_base = DRAW_STATE_DW + draw_idx * DRAW_STATE_DWORDS;

    // Read combiner control: bits[3:0] = number of general combiner stages.
    uint combiner_control = gpu.data[snap_base + DS_COMBINER_CONTROL];
    uint num_stages = combiner_control & 0xF;
    if (num_stages == 0) num_stages = 1;
    if (num_stages > 8) num_stages = 8;

    // Read fog color from per-draw snapshot.
    vec4 fog_color = unpack_color(gpu.data[snap_base + DS_FOG_COLOR]);

    // Initialize combiner register file.
    // C0 and C1 are per-stage; start with stage 0 values (updated per-stage below).
    comb_regs[0]  = vec4(0.0);                    // ZERO
    comb_regs[1]  = unpack_color(gpu.data[snap_base + DS_FACTOR0_0]);  // C0 stage 0
    comb_regs[2]  = unpack_color(gpu.data[snap_base + DS_FACTOR1_0]);  // C1 stage 0
    comb_regs[3]  = fog_color;                    // FOG_COLOR
    comb_regs[4]  = v_color0;                     // PRIMARY_COLOR
    comb_regs[5]  = v_color1;                     // SECONDARY_COLOR
    comb_regs[6]  = vec4(0.0);
    comb_regs[7]  = vec4(0.0);
    comb_regs[8]  = v_tex0;                       // TEXTURE0
    comb_regs[9]  = v_tex1;                       // TEXTURE1
    comb_regs[10] = v_tex2;                       // TEXTURE2
    comb_regs[11] = v_tex3;                       // TEXTURE3
    comb_regs[12] = vec4(0.0);                    // SPARE0
    comb_regs[13] = vec4(0.0);                    // SPARE1
    comb_regs[14] = vec4(0.0);                    // SPARE0_PLUS_SECONDARY
    comb_regs[15] = vec4(0.0);                    // EF_PROD

    // Set SPARE0 initial alpha to TEXTURE0 alpha (NV2A convention).
    comb_regs[12].a = v_tex0.a;

    // ===================== General combiner stages =========================
    for (uint stage = 0; stage < num_stages; stage++) {
        // Update per-stage combiner constants C0 and C1.
        comb_regs[1] = unpack_color(gpu.data[snap_base + DS_FACTOR0_0 + stage]);
        comb_regs[2] = unpack_color(gpu.data[snap_base + DS_FACTOR1_0 + stage]);

        // Color ICW: 4 input bytes packed in one dword (A, B, C, D).
        uint color_icw = gpu.data[snap_base + DS_COLOR_ICW_0 + stage];
        // Alpha ICW:
        uint alpha_icw = gpu.data[snap_base + DS_ALPHA_ICW_0 + stage];
        // Color OCW:
        uint color_ocw = gpu.data[snap_base + DS_COLOR_OCW_0 + stage];
        // Alpha OCW:
        uint alpha_ocw = gpu.data[snap_base + DS_ALPHA_OCW_0 + stage];

        // --- Color combiner: AB + CD ---
        vec4 cA = decode_input((color_icw >> 24) & 0xFF);
        vec4 cB = decode_input((color_icw >> 16) & 0xFF);
        vec4 cC = decode_input((color_icw >>  8) & 0xFF);
        vec4 cD = decode_input((color_icw >>  0) & 0xFF);

        vec3 ab = cA.rgb * cB.rgb;
        vec3 cd = cC.rgb * cD.rgb;
        vec3 color_sum = ab + cd;

        // OCW: bits[7:4] = output AB register, bits[3:0] = output CD register
        //      bits[11:8] = output SUM register
        //      bits[14:12] = scale/bias for AB
        //      bits[17:15] = scale/bias for CD
        //      bits[20:18] = scale/bias for SUM
        //      bit[21] = AB_DOT (dot product mode)
        //      bit[22] = CD_DOT (dot product mode)
        //      bit[23] = MUX (use MUX instead of SUM)
        uint out_ab_reg  = (color_ocw >> 4) & 0xF;
        uint out_cd_reg  = (color_ocw >> 0) & 0xF;
        uint out_sum_reg = (color_ocw >> 8) & 0xF;
        bool ab_dot  = (color_ocw & (1 << 21)) != 0;
        bool cd_dot  = (color_ocw & (1 << 22)) != 0;
        bool use_mux = (color_ocw & (1 << 23)) != 0;

        if (ab_dot) ab = vec3(dot(cA.rgb, cB.rgb));
        if (cd_dot) cd = vec3(dot(cC.rgb, cD.rgb));

        if (use_mux)
            color_sum = (comb_regs[12].a >= 0.5) ? cd : ab;
        else
            color_sum = ab + cd;

        // Write color outputs.
        if (out_ab_reg != 0)  comb_regs[out_ab_reg].rgb  = ab;
        if (out_cd_reg != 0)  comb_regs[out_cd_reg].rgb  = cd;
        if (out_sum_reg != 0) comb_regs[out_sum_reg].rgb = color_sum;

        // --- Alpha combiner: aAB + aCD ---
        float aA = decode_input((alpha_icw >> 24) & 0xFF).a;
        float aB = decode_input((alpha_icw >> 16) & 0xFF).a;
        float aC = decode_input((alpha_icw >>  8) & 0xFF).a;
        float aD = decode_input((alpha_icw >>  0) & 0xFF).a;

        float a_ab  = aA * aB;
        float a_cd  = aC * aD;
        float a_sum;

        uint a_out_ab_reg  = (alpha_ocw >> 4) & 0xF;
        uint a_out_cd_reg  = (alpha_ocw >> 0) & 0xF;
        uint a_out_sum_reg = (alpha_ocw >> 8) & 0xF;
        bool a_use_mux = (alpha_ocw & (1 << 23)) != 0;

        if (a_use_mux)
            a_sum = (comb_regs[12].a >= 0.5) ? a_cd : a_ab;
        else
            a_sum = a_ab + a_cd;

        if (a_out_ab_reg != 0)  comb_regs[a_out_ab_reg].a  = a_ab;
        if (a_out_cd_reg != 0)  comb_regs[a_out_cd_reg].a  = a_cd;
        if (a_out_sum_reg != 0) comb_regs[a_out_sum_reg].a = a_sum;
    }

    // Update SPARE0_PLUS_SECONDARY.
    comb_regs[14] = comb_regs[12] + comb_regs[5];  // SPARE0 + SECONDARY

    // ===================== Final combiner ==================================
    // NV2A final combiner: RGB = A*B + (1-A)*C + D,  Alpha = G
    //
    // SPECULAR_FOG_CW0 encodes inputs A, B, C, D (same ICW byte format).
    // SPECULAR_FOG_CW1 encodes inputs E, F, G (plus flags).
    // Before evaluating the final combiner, compute E*F product → EF_PROD (reg 15).

    uint cw0 = gpu.data[snap_base + DS_SPEC_FOG_CW0];
    uint cw1 = gpu.data[snap_base + DS_SPEC_FOG_CW1];

    // Decode E and F from CW1 (E = bits[31:24], F = bits[23:16]).
    vec4 fc_E = decode_input((cw1 >> 24) & 0xFF);
    vec4 fc_F = decode_input((cw1 >> 16) & 0xFF);
    comb_regs[15] = fc_E * fc_F;  // EF_PROD

    // Decode final combiner inputs A, B, C, D from CW0.
    vec4 fc_A = decode_input((cw0 >> 24) & 0xFF);
    vec4 fc_B = decode_input((cw0 >> 16) & 0xFF);
    vec4 fc_C = decode_input((cw0 >>  8) & 0xFF);
    vec4 fc_D = decode_input((cw0 >>  0) & 0xFF);

    // Final combiner RGB = A*B + (1-A)*C + D.
    vec3 final_rgb = fc_A.rgb * fc_B.rgb + (vec3(1.0) - fc_A.rgb) * fc_C.rgb + fc_D.rgb;

    // Final combiner Alpha = G (bits[7:0] of CW1).
    float final_alpha = decode_input(cw1 & 0xFF).a;

    // If CW0 is zero (not programmed), fall back to SPARE0 output.
    vec4 final_color;
    if (cw0 == 0 && cw1 == 0) {
        final_color = vec4(clamp(comb_regs[12].rgb, vec3(0.0), vec3(1.0)),
                           clamp(comb_regs[12].a, 0.0, 1.0));
    } else {
        final_color = vec4(clamp(final_rgb, vec3(0.0), vec3(1.0)),
                           clamp(final_alpha, 0.0, 1.0));
    }

    o_color = final_color;
}
