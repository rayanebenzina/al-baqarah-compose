#pragma once

#include <android/native_window.h>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <vector>

namespace baqarah {

class VkRenderer {
   public:
    VkRenderer() = default;
    ~VkRenderer();

    VkRenderer(const VkRenderer&) = delete;
    VkRenderer& operator=(const VkRenderer&) = delete;

    bool attachWindow(ANativeWindow* window);
    void detachWindow();
    bool drawFrame();

    // Replace the SDF atlas texture and rebuild the test geometry to a single
    // glyph-sized quad centered on the screen. Used for Phase 2b development.
    bool setGlyphSdf(const uint8_t* sdfPixels, int w, int h);

    bool valid() const { return device_ != VK_NULL_HANDLE && swapchain_ != VK_NULL_HANDLE; }

   private:
    bool initInstance();
    bool pickPhysicalDevice();
    bool createDevice();
    bool createSwapchain();
    bool createRenderPass();
    bool createFramebuffers();
    bool createCommandBuffers();
    bool createSyncObjects();
    bool createDescriptorSetLayout();
    bool createPipeline();
    bool createSampler();
    bool createSdfTexture();
    bool createVertexBuffer();
    bool createDescriptorPoolAndSet();

    void recordCommandBuffer(uint32_t imageIndex);
    void destroySwapchainResources();
    void destroyPipelineResources();
    void destroyAll();
    bool recreateSwapchain();

    uint32_t findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags props);
    bool createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags props,
                      VkBuffer& buf, VkDeviceMemory& mem);
    bool runOneShot(void (*record)(VkCommandBuffer, void*), void* user);
    bool uploadImageR8(VkImage image, uint32_t w, uint32_t h, const uint8_t* pixels);

    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memProps_{};
    VkDevice device_ = VK_NULL_HANDLE;
    uint32_t graphicsFamily_ = 0;
    VkQueue queue_ = VK_NULL_HANDLE;

    ANativeWindow* window_ = nullptr;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;

    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat scFormat_ = VK_FORMAT_UNDEFINED;
    VkExtent2D scExtent_{0, 0};
    std::vector<VkImage> scImages_;
    std::vector<VkImageView> scViews_;
    std::vector<VkFramebuffer> scFramebuffers_;

    VkRenderPass renderPass_ = VK_NULL_HANDLE;
    VkCommandPool cmdPool_ = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> cmdBuffers_;

    VkDescriptorSetLayout dsLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;

    VkImage sdfImage_ = VK_NULL_HANDLE;
    VkDeviceMemory sdfMemory_ = VK_NULL_HANDLE;
    VkImageView sdfView_ = VK_NULL_HANDLE;
    uint32_t sdfW_ = 0;
    uint32_t sdfH_ = 0;

    VkBuffer vbuf_ = VK_NULL_HANDLE;
    VkDeviceMemory vbufMem_ = VK_NULL_HANDLE;
    uint32_t vertexCount_ = 0;

    VkDescriptorPool descPool_ = VK_NULL_HANDLE;
    VkDescriptorSet descSet_ = VK_NULL_HANDLE;

    static constexpr int kFramesInFlight = 2;
    int frameSlot_ = 0;
    VkSemaphore acquireSem_[kFramesInFlight]{VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkSemaphore renderSem_[kFramesInFlight]{VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkFence inFlightFence_[kFramesInFlight]{VK_NULL_HANDLE, VK_NULL_HANDLE};

    float clearR_ = 0.02f;
    float clearG_ = 0.05f;
    float clearB_ = 0.04f;
};

}  // namespace baqarah
