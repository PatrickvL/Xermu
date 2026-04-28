// ---------------------------------------------------------------------------
// scanout.vert.glsl — Fullscreen triangle vertex shader.
//
// Generates a single triangle that covers the entire viewport.
// No vertex input — positions derived from gl_VertexIndex.
// ---------------------------------------------------------------------------
#version 450

layout(location = 0) out vec2 v_uv;

void main() {
    // Fullscreen triangle: vertices at (-1,-1), (3,-1), (-1,3).
    // UV coords: (0,0), (2,0), (0,2) — the rasterizer clips to [0,1].
    float x = float((gl_VertexIndex & 1) << 2) - 1.0;
    float y = float((gl_VertexIndex & 2) << 0) - 1.0;
    v_uv = vec2((x + 1.0) * 0.5, (y + 1.0) * 0.5);
    gl_Position = vec4(x, y, 0.0, 1.0);
}
