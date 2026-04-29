// ---------------------------------------------------------------------------
// nv2a_vk_renderer.cpp — GPU-driven NV2A renderer implementation.
// ---------------------------------------------------------------------------

#include "nv2a_vk_renderer.hpp"
#include "nv2a_shaders.hpp"       // runtime GLSL→SPIR-V
#include <algorithm>
#include <cstddef>
#include <cstdio>

namespace xbox {

// Persistent shader SPIR-V — compiled once at init.
static Nv2aShaders g_shaders;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void vk_check(VkResult r, const char* msg) {
    if (r != VK_SUCCESS)
        fprintf(stderr, "[nv2a_vk] %s failed: %d\n", msg, (int)r);
}

static VkShaderModule create_shader(VkDevice dev, const uint32_t* code, size_t size) {
    VkShaderModuleCreateInfo ci = {};
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = size;
    ci.pCode = code;
    VkShaderModule sm = VK_NULL_HANDLE;
    vk_check(vkCreateShaderModule(dev, &ci, nullptr, &sm), "vkCreateShaderModule");
    return sm;
}

// ---------------------------------------------------------------------------
// GPU Buffer Allocation
// ---------------------------------------------------------------------------

uint32_t Nv2aVkRenderer::find_memory_type(uint32_t type_bits, VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties mem_props;
    vkGetPhysicalDeviceMemoryProperties(physical, &mem_props);
    for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++) {
        if ((type_bits & (1u << i)) &&
            (mem_props.memoryTypes[i].propertyFlags & props) == props)
            return i;
    }
    return UINT32_MAX;
}

bool Nv2aVkRenderer::create_gpu_buffer() {
    // Create a single large buffer covering Xbox RAM + GPU state.
    VkBufferCreateInfo buf_ci = {};
    buf_ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buf_ci.size  = GpuBufferLayout::TOTAL_SIZE;
    buf_ci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
                 | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT
                 | VK_BUFFER_USAGE_TRANSFER_DST_BIT
                 | VK_BUFFER_USAGE_TRANSFER_SRC_BIT
                 | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    buf_ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    vk_check(vkCreateBuffer(device, &buf_ci, nullptr, &gpu_buffer), "create gpu_buffer");
    if (!gpu_buffer) return false;

    VkMemoryRequirements mem_req;
    vkGetBufferMemoryRequirements(device, gpu_buffer, &mem_req);

    // Prefer device-local + host-visible (resizable BAR).
    // Fallback to host-visible + host-coherent (staging path).
    VkMemoryPropertyFlags desired =
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

    uint32_t mem_type = find_memory_type(mem_req.memoryTypeBits, desired);
    if (mem_type == UINT32_MAX) {
        // Fallback: host-visible only (non-BAR)
        desired = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                  VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        mem_type = find_memory_type(mem_req.memoryTypeBits, desired);
    }
    if (mem_type == UINT32_MAX) {
        fprintf(stderr, "[nv2a_vk] No suitable memory type for GPU buffer\n");
        return false;
    }

    // Allocate with device address enabled
    VkMemoryAllocateFlagsInfo flags_info = {};
    flags_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
    flags_info.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;

    VkMemoryAllocateInfo alloc_info = {};
    alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.pNext = &flags_info;
    alloc_info.allocationSize = mem_req.size;
    alloc_info.memoryTypeIndex = mem_type;

    vk_check(vkAllocateMemory(device, &alloc_info, nullptr, &gpu_memory), "allocate gpu_memory");
    if (!gpu_memory) return false;

    vk_check(vkBindBufferMemory(device, gpu_buffer, gpu_memory, 0), "bind gpu_buffer");

    // Persistently map
    vk_check(vkMapMemory(device, gpu_memory, 0, VK_WHOLE_SIZE, 0, &mapped_ptr), "map gpu_memory");
    memset(mapped_ptr, 0, GpuBufferLayout::TOTAL_SIZE);

    // Get buffer device address
    VkBufferDeviceAddressInfo addr_info = {};
    addr_info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    addr_info.buffer = gpu_buffer;
    gpu_buffer_addr = vkGetBufferDeviceAddress(device, &addr_info);

    fprintf(stderr, "[nv2a_vk] GPU buffer: %u MB mapped at %p, device addr 0x%llX\n",
            (unsigned)(GpuBufferLayout::TOTAL_SIZE / (1024*1024)),
            mapped_ptr, (unsigned long long)gpu_buffer_addr);
    return true;
}

// ---------------------------------------------------------------------------
// Push Buffer Parser Compute Pipeline
// ---------------------------------------------------------------------------

bool Nv2aVkRenderer::create_pushbuf_pipeline() {
    // Descriptor set layout: single storage buffer (the entire GPU allocation)
    VkDescriptorSetLayoutBinding binding = {};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo dsl_ci = {};
    dsl_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dsl_ci.bindingCount = 1;
    dsl_ci.pBindings = &binding;
    vk_check(vkCreateDescriptorSetLayout(device, &dsl_ci, nullptr, &pushbuf_dsl),
             "pushbuf descriptor set layout");

    // Push constants: frame parameters
    VkPushConstantRange pc_range = {};
    pc_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pc_range.offset = 0;
    pc_range.size = 16;  // max_dwords, frame_id, reserved[2]

    VkPipelineLayoutCreateInfo pl_ci = {};
    pl_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pl_ci.setLayoutCount = 1;
    pl_ci.pSetLayouts = &pushbuf_dsl;
    pl_ci.pushConstantRangeCount = 1;
    pl_ci.pPushConstantRanges = &pc_range;
    vk_check(vkCreatePipelineLayout(device, &pl_ci, nullptr, &pushbuf_layout),
             "pushbuf pipeline layout");

    // Shader module
    pushbuf_shader = create_shader(device, g_shaders.pushbuf_comp.data(),
                                   g_shaders.pushbuf_comp.size() * sizeof(uint32_t));
    if (!pushbuf_shader) return false;

    // Compute pipeline
    VkComputePipelineCreateInfo cp_ci = {};
    cp_ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cp_ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cp_ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cp_ci.stage.module = pushbuf_shader;
    cp_ci.stage.pName = "main";
    cp_ci.layout = pushbuf_layout;

    vk_check(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cp_ci, nullptr,
                                      &pushbuf_pipeline), "pushbuf compute pipeline");

    // Descriptor pool + set
    VkDescriptorPoolSize pool_size = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 };
    VkDescriptorPoolCreateInfo dp_ci = {};
    dp_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dp_ci.maxSets = 1;
    dp_ci.poolSizeCount = 1;
    dp_ci.pPoolSizes = &pool_size;
    vk_check(vkCreateDescriptorPool(device, &dp_ci, nullptr, &pushbuf_desc_pool),
             "pushbuf descriptor pool");

    VkDescriptorSetAllocateInfo ds_ai = {};
    ds_ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ds_ai.descriptorPool = pushbuf_desc_pool;
    ds_ai.descriptorSetCount = 1;
    ds_ai.pSetLayouts = &pushbuf_dsl;
    vk_check(vkAllocateDescriptorSets(device, &ds_ai, &pushbuf_desc_set),
             "pushbuf descriptor set");

    // Write descriptor: bind entire GPU buffer
    VkDescriptorBufferInfo buf_info = {};
    buf_info.buffer = gpu_buffer;
    buf_info.offset = 0;
    buf_info.range  = VK_WHOLE_SIZE;

    VkWriteDescriptorSet wr = {};
    wr.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wr.dstSet = pushbuf_desc_set;
    wr.dstBinding = 0;
    wr.descriptorCount = 1;
    wr.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    wr.pBufferInfo = &buf_info;
    vkUpdateDescriptorSets(device, 1, &wr, 0, nullptr);

    return true;
}

// ---------------------------------------------------------------------------
// Scanout Pipeline (blit framebuffer to swapchain)
// ---------------------------------------------------------------------------

bool Nv2aVkRenderer::create_scanout_pipeline(VkRenderPass present_pass, VkFormat) {
    // Single SSBO binding: the entire GPU buffer (Xbox RAM + PGRAPH state).
    // The shader reads PCRTC_START, surface format/pitch directly — no push constants.
    VkDescriptorSetLayoutBinding binding = {};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo dsl_ci = {};
    dsl_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dsl_ci.bindingCount = 1;
    dsl_ci.pBindings = &binding;
    vk_check(vkCreateDescriptorSetLayout(device, &dsl_ci, nullptr, &scanout_dsl),
             "scanout dsl");

    VkPipelineLayoutCreateInfo pl_ci = {};
    pl_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pl_ci.setLayoutCount = 1;
    pl_ci.pSetLayouts = &scanout_dsl;
    vk_check(vkCreatePipelineLayout(device, &pl_ci, nullptr, &scanout_layout),
             "scanout pipeline layout");

    // Shaders
    scanout_vert = create_shader(device, g_shaders.scanout_vert.data(),
                                 g_shaders.scanout_vert.size() * sizeof(uint32_t));
    scanout_frag = create_shader(device, g_shaders.scanout_frag.data(),
                                 g_shaders.scanout_frag.size() * sizeof(uint32_t));
    if (!scanout_vert || !scanout_frag) return false;

    // Graphics pipeline (fullscreen triangle, no vertex input)
    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = scanout_vert;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = scanout_frag;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vi = {};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo ia = {};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp = {};
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs = {};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms = {};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState cba = {};
    cba.colorWriteMask = 0xF;

    VkPipelineColorBlendStateCreateInfo cb = {};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;

    VkDynamicState dyn_states[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dyn = {};
    dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dyn_states;

    VkGraphicsPipelineCreateInfo gp_ci = {};
    gp_ci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gp_ci.stageCount = 2;
    gp_ci.pStages = stages;
    gp_ci.pVertexInputState = &vi;
    gp_ci.pInputAssemblyState = &ia;
    gp_ci.pViewportState = &vp;
    gp_ci.pRasterizationState = &rs;
    gp_ci.pMultisampleState = &ms;
    gp_ci.pColorBlendState = &cb;
    gp_ci.pDynamicState = &dyn;
    gp_ci.layout = scanout_layout;
    gp_ci.renderPass = present_pass;
    gp_ci.subpass = 0;

    vk_check(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gp_ci, nullptr,
                                       &scanout_pipeline), "scanout graphics pipeline");
    if (!scanout_pipeline) return false;

    // Allocate descriptor set and bind the GPU buffer.
    VkDescriptorPoolSize pool_size = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 };
    VkDescriptorPoolCreateInfo dp_ci = {};
    dp_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dp_ci.maxSets = 1;
    dp_ci.poolSizeCount = 1;
    dp_ci.pPoolSizes = &pool_size;
    vk_check(vkCreateDescriptorPool(device, &dp_ci, nullptr, &scanout_desc_pool),
             "scanout descriptor pool");

    VkDescriptorSetAllocateInfo ds_ai = {};
    ds_ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ds_ai.descriptorPool = scanout_desc_pool;
    ds_ai.descriptorSetCount = 1;
    ds_ai.pSetLayouts = &scanout_dsl;
    vk_check(vkAllocateDescriptorSets(device, &ds_ai, &scanout_desc_set),
             "scanout descriptor set");

    VkDescriptorBufferInfo buf_info = {};
    buf_info.buffer = gpu_buffer;
    buf_info.offset = 0;
    buf_info.range  = VK_WHOLE_SIZE;

    VkWriteDescriptorSet wr = {};
    wr.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wr.dstSet = scanout_desc_set;
    wr.dstBinding = 0;
    wr.descriptorCount = 1;
    wr.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    wr.pBufferInfo = &buf_info;
    vkUpdateDescriptorSets(device, 1, &wr, 0, nullptr);

    return true;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

bool Nv2aVkRenderer::init(VkDevice dev, VkPhysicalDevice phys,
                           VkQueue compute_q, uint32_t compute_family,
                           VkQueue graphics_q, uint32_t graphics_family,
                           VkRenderPass present_pass, VkFormat present_format) {
    device         = dev;
    physical       = phys;
    compute_queue  = compute_q;
    graphics_queue = graphics_q;
    compute_qf     = compute_family;
    graphics_qf    = graphics_family;

    if (!create_gpu_buffer()) return false;

    // Compile GLSL shaders to SPIR-V (once, at first init).
    if (g_shaders.pushbuf_comp.empty()) {
        glslang_initialize_process();
        if (!g_shaders.compile_all()) {
            fprintf(stderr, "[nv2a_vk] Shader compilation failed\n");
            return false;
        }
    }

    if (!create_pushbuf_pipeline()) return false;
    if (!create_scanout_pipeline(present_pass, present_format)) return false;
    if (!create_render_target()) return false;
    if (!create_draw_pipeline()) return false;
    if (!create_command_buffers()) return false;
    if (!create_sync_objects()) return false;

    fprintf(stderr, "[nv2a_vk] Renderer initialized\n");
    return true;
}

void Nv2aVkRenderer::destroy() {
    if (!device) return;
    vkDeviceWaitIdle(device);

    // Sync
    if (frame_fence)   vkDestroyFence(device, frame_fence, nullptr);
    if (compute_done)  vkDestroySemaphore(device, compute_done, nullptr);

    // Command pool
    if (cmd_pool) vkDestroyCommandPool(device, cmd_pool, nullptr);

    // Render target
    if (rt_framebuffer)  vkDestroyFramebuffer(device, rt_framebuffer, nullptr);
    if (rt_render_pass)  vkDestroyRenderPass(device, rt_render_pass, nullptr);
    if (rt_color_view)   vkDestroyImageView(device, rt_color_view, nullptr);
    if (rt_depth_view)   vkDestroyImageView(device, rt_depth_view, nullptr);
    if (rt_color)        vkDestroyImage(device, rt_color, nullptr);
    if (rt_depth)        vkDestroyImage(device, rt_depth, nullptr);
    if (rt_color_mem)    vkFreeMemory(device, rt_color_mem, nullptr);
    if (rt_depth_mem)    vkFreeMemory(device, rt_depth_mem, nullptr);

    // Draw pipeline
    if (draw_pipeline)   vkDestroyPipeline(device, draw_pipeline, nullptr);
    if (draw_layout)     vkDestroyPipelineLayout(device, draw_layout, nullptr);
    if (draw_dsl)        vkDestroyDescriptorSetLayout(device, draw_dsl, nullptr);
    if (draw_vert)       vkDestroyShaderModule(device, draw_vert, nullptr);
    if (draw_frag)       vkDestroyShaderModule(device, draw_frag, nullptr);
    if (draw_desc_pool)  vkDestroyDescriptorPool(device, draw_desc_pool, nullptr);

    // Scanout
    if (scanout_pipeline) vkDestroyPipeline(device, scanout_pipeline, nullptr);
    if (scanout_layout)   vkDestroyPipelineLayout(device, scanout_layout, nullptr);
    if (scanout_dsl)       vkDestroyDescriptorSetLayout(device, scanout_dsl, nullptr);
    if (scanout_vert)      vkDestroyShaderModule(device, scanout_vert, nullptr);
    if (scanout_frag)      vkDestroyShaderModule(device, scanout_frag, nullptr);
    if (scanout_desc_pool) vkDestroyDescriptorPool(device, scanout_desc_pool, nullptr);

    // Push buffer
    if (pushbuf_pipeline)  vkDestroyPipeline(device, pushbuf_pipeline, nullptr);
    if (pushbuf_layout)    vkDestroyPipelineLayout(device, pushbuf_layout, nullptr);
    if (pushbuf_dsl)       vkDestroyDescriptorSetLayout(device, pushbuf_dsl, nullptr);
    if (pushbuf_shader)    vkDestroyShaderModule(device, pushbuf_shader, nullptr);
    if (pushbuf_desc_pool) vkDestroyDescriptorPool(device, pushbuf_desc_pool, nullptr);

    // GPU buffer
    if (mapped_ptr)  vkUnmapMemory(device, gpu_memory);
    if (gpu_buffer)  vkDestroyBuffer(device, gpu_buffer, nullptr);
    if (gpu_memory)  vkFreeMemory(device, gpu_memory, nullptr);

    device = VK_NULL_HANDLE;
}

// ---------------------------------------------------------------------------
// Command Buffers + Sync
// ---------------------------------------------------------------------------

bool Nv2aVkRenderer::create_command_buffers() {
    VkCommandPoolCreateInfo cp_ci = {};
    cp_ci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cp_ci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cp_ci.queueFamilyIndex = graphics_qf;
    vk_check(vkCreateCommandPool(device, &cp_ci, nullptr, &cmd_pool), "cmd_pool");

    VkCommandBufferAllocateInfo cb_ai = {};
    cb_ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cb_ai.commandPool = cmd_pool;
    cb_ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cb_ai.commandBufferCount = 1;

    vk_check(vkAllocateCommandBuffers(device, &cb_ai, &cmd_compute), "cmd_compute");
    vk_check(vkAllocateCommandBuffers(device, &cb_ai, &cmd_render), "cmd_render");
    return true;
}

bool Nv2aVkRenderer::create_sync_objects() {
    VkSemaphoreCreateInfo sem_ci = {};
    sem_ci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    vk_check(vkCreateSemaphore(device, &sem_ci, nullptr, &compute_done), "compute_done sem");

    VkFenceCreateInfo fence_ci = {};
    fence_ci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fence_ci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    vk_check(vkCreateFence(device, &fence_ci, nullptr, &frame_fence), "frame_fence");

    return true;
}

// Unused stubs for now (render target creation deferred)
bool Nv2aVkRenderer::create_draw_pipeline() {
    // Descriptor: same SSBO layout as pushbuf/scanout (entire GPU buffer).
    VkDescriptorSetLayoutBinding binding = {};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo dsl_ci = {};
    dsl_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dsl_ci.bindingCount = 1;
    dsl_ci.pBindings = &binding;

    VkDescriptorSetLayout tmp_draw_dsl = VK_NULL_HANDLE;
    vk_check(vkCreateDescriptorSetLayout(device, &dsl_ci, nullptr, &tmp_draw_dsl), "draw dsl");
    draw_dsl = tmp_draw_dsl;

    VkPipelineLayoutCreateInfo pl_ci = {};
    pl_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pl_ci.setLayoutCount = 1;
    pl_ci.pSetLayouts = &draw_dsl;
    vk_check(vkCreatePipelineLayout(device, &pl_ci, nullptr, &draw_layout), "draw pipeline layout");

    // Shader modules
    draw_vert = create_shader(device, g_shaders.draw_vert.data(),
                              g_shaders.draw_vert.size() * sizeof(uint32_t));
    draw_frag = create_shader(device, g_shaders.draw_frag.data(),
                              g_shaders.draw_frag.size() * sizeof(uint32_t));
    if (!draw_vert || !draw_frag) return false;

    // Graphics pipeline
    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = draw_vert;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = draw_frag;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vi = {};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    // No vertex input — the VS fetches from SSBO.

    VkPipelineInputAssemblyStateCreateInfo ia = {};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp = {};
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs = {};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms = {};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds = {};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = VK_TRUE;
    ds.depthWriteEnable = VK_TRUE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS;

    VkPipelineColorBlendAttachmentState cba = {};
    cba.colorWriteMask = 0xF;
    cba.blendEnable = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo cb = {};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;

    VkDynamicState dyn_states[] = {
        VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
        VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,
        VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
        VK_DYNAMIC_STATE_BLEND_CONSTANTS,
    };
    VkPipelineDynamicStateCreateInfo dyn = {};
    dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = sizeof(dyn_states) / sizeof(dyn_states[0]);
    dyn.pDynamicStates = dyn_states;

    VkGraphicsPipelineCreateInfo gp_ci = {};
    gp_ci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gp_ci.stageCount = 2;
    gp_ci.pStages = stages;
    gp_ci.pVertexInputState = &vi;
    gp_ci.pInputAssemblyState = &ia;
    gp_ci.pViewportState = &vp;
    gp_ci.pRasterizationState = &rs;
    gp_ci.pMultisampleState = &ms;
    gp_ci.pDepthStencilState = &ds;
    gp_ci.pColorBlendState = &cb;
    gp_ci.pDynamicState = &dyn;
    gp_ci.layout = draw_layout;
    gp_ci.renderPass = rt_render_pass;
    gp_ci.subpass = 0;

    vk_check(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gp_ci, nullptr,
                                       &draw_pipeline), "draw graphics pipeline");

    // Allocate draw descriptor set (reuse scanout desc pool pattern).
    VkDescriptorPoolSize pool_size = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 };
    VkDescriptorPoolCreateInfo dp_ci = {};
    dp_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dp_ci.maxSets = 1;
    dp_ci.poolSizeCount = 1;
    dp_ci.pPoolSizes = &pool_size;

    VkDescriptorPool tmp_dp = VK_NULL_HANDLE;
    vk_check(vkCreateDescriptorPool(device, &dp_ci, nullptr, &tmp_dp), "draw desc pool");
    draw_desc_pool = tmp_dp;

    VkDescriptorSetAllocateInfo ds_ai = {};
    ds_ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ds_ai.descriptorPool = draw_desc_pool;
    ds_ai.descriptorSetCount = 1;
    ds_ai.pSetLayouts = &draw_dsl;

    VkDescriptorSet tmp_draw_desc_set = VK_NULL_HANDLE;
    vk_check(vkAllocateDescriptorSets(device, &ds_ai, &tmp_draw_desc_set), "draw desc set");
    draw_desc_set = tmp_draw_desc_set;

    VkDescriptorBufferInfo buf_info = {};
    buf_info.buffer = gpu_buffer;
    buf_info.offset = 0;
    buf_info.range  = VK_WHOLE_SIZE;

    VkWriteDescriptorSet wr = {};
    wr.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wr.dstSet = draw_desc_set;
    wr.dstBinding = 0;
    wr.descriptorCount = 1;
    wr.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    wr.pBufferInfo = &buf_info;
    vkUpdateDescriptorSets(device, 1, &wr, 0, nullptr);

    return draw_pipeline != VK_NULL_HANDLE;
}

bool Nv2aVkRenderer::create_render_target() {
    // Off-screen render target: 640x480, color + depth.
    // Color: B8G8R8A8_UNORM (matches NV2A X8R8G8B8).
    // Depth: D24_UNORM_S8_UINT (matches NV2A Z24S8).

    VkFormat color_fmt = VK_FORMAT_B8G8R8A8_UNORM;
    VkFormat depth_fmt = VK_FORMAT_D24_UNORM_S8_UINT;

    // Check if D24_UNORM_S8_UINT is supported; fall back to D32_SFLOAT_S8_UINT.
    VkFormatProperties fmt_props;
    vkGetPhysicalDeviceFormatProperties(physical, depth_fmt, &fmt_props);
    if (!(fmt_props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT))
        depth_fmt = VK_FORMAT_D32_SFLOAT_S8_UINT;

    // Color image
    VkImageCreateInfo img_ci = {};
    img_ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    img_ci.imageType = VK_IMAGE_TYPE_2D;
    img_ci.format = color_fmt;
    img_ci.extent = { rt_width, rt_height, 1 };
    img_ci.mipLevels = 1;
    img_ci.arrayLayers = 1;
    img_ci.samples = VK_SAMPLE_COUNT_1_BIT;
    img_ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    img_ci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    vk_check(vkCreateImage(device, &img_ci, nullptr, &rt_color), "rt_color image");

    // Depth image
    img_ci.format = depth_fmt;
    img_ci.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    vk_check(vkCreateImage(device, &img_ci, nullptr, &rt_depth), "rt_depth image");

    // Allocate and bind memory for both images.
    auto alloc_image = [&](VkImage img) -> VkDeviceMemory {
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(device, img, &req);
        uint32_t mt = find_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VkMemoryAllocateInfo ai = {};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = mt;
        VkDeviceMemory mem = VK_NULL_HANDLE;
        vk_check(vkAllocateMemory(device, &ai, nullptr, &mem), "rt image memory");
        vk_check(vkBindImageMemory(device, img, mem, 0), "bind rt image");
        return mem;
    };

    rt_color_mem = alloc_image(rt_color);
    rt_depth_mem = alloc_image(rt_depth);

    // Color view
    VkImageViewCreateInfo iv_ci = {};
    iv_ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    iv_ci.image = rt_color;
    iv_ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    iv_ci.format = color_fmt;
    iv_ci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vk_check(vkCreateImageView(device, &iv_ci, nullptr, &rt_color_view), "rt_color view");

    // Depth view
    iv_ci.image = rt_depth;
    iv_ci.format = depth_fmt;
    iv_ci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    vk_check(vkCreateImageView(device, &iv_ci, nullptr, &rt_depth_view), "rt_depth view");

    // Render pass: color + depth attachments.
    VkAttachmentDescription atts[2] = {};
    atts[0].format = color_fmt;
    atts[0].samples = VK_SAMPLE_COUNT_1_BIT;
    atts[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    atts[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    atts[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    atts[0].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    atts[1].format = depth_fmt;
    atts[1].samples = VK_SAMPLE_COUNT_1_BIT;
    atts[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    atts[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    atts[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    atts[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    atts[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    atts[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference color_ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkAttachmentReference depth_ref = { 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };

    VkSubpassDescription subpass = {};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_ref;
    subpass.pDepthStencilAttachment = &depth_ref;

    VkSubpassDependency dep = {};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dep.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    dep.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo rp_ci = {};
    rp_ci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rp_ci.attachmentCount = 2;
    rp_ci.pAttachments = atts;
    rp_ci.subpassCount = 1;
    rp_ci.pSubpasses = &subpass;
    rp_ci.dependencyCount = 1;
    rp_ci.pDependencies = &dep;
    vk_check(vkCreateRenderPass(device, &rp_ci, nullptr, &rt_render_pass), "rt render pass");

    // Framebuffer
    VkImageView fb_views[] = { rt_color_view, rt_depth_view };
    VkFramebufferCreateInfo fb_ci = {};
    fb_ci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fb_ci.renderPass = rt_render_pass;
    fb_ci.attachmentCount = 2;
    fb_ci.pAttachments = fb_views;
    fb_ci.width = rt_width;
    fb_ci.height = rt_height;
    fb_ci.layers = 1;
    vk_check(vkCreateFramebuffer(device, &fb_ci, nullptr, &rt_framebuffer), "rt framebuffer");

    fprintf(stderr, "[nv2a_vk] Render target: %ux%u\n", rt_width, rt_height);
    return rt_framebuffer != VK_NULL_HANDLE;
}

// ---------------------------------------------------------------------------
// Per-Frame Operations
// ---------------------------------------------------------------------------

void Nv2aVkRenderer::set_dma_put(uint32_t put_addr) {
    auto* ctl = mapped<PfifoControlBlock>(GpuBufferLayout::PFIFO_CTL_OFFSET);
    ctl->dma_put = put_addr;
    ctl->push_enabled = 1;
}

void Nv2aVkRenderer::set_pcrtc_start(uint32_t addr) {
    auto* ctl = mapped<PfifoControlBlock>(GpuBufferLayout::PFIFO_CTL_OFFSET);
    ctl->pcrtc_start = addr;
}

void Nv2aVkRenderer::dispatch_pushbuf_parse(VkCommandBuffer cmd) {
    // Reset draw_count to 0 at the start of each frame so the compute shader
    // can emit fresh draws via atomicAdd.
    VkDeviceSize draw_count_offset = GpuBufferLayout::PFIFO_CTL_OFFSET
                                   + offsetof(PfifoControlBlock, draw_count);
    vkCmdFillBuffer(cmd, gpu_buffer, draw_count_offset, 4, 0);

    // Barrier: transfer write → compute shader read/write
    VkMemoryBarrier fill_barrier = {};
    fill_barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    fill_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    fill_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 1, &fill_barrier, 0, nullptr, 0, nullptr);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pushbuf_pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pushbuf_layout,
                            0, 1, &pushbuf_desc_set, 0, nullptr);

    // Push constants: max_dwords to process this frame
    struct { uint32_t max_dwords; uint32_t frame_id; uint32_t pad[2]; } pc;
    pc.max_dwords = 1048576;  // process up to 1M dwords (~4 MB) per dispatch
    pc.frame_id = frame_id;
    pc.pad[0] = pc.pad[1] = 0;
    vkCmdPushConstants(cmd, pushbuf_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 16, &pc);

    // Single workgroup — the shader loops internally over the push buffer.
    // This is intentional: NV2A push buffer processing is inherently serial.
    vkCmdDispatch(cmd, 1, 1, 1);

    // Memory barrier: compute writes → indirect draw reads + vertex fetch
    VkMemoryBarrier barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_INDIRECT_COMMAND_READ_BIT |
                            VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT |
                         VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 1, &barrier, 0, nullptr, 0, nullptr);
}

void Nv2aVkRenderer::execute_draws(VkCommandBuffer cmd) {
    // GPU-driven indirect draws — draw_count is read from the PFIFO control
    // block by the GPU at draw time, no CPU readback.
    if (!draw_pipeline || !rt_framebuffer) return;

    // Begin off-screen render pass.
    VkClearValue clears[2] = {};
    clears[0].color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    clears[1].depthStencil = {1.0f, 0};

    VkRenderPassBeginInfo rp_begin = {};
    rp_begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp_begin.renderPass = rt_render_pass;
    rp_begin.framebuffer = rt_framebuffer;
    rp_begin.renderArea.extent = { rt_width, rt_height };
    rp_begin.clearValueCount = 2;
    rp_begin.pClearValues = clears;
    vkCmdBeginRenderPass(cmd, &rp_begin, VK_SUBPASS_CONTENTS_INLINE);

    // Bind 3D draw pipeline + descriptor set.
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, draw_pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, draw_layout,
                            0, 1, &draw_desc_set, 0, nullptr);

    VkViewport viewport = { 0, 0, (float)rt_width, (float)rt_height, 0, 1 };
    VkRect2D scissor = { {0, 0}, { rt_width, rt_height } };
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    // vkCmdDrawIndirectCount reads the count from the buffer at runtime.
    // maxDrawCount caps it to prevent runaway draws.
    VkDeviceSize cmd_offset   = GpuBufferLayout::DRAW_CMD_OFFSET;
    VkDeviceSize count_offset = GpuBufferLayout::PFIFO_CTL_OFFSET
                              + offsetof(PfifoControlBlock, draw_count);

    vkCmdDrawIndirectCount(cmd,
        gpu_buffer, cmd_offset,
        gpu_buffer, count_offset,
        1024,                     // maxDrawCount
        sizeof(GpuDrawCommand));

    vkCmdEndRenderPass(cmd);
}

void Nv2aVkRenderer::scanout(VkCommandBuffer cmd, VkFramebuffer /*dst_fb*/,
                              VkExtent2D extent) {
    // Scanout: the fragment shader reads PCRTC_START, surface format/pitch
    // directly from the GPU buffer (PFIFO control block + PGRAPH state).
    // No CPU readback — only the viewport (host swapchain size) is set here.
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, scanout_pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, scanout_layout,
                            0, 1, &scanout_desc_set, 0, nullptr);

    VkViewport viewport = { 0, 0, (float)extent.width, (float)extent.height, 0, 1 };
    VkRect2D scissor = { {0, 0}, extent };
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    // Fullscreen triangle (3 vertices, no vertex buffer).
    // The shader reads all NV2A state from the bound storage buffer.
    vkCmdDraw(cmd, 3, 1, 0, 0);
}

VkCommandBuffer Nv2aVkRenderer::record_frame(VkFramebuffer dst_fb, VkExtent2D extent) {
    // Wait for previous frame
    vkWaitForFences(device, 1, &frame_fence, VK_TRUE, UINT64_MAX);
    vkResetFences(device, 1, &frame_fence);

    ++frame_id;

    // Record compute + render into a single command buffer
    VkCommandBuffer cmd = cmd_render;
    vkResetCommandBuffer(cmd, 0);

    VkCommandBufferBeginInfo begin = {};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);

    // Reset draw_count and update frame_id via GPU commands (no CPU mapped writes)
    VkDeviceSize count_off = GpuBufferLayout::PFIFO_CTL_OFFSET
                           + offsetof(PfifoControlBlock, draw_count);
    VkDeviceSize fid_off   = GpuBufferLayout::PFIFO_CTL_OFFSET
                           + offsetof(PfifoControlBlock, frame_id);
    vkCmdFillBuffer(cmd, gpu_buffer, count_off, sizeof(uint32_t), 0);
    vkCmdUpdateBuffer(cmd, gpu_buffer, fid_off, sizeof(uint32_t), &frame_id);

    // Barrier: transfer writes → compute shader reads
    VkMemoryBarrier xfer_bar = {};
    xfer_bar.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    xfer_bar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    xfer_bar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &xfer_bar, 0, nullptr, 0, nullptr);

    // Phase 1: parse push buffer (compute)
    dispatch_pushbuf_parse(cmd);

    // Phase 2: execute 3D draws (currently a no-op until full pipeline is ready)
    execute_draws(cmd);

    // Phase 3: scanout is done by the caller inside their render pass
    // (the scanout() method is called separately within the presentation pass)

    vkEndCommandBuffer(cmd);
    return cmd;
}

} // namespace xbox
