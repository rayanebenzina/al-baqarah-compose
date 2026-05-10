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
    void recordCommandBuffer(uint32_t imageIndex);
    void destroySwapchainResources();
    void destroyAll();
    bool recreateSwapchain();

    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
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

    static constexpr int kFramesInFlight = 2;
    int frameSlot_ = 0;
    VkSemaphore acquireSem_[kFramesInFlight]{VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkSemaphore renderSem_[kFramesInFlight]{VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkFence inFlightFence_[kFramesInFlight]{VK_NULL_HANDLE, VK_NULL_HANDLE};

    float clearR_ = 0.05f;
    float clearG_ = 0.18f;
    float clearB_ = 0.12f;
};

}  // namespace baqarah
