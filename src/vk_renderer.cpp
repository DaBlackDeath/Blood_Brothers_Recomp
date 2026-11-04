#include "vk_renderer.h"

#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// SPIR-V of src/shaders/*, made by glslang at build time (const uint32_t arrays)
#include "quad_vert_spv.h"
#include "crt_frag_spv.h"
#include "blit_frag_spv.h"

namespace {

// ---- Vulkan entry points: loaded at run time through SDL (no link-time dependency on vulkan-1.dll) ----
#define BB_VK_GLOBAL(X) X(vkCreateInstance) X(vkEnumerateInstanceLayerProperties)

#define BB_VK_INSTANCE(X)                                                                                         \
    X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties)                          \
    X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkGetPhysicalDeviceMemoryProperties)                           \
    X(vkGetPhysicalDeviceSurfaceSupportKHR) X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR)                         \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR) X(vkGetPhysicalDeviceSurfacePresentModesKHR)                         \
    X(vkEnumerateDeviceExtensionProperties) X(vkCreateDevice) X(vkGetDeviceProcAddr)

#define BB_VK_DEVICE(X)                                                                                           \
    X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkDeviceWaitIdle) X(vkQueueWaitIdle) X(vkQueueSubmit)               \
    X(vkQueuePresentKHR) X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR) X(vkGetSwapchainImagesKHR)             \
    X(vkAcquireNextImageKHR) X(vkCreateImageView) X(vkDestroyImageView) X(vkCreateImage) X(vkDestroyImage)        \
    X(vkGetImageMemoryRequirements) X(vkBindImageMemory) X(vkCreateBuffer) X(vkDestroyBuffer)                     \
    X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory) X(vkAllocateMemory) X(vkFreeMemory) X(vkMapMemory)     \
    X(vkUnmapMemory) X(vkCreateSampler) X(vkDestroySampler) X(vkCreateShaderModule) X(vkDestroyShaderModule)      \
    X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout) X(vkCreatePipelineLayout)                      \
    X(vkDestroyPipelineLayout) X(vkCreateDescriptorPool) X(vkDestroyDescriptorPool) X(vkAllocateDescriptorSets)   \
    X(vkFreeDescriptorSets) X(vkUpdateDescriptorSets) X(vkCreateRenderPass) X(vkDestroyRenderPass)                \
    X(vkCreateGraphicsPipelines) X(vkDestroyPipeline) X(vkCreateFramebuffer) X(vkDestroyFramebuffer)              \
    X(vkCreateCommandPool) X(vkDestroyCommandPool) X(vkAllocateCommandBuffers) X(vkFreeCommandBuffers)            \
    X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkResetCommandBuffer) X(vkCreateFence) X(vkDestroyFence)      \
    X(vkWaitForFences) X(vkResetFences) X(vkCreateSemaphore) X(vkDestroySemaphore) X(vkCmdPipelineBarrier)        \
    X(vkCmdCopyBufferToImage) X(vkCmdCopyImageToBuffer) X(vkCmdBeginRenderPass) X(vkCmdEndRenderPass)             \
    X(vkCmdBindPipeline) X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCmdBindDescriptorSets)                       \
    X(vkCmdPushConstants) X(vkCmdDraw)

#define BB_VK_DECLARE(name) PFN_##name name = nullptr;
PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;
BB_VK_GLOBAL(BB_VK_DECLARE)
BB_VK_INSTANCE(BB_VK_DECLARE)
BB_VK_DEVICE(BB_VK_DECLARE)
#undef BB_VK_DECLARE

constexpr VkFormat kTexFormat = VK_FORMAT_B8G8R8A8_UNORM;   // = 0xAARRGGBB words in memory (little endian)

// the push constants of crt.frag
struct CrtParams {
    float src_size[2], out_size[2];
    float scan, mask, curv, glow, vig, bright, sat;
    int32_t filter;
};
static_assert(sizeof(CrtParams) == 48, "CrtParams must match the push constant block in crt.frag");

std::string vk_error(const char* what, VkResult r) { return std::string(what) + " failed (VkResult " + std::to_string((int)r) + ")"; }

// 2x2 box filter for the bezel mip levels (straight alpha, like glGenerateMipmap)
std::vector<uint32_t> half_size(const std::vector<uint32_t>& src, int w, int h, int& nw, int& nh) {
    nw = std::max(1, w / 2);
    nh = std::max(1, h / 2);
    std::vector<uint32_t> dst((size_t)nw * nh);
    for (int y = 0; y < nh; y++)
        for (int x = 0; x < nw; x++) {
            const int x0 = std::min(2 * x, w - 1), x1 = std::min(2 * x + 1, w - 1);
            const int y0 = std::min(2 * y, h - 1), y1 = std::min(2 * y + 1, h - 1);
            const uint32_t p[4] = {src[(size_t)y0 * w + x0], src[(size_t)y0 * w + x1], src[(size_t)y1 * w + x0], src[(size_t)y1 * w + x1]};
            uint32_t out = 0;
            for (int sh = 0; sh < 32; sh += 8) {
                const uint32_t s = ((p[0] >> sh) & 0xFF) + ((p[1] >> sh) & 0xFF) + ((p[2] >> sh) & 0xFF) + ((p[3] >> sh) & 0xFF);
                out |= ((s + 2) / 4) << sh;
            }
            dst[(size_t)y * nw + x] = out;
        }
    return dst;
}

}  // namespace

// ---------------------------------------------------------------------------
// set-up
// ---------------------------------------------------------------------------

bool VkRenderer::init(SDL_Window* window, bool vsync, std::string& error) {
    window_ = window;
    vsync_ = vsync;
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    if (!create_instance(error) || !pick_device(error) || !create_device(error) || !create_static_objects(error) ||
        !create_swapchain(w, h, error)) {
        if (error.empty()) error = "the window has no drawable area";
        shutdown();
        return false;
    }
    return true;
}

bool VkRenderer::create_instance(std::string& error) {
    vkGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_Vulkan_GetVkGetInstanceProcAddr());
    if (!vkGetInstanceProcAddr) {
        error = std::string("no Vulkan loader: ") + SDL_GetError();
        return false;
    }
#define BB_VK_LOAD(name)                                                                                  \
    name = reinterpret_cast<PFN_##name>(vkGetInstanceProcAddr(nullptr, #name));                           \
    if (!name) { error = "Vulkan function " #name " is missing"; return false; }
    BB_VK_GLOBAL(BB_VK_LOAD)
#undef BB_VK_LOAD

    Uint32 ext_count = 0;
    const char* const* exts = SDL_Vulkan_GetInstanceExtensions(&ext_count);
    if (!exts) {
        error = std::string("no Vulkan surface extensions: ") + SDL_GetError();
        return false;
    }

    // BLOODBRO_VK_VALIDATION=1: Khronos validation layer (if installed, e.g. with the Vulkan SDK)
    std::vector<const char*> layers;
    if (const char* v = std::getenv("BLOODBRO_VK_VALIDATION"); v && *v && *v != '0') {
        uint32_t n = 0;
        vkEnumerateInstanceLayerProperties(&n, nullptr);
        std::vector<VkLayerProperties> props(n);
        vkEnumerateInstanceLayerProperties(&n, props.data());
        for (const auto& p : props)
            if (std::strcmp(p.layerName, "VK_LAYER_KHRONOS_validation") == 0) layers.push_back("VK_LAYER_KHRONOS_validation");
        std::fprintf(stderr, "vulkan: validation layer %s\n", layers.empty() ? "not installed" : "on");
    }

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "Blood Bros. recompiled";
    app.applicationVersion = 1;
    app.pEngineName = "bloodbro";
    app.apiVersion = VK_API_VERSION_1_0;
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = ext_count;
    ci.ppEnabledExtensionNames = exts;
    ci.enabledLayerCount = (uint32_t)layers.size();
    ci.ppEnabledLayerNames = layers.data();
    const VkResult r = vkCreateInstance(&ci, nullptr, &instance_);
    if (r != VK_SUCCESS) {
        error = vk_error("vkCreateInstance", r);
        return false;
    }
#define BB_VK_LOAD(name)                                                                                  \
    name = reinterpret_cast<PFN_##name>(vkGetInstanceProcAddr(instance_, #name));                         \
    if (!name) { error = "Vulkan function " #name " is missing"; return false; }
    BB_VK_INSTANCE(BB_VK_LOAD)
#undef BB_VK_LOAD

    if (!SDL_Vulkan_CreateSurface(window_, instance_, nullptr, &surface_)) {
        error = std::string("no Vulkan surface: ") + SDL_GetError();
        return false;
    }
    return true;
}

bool VkRenderer::pick_device(std::string& error) {
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(instance_, &n, nullptr);
    std::vector<VkPhysicalDevice> devs(n);
    if (n) vkEnumeratePhysicalDevices(instance_, &n, devs.data());
    int best_score = -1;
    for (VkPhysicalDevice d : devs) {
        uint32_t ne = 0;
        vkEnumerateDeviceExtensionProperties(d, nullptr, &ne, nullptr);
        std::vector<VkExtensionProperties> exts(ne);
        vkEnumerateDeviceExtensionProperties(d, nullptr, &ne, exts.data());
        bool swap = false;
        for (const auto& e : exts) swap |= std::strcmp(e.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0;
        if (!swap) continue;

        uint32_t nf = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(d, surface_, &nf, nullptr);
        if (nf == 0) continue;

        uint32_t nq = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(d, &nq, nullptr);
        std::vector<VkQueueFamilyProperties> qs(nq);
        vkGetPhysicalDeviceQueueFamilyProperties(d, &nq, qs.data());
        int family = -1;
        for (uint32_t i = 0; i < nq && family < 0; i++) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(d, i, surface_, &present);
            if ((qs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) family = (int)i;
        }
        if (family < 0) continue;

        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(d, &p);
        const int score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3 : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2 : 1;
        if (score > best_score) {
            best_score = score;
            gpu_ = d;
            queue_family_ = (uint32_t)family;
            info_ = std::string(p.deviceName) + ", Vulkan " + std::to_string(VK_API_VERSION_MAJOR(p.apiVersion)) + "." +
                    std::to_string(VK_API_VERSION_MINOR(p.apiVersion)) + "." + std::to_string(VK_API_VERSION_PATCH(p.apiVersion));
        }
    }
    if (!gpu_) {
        error = "no Vulkan device can draw into this window";
        return false;
    }
    vkGetPhysicalDeviceMemoryProperties(gpu_, &mem_props_);
    return true;
}

bool VkRenderer::create_device(std::string& error) {
    const float prio = 1.0f;
    VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueFamilyIndex = queue_family_;
    qi.queueCount = 1;
    qi.pQueuePriorities = &prio;
    const char* ext = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    VkDeviceCreateInfo ci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    ci.queueCreateInfoCount = 1;
    ci.pQueueCreateInfos = &qi;
    ci.enabledExtensionCount = 1;
    ci.ppEnabledExtensionNames = &ext;
    const VkResult r = vkCreateDevice(gpu_, &ci, nullptr, &device_);
    if (r != VK_SUCCESS) {
        error = vk_error("vkCreateDevice", r);
        return false;
    }
#define BB_VK_LOAD(name)                                                                                  \
    name = reinterpret_cast<PFN_##name>(vkGetDeviceProcAddr(device_, #name));                             \
    if (!name) { error = "Vulkan function " #name " is missing"; return false; }
    BB_VK_DEVICE(BB_VK_LOAD)
#undef BB_VK_LOAD
    vkGetDeviceQueue(device_, queue_family_, 0, &queue_);
    return true;
}

bool VkRenderer::create_static_objects(std::string& error) {
    VkResult r;
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = queue_family_;
    if ((r = vkCreateCommandPool(device_, &pci, nullptr, &cmd_pool_)) != VK_SUCCESS) { error = vk_error("vkCreateCommandPool", r); return false; }

    for (Frame& f : frames_) {
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = cmd_pool_;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        if ((r = vkAllocateCommandBuffers(device_, &ai, &f.cmd)) != VK_SUCCESS) { error = vk_error("vkAllocateCommandBuffers", r); return false; }
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        if ((r = vkCreateFence(device_, &fi, nullptr, &f.fence)) != VK_SUCCESS) { error = vk_error("vkCreateFence", r); return false; }
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        if ((r = vkCreateSemaphore(device_, &si, nullptr, &f.acquired)) != VK_SUCCESS) { error = vk_error("vkCreateSemaphore", r); return false; }
    }

    // one combined image sampler per draw; the CRT parameters are push constants
    VkDescriptorSetLayoutBinding b{};
    b.binding = 0;
    b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b.descriptorCount = 1;
    b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 1;
    li.pBindings = &b;
    if ((r = vkCreateDescriptorSetLayout(device_, &li, nullptr, &set_layout_)) != VK_SUCCESS) { error = vk_error("vkCreateDescriptorSetLayout", r); return false; }

    VkPushConstantRange pc{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(CrtParams)};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &set_layout_;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pc;
    if ((r = vkCreatePipelineLayout(device_, &pli, nullptr, &pipe_layout_)) != VK_SUCCESS) { error = vk_error("vkCreatePipelineLayout", r); return false; }

    const uint32_t kSets = 2 * kFrames + 2;   // game + overlay per frame, bezel, one spare
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kSets};
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    dpi.maxSets = kSets;
    dpi.poolSizeCount = 1;
    dpi.pPoolSizes = &ps;
    if ((r = vkCreateDescriptorPool(device_, &dpi, nullptr, &desc_pool_)) != VK_SUCCESS) { error = vk_error("vkCreateDescriptorPool", r); return false; }

    // linear filtering: the CRT shader picks its own sampling positions (sharp bilinear / linear / nearest)
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.maxLod = 0.0f;
    if ((r = vkCreateSampler(device_, &sci, nullptr, &sampler_linear_)) != VK_SUCCESS) { error = vk_error("vkCreateSampler", r); return false; }
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;   // the bezel is usually shown smaller than the image
    sci.maxLod = VK_LOD_CLAMP_NONE;
    if ((r = vkCreateSampler(device_, &sci, nullptr, &sampler_mip_)) != VK_SUCCESS) { error = vk_error("vkCreateSampler", r); return false; }

    auto module = [&](const uint32_t* code, size_t size, VkShaderModule& m) {
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = size;
        ci.pCode = code;
        const VkResult res = vkCreateShaderModule(device_, &ci, nullptr, &m);
        if (res != VK_SUCCESS) error = vk_error("vkCreateShaderModule", res);
        return res == VK_SUCCESS;
    };
    return module(kQuadVertSpv, sizeof kQuadVertSpv, vert_) && module(kCrtFragSpv, sizeof kCrtFragSpv, frag_crt_) &&
           module(kBlitFragSpv, sizeof kBlitFragSpv, frag_blit_);
}

bool VkRenderer::create_render_pass(VkFormat format, std::string& error) {
    VkAttachmentDescription a{};
    a.format = format;
    a.samples = VK_SAMPLE_COUNT_1_BIT;
    a.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    a.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    a.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;
    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo ci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    ci.attachmentCount = 1;
    ci.pAttachments = &a;
    ci.subpassCount = 1;
    ci.pSubpasses = &sub;
    ci.dependencyCount = 1;
    ci.pDependencies = &dep;
    const VkResult r = vkCreateRenderPass(device_, &ci, nullptr, &render_pass_);
    if (r != VK_SUCCESS) {
        error = vk_error("vkCreateRenderPass", r);
        return false;
    }
    pass_format_ = format;
    return true;
}

bool VkRenderer::create_pipelines(std::string& error) {
    auto make = [&](VkShaderModule frag, bool blend, VkPipeline& out) {
        VkPipelineShaderStageCreateInfo st[2]{};
        st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        st[0].module = vert_;
        st[0].pName = "main";
        st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        st[1].module = frag;
        st[1].pName = "main";
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vp.viewportCount = 1;
        vp.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = VK_CULL_MODE_NONE;
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState cb{};
        cb.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        if (blend) {
            cb.blendEnable = VK_TRUE;
            cb.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            cb.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            cb.colorBlendOp = VK_BLEND_OP_ADD;
            cb.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            cb.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            cb.alphaBlendOp = VK_BLEND_OP_ADD;
        }
        VkPipelineColorBlendStateCreateInfo cbs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cbs.attachmentCount = 1;
        cbs.pAttachments = &cb;
        const VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        ds.dynamicStateCount = 2;
        ds.pDynamicStates = dyn;
        VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        ci.stageCount = 2;
        ci.pStages = st;
        ci.pVertexInputState = &vi;
        ci.pInputAssemblyState = &ia;
        ci.pViewportState = &vp;
        ci.pRasterizationState = &rs;
        ci.pMultisampleState = &ms;
        ci.pColorBlendState = &cbs;
        ci.pDynamicState = &ds;
        ci.layout = pipe_layout_;
        ci.renderPass = render_pass_;
        ci.subpass = 0;
        const VkResult r = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &ci, nullptr, &out);
        if (r != VK_SUCCESS) error = vk_error("vkCreateGraphicsPipelines", r);
        return r == VK_SUCCESS;
    };
    return make(frag_crt_, false, pipe_crt_) && make(frag_blit_, true, pipe_blit_);
}

// ---------------------------------------------------------------------------
// swapchain
// ---------------------------------------------------------------------------

bool VkRenderer::create_swapchain(int w, int h, std::string& error) {
    VkSurfaceCapabilitiesKHR caps;
    VkResult r = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu_, surface_, &caps);
    if (r != VK_SUCCESS) {
        error = vk_error("vkGetPhysicalDeviceSurfaceCapabilitiesKHR", r);
        return false;
    }
    VkExtent2D ext = caps.currentExtent;
    if (ext.width == 0xFFFFFFFFu) {
        ext.width = (uint32_t)std::clamp<int64_t>(w, caps.minImageExtent.width, caps.maxImageExtent.width);
        ext.height = (uint32_t)std::clamp<int64_t>(h, caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    if (ext.width == 0 || ext.height == 0) return false;   // minimised: try again later
    if (device_) vkDeviceWaitIdle(device_);

    // a plain UNORM format: the shader does its own gamma, as with OpenGL
    uint32_t nf = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(gpu_, surface_, &nf, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(nf);
    vkGetPhysicalDeviceSurfaceFormatsKHR(gpu_, surface_, &nf, formats.data());
    if (formats.empty()) {
        error = "the Vulkan surface has no formats";
        return false;
    }
    VkSurfaceFormatKHR fmt = formats[0];
    for (VkFormat want : {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM}) {
        bool found = false;
        for (const auto& f : formats)
            if (f.format == want && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { fmt = f; found = true; break; }
        if (found) break;
    }
    if (fmt.format == VK_FORMAT_UNDEFINED) fmt.format = VK_FORMAT_B8G8R8A8_UNORM;

    if (fmt.format != pass_format_) {   // the render pass and the pipelines depend on the format
        if (pipe_crt_) vkDestroyPipeline(device_, pipe_crt_, nullptr);
        if (pipe_blit_) vkDestroyPipeline(device_, pipe_blit_, nullptr);
        if (render_pass_) vkDestroyRenderPass(device_, render_pass_, nullptr);
        pipe_crt_ = pipe_blit_ = VK_NULL_HANDLE;
        render_pass_ = VK_NULL_HANDLE;
        pass_format_ = VK_FORMAT_UNDEFINED;
        if (!create_render_pass(fmt.format, error) || !create_pipelines(error)) return false;
    }

    uint32_t nm = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(gpu_, surface_, &nm, nullptr);
    std::vector<VkPresentModeKHR> modes(nm);
    vkGetPhysicalDeviceSurfacePresentModesKHR(gpu_, surface_, &nm, modes.data());
    auto has_mode = [&](VkPresentModeKHR m) { return std::find(modes.begin(), modes.end(), m) != modes.end(); };
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;   // always available; = vsync
    if (!vsync_) mode = has_mode(VK_PRESENT_MODE_MAILBOX_KHR) ? VK_PRESENT_MODE_MAILBOX_KHR
                      : has_mode(VK_PRESENT_MODE_IMMEDIATE_KHR) ? VK_PRESENT_MODE_IMMEDIATE_KHR : VK_PRESENT_MODE_FIFO_KHR;

    uint32_t count = caps.minImageCount + 1;
    if (caps.maxImageCount > 0) count = std::min(count, caps.maxImageCount);
    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & alpha))
        for (VkCompositeAlphaFlagBitsKHR a : {VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR, VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR})
            if (caps.supportedCompositeAlpha & a) { alpha = a; break; }

    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = surface_;
    ci.minImageCount = count;
    ci.imageFormat = fmt.format;
    ci.imageColorSpace = fmt.colorSpace;
    ci.imageExtent = ext;
    ci.imageArrayLayers = 1;
    swap_readable_ = (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | (swap_readable_ ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0);
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = alpha;
    ci.presentMode = mode;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = swapchain_;
    VkSwapchainKHR fresh = VK_NULL_HANDLE;
    r = vkCreateSwapchainKHR(device_, &ci, nullptr, &fresh);
    destroy_swapchain();   // the old one (it was handed over as oldSwapchain)
    if (r != VK_SUCCESS) {
        error = vk_error("vkCreateSwapchainKHR", r);
        return false;
    }
    swapchain_ = fresh;
    swap_format_ = fmt.format;
    swap_extent_ = ext;

    uint32_t ni = 0;
    vkGetSwapchainImagesKHR(device_, swapchain_, &ni, nullptr);
    swap_images_.resize(ni);
    vkGetSwapchainImagesKHR(device_, swapchain_, &ni, swap_images_.data());
    swap_views_.assign(ni, VK_NULL_HANDLE);
    framebuffers_.assign(ni, VK_NULL_HANDLE);
    render_done_.assign(ni, VK_NULL_HANDLE);
    for (uint32_t i = 0; i < ni; i++) {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = swap_images_[i];
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = fmt.format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if ((r = vkCreateImageView(device_, &vi, nullptr, &swap_views_[i])) != VK_SUCCESS) { error = vk_error("vkCreateImageView", r); return false; }
        VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fi.renderPass = render_pass_;
        fi.attachmentCount = 1;
        fi.pAttachments = &swap_views_[i];
        fi.width = ext.width;
        fi.height = ext.height;
        fi.layers = 1;
        if ((r = vkCreateFramebuffer(device_, &fi, nullptr, &framebuffers_[i])) != VK_SUCCESS) { error = vk_error("vkCreateFramebuffer", r); return false; }
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        if ((r = vkCreateSemaphore(device_, &si, nullptr, &render_done_[i])) != VK_SUCCESS) { error = vk_error("vkCreateSemaphore", r); return false; }
    }
    requested_w_ = w;
    requested_h_ = h;
    need_recreate_ = false;
    return true;
}

void VkRenderer::destroy_swapchain() {
    if (!device_) return;
    for (VkFramebuffer f : framebuffers_) if (f) vkDestroyFramebuffer(device_, f, nullptr);
    for (VkImageView v : swap_views_) if (v) vkDestroyImageView(device_, v, nullptr);
    for (VkSemaphore s : render_done_) if (s) vkDestroySemaphore(device_, s, nullptr);
    framebuffers_.clear();
    swap_views_.clear();
    render_done_.clear();
    swap_images_.clear();
    if (swapchain_) vkDestroySwapchainKHR(device_, swapchain_, nullptr);
    swapchain_ = VK_NULL_HANDLE;
}

bool VkRenderer::ensure_swapchain(int w, int h) {
    if (w <= 0 || h <= 0) return false;
    if (swapchain_ && !need_recreate_ && w == requested_w_ && h == requested_h_) return true;
    std::string err;
    if (create_swapchain(w, h, err)) return true;
    if (!err.empty()) {
        static bool reported = false;
        if (!reported) std::fprintf(stderr, "vulkan: %s\n", err.c_str());
        reported = true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// buffers and textures
// ---------------------------------------------------------------------------

uint32_t VkRenderer::find_memory(uint32_t type_bits, VkMemoryPropertyFlags props) const {
    for (uint32_t i = 0; i < mem_props_.memoryTypeCount; i++)
        if ((type_bits & (1u << i)) && (mem_props_.memoryTypes[i].propertyFlags & props) == props) return i;
    return UINT32_MAX;
}

bool VkRenderer::create_buffer(Buffer& b, VkDeviceSize size, VkBufferUsageFlags usage) {
    VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    ci.size = size;
    ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(device_, &ci, nullptr, &b.buffer) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device_, b.buffer, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = find_memory(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (ai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(device_, &ai, nullptr, &b.memory) != VK_SUCCESS ||
        vkBindBufferMemory(device_, b.buffer, b.memory, 0) != VK_SUCCESS || vkMapMemory(device_, b.memory, 0, size, 0, &b.mapped) != VK_SUCCESS) {
        destroy_buffer(b);
        return false;
    }
    b.size = size;
    return true;
}

void VkRenderer::destroy_buffer(Buffer& b) {
    if (b.mapped) vkUnmapMemory(device_, b.memory);
    if (b.buffer) vkDestroyBuffer(device_, b.buffer, nullptr);
    if (b.memory) vkFreeMemory(device_, b.memory, nullptr);
    b = Buffer();
}

bool VkRenderer::create_texture(Texture& t, int w, int h, uint32_t mips, VkSampler sampler) {
    destroy_texture(t);
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = kTexFormat;
    ci.extent = {(uint32_t)w, (uint32_t)h, 1};
    ci.mipLevels = mips;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device_, &ci, nullptr, &t.image) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device_, t.image, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = find_memory(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (ai.memoryTypeIndex == UINT32_MAX) ai.memoryTypeIndex = find_memory(req.memoryTypeBits, 0);
    if (vkAllocateMemory(device_, &ai, nullptr, &t.memory) != VK_SUCCESS || vkBindImageMemory(device_, t.image, t.memory, 0) != VK_SUCCESS) {
        destroy_texture(t);
        return false;
    }
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = t.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = kTexFormat;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, 1};
    if (vkCreateImageView(device_, &vi, nullptr, &t.view) != VK_SUCCESS) {
        destroy_texture(t);
        return false;
    }
    VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dai.descriptorPool = desc_pool_;
    dai.descriptorSetCount = 1;
    dai.pSetLayouts = &set_layout_;
    if (vkAllocateDescriptorSets(device_, &dai, &t.set) != VK_SUCCESS) {
        t.set = VK_NULL_HANDLE;
        destroy_texture(t);
        return false;
    }
    VkDescriptorImageInfo ii{sampler, t.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet wr{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    wr.dstSet = t.set;
    wr.dstBinding = 0;
    wr.descriptorCount = 1;
    wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wr.pImageInfo = &ii;
    vkUpdateDescriptorSets(device_, 1, &wr, 0, nullptr);
    t.w = w;
    t.h = h;
    t.mips = mips;
    t.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    return true;
}

void VkRenderer::destroy_texture(Texture& t) {
    if (!device_) return;
    if (t.set && desc_pool_) vkFreeDescriptorSets(device_, desc_pool_, 1, &t.set);
    if (t.view) vkDestroyImageView(device_, t.view, nullptr);
    if (t.image) vkDestroyImage(device_, t.image, nullptr);
    if (t.memory) vkFreeMemory(device_, t.memory, nullptr);
    t = Texture();
}

void VkRenderer::record_upload(VkCommandBuffer cmd, Texture& t, const Buffer& staging, VkDeviceSize offset) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = t.image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    b.oldLayout = t.layout;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.srcAccessMask = t.layout == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_SHADER_READ_BIT;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, t.layout == VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    VkBufferImageCopy c{};
    c.bufferOffset = offset;
    c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.imageExtent = {(uint32_t)t.w, (uint32_t)t.h, 1};
    vkCmdCopyBufferToImage(cmd, staging.buffer, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    t.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

VkCommandBuffer VkRenderer::begin_once() {
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = cmd_pool_;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(device_, &ai, &cmd) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    return cmd;
}

void VkRenderer::end_once(VkCommandBuffer cmd) {
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vkQueueSubmit(queue_, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue_);
    vkFreeCommandBuffers(device_, cmd_pool_, 1, &cmd);
}

void VkRenderer::set_bezel(const uint32_t* argb, int w, int h) {
    if (!device_ || !argb || w <= 0 || h <= 0) return;
    vkDeviceWaitIdle(device_);
    destroy_texture(bezel_);

    // all mip levels, made on the CPU (once)
    std::vector<std::vector<uint32_t>> levels;
    std::vector<std::pair<int, int>> sizes;
    levels.emplace_back(argb, argb + (size_t)w * h);
    sizes.push_back({w, h});
    while (sizes.back().first > 1 || sizes.back().second > 1) {
        int nw, nh;
        levels.push_back(half_size(levels.back(), sizes.back().first, sizes.back().second, nw, nh));
        sizes.push_back({nw, nh});
    }
    VkDeviceSize total = 0;
    for (const auto& l : levels) total += l.size() * 4;

    Buffer staging;
    if (!create_buffer(staging, total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT)) return;
    if (!create_texture(bezel_, w, h, (uint32_t)levels.size(), sampler_mip_)) {
        std::fprintf(stderr, "vulkan: the bezel texture could not be created\n");
        destroy_buffer(staging);
        return;
    }
    std::vector<VkBufferImageCopy> copies;
    VkDeviceSize off = 0;
    for (size_t i = 0; i < levels.size(); i++) {
        std::memcpy((uint8_t*)staging.mapped + off, levels[i].data(), levels[i].size() * 4);
        VkBufferImageCopy c{};
        c.bufferOffset = off;
        c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)i, 0, 1};
        c.imageExtent = {(uint32_t)sizes[i].first, (uint32_t)sizes[i].second, 1};
        copies.push_back(c);
        off += levels[i].size() * 4;
    }
    VkCommandBuffer cmd = begin_once();
    if (cmd) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = bezel_.image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, bezel_.mips, 0, 1};
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        vkCmdCopyBufferToImage(cmd, staging.buffer, bezel_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, (uint32_t)copies.size(), copies.data());
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        end_once(cmd);
        bezel_.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    } else {
        destroy_texture(bezel_);
    }
    destroy_buffer(staging);
}

// ---------------------------------------------------------------------------
// drawing
// ---------------------------------------------------------------------------

void VkRenderer::draw_quad(VkCommandBuffer cmd, VkPipeline pipe, const Texture& t, const SDL_Rect& r, const void* push, uint32_t push_size) {
    // the scissor must lie inside the framebuffer; the viewport may stick out
    const int x0 = std::max(r.x, 0), y0 = std::max(r.y, 0);
    const int x1 = std::min(r.x + r.w, (int)swap_extent_.width), y1 = std::min(r.y + r.h, (int)swap_extent_.height);
    if (x1 <= x0 || y1 <= y0 || !t.set) return;
    const VkViewport vp{(float)r.x, (float)r.y, (float)r.w, (float)r.h, 0.0f, 1.0f};
    const VkRect2D sc{{x0, y0}, {(uint32_t)(x1 - x0), (uint32_t)(y1 - y0)}};
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe_layout_, 0, 1, &t.set, 0, nullptr);
    if (push) vkCmdPushConstants(cmd, pipe_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, push_size, push);
    vkCmdDraw(cmd, 4, 1, 0, 0);
}

void VkRenderer::draw(const uint32_t* game_argb, int gw, int gh, const VideoSettings& vs, int win_w, int win_h,
                      const SDL_Rect& dst, const uint32_t* overlay, int ow, int oh, const SDL_Rect* bezel_dst, bool bezel_on_top) {
    frame_ready_ = false;
    if (!device_ || !ensure_swapchain(win_w, win_h)) return;
    Frame& f = frames_[cur_];
    vkWaitForFences(device_, 1, &f.fence, VK_TRUE, UINT64_MAX);   // this frame's command buffer and textures are free again

    // textures and staging memory of this frame (made on first use)
    if ((f.game.w != gw || f.game.h != gh) && !create_texture(f.game, gw, gh, 1, sampler_linear_)) return;
    if (overlay && (f.overlay.w != ow || f.overlay.h != oh) && !create_texture(f.overlay, ow, oh, 1, sampler_linear_)) return;
    const VkDeviceSize game_bytes = (VkDeviceSize)gw * gh * 4;
    const VkDeviceSize need = game_bytes + (overlay ? (VkDeviceSize)ow * oh * 4 : 0);
    if (f.staging.size < need) {
        destroy_buffer(f.staging);
        if (!create_buffer(f.staging, std::max<VkDeviceSize>(need, game_bytes + (VkDeviceSize)512 * 448 * 4), VK_BUFFER_USAGE_TRANSFER_SRC_BIT)) return;
    }

    VkResult r = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, f.acquired, VK_NULL_HANDLE, &image_index_);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        need_recreate_ = true;
        return;
    }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) return;
    if (r == VK_SUBOPTIMAL_KHR) need_recreate_ = true;   // still usable for this frame
    vkResetFences(device_, 1, &f.fence);

    std::memcpy(f.staging.mapped, afterglow(game_argb, gw, gh, vs.persistence), (size_t)game_bytes);
    if (overlay) std::memcpy((uint8_t*)f.staging.mapped + game_bytes, overlay, (size_t)ow * oh * 4);

    VkCommandBuffer cmd = f.cmd;
    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    record_upload(cmd, f.game, f.staging, 0);
    if (overlay) record_upload(cmd, f.overlay, f.staging, game_bytes);

    VkClearValue clear{};
    clear.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rp.renderPass = render_pass_;
    rp.framebuffer = framebuffers_[image_index_];
    rp.renderArea = {{0, 0}, swap_extent_};
    rp.clearValueCount = 1;
    rp.pClearValues = &clear;
    vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);

    const bool bezel = bezel_dst && bezel_.set;
    if (bezel && !bezel_on_top) draw_quad(cmd, pipe_blit_, bezel_, *bezel_dst, nullptr, 0);   // a plain background picture goes first

    CrtParams p{};
    p.src_size[0] = (float)gw;
    p.src_size[1] = (float)gh;
    p.out_size[0] = (float)dst.w;
    p.out_size[1] = (float)dst.h;
    p.scan = vs.scanlines / 100.f;
    p.mask = vs.mask / 100.f;
    p.curv = vs.curvature / 100.f;
    p.glow = vs.glow / 100.f;
    p.vig = vs.vignette / 100.f;
    p.bright = vs.brightness / 100.f;
    p.sat = vs.saturation / 100.f;
    p.filter = vs.filter;
    draw_quad(cmd, pipe_crt_, f.game, dst, &p, sizeof p);

    if (bezel && bezel_on_top) draw_quad(cmd, pipe_blit_, bezel_, *bezel_dst, nullptr, 0);
    if (overlay) draw_quad(cmd, pipe_blit_, f.overlay, dst, nullptr, 0);

    vkCmdEndRenderPass(cmd);
    vkEndCommandBuffer(cmd);

    const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &f.acquired;
    si.pWaitDstStageMask = &wait_stage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &render_done_[image_index_];
    r = vkQueueSubmit(queue_, 1, &si, f.fence);
    if (r != VK_SUCCESS) {
        static bool reported = false;
        if (!reported) std::fprintf(stderr, "vulkan: %s\n", vk_error("vkQueueSubmit", r).c_str());
        reported = true;
        return;
    }
    frame_ready_ = true;
}

void VkRenderer::present() {
    if (!frame_ready_) return;
    frame_ready_ = false;
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &render_done_[image_index_];
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain_;
    pi.pImageIndices = &image_index_;
    const VkResult r = vkQueuePresentKHR(queue_, &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) need_recreate_ = true;
    cur_ = (cur_ + 1) % kFrames;
}

bool VkRenderer::read_pixels(std::vector<uint32_t>& out, int& w, int& h) {
    if (!frame_ready_ || !swap_readable_) return false;
    vkQueueWaitIdle(queue_);
    w = (int)swap_extent_.width;
    h = (int)swap_extent_.height;
    Buffer buf;
    if (!create_buffer(buf, (VkDeviceSize)w * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT)) return false;
    VkCommandBuffer cmd = begin_once();
    if (!cmd) {
        destroy_buffer(buf);
        return false;
    }
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = swap_images_[image_index_];
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    b.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    VkBufferImageCopy c{};
    c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.imageExtent = {swap_extent_.width, swap_extent_.height, 1};
    vkCmdCopyImageToBuffer(cmd, b.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf.buffer, 1, &c);
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b.dstAccessMask = 0;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    end_once(cmd);

    out.resize((size_t)w * h);
    const uint32_t* src = (const uint32_t*)buf.mapped;
    const bool rgba = swap_format_ == VK_FORMAT_R8G8B8A8_UNORM || swap_format_ == VK_FORMAT_R8G8B8A8_SRGB;
    for (size_t i = 0; i < out.size(); i++) {
        const uint32_t p = src[i];
        out[i] = 0xFF000000u | (rgba ? ((p & 0xFF) << 16) | (p & 0xFF00) | ((p >> 16) & 0xFF) : (p & 0xFFFFFF));
    }
    destroy_buffer(buf);
    return true;
}

void VkRenderer::set_vsync(bool on) {
    if (on == vsync_) return;
    vsync_ = on;
    need_recreate_ = true;
}

void VkRenderer::shutdown() {
    if (device_) {
        vkDeviceWaitIdle(device_);
        for (Frame& f : frames_) {
            destroy_texture(f.game);
            destroy_texture(f.overlay);
            destroy_buffer(f.staging);
            if (f.fence) vkDestroyFence(device_, f.fence, nullptr);
            if (f.acquired) vkDestroySemaphore(device_, f.acquired, nullptr);
            f = Frame();
        }
        destroy_texture(bezel_);
        destroy_swapchain();
        if (pipe_crt_) vkDestroyPipeline(device_, pipe_crt_, nullptr);
        if (pipe_blit_) vkDestroyPipeline(device_, pipe_blit_, nullptr);
        if (render_pass_) vkDestroyRenderPass(device_, render_pass_, nullptr);
        for (VkShaderModule m : {vert_, frag_crt_, frag_blit_}) if (m) vkDestroyShaderModule(device_, m, nullptr);
        if (sampler_linear_) vkDestroySampler(device_, sampler_linear_, nullptr);
        if (sampler_mip_) vkDestroySampler(device_, sampler_mip_, nullptr);
        if (pipe_layout_) vkDestroyPipelineLayout(device_, pipe_layout_, nullptr);
        if (set_layout_) vkDestroyDescriptorSetLayout(device_, set_layout_, nullptr);
        if (desc_pool_) vkDestroyDescriptorPool(device_, desc_pool_, nullptr);
        if (cmd_pool_) vkDestroyCommandPool(device_, cmd_pool_, nullptr);
        vkDestroyDevice(device_, nullptr);
    }
    if (surface_) SDL_Vulkan_DestroySurface(instance_, surface_, nullptr);
    if (instance_) vkDestroyInstance(instance_, nullptr);
    device_ = VK_NULL_HANDLE;
    surface_ = VK_NULL_HANDLE;
    instance_ = VK_NULL_HANDLE;
    gpu_ = VK_NULL_HANDLE;
    queue_ = VK_NULL_HANDLE;
    cmd_pool_ = VK_NULL_HANDLE;
    desc_pool_ = VK_NULL_HANDLE;
    set_layout_ = VK_NULL_HANDLE;
    pipe_layout_ = VK_NULL_HANDLE;
    sampler_linear_ = sampler_mip_ = VK_NULL_HANDLE;
    vert_ = frag_crt_ = frag_blit_ = VK_NULL_HANDLE;
    render_pass_ = VK_NULL_HANDLE;
    pass_format_ = VK_FORMAT_UNDEFINED;
    pipe_crt_ = pipe_blit_ = VK_NULL_HANDLE;
    frame_ready_ = false;
}
