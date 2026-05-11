#pragma once

#include <android/native_window.h>
#include <vulkan/vulkan.h>

#include <atomic>
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

    // Replace all SSBOs and the vertex buffer with N COLR layers.
    //
    //   `allCurves`     — every layer's curves concatenated, 6 floats per curve.
    //   `layerData`     — 8 floats per layer (2 vec4 in std430):
    //                     [curveOffset, curveCount, _pad, _pad, r, g, b, a]
    //   `layerRects`    — 4 floats per layer: [dstX, dstY, dstW, dstH]
    //   `bands`         — `layerCount * NUM_BANDS` ivec2s (offset, count) into
    //                     curveIndices. Each band covers a horizontal slice
    //                     of the layer's local UV (0..1).
    //   `curveIndices`  — flat int array; each entry is a curve index local
    //                     to its layer (0..layer.curveCount-1).
    //
    // One quad per layer is emitted at its rect, tagged with its layer
    // index. The fragment shader uses pixel UV.v to pick a band, then
    // iterates only the curves listed in that band.
    bool setColrGlyphs(const float* allCurves, int totalCurveCount,
                       const float* layerData, const float* layerRects,
                       int layerCount,
                       const int* bands, int bandsCount,
                       const int* curveIndices, int curveIndicesCount);

    static constexpr int kNumBands = 32;

    void setScrollY(float y) { scrollY_ = y; }

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
    bool createDescriptorPool();
    bool ensureCurveBuffer(VkDeviceSize bytes);
    bool ensureLayerBuffer(VkDeviceSize bytes);
    bool ensureBandsBuffer(VkDeviceSize bytes);
    bool ensureIndicesBuffer(VkDeviceSize bytes);
    bool ensureVertexBuffer(VkDeviceSize bytes);

    void recordCommandBuffer(uint32_t imageIndex);
    void destroySwapchainResources();
    void destroyPipelineResources();
    void destroyAll();
    bool recreateSwapchain();

    uint32_t findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags props);
    bool createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags props,
                      VkBuffer& buf, VkDeviceMemory& mem);

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

    VkBuffer curveBuffer_ = VK_NULL_HANDLE;
    VkDeviceMemory curveBufferMem_ = VK_NULL_HANDLE;
    VkDeviceSize curveBufferCapacity_ = 0;
    uint32_t curveCount_ = 0;

    VkBuffer layerBuffer_ = VK_NULL_HANDLE;
    VkDeviceMemory layerBufferMem_ = VK_NULL_HANDLE;
    VkDeviceSize layerBufferCapacity_ = 0;
    uint32_t layerCount_ = 0;

    VkBuffer bandsBuffer_ = VK_NULL_HANDLE;
    VkDeviceMemory bandsBufferMem_ = VK_NULL_HANDLE;
    VkDeviceSize bandsBufferCapacity_ = 0;

    VkBuffer indicesBuffer_ = VK_NULL_HANDLE;
    VkDeviceMemory indicesBufferMem_ = VK_NULL_HANDLE;
    VkDeviceSize indicesBufferCapacity_ = 0;

    VkBuffer vbuf_ = VK_NULL_HANDLE;
    VkDeviceMemory vbufMem_ = VK_NULL_HANDLE;
    VkDeviceSize vbufCapacity_ = 0;
    uint32_t vertexCount_ = 0;

    VkDescriptorPool descPool_ = VK_NULL_HANDLE;
    VkDescriptorSet descSet_ = VK_NULL_HANDLE;

    static constexpr int kFramesInFlight = 2;
    int frameSlot_ = 0;
    VkSemaphore acquireSem_[kFramesInFlight]{VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkSemaphore renderSem_[kFramesInFlight]{VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkFence inFlightFence_[kFramesInFlight]{VK_NULL_HANDLE, VK_NULL_HANDLE};

    // Cream Mushaf page background (sRGB ≈ #F5EAD2). Picked to read as a
    // warm parchment under the dark-brown text fallback below.
    float clearR_ = 0.961f;
    float clearG_ = 0.918f;
    float clearB_ = 0.824f;

    std::atomic<float> scrollY_{0.0f};
};

}  // namespace baqarah
