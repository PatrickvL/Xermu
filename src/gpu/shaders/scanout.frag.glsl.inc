R"GLSL(
// ---------------------------------------------------------------------------
// scanout.frag.glsl — Framebuffer scanout fragment shader.
//
// Reads the Xbox framebuffer from the GPU buffer (Xbox RAM region),
// using PCRTC_START and surface format/pitch from the PFIFO control block
// and PGRAPH state.  Outputs to the host swapchain.
// ---------------------------------------------------------------------------
#version 450

layout(location = 0) in  vec2 v_uv;
layout(location = 0) out vec4 o_color;

layout(set = 0, binding = 0) buffer readonly GpuBuffer {
    uint data[];
} gpu;

// ===================== Layout constants (must match C++) ===================

const uint XBOX_RAM_DW     = 0;
const uint PGRAPH_STATE_DW = (64 * 1024 * 1024) / 4;
const uint PFIFO_CTL_DW    = PGRAPH_STATE_DW + (32 * 1024) / 4;

// PfifoControlBlock field offsets (dword index from PFIFO_CTL_DW).
const uint CTL_PCRTC_START    = PFIFO_CTL_DW + 8;
const uint CTL_DISPLAY_WIDTH  = PFIFO_CTL_DW + 10;
const uint CTL_DISPLAY_HEIGHT = PFIFO_CTL_DW + 11;
const uint CTL_DISPLAY_PITCH  = PFIFO_CTL_DW + 12;
const uint CTL_DISPLAY_BPP    = PFIFO_CTL_DW + 13;

// ===================== Helpers =============================================

// Unpack an X8R8G8B8 (0xAARRGGBB) pixel to linear RGBA.
vec4 unpack_x8r8g8b8(uint pixel) {
    float b = float((pixel >>  0) & 0xFF) / 255.0;
    float g = float((pixel >>  8) & 0xFF) / 255.0;
    float r = float((pixel >> 16) & 0xFF) / 255.0;
    float a = 1.0;
    return vec4(r, g, b, a);
}

// Unpack an A8R8G8B8 pixel.
vec4 unpack_a8r8g8b8(uint pixel) {
    float b = float((pixel >>  0) & 0xFF) / 255.0;
    float g = float((pixel >>  8) & 0xFF) / 255.0;
    float r = float((pixel >> 16) & 0xFF) / 255.0;
    float a = float((pixel >> 24) & 0xFF) / 255.0;
    return vec4(r, g, b, a);
}

// Unpack R5G6B5 (16-bit, packed in low half of dword).
vec4 unpack_r5g6b5(uint pixel16) {
    float r = float((pixel16 >> 11) & 0x1F) / 31.0;
    float g = float((pixel16 >>  5) & 0x3F) / 63.0;
    float b = float((pixel16 >>  0) & 0x1F) / 31.0;
    return vec4(r, g, b, 1.0);
}

// ===================== Main ================================================

void main() {
    // Read scanout configuration from the PFIFO control block (set by CPU).
    uint fb_start   = gpu.data[CTL_PCRTC_START];
    uint fb_width   = gpu.data[CTL_DISPLAY_WIDTH];
    uint fb_height  = gpu.data[CTL_DISPLAY_HEIGHT];
    uint pitch      = gpu.data[CTL_DISPLAY_PITCH];
    uint bpp        = gpu.data[CTL_DISPLAY_BPP];

    // Fallback defaults if not configured.
    if (fb_width  == 0) fb_width  = 640;
    if (fb_height == 0) fb_height = 480;
    if (bpp       == 0) bpp       = 4;
    if (pitch     == 0) pitch     = fb_width * bpp;

    // Compute pixel coordinates.
    uint px = uint(v_uv.x * float(fb_width));
    uint py = uint(v_uv.y * float(fb_height));
    px = min(px, fb_width  - 1);
    py = min(py, fb_height - 1);

    // Byte offset of this pixel in Xbox RAM.
    uint byte_off = fb_start + py * pitch;

    // Read and decode based on bytes-per-pixel.
    vec4 color;

    if (bpp == 4) {
        // 32-bit: X8R8G8B8
        uint pixel_off = byte_off + px * 4;
        uint dw_idx = XBOX_RAM_DW + pixel_off / 4;
        uint pixel  = gpu.data[dw_idx];
        color = unpack_x8r8g8b8(pixel);
    }
    else if (bpp == 2) {
        // 16-bit: R5G6B5
        uint pixel_off = byte_off + px * 2;
        uint dw_idx = XBOX_RAM_DW + pixel_off / 4;
        uint dword  = gpu.data[dw_idx];
        uint shift  = (pixel_off & 2) * 8;  // 0 or 16
        uint pixel16 = (dword >> shift) & 0xFFFF;
        color = unpack_r5g6b5(pixel16);
    }
    else {
        // Unknown bpp — magenta error color.
        color = vec4(1.0, 0.0, 1.0, 1.0);
    }

    o_color = color;
}
)GLSL"
