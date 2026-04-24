// ---------------------------------------------------------------------------
// test_pgraph.cpp — Unit test for PgraphState method handling.
//
// Constructs NV2A push buffer commands, feeds them through Nv2aState's
// tick_fifo → PgraphState::handle_method chain, and verifies that the
// PGRAPH state shadow captures the correct values.
// ---------------------------------------------------------------------------

#include "xbox/nv2a.hpp"
#include "xbox/pgraph.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; fprintf(stderr, "FAIL: %s\n", msg); } \
} while(0)

// Build an INCREASING method header.
static uint32_t pb_inc(uint32_t method, uint32_t subchan, uint32_t count) {
    return (count << 18) | (subchan << 13) | method;
}

int main() {
    using namespace xbox;
    using namespace xbox::nv097;

    // Set up NV2A state + PGRAPH state shadow.
    Nv2aState nv2a;
    PgraphState pgraph;
    nv2a.method_handler = pgraph_method_handler;
    nv2a.method_user    = &pgraph;

    // Allocate a small RAM buffer for the push buffer.
    constexpr uint32_t RAM_SIZE = 1024 * 1024; // 1 MB
    auto* ram = static_cast<uint8_t*>(calloc(1, RAM_SIZE));
    if (!ram) { fprintf(stderr, "Cannot allocate RAM\n"); return 1; }

    uint32_t pb_offset = 0x10000; // push buffer at 64K
    uint32_t pos = pb_offset;

    // === Command 1: SET_BLEND_ENABLE = 1 ===
    uint32_t hdr1 = pb_inc(SET_BLEND_ENABLE, 0, 1);
    memcpy(ram + pos, &hdr1, 4); pos += 4;
    uint32_t val1 = 1;
    memcpy(ram + pos, &val1, 4); pos += 4;

    // === Command 2: SET_DEPTH_TEST_ENABLE = 1, SET_DEPTH_FUNC = 0x0203 (LEQUAL) ===
    // INCREASING: method=SET_DEPTH_TEST_ENABLE, count=2 (covers DEPTH_TEST + DEPTH_FUNC)
    uint32_t hdr2 = pb_inc(SET_DEPTH_TEST_ENABLE, 0, 2);
    memcpy(ram + pos, &hdr2, 4); pos += 4;
    uint32_t depth_en = 1;
    memcpy(ram + pos, &depth_en, 4); pos += 4;
    uint32_t depth_func = 0x0203;
    memcpy(ram + pos, &depth_func, 4); pos += 4;

    // === Command 3: SET_SURFACE_COLOR_OFFSET = 0x00100000 ===
    uint32_t hdr3 = pb_inc(SET_SURFACE_COLOR_OFFSET, 0, 1);
    memcpy(ram + pos, &hdr3, 4); pos += 4;
    uint32_t color_off = 0x00100000;
    memcpy(ram + pos, &color_off, 4); pos += 4;

    // === Command 4: SET_TEXTURE_OFFSET stage 0 = 0x00200000 ===
    uint32_t hdr4 = pb_inc(SET_TEXTURE_OFFSET + 0*64, 0, 1);
    memcpy(ram + pos, &hdr4, 4); pos += 4;
    uint32_t tex_off = 0x00200000;
    memcpy(ram + pos, &tex_off, 4); pos += 4;

    // === Command 5: SET_TEXTURE_OFFSET stage 2 = 0x00300000 ===
    uint32_t hdr5 = pb_inc(SET_TEXTURE_OFFSET + 2*64, 0, 1);
    memcpy(ram + pos, &hdr5, 4); pos += 4;
    uint32_t tex2_off = 0x00300000;
    memcpy(ram + pos, &tex2_off, 4); pos += 4;

    // === Command 6: SET_BEGIN_END = 4 (TRIANGLES), then SET_BEGIN_END = 0 (END) ===
    uint32_t hdr6a = pb_inc(SET_BEGIN_END, 0, 1);
    memcpy(ram + pos, &hdr6a, 4); pos += 4;
    uint32_t begin_val = 4; // TRIANGLES
    memcpy(ram + pos, &begin_val, 4); pos += 4;

    uint32_t hdr6b = pb_inc(SET_BEGIN_END, 0, 1);
    memcpy(ram + pos, &hdr6b, 4); pos += 4;
    uint32_t end_val = 0;
    memcpy(ram + pos, &end_val, 4); pos += 4;

    // === Command 7: CLEAR_SURFACE ===
    uint32_t hdr7 = pb_inc(CLEAR_SURFACE, 0, 1);
    memcpy(ram + pos, &hdr7, 4); pos += 4;
    uint32_t clear_val = 0xF0;
    memcpy(ram + pos, &clear_val, 4); pos += 4;

    // === Command 8: SET_CULL_FACE_ENABLE = 1, SET_CULL_FACE = 0x0404 (FRONT) ===
    uint32_t hdr8 = pb_inc(SET_CULL_FACE_ENABLE, 0, 1);
    memcpy(ram + pos, &hdr8, 4); pos += 4;
    uint32_t cull_en = 1;
    memcpy(ram + pos, &cull_en, 4); pos += 4;

    uint32_t hdr8b = pb_inc(SET_CULL_FACE, 0, 1);
    memcpy(ram + pos, &hdr8b, 4); pos += 4;
    uint32_t cull_face = 0x0404;
    memcpy(ram + pos, &cull_face, 4); pos += 4;

    // === Command 9: SET_SURFACE_FORMAT = 0x00090105 ===
    //   color=5 (X8R8G8B8), zeta=0 (none), type=1 (pitch),
    //   aa=0, width_log2=9, height_log2=0
    uint32_t hdr9 = pb_inc(SET_SURFACE_FORMAT, 0, 1);
    memcpy(ram + pos, &hdr9, 4); pos += 4;
    uint32_t surf_fmt = 0x00090105;
    memcpy(ram + pos, &surf_fmt, 4); pos += 4;

    // === Command 10: SET_SURFACE_PITCH = 0x00400100 ===
    //   color pitch = 0x0100 (256), zeta pitch = 0x0040 (64)
    uint32_t hdr10 = pb_inc(SET_SURFACE_PITCH, 0, 1);
    memcpy(ram + pos, &hdr10, 4); pos += 4;
    uint32_t surf_pitch = 0x00400100;
    memcpy(ram + pos, &surf_pitch, 4); pos += 4;

    // === Command 11: SET_SURFACE_CLIP_HORIZONTAL = 0x028000A0 ===
    //   x=0x00A0 (160), width=0x0280 (640)
    uint32_t hdr11 = pb_inc(SET_SURFACE_CLIP_HORIZONTAL, 0, 1);
    memcpy(ram + pos, &hdr11, 4); pos += 4;
    uint32_t clip_h = 0x028000A0;
    memcpy(ram + pos, &clip_h, 4); pos += 4;

    // === Command 12: SET_SURFACE_CLIP_VERTICAL = 0x01E00050 ===
    //   y=0x0050 (80), height=0x01E0 (480)
    uint32_t hdr12 = pb_inc(SET_SURFACE_CLIP_VERTICAL, 0, 1);
    memcpy(ram + pos, &hdr12, 4); pos += 4;
    uint32_t clip_v = 0x01E00050;
    memcpy(ram + pos, &clip_v, 4); pos += 4;

    // === Command 13: SET_SURFACE_ZETA_OFFSET = 0x00500000 ===
    uint32_t hdr13 = pb_inc(SET_SURFACE_ZETA_OFFSET, 0, 1);
    memcpy(ram + pos, &hdr13, 4); pos += 4;
    uint32_t zeta_off = 0x00500000;
    memcpy(ram + pos, &zeta_off, 4); pos += 4;

    // === Command 14: CLEAR_SURFACE with depth+stencil = 0x03 ===
    uint32_t hdr14 = pb_inc(CLEAR_SURFACE, 0, 1);
    memcpy(ram + pos, &hdr14, 4); pos += 4;
    uint32_t clear_depth = 0x03;  // Z + stencil
    memcpy(ram + pos, &clear_depth, 4); pos += 4;

    // === Command 15: CLEAR_SURFACE with color+depth = 0xF3 ===
    uint32_t hdr15 = pb_inc(CLEAR_SURFACE, 0, 1);
    memcpy(ram + pos, &hdr15, 4); pos += 4;
    uint32_t clear_all = 0xF3;
    memcpy(ram + pos, &clear_all, 4); pos += 4;

    // === Command 16: SET_VIEWPORT_OFFSET (4 floats: x, y, z, w) ===
    uint32_t hdr16 = pb_inc(SET_VIEWPORT_OFFSET_X, 0, 4);
    memcpy(ram + pos, &hdr16, 4); pos += 4;
    float vp_off[4] = { 320.0f, 240.0f, 0.0f, 0.0f };
    memcpy(ram + pos, vp_off, 16); pos += 16;

    // === Command 17: SET_VIEWPORT_SCALE (4 floats: x, y, z, w) ===
    uint32_t hdr17 = pb_inc(SET_VIEWPORT_SCALE_X, 0, 4);
    memcpy(ram + pos, &hdr17, 4); pos += 4;
    float vp_sc[4] = { 320.0f, -240.0f, 16777215.0f, 0.0f };
    memcpy(ram + pos, vp_sc, 16); pos += 16;

    // === Command 18: SET_SHADE_MODE = 0x1D01 (SMOOTH) ===
    uint32_t hdr18 = pb_inc(SET_SHADE_MODE, 0, 1);
    memcpy(ram + pos, &hdr18, 4); pos += 4;
    uint32_t shade = 0x1D01;
    memcpy(ram + pos, &shade, 4); pos += 4;

    // === Command 19: SET_COLOR_MASK = 0x01010101 (RGBA all enabled) ===
    uint32_t hdr19 = pb_inc(SET_COLOR_MASK, 0, 1);
    memcpy(ram + pos, &hdr19, 4); pos += 4;
    uint32_t cmask = 0x01010101;
    memcpy(ram + pos, &cmask, 4); pos += 4;

    // === Command 20: SET_TEXTURE stage 0: offset + format + control + filter ===
    uint32_t hdr20 = pb_inc(SET_TEXTURE_FORMAT + 0*64, 0, 1);
    memcpy(ram + pos, &hdr20, 4); pos += 4;
    uint32_t tex0_fmt = 0x0001162A;  // example packed format
    memcpy(ram + pos, &tex0_fmt, 4); pos += 4;

    uint32_t hdr20b = pb_inc(SET_TEXTURE_CONTROL0 + 0*64, 0, 1);
    memcpy(ram + pos, &hdr20b, 4); pos += 4;
    uint32_t tex0_ctl = 0x30101000;  // enable + min/max LOD
    memcpy(ram + pos, &tex0_ctl, 4); pos += 4;

    uint32_t hdr20c = pb_inc(SET_TEXTURE_FILTER + 0*64, 0, 1);
    memcpy(ram + pos, &hdr20c, 4); pos += 4;
    uint32_t tex0_flt = 0x04012000;  // bilinear
    memcpy(ram + pos, &tex0_flt, 4); pos += 4;

    uint32_t hdr20d = pb_inc(SET_TEXTURE_IMAGE_RECT + 0*64, 0, 1);
    memcpy(ram + pos, &hdr20d, 4); pos += 4;
    uint32_t tex0_rect = 0x02000100;  // 512 wide, 256 tall
    memcpy(ram + pos, &tex0_rect, 4); pos += 4;

    // === Command 21: SET_TEXTURE stage 1 offset ===
    uint32_t hdr21 = pb_inc(SET_TEXTURE_OFFSET + 1*64, 0, 1);
    memcpy(ram + pos, &hdr21, 4); pos += 4;
    uint32_t tex1_off = 0x00400000;
    memcpy(ram + pos, &tex1_off, 4); pos += 4;

    // === Command 22: SET_BLEND_FUNC_SFACTOR = 0x0302 (SRC_ALPHA) ===
    uint32_t hdr22 = pb_inc(SET_BLEND_FUNC_SFACTOR, 0, 1);
    memcpy(ram + pos, &hdr22, 4); pos += 4;
    uint32_t sfactor = 0x0302;
    memcpy(ram + pos, &sfactor, 4); pos += 4;

    // === Command 23: SET_BLEND_FUNC_DFACTOR = 0x0303 (ONE_MINUS_SRC_ALPHA) ===
    uint32_t hdr23 = pb_inc(SET_BLEND_FUNC_DFACTOR, 0, 1);
    memcpy(ram + pos, &hdr23, 4); pos += 4;
    uint32_t dfactor = 0x0303;
    memcpy(ram + pos, &dfactor, 4); pos += 4;

    // === Command 24: SET_BLEND_EQUATION = 0x8006 (FUNC_ADD) ===
    uint32_t hdr24 = pb_inc(SET_BLEND_EQUATION, 0, 1);
    memcpy(ram + pos, &hdr24, 4); pos += 4;
    uint32_t blend_eq = 0x8006;
    memcpy(ram + pos, &blend_eq, 4); pos += 4;

    // === Command 25: SET_BLEND_COLOR = 0x80402010 ===
    uint32_t hdr25 = pb_inc(SET_BLEND_COLOR, 0, 1);
    memcpy(ram + pos, &hdr25, 4); pos += 4;
    uint32_t blend_col = 0x80402010;
    memcpy(ram + pos, &blend_col, 4); pos += 4;

    // === Command 26: SET_STENCIL_TEST_ENABLE = 1 ===
    uint32_t hdr26 = pb_inc(SET_STENCIL_TEST_ENABLE, 0, 1);
    memcpy(ram + pos, &hdr26, 4); pos += 4;
    uint32_t stencil_en = 1;
    memcpy(ram + pos, &stencil_en, 4); pos += 4;

    // === Command 27: SET_STENCIL_FUNC_REF = 0x80 ===
    uint32_t hdr27 = pb_inc(SET_STENCIL_FUNC_REF, 0, 1);
    memcpy(ram + pos, &hdr27, 4); pos += 4;
    uint32_t stencil_ref = 0x80;
    memcpy(ram + pos, &stencil_ref, 4); pos += 4;

    // --- Run the DMA pusher ---
    nv2a.pfifo_regs[pfifo::CACHE1_DMA_PUSH / 4] = 1;
    nv2a.pfifo_regs[pfifo::CACHE1_PUSH0 / 4] = 1;
    nv2a.pfifo_regs[pfifo::CACHE1_DMA_GET / 4] = pb_offset;
    nv2a.pfifo_regs[pfifo::CACHE1_DMA_PUT / 4] = pos;

    // Tick multiple times to drain the push buffer.
    for (int i = 0; i < 10; i++)
        nv2a.tick_fifo(ram, RAM_SIZE);

    // === Verify PGRAPH state ===
    CHECK(pgraph.reg(SET_BLEND_ENABLE) == 1, "blend_enable == 1");
    CHECK(pgraph.reg(SET_DEPTH_TEST_ENABLE) == 1, "depth_test_enable == 1");
    CHECK(pgraph.reg(SET_DEPTH_FUNC) == 0x0203, "depth_func == LEQUAL (0x0203)");
    CHECK(pgraph.reg(SET_SURFACE_COLOR_OFFSET) == 0x00100000, "surface_color_offset == 0x100000");
    CHECK(pgraph.reg(SET_TEXTURE_OFFSET + 0*64) == 0x00200000, "texture[0].offset == 0x200000");
    CHECK(pgraph.reg(SET_TEXTURE_OFFSET + 2*64) == 0x00300000, "texture[2].offset == 0x300000");
    CHECK(pgraph.reg(SET_BEGIN_END) == 0, "begin_end_mode == 0 (END)");
    CHECK(pgraph.draw_count == 1, "draw_count == 1");
    CHECK(pgraph.clear_count == 3, "clear_count == 3");
    CHECK(pgraph.reg(CLEAR_SURFACE) == 0xF3, "clear_surface == 0xF3 (last clear)");
    CHECK(pgraph.reg(SET_CULL_FACE_ENABLE) == 1, "cull_face_enable == 1");
    CHECK(pgraph.reg(SET_CULL_FACE) == 0x0404, "cull_face == FRONT (0x0404)");

    // FIFO stats
    CHECK(nv2a.pfifo_regs[pfifo::EXT_METHODS / 4] == pgraph.total_methods,
          "fifo_methods == pgraph.total_methods");
    CHECK(nv2a.pfifo_regs[pfifo::CACHE1_DMA_GET / 4] == pos, "DMA_GET == PUT (drained)");

    // --- Surface decode tests ---
    CHECK(pgraph.surface_color_format() == 5, "surface_color_format == 5 (X8R8G8B8)");
    CHECK(pgraph.surface_zeta_format() == 0, "surface_zeta_format == 0 (none)");
    CHECK(pgraph.surface_type() == 1, "surface_type == 1 (pitch)");
    CHECK(pgraph.surface_aa_mode() == 0, "surface_aa_mode == 0");
    CHECK(pgraph.surface_width_log2() == 9, "surface_width_log2 == 9");
    CHECK(pgraph.surface_color_pitch() == 0x0100, "surface_color_pitch == 256");
    CHECK(pgraph.surface_zeta_pitch() == 0x0040, "surface_zeta_pitch == 64");
    CHECK(pgraph.surface_clip_x() == 0x00A0, "surface_clip_x == 160");
    CHECK(pgraph.surface_clip_width() == 0x0280, "surface_clip_width == 640");
    CHECK(pgraph.surface_clip_y() == 0x0050, "surface_clip_y == 80");
    CHECK(pgraph.surface_clip_height() == 0x01E0, "surface_clip_height == 480");

    // Clear tracking
    CHECK(pgraph.clear_color_count == 2, "clear_color_count == 2 (0xF0, 0xF3)");
    CHECK(pgraph.clear_depth_count == 2, "clear_depth_count == 2 (0x03, 0xF3)");

    // Surface offsets
    CHECK(pgraph.reg(SET_SURFACE_COLOR_OFFSET) == 0x00100000, "color_offset == 0x100000 (decode)");
    CHECK(pgraph.reg(SET_SURFACE_ZETA_OFFSET)  == 0x00500000, "zeta_offset == 0x500000");

    // Viewport state
    CHECK(pgraph.reg_float(SET_VIEWPORT_OFFSET_X)     == 320.0f, "vp_offset_x == 320");
    CHECK(pgraph.reg_float(SET_VIEWPORT_OFFSET_X + 4)  == 240.0f, "vp_offset_y == 240");
    CHECK(pgraph.reg_float(SET_VIEWPORT_SCALE_X)       == 320.0f, "vp_scale_x == 320");
    CHECK(pgraph.reg_float(SET_VIEWPORT_SCALE_X + 4)   == -240.0f, "vp_scale_y == -240");
    CHECK(pgraph.reg(SET_SHADE_MODE)  == 0x1D01, "shade_mode == SMOOTH");
    CHECK(pgraph.reg(SET_COLOR_MASK)  == 0x01010101, "color_mask == all-enabled");

    // Texture state
    CHECK(pgraph.reg(SET_TEXTURE_FORMAT + 0*64)      == 0x0001162A, "tex0_format");
    CHECK(pgraph.reg(SET_TEXTURE_CONTROL0 + 0*64)    == 0x30101000, "tex0_control0");
    CHECK(pgraph.reg(SET_TEXTURE_FILTER + 0*64)      == 0x04012000, "tex0_filter");
    CHECK(pgraph.reg(SET_TEXTURE_IMAGE_RECT + 0*64)  == 0x02000100, "tex0_image_rect");
    CHECK(pgraph.reg(SET_TEXTURE_OFFSET + 1*64)      == 0x00400000, "tex1_offset");

    // Blend state
    CHECK(pgraph.reg(SET_BLEND_FUNC_SFACTOR) == 0x0302, "blend_sfactor == SRC_ALPHA");
    CHECK(pgraph.reg(SET_BLEND_FUNC_DFACTOR) == 0x0303, "blend_dfactor == ONE_MINUS_SRC_ALPHA");
    CHECK(pgraph.reg(SET_BLEND_EQUATION)     == 0x8006, "blend_equation == FUNC_ADD");
    CHECK(pgraph.reg(SET_BLEND_COLOR)        == 0x80402010, "blend_color");

    // Stencil state
    CHECK(pgraph.reg(SET_STENCIL_TEST_ENABLE) == 1, "stencil_test_enable");
    CHECK(pgraph.reg(SET_STENCIL_FUNC_REF)    == 0x80, "stencil_func_ref == 0x80");

    free(ram);

    printf("PGRAPH state test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}
