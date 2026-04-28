#pragma once
// ---------------------------------------------------------------------------
// nv2a_vk_renderer.hpp — GPU-driven NV2A renderer (Vulkan).
//
// Architecture:
//   1. Xbox RAM resides in a single host-visible VkBuffer (BAR / staging).
//   2. NV2A PGRAPH state lives in a device-local storage buffer, updated
//      exclusively by a compute shader — the CPU never touches render state.
//   3. A push buffer parser compute shader consumes NV2A DMA commands:
//        • Reads [DMA_GET .. DMA_PUT] from Xbox RAM buffer.
//        • Decodes method/data pairs and writes into the PGRAPH state buffer.
//        • On draw triggers (BEGIN_END, DRAW_ARRAYS, CLEAR_SURFACE), emits
//          entries into an indirect draw buffer + per-draw state snapshot.
//        • Advances DMA_GET atomically.
//   4. Draw execution uses vkCmdDrawIndirectCount, where:
//        • The vertex shader fetches vertices from Xbox RAM using format
//          descriptors in the per-draw state snapshot.
//        • The fragment shader implements NV2A register combiners by reading
//          combiner config from the per-draw state buffer.
//   5. Pipeline state (blend/depth/stencil/cull) is interpreted in shaders
//      or configured via Vulkan 1.3 extended dynamic state from constants —
//      no CPU readback of GPU state.  Per-frame resets use vkCmdFillBuffer.
//
// Memory layout (single VkDeviceMemory, persistently mapped — Xbox RAM
// is the emulator's physical memory, accessed directly via the mapping):
//   [0x0000_0000 .. 0x0400_0000)  Xbox RAM (64 MB)
//   [0x0400_0000 .. 0x0400_8000)  PGRAPH state buffer (32 KB)
//   [0x0400_8000 .. 0x0401_0000)  PFIFO control buffer (DMA_GET/PUT, etc.)
//   [0x0401_0000 .. 0x0402_0000)  Indirect draw buffer (draw commands)
//   [0x0402_0000 .. 0x0404_0000)  Per-draw state snapshots (128 KB)
//   [0x0404_0000 .. 0x0406_0000)  Vertex shader program + constants (128 KB)
//
// All offsets are relative to the base of the allocation.
// ---------------------------------------------------------------------------

#include <volk.h>
#include <cstdint>
#include <cstring>

namespace xbox {

// ============================= GPU Buffer Layout ===========================

struct GpuBufferLayout {
    static constexpr uint32_t XBOX_RAM_OFFSET      = 0;
    static constexpr uint32_t XBOX_RAM_SIZE        = 64 * 1024 * 1024;

    static constexpr uint32_t PGRAPH_STATE_OFFSET  = XBOX_RAM_SIZE;
    static constexpr uint32_t PGRAPH_STATE_SIZE    = 32 * 1024;   // 2048 uint32 regs + padding

    static constexpr uint32_t PFIFO_CTL_OFFSET     = PGRAPH_STATE_OFFSET + PGRAPH_STATE_SIZE;
    static constexpr uint32_t PFIFO_CTL_SIZE       = 32 * 1024;

    static constexpr uint32_t DRAW_CMD_OFFSET      = PFIFO_CTL_OFFSET + PFIFO_CTL_SIZE;
    static constexpr uint32_t DRAW_CMD_SIZE        = 64 * 1024;    // up to 1024 draws/frame

    static constexpr uint32_t DRAW_STATE_OFFSET    = DRAW_CMD_OFFSET + DRAW_CMD_SIZE;
    static constexpr uint32_t DRAW_STATE_SIZE      = 128 * 1024;

    static constexpr uint32_t VS_PROGRAM_OFFSET    = DRAW_STATE_OFFSET + DRAW_STATE_SIZE;
    static constexpr uint32_t VS_PROGRAM_SIZE      = 128 * 1024;   // 136 slots × 16B + constants

    static constexpr uint32_t TOTAL_SIZE           = VS_PROGRAM_OFFSET + VS_PROGRAM_SIZE;
};

// ============================= PFIFO Control Block =========================
// Written by CPU (DMA_PUT), read/written by compute shader (DMA_GET, flags).

struct PfifoControlBlock {
    uint32_t dma_put;            // written by CPU when guest writes CACHE1_DMA_PUT
    uint32_t dma_get;            // read/written by compute shader
    uint32_t push_enabled;       // 1 = DMA push active
    uint32_t draw_count;         // atomic counter: draws emitted this frame
    uint32_t frame_id;           // incremented per frame via vkCmdUpdateBuffer
    uint32_t flags;              // bit 0 = flush requested
    uint32_t subroutine_return;  // CALL/RETURN stack (single level)
    uint32_t subroutine_active;
    uint32_t pcrtc_start;        // written by CPU on guest PCRTC_START MMIO write
    uint32_t pad[7];             // pad to 64 bytes
};

// ============================= Indirect Draw Command =======================
// Written by the compute shader, consumed by vkCmdDrawIndirectCount.

struct GpuDrawCommand {
    // VkDrawIndirectCommand fields:
    uint32_t vertex_count;
    uint32_t instance_count;
    uint32_t first_vertex;
    uint32_t first_instance;     // encodes draw index for per-draw state lookup
};

// ============================= Per-Draw State Snapshot =====================
// Captured by the compute shader at each draw trigger.  Read by vertex/
// fragment shaders to configure transforms, textures, and combiners.

struct alignas(16) DrawStateSnapshot {
    // Surface
    uint32_t surface_format;     // SET_SURFACE_FORMAT register
    uint32_t surface_pitch;
    uint32_t color_offset;
    uint32_t zeta_offset;

    // Viewport (floats)
    float    viewport_offset[4];
    float    viewport_scale[4];

    // Blend/depth/stencil (raw register values — decoded by fragment shader)
    uint32_t blend_enable;
    uint32_t blend_sfactor;
    uint32_t blend_dfactor;
    uint32_t blend_equation;
    uint32_t depth_test_enable;
    uint32_t depth_func;
    uint32_t depth_mask;
    uint32_t color_mask;

    // Combiner control
    uint32_t combiner_control;
    uint32_t combiner_color_icw[8];
    uint32_t combiner_color_ocw[8];
    uint32_t combiner_alpha_icw[8];
    uint32_t combiner_alpha_ocw[8];

    // Texture state (4 stages)
    uint32_t tex_offset[4];
    uint32_t tex_format[4];
    uint32_t tex_control0[4];
    uint32_t tex_filter[4];
    uint32_t tex_image_rect[4];

    // Vertex format (inline vertex array attributes)
    uint32_t vertex_attr_format[16];  // NV2A vertex attribute format descriptors
    uint32_t vertex_data_offset;      // offset into Xbox RAM for vertex data

    // Primitive type (from BEGIN_END)
    uint32_t primitive_mode;

    // Padding to 256 bytes
    uint32_t _pad[3];
};
static_assert(sizeof(DrawStateSnapshot) <= 512, "DrawStateSnapshot must fit in 512B");

// ============================= Renderer ====================================

struct Nv2aVkRenderer {
    // --- Vulkan handles (externally owned device/queue) ---
    VkDevice         device         = VK_NULL_HANDLE;
    VkPhysicalDevice physical       = VK_NULL_HANDLE;
    VkQueue          compute_queue  = VK_NULL_HANDLE;
    VkQueue          graphics_queue = VK_NULL_HANDLE;
    uint32_t         compute_qf     = 0;
    uint32_t         graphics_qf    = 0;

    // --- Main GPU allocation ---
    VkDeviceMemory   gpu_memory     = VK_NULL_HANDLE;
    VkBuffer         gpu_buffer     = VK_NULL_HANDLE;  // entire allocation as storage buffer
    void*            mapped_ptr     = nullptr;         // persistently mapped (BAR)
    VkDeviceAddress  gpu_buffer_addr = 0;              // buffer device address

    // --- Push buffer parser compute pipeline ---
    VkShaderModule       pushbuf_shader   = VK_NULL_HANDLE;
    VkPipelineLayout     pushbuf_layout   = VK_NULL_HANDLE;
    VkPipeline           pushbuf_pipeline = VK_NULL_HANDLE;
    VkDescriptorSetLayout pushbuf_dsl     = VK_NULL_HANDLE;
    VkDescriptorPool     pushbuf_desc_pool = VK_NULL_HANDLE;
    VkDescriptorSet      pushbuf_desc_set = VK_NULL_HANDLE;

    // --- Scanout (framebuffer blit to swapchain) ---
    VkShaderModule   scanout_vert     = VK_NULL_HANDLE;
    VkShaderModule   scanout_frag     = VK_NULL_HANDLE;
    VkPipelineLayout scanout_layout   = VK_NULL_HANDLE;
    VkPipeline       scanout_pipeline = VK_NULL_HANDLE;
    VkDescriptorSetLayout scanout_dsl = VK_NULL_HANDLE;
    VkDescriptorPool scanout_desc_pool = VK_NULL_HANDLE;
    VkDescriptorSet  scanout_desc_set = VK_NULL_HANDLE;

    // --- 3D rendering pipeline (vertex fetch + register combiners) ---
    VkShaderModule   draw_vert        = VK_NULL_HANDLE;
    VkShaderModule   draw_frag        = VK_NULL_HANDLE;
    VkPipelineLayout draw_layout      = VK_NULL_HANDLE;
    VkPipeline       draw_pipeline    = VK_NULL_HANDLE;
    VkDescriptorSetLayout draw_dsl    = VK_NULL_HANDLE;
    VkDescriptorPool draw_desc_pool   = VK_NULL_HANDLE;
    VkDescriptorSet  draw_desc_set    = VK_NULL_HANDLE;

    // --- Render target (off-screen, NV2A resolution) ---
    VkImage          rt_color         = VK_NULL_HANDLE;
    VkDeviceMemory   rt_color_mem     = VK_NULL_HANDLE;
    VkImageView      rt_color_view    = VK_NULL_HANDLE;
    VkImage          rt_depth         = VK_NULL_HANDLE;
    VkDeviceMemory   rt_depth_mem     = VK_NULL_HANDLE;
    VkImageView      rt_depth_view    = VK_NULL_HANDLE;
    VkRenderPass     rt_render_pass   = VK_NULL_HANDLE;
    VkFramebuffer    rt_framebuffer   = VK_NULL_HANDLE;
    uint32_t         rt_width         = 640;
    uint32_t         rt_height        = 480;

    // --- Command buffers ---
    VkCommandPool    cmd_pool         = VK_NULL_HANDLE;
    VkCommandBuffer  cmd_compute      = VK_NULL_HANDLE;  // push buffer parse
    VkCommandBuffer  cmd_render       = VK_NULL_HANDLE;  // 3D draws + scanout

    // --- Synchronization ---
    VkSemaphore      compute_done     = VK_NULL_HANDLE;
    VkFence          frame_fence      = VK_NULL_HANDLE;

    // --- Frame state ---
    uint32_t         frame_id         = 0;

    // ----- Lifecycle -----

    // Initialize: allocate GPU memory, create pipelines, etc.
    // After init(), xbox_ram() returns a pointer the emulator uses directly as
    // Xbox physical memory — no separate upload step.
    bool init(VkDevice dev, VkPhysicalDevice phys,
              VkQueue compute_q, uint32_t compute_family,
              VkQueue graphics_q, uint32_t graphics_family,
              VkRenderPass present_pass, VkFormat present_format);

    void destroy();

    // ----- Per-frame -----

    // Called by the CPU when the guest writes CACHE1_DMA_PUT.
    // Updates the PFIFO control block in mapped GPU memory.
    void set_dma_put(uint32_t put_addr);

    // Dispatch the push buffer parser compute shader.
    // This processes [DMA_GET..DMA_PUT], updates PGRAPH state, emits draws.
    void dispatch_pushbuf_parse(VkCommandBuffer cmd);

    // Execute the emitted draw commands using state from the GPU buffers.
    // Renders to the off-screen render target.
    void execute_draws(VkCommandBuffer cmd);

    // Blit the off-screen render target to the given swapchain framebuffer.
    // Uses the PCRTC_START register to determine the scanout address.
    void scanout(VkCommandBuffer cmd, VkFramebuffer dst_fb,
                 VkExtent2D extent);

    // Full frame: parse + draw + scanout.  Returns the command buffer to submit.
    VkCommandBuffer record_frame(VkFramebuffer dst_fb, VkExtent2D extent);

    // ----- Helpers -----

    // Direct pointer to Xbox RAM within the mapped GPU buffer.
    // The emulator reads/writes Xbox RAM through this pointer — no copies.
    uint8_t* xbox_ram() { return static_cast<uint8_t*>(mapped_ptr); }

    // Called by CPU when guest writes PCRTC_START (not a push buffer method).
    void set_pcrtc_start(uint32_t addr);

    // Get pointer into mapped GPU memory at given offset.
    template<typename T = void>
    T* mapped(uint32_t offset) {
        return reinterpret_cast<T*>(static_cast<uint8_t*>(mapped_ptr) + offset);
    }

private:
    bool create_gpu_buffer();
    bool create_pushbuf_pipeline();
    bool create_scanout_pipeline(VkRenderPass present_pass, VkFormat present_format);
    bool create_draw_pipeline();
    bool create_render_target();
    bool create_command_buffers();
    bool create_sync_objects();

    uint32_t find_memory_type(uint32_t type_bits, VkMemoryPropertyFlags props);
};

} // namespace xbox
