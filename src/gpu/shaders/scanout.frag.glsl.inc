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
const uint CTL_PCRTC_START = PFIFO_CTL_DW + 8;

// NV097 PGRAPH register offsets.
const uint NV097_SET_SURFACE_FORMAT = 0x0208;
const uint NV097_SET_SURFACE_PITCH  = 0x020C;
const uint NV097_SET_SURFACE_CLIP_HORIZONTAL = 0x0200;
const uint NV097_SET_SURFACE_CLIP_VERTICAL   = 0x0204;

// ===================== Helpers =============================================

uint pg_read(uint method_byte_offset) {
    return gpu.data[PGRAPH_STATE_DW + method_byte_offset / 4];
}

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
    // Read scanout configuration.
    uint fb_start   = gpu.data[CTL_PCRTC_START];  // byte offset in Xbox RAM
    uint surf_fmt   = pg_read(NV097_SET_SURFACE_FORMAT);
    uint surf_pitch = pg_read(NV097_SET_SURFACE_PITCH);
    uint clip_h     = pg_read(NV097_SET_SURFACE_CLIP_HORIZONTAL);
    uint clip_v     = pg_read(NV097_SET_SURFACE_CLIP_VERTICAL);

    // Decode surface info.
    uint color_format = surf_fmt & 0xF;
    uint pitch        = surf_pitch & 0xFFFF;  // color pitch in bytes
    uint fb_width     = (clip_h >> 16) & 0xFFFF;
    uint fb_height    = (clip_v >> 16) & 0xFFFF;

    // Fallback defaults if not configured.
    if (fb_width  == 0) fb_width  = 640;
    if (fb_height == 0) fb_height = 480;
    if (pitch     == 0) pitch     = fb_width * 4;

    // Compute pixel coordinates.
    uint px = uint(v_uv.x * float(fb_width));
    uint py = uint(v_uv.y * float(fb_height));
    px = min(px, fb_width  - 1);
    py = min(py, fb_height - 1);

    // Byte offset of this pixel in Xbox RAM.
    uint byte_off = fb_start + py * pitch;

    // Read and decode based on surface color format.
    // Format enum (from SET_SURFACE_FORMAT bits[3:0]):
    //   0x01 = X1R5G5B5    (2 bpp)
    //   0x03 = R5G6B5       (2 bpp)
    //   0x05 = X8R8G8B8     (4 bpp)
    //   0x08 = A8R8G8B8     (4 bpp)
    vec4 color;

    if (color_format == 0x05 || color_format == 0x08 || color_format == 0x04) {
        // 32-bit formats: X8R8G8B8 / A8R8G8B8
        uint pixel_off = byte_off + px * 4;
        uint dw_idx = XBOX_RAM_DW + pixel_off / 4;
        uint pixel  = gpu.data[dw_idx];
        color = (color_format == 0x08) ? unpack_a8r8g8b8(pixel) : unpack_x8r8g8b8(pixel);
    }
    else if (color_format == 0x03 || color_format == 0x01) {
        // 16-bit formats: R5G6B5 / X1R5G5B5
        uint pixel_off = byte_off + px * 2;
        uint dw_idx = XBOX_RAM_DW + pixel_off / 4;
        uint dword  = gpu.data[dw_idx];
        // Extract the correct 16-bit half.
        uint shift  = (pixel_off & 2) * 8;  // 0 or 16
        uint pixel16 = (dword >> shift) & 0xFFFF;
        color = unpack_r5g6b5(pixel16);
    }
    else {
        // Unknown format — magenta error color.
        color = vec4(1.0, 0.0, 1.0, 1.0);
    }

    o_color = color;
}
)GLSL"
