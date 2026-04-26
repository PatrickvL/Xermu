// ---------------------------------------------------------------------------
// main.cpp — Xermu: Xbox emulator with SDL3 / ImGui / Vulkan UI.
//
// Creates a window, lets the user select an XBE or XISO to boot, and runs
// the emulated Xbox system.  Falls back to the dashboard XBE if present.
// ---------------------------------------------------------------------------

#include "xbox/hle/bootstrap.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

// Use volk for Vulkan dynamic loading (no SDK required)
#include <volk.h>

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_vulkan.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Vulkan helpers — minimal single-queue setup for ImGui rendering.
// ---------------------------------------------------------------------------

struct VulkanContext {
    VkInstance               instance       = VK_NULL_HANDLE;
    VkPhysicalDevice         physical       = VK_NULL_HANDLE;
    VkDevice                 device         = VK_NULL_HANDLE;
    VkQueue                  queue          = VK_NULL_HANDLE;
    uint32_t                 queue_family   = 0;
    VkSurfaceKHR             surface        = VK_NULL_HANDLE;
    VkDescriptorPool         descriptor_pool= VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debug_messenger= VK_NULL_HANDLE;

    // ImGui render pass resources
    VkRenderPass             render_pass    = VK_NULL_HANDLE;
    VkSwapchainKHR           swapchain      = VK_NULL_HANDLE;
    std::vector<VkImage>     swapchain_images;
    std::vector<VkImageView> swapchain_views;
    std::vector<VkFramebuffer> framebuffers;
    VkCommandPool            cmd_pool       = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> cmd_buffers;
    std::vector<VkSemaphore> image_available;
    std::vector<VkSemaphore> render_finished;
    std::vector<VkFence>     in_flight;
    VkFormat                 swapchain_format = VK_FORMAT_B8G8R8A8_UNORM;
    VkExtent2D               swapchain_extent = {1280, 720};
    uint32_t                 frame_count    = 0;
    uint32_t                 current_frame  = 0;
};

static void check_vk(VkResult r, const char* msg) {
    if (r != VK_SUCCESS) {
        fprintf(stderr, "[vulkan] %s failed: %d\n", msg, (int)r);
    }
}

static bool init_vulkan(VulkanContext& vk, SDL_Window* window) {
    // Initialize volk (dynamically loads vulkan-1.dll)
    if (volkInitialize() != VK_SUCCESS) {
        fprintf(stderr, "[vulkan] volkInitialize failed — no Vulkan driver?\n");
        return false;
    }

    // Instance
    uint32_t sdl_ext_count = 0;
    const char* const* sdl_exts = SDL_Vulkan_GetInstanceExtensions(&sdl_ext_count);

    VkApplicationInfo app_info = {};
    app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName = "Xermu";
    app_info.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    app_info.pEngineName = "Xermu";
    app_info.engineVersion = VK_MAKE_VERSION(0, 1, 0);
    app_info.apiVersion = VK_API_VERSION_1_2;

    VkInstanceCreateInfo inst_info = {};
    inst_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    inst_info.pApplicationInfo = &app_info;
    inst_info.enabledExtensionCount = sdl_ext_count;
    inst_info.ppEnabledExtensionNames = sdl_exts;

    check_vk(vkCreateInstance(&inst_info, nullptr, &vk.instance), "vkCreateInstance");
    if (!vk.instance) return false;
    volkLoadInstance(vk.instance);

    // Surface
    if (!SDL_Vulkan_CreateSurface(window, vk.instance, nullptr, &vk.surface)) {
        fprintf(stderr, "[vulkan] SDL_Vulkan_CreateSurface failed: %s\n", SDL_GetError());
        return false;
    }

    // Physical device — pick first discrete GPU, or first available
    uint32_t gpu_count = 0;
    vkEnumeratePhysicalDevices(vk.instance, &gpu_count, nullptr);
    if (gpu_count == 0) { fprintf(stderr, "[vulkan] No GPUs found\n"); return false; }

    std::vector<VkPhysicalDevice> gpus(gpu_count);
    vkEnumeratePhysicalDevices(vk.instance, &gpu_count, gpus.data());
    vk.physical = gpus[0];
    for (auto& g : gpus) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(g, &props);
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            vk.physical = g;
            break;
        }
    }

    // Queue family — graphics + present
    uint32_t qf_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(vk.physical, &qf_count, nullptr);
    std::vector<VkQueueFamilyProperties> qf_props(qf_count);
    vkGetPhysicalDeviceQueueFamilyProperties(vk.physical, &qf_count, qf_props.data());

    vk.queue_family = UINT32_MAX;
    for (uint32_t i = 0; i < qf_count; i++) {
        VkBool32 present_support = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(vk.physical, i, vk.surface, &present_support);
        if ((qf_props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present_support) {
            vk.queue_family = i;
            break;
        }
    }
    if (vk.queue_family == UINT32_MAX) {
        fprintf(stderr, "[vulkan] No suitable queue family\n");
        return false;
    }

    // Logical device
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info = {};
    queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_info.queueFamilyIndex = vk.queue_family;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;

    const char* dev_exts[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    VkDeviceCreateInfo dev_info = {};
    dev_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dev_info.queueCreateInfoCount = 1;
    dev_info.pQueueCreateInfos = &queue_info;
    dev_info.enabledExtensionCount = 1;
    dev_info.ppEnabledExtensionNames = dev_exts;

    check_vk(vkCreateDevice(vk.physical, &dev_info, nullptr, &vk.device), "vkCreateDevice");
    if (!vk.device) return false;
    volkLoadDevice(vk.device);
    vkGetDeviceQueue(vk.device, vk.queue_family, 0, &vk.queue);

    // Descriptor pool for ImGui
    VkDescriptorPoolSize pool_sizes[] = {
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 100 },
    };
    VkDescriptorPoolCreateInfo pool_info = {};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pool_info.maxSets = 100;
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes = pool_sizes;
    check_vk(vkCreateDescriptorPool(vk.device, &pool_info, nullptr, &vk.descriptor_pool),
             "vkCreateDescriptorPool");

    return true;
}

static bool create_swapchain(VulkanContext& vk, SDL_Window* window) {
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vk.physical, vk.surface, &caps);

    int w, h;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    vk.swapchain_extent = { (uint32_t)w, (uint32_t)h };

    uint32_t image_count = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && image_count > caps.maxImageCount)
        image_count = caps.maxImageCount;
    vk.frame_count = image_count;

    VkSwapchainCreateInfoKHR sc_info = {};
    sc_info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    sc_info.surface = vk.surface;
    sc_info.minImageCount = image_count;
    sc_info.imageFormat = vk.swapchain_format;
    sc_info.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    sc_info.imageExtent = vk.swapchain_extent;
    sc_info.imageArrayLayers = 1;
    sc_info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    sc_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sc_info.preTransform = caps.currentTransform;
    sc_info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    sc_info.presentMode = VK_PRESENT_MODE_FIFO_KHR;  // vsync
    sc_info.clipped = VK_TRUE;
    sc_info.oldSwapchain = vk.swapchain;

    check_vk(vkCreateSwapchainKHR(vk.device, &sc_info, nullptr, &vk.swapchain),
             "vkCreateSwapchainKHR");

    // Swapchain images + views
    vkGetSwapchainImagesKHR(vk.device, vk.swapchain, &image_count, nullptr);
    vk.swapchain_images.resize(image_count);
    vkGetSwapchainImagesKHR(vk.device, vk.swapchain, &image_count, vk.swapchain_images.data());

    vk.swapchain_views.resize(image_count);
    for (uint32_t i = 0; i < image_count; i++) {
        VkImageViewCreateInfo iv = {};
        iv.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        iv.image = vk.swapchain_images[i];
        iv.viewType = VK_IMAGE_VIEW_TYPE_2D;
        iv.format = vk.swapchain_format;
        iv.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        check_vk(vkCreateImageView(vk.device, &iv, nullptr, &vk.swapchain_views[i]),
                 "vkCreateImageView");
    }

    // Render pass
    VkAttachmentDescription att = {};
    att.format = vk.swapchain_format;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference att_ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription subpass = {};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &att_ref;

    VkSubpassDependency dep = {};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.srcAccessMask = 0;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo rp_info = {};
    rp_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rp_info.attachmentCount = 1;
    rp_info.pAttachments = &att;
    rp_info.subpassCount = 1;
    rp_info.pSubpasses = &subpass;
    rp_info.dependencyCount = 1;
    rp_info.pDependencies = &dep;
    check_vk(vkCreateRenderPass(vk.device, &rp_info, nullptr, &vk.render_pass),
             "vkCreateRenderPass");

    // Framebuffers
    vk.framebuffers.resize(image_count);
    for (uint32_t i = 0; i < image_count; i++) {
        VkFramebufferCreateInfo fb = {};
        fb.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fb.renderPass = vk.render_pass;
        fb.attachmentCount = 1;
        fb.pAttachments = &vk.swapchain_views[i];
        fb.width = vk.swapchain_extent.width;
        fb.height = vk.swapchain_extent.height;
        fb.layers = 1;
        check_vk(vkCreateFramebuffer(vk.device, &fb, nullptr, &vk.framebuffers[i]),
                 "vkCreateFramebuffer");
    }

    // Command pool + buffers
    VkCommandPoolCreateInfo cp = {};
    cp.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cp.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cp.queueFamilyIndex = vk.queue_family;
    check_vk(vkCreateCommandPool(vk.device, &cp, nullptr, &vk.cmd_pool),
             "vkCreateCommandPool");

    vk.cmd_buffers.resize(image_count);
    VkCommandBufferAllocateInfo cb = {};
    cb.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cb.commandPool = vk.cmd_pool;
    cb.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cb.commandBufferCount = image_count;
    check_vk(vkAllocateCommandBuffers(vk.device, &cb, vk.cmd_buffers.data()),
             "vkAllocateCommandBuffers");

    // Sync objects
    vk.image_available.resize(image_count);
    vk.render_finished.resize(image_count);
    vk.in_flight.resize(image_count);
    for (uint32_t i = 0; i < image_count; i++) {
        VkSemaphoreCreateInfo si = {};
        si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        vkCreateSemaphore(vk.device, &si, nullptr, &vk.image_available[i]);
        vkCreateSemaphore(vk.device, &si, nullptr, &vk.render_finished[i]);
        VkFenceCreateInfo fi = {};
        fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        vkCreateFence(vk.device, &fi, nullptr, &vk.in_flight[i]);
    }

    return true;
}

static void cleanup_vulkan(VulkanContext& vk) {
    if (vk.device) vkDeviceWaitIdle(vk.device);

    for (auto& f : vk.in_flight)       if (f) vkDestroyFence(vk.device, f, nullptr);
    for (auto& s : vk.render_finished) if (s) vkDestroySemaphore(vk.device, s, nullptr);
    for (auto& s : vk.image_available) if (s) vkDestroySemaphore(vk.device, s, nullptr);
    if (vk.cmd_pool)        vkDestroyCommandPool(vk.device, vk.cmd_pool, nullptr);
    for (auto& fb : vk.framebuffers)   if (fb) vkDestroyFramebuffer(vk.device, fb, nullptr);
    if (vk.render_pass)     vkDestroyRenderPass(vk.device, vk.render_pass, nullptr);
    for (auto& iv : vk.swapchain_views) if (iv) vkDestroyImageView(vk.device, iv, nullptr);
    if (vk.swapchain)       vkDestroySwapchainKHR(vk.device, vk.swapchain, nullptr);
    if (vk.descriptor_pool) vkDestroyDescriptorPool(vk.device, vk.descriptor_pool, nullptr);
    if (vk.device)          vkDestroyDevice(vk.device, nullptr);
    if (vk.surface)         vkDestroySurfaceKHR(vk.instance, vk.surface, nullptr);
    if (vk.instance)        vkDestroyInstance(vk.instance, nullptr);
}

// ---------------------------------------------------------------------------
// Application state
// ---------------------------------------------------------------------------

enum class AppState {
    Menu,       // showing file picker / boot options
    Running,    // emulation active
    Halted,     // guest halted or crashed
};

struct AppContext {
    AppState               state       = AppState::Menu;
    xbox::XboxSystem       sys;
    xbox::BootConfig       cfg;
    std::string            status_msg  = "Ready";
    std::vector<std::string> log_lines;

    // Paths found on disk
    std::string            dashboard_xbe;  // auto-detected dashboard
    std::string            bios_path;
    std::string            mcpx_path;

    void log(const char* msg) {
        log_lines.emplace_back(msg);
        if (log_lines.size() > 500) log_lines.erase(log_lines.begin());
        status_msg = msg;
    }
};

// Scan data/ for known files
static void scan_data_dir(AppContext& app) {
    // Check common locations for dashboard and BIOS
    const char* dash_paths[] = {
        "data/xbox dash orig_5960/xboxdash.xbe",
        "data/xboxdash.xbe",
        "xboxdash.xbe",
    };
    for (auto& p : dash_paths) {
        FILE* f = fopen(p, "rb");
        if (f) { fclose(f); app.dashboard_xbe = p; break; }
    }

    const char* bios_paths[] = {
        "data/bios.bin",
        "bios.bin",
        "data/complex_4627.bin",
    };
    for (auto& p : bios_paths) {
        FILE* f = fopen(p, "rb");
        if (f) { fclose(f); app.bios_path = p; break; }
    }

    const char* mcpx_paths[] = {
        "data/mcpx_1.0.bin",
        "mcpx.bin",
    };
    for (auto& p : mcpx_paths) {
        FILE* f = fopen(p, "rb");
        if (f) { fclose(f); app.mcpx_path = p; break; }
    }
}

// File dialog helper — uses SDL3 file dialog (async callback)
static std::string g_picked_file;
static bool g_file_picked = false;

static void SDLCALL file_dialog_callback(void* /*userdata*/, const char* const* filelist, int /*filter*/) {
    if (filelist && filelist[0]) {
        g_picked_file = filelist[0];
        g_file_picked = true;
    }
}

// ---------------------------------------------------------------------------
// ImGui frame — menu / boot UI
// ---------------------------------------------------------------------------

static void draw_menu(AppContext& app) {
    ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(500, 400), ImGuiCond_FirstUseEver);

    ImGui::Begin("Xermu — Xbox Emulator");

    // Status
    ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "%s", app.status_msg.c_str());
    ImGui::Separator();

    // Auto-detected files
    if (!app.dashboard_xbe.empty())
        ImGui::Text("Dashboard: %s", app.dashboard_xbe.c_str());
    else
        ImGui::TextColored(ImVec4(1, 0.5f, 0, 1), "No dashboard found");

    if (!app.bios_path.empty())
        ImGui::Text("BIOS: %s", app.bios_path.c_str());

    ImGui::Separator();

    // Boot options
    ImGui::Text("Boot Options:");
    ImGui::Spacing();

    // Boot dashboard (HLE)
    if (!app.dashboard_xbe.empty()) {
        if (ImGui::Button("Boot Dashboard (HLE)", ImVec2(300, 30))) {
            app.cfg = {};
            app.cfg.xbe_path = app.dashboard_xbe;
            app.log("[ui] Booting dashboard (HLE)...");

            auto log_fn = [&](const char* msg) { app.log(msg); };
            if (xbox::boot_hle(app.sys, app.cfg, log_fn)) {
                app.state = AppState::Running;
            } else {
                app.log("[ui] Boot failed!");
                app.state = AppState::Halted;
            }
        }
    }

    // Boot custom XBE
    if (ImGui::Button("Select XBE...", ImVec2(300, 30))) {
        SDL_DialogFileFilter filters[] = {{ "XBE files", "xbe" }, { "All files", "*" }};
        SDL_ShowOpenFileDialog(file_dialog_callback, nullptr, nullptr, filters, 2, nullptr, false);
    }

    // Boot XISO
    if (ImGui::Button("Select XISO...", ImVec2(300, 30))) {
        SDL_DialogFileFilter filters[] = {{ "XISO images", "iso;xiso" }, { "All files", "*" }};
        SDL_ShowOpenFileDialog(file_dialog_callback, nullptr, nullptr, filters, 2, nullptr, false);
    }

    // LLE boot (if BIOS available)
    if (!app.bios_path.empty()) {
        ImGui::Spacing();
        if (ImGui::Button("Boot BIOS (LLE)", ImVec2(300, 30))) {
            app.cfg = {};
            app.cfg.bios_path = app.bios_path;
            app.cfg.mcpx_path = app.mcpx_path;
            app.log("[ui] Booting BIOS (LLE)...");

            auto log_fn = [&](const char* msg) { app.log(msg); };
            if (xbox::boot_lle(app.sys, app.cfg, log_fn)) {
                app.state = AppState::Running;
            } else {
                app.log("[ui] LLE boot failed!");
                app.state = AppState::Halted;
            }
        }
    }

    ImGui::Separator();

    // Log window
    if (ImGui::CollapsingHeader("Log", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::BeginChild("LogScroll", ImVec2(0, 150), ImGuiChildFlags_Borders);
        for (auto& line : app.log_lines)
            ImGui::TextUnformatted(line.c_str());
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
            ImGui::SetScrollHereY(1.0f);
        ImGui::EndChild();
    }

    ImGui::End();
}

static void draw_running(AppContext& app) {
    ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(400, 300), ImGuiCond_FirstUseEver);

    ImGui::Begin("Xermu — Running");

    auto& ctx = app.sys.exec->ctx;
    ImGui::Text("EIP: 0x%08X  EAX: 0x%08X", ctx.eip, ctx.gp[GP_EAX]);
    ImGui::Text("ESP: 0x%08X  EBP: 0x%08X", ctx.gp[GP_ESP], ctx.gp[GP_EBP]);
    ImGui::Text("State: %s", app.sys.running ? "Running" : "Halted");

    ImGui::Separator();

    if (ImGui::Button("Stop", ImVec2(100, 25))) {
        app.sys.running = false;
        app.state = AppState::Halted;
        app.log("[ui] Stopped by user");
    }

    ImGui::SameLine();
    if (ImGui::Button("Back to Menu", ImVec2(150, 25))) {
        app.sys.shutdown();
        app.sys = xbox::XboxSystem();
        app.state = AppState::Menu;
        app.log("[ui] Returned to menu");
    }

    // Log
    ImGui::Separator();
    ImGui::BeginChild("LogScroll", ImVec2(0, 120), ImGuiChildFlags_Borders);
    for (auto& line : app.log_lines)
        ImGui::TextUnformatted(line.c_str());
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
        ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();

    ImGui::End();
}

// ---------------------------------------------------------------------------
// Main loop
// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
    (void)argc; (void)argv;

    // Init SDL
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Window* window = SDL_CreateWindow("Xermu — Xbox Emulator",
                                          1280, 720,
                                          SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
    if (!window) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    // Init Vulkan
    VulkanContext vk;
    if (!init_vulkan(vk, window)) {
        fprintf(stderr, "Vulkan init failed\n");
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    if (!create_swapchain(vk, window)) {
        fprintf(stderr, "Swapchain creation failed\n");
        cleanup_vulkan(vk);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    // Init ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();

    ImGui_ImplSDL3_InitForVulkan(window);

    ImGui_ImplVulkan_InitInfo init_info = {};
    init_info.Instance = vk.instance;
    init_info.PhysicalDevice = vk.physical;
    init_info.Device = vk.device;
    init_info.QueueFamily = vk.queue_family;
    init_info.Queue = vk.queue;
    init_info.DescriptorPool = vk.descriptor_pool;
    init_info.RenderPass = vk.render_pass;
    init_info.MinImageCount = vk.frame_count;
    init_info.ImageCount = vk.frame_count;
    init_info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    ImGui_ImplVulkan_Init(&init_info);

    // App state
    AppContext app;
    scan_data_dir(app);

    // Handle command-line XBE arg
    if (argc >= 2) {
        std::string arg = argv[1];
        if (arg.size() > 4 && (arg.substr(arg.size()-4) == ".xbe" || arg.substr(arg.size()-4) == ".XBE")) {
            app.cfg.xbe_path = arg;
            auto log_fn = [&](const char* msg) { app.log(msg); };
            if (xbox::boot_hle(app.sys, app.cfg, log_fn)) {
                app.state = AppState::Running;
            } else {
                app.log("[ui] Boot failed for command-line XBE");
            }
        }
    }

    // Main loop
    bool quit = false;
    while (!quit) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL3_ProcessEvent(&event);
            if (event.type == SDL_EVENT_QUIT)
                quit = true;
            if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
                quit = true;
        }

        // Handle file dialog result
        if (g_file_picked) {
            g_file_picked = false;
            std::string path = g_picked_file;
            g_picked_file.clear();

            // Determine file type by extension
            std::string ext;
            auto dot = path.rfind('.');
            if (dot != std::string::npos) ext = path.substr(dot);
            for (auto& c : ext) c = (char)tolower(c);

            if (ext == ".xbe") {
                app.cfg = {};
                app.cfg.xbe_path = path;
                app.log(("[ui] Loading: " + path).c_str());
                auto log_fn = [&](const char* msg) { app.log(msg); };
                if (xbox::boot_hle(app.sys, app.cfg, log_fn)) {
                    app.state = AppState::Running;
                } else {
                    app.log("[ui] Boot failed!");
                }
            } else if (ext == ".iso" || ext == ".xiso") {
                // XISO: look for default.xbe inside or mount as D:
                app.cfg = {};
                if (!app.dashboard_xbe.empty()) {
                    app.cfg.xbe_path = app.dashboard_xbe;
                    app.cfg.xiso_path = path;
                    app.log(("[ui] Mounting XISO: " + path).c_str());
                    auto log_fn = [&](const char* msg) { app.log(msg); };
                    if (xbox::boot_hle(app.sys, app.cfg, log_fn)) {
                        app.state = AppState::Running;
                    } else {
                        app.log("[ui] Boot failed!");
                    }
                } else {
                    app.log("[ui] No dashboard XBE available to host XISO");
                }
            }
        }

        // Run emulation steps if active
        if (app.state == AppState::Running && app.sys.running) {
            if (!xbox::run_step(app.sys, 500'000)) {
                app.state = AppState::Halted;
                char msg[128];
                snprintf(msg, sizeof(msg), "[emu] Halted: EIP=0x%08X EAX=0x%08X",
                         app.sys.exec->ctx.eip, app.sys.exec->ctx.gp[GP_EAX]);
                app.log(msg);
            }
        }

        // Render ImGui frame
        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        switch (app.state) {
        case AppState::Menu:   draw_menu(app);    break;
        case AppState::Running:
        case AppState::Halted: draw_running(app); break;
        }

        ImGui::Render();

        // Vulkan rendering
        uint32_t fi = vk.current_frame;
        vkWaitForFences(vk.device, 1, &vk.in_flight[fi], VK_TRUE, UINT64_MAX);
        vkResetFences(vk.device, 1, &vk.in_flight[fi]);

        uint32_t img_idx;
        VkResult acq = vkAcquireNextImageKHR(vk.device, vk.swapchain, UINT64_MAX,
                                              vk.image_available[fi], VK_NULL_HANDLE, &img_idx);
        if (acq == VK_ERROR_OUT_OF_DATE_KHR) {
            // Recreate swapchain on resize (simplified: skip this frame)
            vk.current_frame = (fi + 1) % vk.frame_count;
            continue;
        }

        vkResetCommandBuffer(vk.cmd_buffers[fi], 0);
        VkCommandBufferBeginInfo begin = {};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(vk.cmd_buffers[fi], &begin);

        VkClearValue clear = {{{0.1f, 0.1f, 0.12f, 1.0f}}};
        VkRenderPassBeginInfo rp_begin = {};
        rp_begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rp_begin.renderPass = vk.render_pass;
        rp_begin.framebuffer = vk.framebuffers[img_idx];
        rp_begin.renderArea.extent = vk.swapchain_extent;
        rp_begin.clearValueCount = 1;
        rp_begin.pClearValues = &clear;
        vkCmdBeginRenderPass(vk.cmd_buffers[fi], &rp_begin, VK_SUBPASS_CONTENTS_INLINE);

        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), vk.cmd_buffers[fi]);

        vkCmdEndRenderPass(vk.cmd_buffers[fi]);
        vkEndCommandBuffer(vk.cmd_buffers[fi]);

        VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo submit = {};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &vk.image_available[fi];
        submit.pWaitDstStageMask = &wait_stage;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &vk.cmd_buffers[fi];
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &vk.render_finished[fi];
        vkQueueSubmit(vk.queue, 1, &submit, vk.in_flight[fi]);

        VkPresentInfoKHR present = {};
        present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &vk.render_finished[fi];
        present.swapchainCount = 1;
        present.pSwapchains = &vk.swapchain;
        present.pImageIndices = &img_idx;
        vkQueuePresentKHR(vk.queue, &present);

        vk.current_frame = (fi + 1) % vk.frame_count;
    }

    // Cleanup
    vkDeviceWaitIdle(vk.device);
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    cleanup_vulkan(vk);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
