// Vulkan renderer with the CRT shader (src/shaders/*.vert/.frag, compiled to SPIR-V at build time).
// Vulkan 1.0 only; vulkan-1.dll is loaded by SDL at run time, so the program also starts without it.
// See renderer.h for the interface.
#pragma once

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>

#include "renderer.h"

class VkRenderer final : public GpuRenderer {
public:
    ~VkRenderer() override { shutdown(); }

    bool init(SDL_Window* window, bool vsync, std::string& error) override;
    void shutdown() override;
    const char* api_name() const override { return "Vulkan"; }

    void set_bezel(const uint32_t* argb, int w, int h) override;
    void draw(const uint32_t* game_argb, int game_w, int game_h, const VideoSettings& vs, int win_w, int win_h,
              const SDL_Rect& dst, const uint32_t* overlay_argb, int overlay_w, int overlay_h,
              const SDL_Rect* bezel_dst = nullptr, bool bezel_on_top = true) override;
    bool read_pixels(std::vector<uint32_t>& out, int& w, int& h) override;
    void present() override;
    void set_vsync(bool on) override;

private:
    static constexpr int kFrames = 2;   // frames in flight

    struct Texture {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkDescriptorSet set = VK_NULL_HANDLE;
        int w = 0, h = 0;
        uint32_t mips = 1;
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    };
    struct Buffer {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        void* mapped = nullptr;
        VkDeviceSize size = 0;
    };
    struct Frame {
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        VkSemaphore acquired = VK_NULL_HANDLE;
        Texture game, overlay;
        Buffer staging;
    };

    bool create_instance(std::string& error);
    bool pick_device(std::string& error);
    bool create_device(std::string& error);
    bool create_static_objects(std::string& error);
    bool create_render_pass(VkFormat format, std::string& error);
    bool create_pipelines(std::string& error);
    bool create_swapchain(int w, int h, std::string& error);
    void destroy_swapchain();
    bool ensure_swapchain(int w, int h);

    uint32_t find_memory(uint32_t type_bits, VkMemoryPropertyFlags props) const;
    bool create_buffer(Buffer& b, VkDeviceSize size, VkBufferUsageFlags usage);
    void destroy_buffer(Buffer& b);
    bool create_texture(Texture& t, int w, int h, uint32_t mips, VkSampler sampler);
    void destroy_texture(Texture& t);
    // Copies `px` into `t` (via the frame's staging buffer at `offset`), recorded into `cmd`.
    void record_upload(VkCommandBuffer cmd, Texture& t, const Buffer& staging, VkDeviceSize offset);
    VkCommandBuffer begin_once();
    void end_once(VkCommandBuffer cmd);
    void draw_quad(VkCommandBuffer cmd, VkPipeline pipe, const Texture& t, const SDL_Rect& r, const void* push, uint32_t push_size);

    SDL_Window* window_ = nullptr;
    bool vsync_ = true;

    VkInstance instance_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice gpu_ = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties mem_props_{};
    uint32_t queue_family_ = 0;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;

    VkCommandPool cmd_pool_ = VK_NULL_HANDLE;
    VkDescriptorPool desc_pool_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipe_layout_ = VK_NULL_HANDLE;
    VkSampler sampler_linear_ = VK_NULL_HANDLE, sampler_mip_ = VK_NULL_HANDLE;
    VkShaderModule vert_ = VK_NULL_HANDLE, frag_crt_ = VK_NULL_HANDLE, frag_blit_ = VK_NULL_HANDLE;
    VkRenderPass render_pass_ = VK_NULL_HANDLE;
    VkFormat pass_format_ = VK_FORMAT_UNDEFINED;
    VkPipeline pipe_crt_ = VK_NULL_HANDLE, pipe_blit_ = VK_NULL_HANDLE;

    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat swap_format_ = VK_FORMAT_UNDEFINED;
    VkExtent2D swap_extent_{0, 0};
    bool swap_readable_ = false;           // swapchain images can be copied (read_pixels)
    int requested_w_ = 0, requested_h_ = 0; // window size the swapchain was made for
    bool need_recreate_ = false;
    std::vector<VkImage> swap_images_;
    std::vector<VkImageView> swap_views_;
    std::vector<VkFramebuffer> framebuffers_;
    std::vector<VkSemaphore> render_done_;  // one per swapchain image

    Frame frames_[kFrames];
    int cur_ = 0;
    uint32_t image_index_ = 0;
    bool frame_ready_ = false;   // draw() submitted a frame that present() has to show
    Texture bezel_;
};
