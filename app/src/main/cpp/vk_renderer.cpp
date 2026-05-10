#include "vk_renderer.h"

#include <android/log.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "textured.vert.h"
#include "textured.frag.h"

#define LOG_TAG "BaqarahVk"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#define VK_CHECK(expr)                                                       \
    do {                                                                     \
        VkResult _r = (expr);                                                \
        if (_r != VK_SUCCESS) {                                              \
            LOGE("%s failed: %d (%s:%d)", #expr, (int)_r, __FILE__, __LINE__);\
            return false;                                                    \
        }                                                                    \
    } while (0)

namespace baqarah {

VkRenderer::~VkRenderer() { destroyAll(); }

bool VkRenderer::attachWindow(ANativeWindow* window) {
    if (window_ == window) return valid();
    detachWindow();
    if (!window) return false;

    window_ = window;
    ANativeWindow_acquire(window_);

    if (instance_ == VK_NULL_HANDLE) {
        if (!initInstance()) return false;
    }

    VkAndroidSurfaceCreateInfoKHR ci{};
    ci.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
    ci.window = window_;
    VK_CHECK(vkCreateAndroidSurfaceKHR(instance_, &ci, nullptr, &surface_));

    if (physical_ == VK_NULL_HANDLE) {
        if (!pickPhysicalDevice()) return false;
        if (!createDevice()) return false;
    }

    if (!createSwapchain()) return false;
    if (renderPass_ == VK_NULL_HANDLE && !createRenderPass()) return false;
    if (!createFramebuffers()) return false;
    if (cmdPool_ == VK_NULL_HANDLE && !createCommandBuffers()) return false;
    if (acquireSem_[0] == VK_NULL_HANDLE && !createSyncObjects()) return false;

    if (dsLayout_ == VK_NULL_HANDLE && !createDescriptorSetLayout()) return false;
    if (sampler_ == VK_NULL_HANDLE && !createSampler()) return false;
    if (sdfImage_ == VK_NULL_HANDLE && !createSdfTexture()) return false;
    if (vbuf_ == VK_NULL_HANDLE && !createVertexBuffer()) return false;
    if (descPool_ == VK_NULL_HANDLE && !createDescriptorPoolAndSet()) return false;
    if (pipeline_ == VK_NULL_HANDLE && !createPipeline()) return false;

    LOGI("attachWindow OK: %dx%d, %u images", scExtent_.width, scExtent_.height,
         (unsigned)scImages_.size());
    return true;
}

void VkRenderer::detachWindow() {
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
    }
    destroySwapchainResources();
    if (surface_ != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(instance_, surface_, nullptr);
        surface_ = VK_NULL_HANDLE;
    }
    if (window_ != nullptr) {
        ANativeWindow_release(window_);
        window_ = nullptr;
    }
}

bool VkRenderer::initInstance() {
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "baqarah";
    app.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    app.pEngineName = "baqarah_vk";
    app.engineVersion = VK_MAKE_VERSION(0, 1, 0);
    app.apiVersion = VK_API_VERSION_1_1;

    const char* extensions[] = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_ANDROID_SURFACE_EXTENSION_NAME,
    };

    VkInstanceCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = sizeof(extensions) / sizeof(extensions[0]);
    ci.ppEnabledExtensionNames = extensions;

    VK_CHECK(vkCreateInstance(&ci, nullptr, &instance_));
    LOGI("VkInstance created");
    return true;
}

bool VkRenderer::pickPhysicalDevice() {
    uint32_t count = 0;
    VK_CHECK(vkEnumeratePhysicalDevices(instance_, &count, nullptr));
    if (count == 0) {
        LOGE("no physical devices");
        return false;
    }
    std::vector<VkPhysicalDevice> devs(count);
    VK_CHECK(vkEnumeratePhysicalDevices(instance_, &count, devs.data()));

    for (auto pd : devs) {
        uint32_t qCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &qCount, nullptr);
        std::vector<VkQueueFamilyProperties> qprops(qCount);
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &qCount, qprops.data());
        for (uint32_t i = 0; i < qCount; ++i) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(pd, i, surface_, &present);
            if ((qprops[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
                physical_ = pd;
                graphicsFamily_ = i;
                VkPhysicalDeviceProperties props;
                vkGetPhysicalDeviceProperties(pd, &props);
                LOGI("Picked GPU: %s api=0x%x", props.deviceName, props.apiVersion);
                return true;
            }
        }
    }
    LOGE("no suitable physical device");
    return false;
}

bool VkRenderer::createDevice() {
    float priority = 1.0f;
    VkDeviceQueueCreateInfo qci{};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = graphicsFamily_;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;

    const char* deviceExts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};

    VkDeviceCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    ci.queueCreateInfoCount = 1;
    ci.pQueueCreateInfos = &qci;
    ci.enabledExtensionCount = 1;
    ci.ppEnabledExtensionNames = deviceExts;

    VK_CHECK(vkCreateDevice(physical_, &ci, nullptr, &device_));
    vkGetDeviceQueue(device_, graphicsFamily_, 0, &queue_);
    vkGetPhysicalDeviceMemoryProperties(physical_, &memProps_);
    LOGI("VkDevice created");
    return true;
}

uint32_t VkRenderer::findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags props) {
    for (uint32_t i = 0; i < memProps_.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) &&
            (memProps_.memoryTypes[i].propertyFlags & props) == props) {
            return i;
        }
    }
    LOGE("no memory type for bits=0x%x props=0x%x", typeBits, props);
    return UINT32_MAX;
}

bool VkRenderer::createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                              VkMemoryPropertyFlags props, VkBuffer& buf,
                              VkDeviceMemory& mem) {
    VkBufferCreateInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = size;
    bi.usage = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VK_CHECK(vkCreateBuffer(device_, &bi, nullptr, &buf));

    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device_, buf, &req);
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = findMemoryType(req.memoryTypeBits, props);
    if (ai.memoryTypeIndex == UINT32_MAX) return false;
    VK_CHECK(vkAllocateMemory(device_, &ai, nullptr, &mem));
    VK_CHECK(vkBindBufferMemory(device_, buf, mem, 0));
    return true;
}

bool VkRenderer::runOneShot(void (*record)(VkCommandBuffer, void*), void* user) {
    VkCommandBufferAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = cmdPool_;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cb;
    VK_CHECK(vkAllocateCommandBuffers(device_, &ai, &cb));

    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &bi);
    record(cb, user);
    vkEndCommandBuffer(cb);

    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    vkQueueSubmit(queue_, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue_);

    vkFreeCommandBuffers(device_, cmdPool_, 1, &cb);
    return true;
}

bool VkRenderer::createSwapchain() {
    VkSurfaceCapabilitiesKHR caps{};
    VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_, surface_, &caps));

    uint32_t fmtCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &fmtCount, nullptr);
    std::vector<VkSurfaceFormatKHR> fmts(fmtCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &fmtCount, fmts.data());

    VkSurfaceFormatKHR chosen = fmts[0];
    for (auto& f : fmts) {
        if (f.format == VK_FORMAT_R8G8B8A8_UNORM &&
            f.colorSpace == VK_COLORSPACE_SRGB_NONLINEAR_KHR) {
            chosen = f;
            break;
        }
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM &&
            f.colorSpace == VK_COLORSPACE_SRGB_NONLINEAR_KHR) {
            chosen = f;
        }
    }
    scFormat_ = chosen.format;
    scExtent_ = caps.currentExtent;
    if (scExtent_.width == UINT32_MAX) {
        scExtent_.width = std::max(caps.minImageExtent.width,
                                   std::min(caps.maxImageExtent.width, 1080u));
        scExtent_.height = std::max(caps.minImageExtent.height,
                                    std::min(caps.maxImageExtent.height, 2400u));
    }

    uint32_t imageCount = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount) {
        imageCount = caps.maxImageCount;
    }

    VkSwapchainCreateInfoKHR ci{};
    ci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    ci.surface = surface_;
    ci.minImageCount = imageCount;
    ci.imageFormat = chosen.format;
    ci.imageColorSpace = chosen.colorSpace;
    ci.imageExtent = scExtent_;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = VK_NULL_HANDLE;

    VK_CHECK(vkCreateSwapchainKHR(device_, &ci, nullptr, &swapchain_));

    uint32_t count = 0;
    vkGetSwapchainImagesKHR(device_, swapchain_, &count, nullptr);
    scImages_.resize(count);
    vkGetSwapchainImagesKHR(device_, swapchain_, &count, scImages_.data());

    scViews_.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
        VkImageViewCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image = scImages_[i];
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = scFormat_;
        vi.components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                         VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
        vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vi.subresourceRange.baseMipLevel = 0;
        vi.subresourceRange.levelCount = 1;
        vi.subresourceRange.baseArrayLayer = 0;
        vi.subresourceRange.layerCount = 1;
        VK_CHECK(vkCreateImageView(device_, &vi, nullptr, &scViews_[i]));
    }
    return true;
}

bool VkRenderer::createRenderPass() {
    VkAttachmentDescription color{};
    color.format = scFormat_;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &ref;

    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.srcAccessMask = 0;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    ci.attachmentCount = 1;
    ci.pAttachments = &color;
    ci.subpassCount = 1;
    ci.pSubpasses = &subpass;
    ci.dependencyCount = 1;
    ci.pDependencies = &dep;
    VK_CHECK(vkCreateRenderPass(device_, &ci, nullptr, &renderPass_));
    return true;
}

bool VkRenderer::createFramebuffers() {
    scFramebuffers_.resize(scViews_.size());
    for (size_t i = 0; i < scViews_.size(); ++i) {
        VkFramebufferCreateInfo fi{};
        fi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fi.renderPass = renderPass_;
        fi.attachmentCount = 1;
        fi.pAttachments = &scViews_[i];
        fi.width = scExtent_.width;
        fi.height = scExtent_.height;
        fi.layers = 1;
        VK_CHECK(vkCreateFramebuffer(device_, &fi, nullptr, &scFramebuffers_[i]));
    }
    return true;
}

bool VkRenderer::createCommandBuffers() {
    VkCommandPoolCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = graphicsFamily_;
    VK_CHECK(vkCreateCommandPool(device_, &pci, nullptr, &cmdPool_));

    cmdBuffers_.resize(kFramesInFlight);
    VkCommandBufferAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = cmdPool_;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = kFramesInFlight;
    VK_CHECK(vkAllocateCommandBuffers(device_, &ai, cmdBuffers_.data()));
    return true;
}

bool VkRenderer::createSyncObjects() {
    VkSemaphoreCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VkFenceCreateInfo fi{};
    fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    for (int i = 0; i < kFramesInFlight; ++i) {
        VK_CHECK(vkCreateSemaphore(device_, &si, nullptr, &acquireSem_[i]));
        VK_CHECK(vkCreateSemaphore(device_, &si, nullptr, &renderSem_[i]));
        VK_CHECK(vkCreateFence(device_, &fi, nullptr, &inFlightFence_[i]));
    }
    return true;
}

bool VkRenderer::createDescriptorSetLayout() {
    VkDescriptorSetLayoutBinding b{};
    b.binding = 0;
    b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b.descriptorCount = 1;
    b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    ci.bindingCount = 1;
    ci.pBindings = &b;
    VK_CHECK(vkCreateDescriptorSetLayout(device_, &ci, nullptr, &dsLayout_));
    return true;
}

bool VkRenderer::createSampler() {
    VkSamplerCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    ci.magFilter = VK_FILTER_LINEAR;
    ci.minFilter = VK_FILTER_LINEAR;
    ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    ci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ci.maxLod = 0.0f;
    ci.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    VK_CHECK(vkCreateSampler(device_, &ci, nullptr, &sampler_));
    return true;
}

bool VkRenderer::uploadImageR8(VkImage image, uint32_t w, uint32_t h, const uint8_t* pixels) {
    VkDeviceSize bytes = (VkDeviceSize)w * h;

    VkBuffer staging;
    VkDeviceMemory stagingMem;
    if (!createBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      staging, stagingMem)) {
        return false;
    }
    void* mapped = nullptr;
    vkMapMemory(device_, stagingMem, 0, bytes, 0, &mapped);
    std::memcpy(mapped, pixels, bytes);
    vkUnmapMemory(device_, stagingMem);

    struct Args {
        VkImage image;
        VkBuffer staging;
        uint32_t w;
        uint32_t h;
    } args{image, staging, w, h};

    runOneShot(
        [](VkCommandBuffer cb, void* u) {
            auto* a = static_cast<Args*>(u);

            VkImageMemoryBarrier toDst{};
            toDst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            toDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            toDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toDst.image = a->image;
            toDst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            toDst.srcAccessMask = 0;
            toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
                                 1, &toDst);

            VkBufferImageCopy region{};
            region.bufferOffset = 0;
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageExtent = {a->w, a->h, 1};
            vkCmdCopyBufferToImage(cb, a->staging, a->image,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

            VkImageMemoryBarrier toRead{};
            toRead.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            toRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            toRead.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toRead.image = a->image;
            toRead.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            toRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0,
                                 nullptr, 1, &toRead);
        },
        &args);

    vkDestroyBuffer(device_, staging, nullptr);
    vkFreeMemory(device_, stagingMem, nullptr);
    return true;
}

bool VkRenderer::createSdfTexture() {
    sdfW_ = 256;
    sdfH_ = 256;
    std::vector<uint8_t> pixels(sdfW_ * sdfH_);
    const float cx = sdfW_ * 0.5f;
    const float cy = sdfH_ * 0.5f;
    const float r = 90.0f;
    const float spread = 30.0f;
    for (uint32_t y = 0; y < sdfH_; ++y) {
        for (uint32_t x = 0; x < sdfW_; ++x) {
            float dx = (float)x - cx;
            float dy = (float)y - cy;
            float dist = std::sqrt(dx * dx + dy * dy);
            float signedDist = r - dist;
            float normalized = 0.5f + (signedDist / spread) * 0.5f;
            normalized = std::max(0.0f, std::min(1.0f, normalized));
            pixels[y * sdfW_ + x] = (uint8_t)(normalized * 255.0f);
        }
    }

    VkImageCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = VK_FORMAT_R8_UNORM;
    ci.extent = {sdfW_, sdfH_, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VK_CHECK(vkCreateImage(device_, &ci, nullptr, &sdfImage_));

    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device_, sdfImage_, &req);
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (ai.memoryTypeIndex == UINT32_MAX) return false;
    VK_CHECK(vkAllocateMemory(device_, &ai, nullptr, &sdfMemory_));
    VK_CHECK(vkBindImageMemory(device_, sdfImage_, sdfMemory_, 0));

    if (!uploadImageR8(sdfImage_, sdfW_, sdfH_, pixels.data())) return false;

    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = sdfImage_;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = VK_FORMAT_R8_UNORM;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VK_CHECK(vkCreateImageView(device_, &vi, nullptr, &sdfView_));
    LOGI("SDF texture uploaded %ux%u", sdfW_, sdfH_);
    return true;
}

bool VkRenderer::createVertexBuffer() {
    struct V {
        float x, y, u, v;
    };

    auto makeQuad = [](std::vector<V>& out, float cx, float cy, float half) {
        float x0 = cx - half, x1 = cx + half;
        float y0 = cy - half, y1 = cy + half;
        out.push_back({x0, y0, 0.0f, 0.0f});
        out.push_back({x1, y0, 1.0f, 0.0f});
        out.push_back({x0, y1, 0.0f, 1.0f});
        out.push_back({x1, y0, 1.0f, 0.0f});
        out.push_back({x1, y1, 1.0f, 1.0f});
        out.push_back({x0, y1, 0.0f, 1.0f});
    };

    std::vector<V> verts;
    const float w = (float)scExtent_.width;
    const float h = (float)scExtent_.height;
    const float size = std::min(w, h) * 0.18f;
    const float cxL = w * 0.30f;
    const float cxR = w * 0.70f;
    const float cyT = h * 0.35f;
    const float cyB = h * 0.65f;
    makeQuad(verts, cxL, cyT, size);
    makeQuad(verts, cxR, cyT, size);
    makeQuad(verts, cxL, cyB, size);
    makeQuad(verts, cxR, cyB, size);

    vertexCount_ = (uint32_t)verts.size();
    VkDeviceSize bytes = vertexCount_ * sizeof(V);

    if (!createBuffer(bytes,
                      VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      vbuf_, vbufMem_)) {
        return false;
    }
    void* mapped = nullptr;
    vkMapMemory(device_, vbufMem_, 0, bytes, 0, &mapped);
    std::memcpy(mapped, verts.data(), bytes);
    vkUnmapMemory(device_, vbufMem_);
    LOGI("vertex buffer: %u verts (%llu bytes)", vertexCount_,
         (unsigned long long)bytes);
    return true;
}

bool VkRenderer::createDescriptorPoolAndSet() {
    VkDescriptorPoolSize ps{};
    ps.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    ps.descriptorCount = 1;

    VkDescriptorPoolCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pci.maxSets = 1;
    pci.poolSizeCount = 1;
    pci.pPoolSizes = &ps;
    VK_CHECK(vkCreateDescriptorPool(device_, &pci, nullptr, &descPool_));

    VkDescriptorSetAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = descPool_;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &dsLayout_;
    VK_CHECK(vkAllocateDescriptorSets(device_, &ai, &descSet_));

    VkDescriptorImageInfo ii{};
    ii.sampler = sampler_;
    ii.imageView = sdfView_;
    ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet w{};
    w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w.dstSet = descSet_;
    w.dstBinding = 0;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &ii;
    vkUpdateDescriptorSets(device_, 1, &w, 0, nullptr);
    return true;
}

bool VkRenderer::createPipeline() {
    auto makeModule = [&](const uint32_t* code, size_t bytes, VkShaderModule& out) -> bool {
        VkShaderModuleCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        ci.codeSize = bytes;
        ci.pCode = code;
        VK_CHECK(vkCreateShaderModule(device_, &ci, nullptr, &out));
        return true;
    };

    VkShaderModule vmod = VK_NULL_HANDLE;
    VkShaderModule fmod = VK_NULL_HANDLE;
    if (!makeModule(kTexturedVertSpv, kTexturedVertSpv_SIZE, vmod)) return false;
    if (!makeModule(kTexturedFragSpv, kTexturedFragSpv_SIZE, fmod)) return false;

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vmod;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fmod;
    stages[1].pName = "main";

    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = sizeof(float) * 4;
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attrs[2]{};
    attrs[0].location = 0;
    attrs[0].binding = 0;
    attrs[0].format = VK_FORMAT_R32G32_SFLOAT;
    attrs[0].offset = 0;
    attrs[1].location = 1;
    attrs[1].binding = 0;
    attrs[1].format = VK_FORMAT_R32G32_SFLOAT;
    attrs[1].offset = sizeof(float) * 2;

    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &binding;
    vi.vertexAttributeDescriptionCount = 2;
    vi.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp{};
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    cba.blendEnable = VK_TRUE;
    cba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cba.colorBlendOp = VK_BLEND_OP_ADD;
    cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    cba.alphaBlendOp = VK_BLEND_OP_ADD;

    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;

    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds{};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    ds.dynamicStateCount = 2;
    ds.pDynamicStates = dyn;

    VkPushConstantRange pc{};
    pc.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pc.offset = 0;
    pc.size = sizeof(float) * 4;  // viewport(2) + scrollY + padding

    VkPipelineLayoutCreateInfo pli{};
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &dsLayout_;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pc;
    VK_CHECK(vkCreatePipelineLayout(device_, &pli, nullptr, &pipelineLayout_));

    VkGraphicsPipelineCreateInfo gpi{};
    gpi.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gpi.stageCount = 2;
    gpi.pStages = stages;
    gpi.pVertexInputState = &vi;
    gpi.pInputAssemblyState = &ia;
    gpi.pViewportState = &vp;
    gpi.pRasterizationState = &rs;
    gpi.pMultisampleState = &ms;
    gpi.pColorBlendState = &cb;
    gpi.pDynamicState = &ds;
    gpi.layout = pipelineLayout_;
    gpi.renderPass = renderPass_;
    gpi.subpass = 0;
    VK_CHECK(vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &gpi, nullptr, &pipeline_));

    vkDestroyShaderModule(device_, vmod, nullptr);
    vkDestroyShaderModule(device_, fmod, nullptr);
    LOGI("Graphics pipeline created");
    return true;
}

bool VkRenderer::setGlyphSdf(const uint8_t* sdfPixels, int w, int h) {
    if (!valid()) return false;
    vkDeviceWaitIdle(device_);

    // Tear down existing SDF texture
    if (sdfView_ != VK_NULL_HANDLE) {
        vkDestroyImageView(device_, sdfView_, nullptr);
        sdfView_ = VK_NULL_HANDLE;
    }
    if (sdfImage_ != VK_NULL_HANDLE) {
        vkDestroyImage(device_, sdfImage_, nullptr);
        sdfImage_ = VK_NULL_HANDLE;
    }
    if (sdfMemory_ != VK_NULL_HANDLE) {
        vkFreeMemory(device_, sdfMemory_, nullptr);
        sdfMemory_ = VK_NULL_HANDLE;
    }

    sdfW_ = (uint32_t)w;
    sdfH_ = (uint32_t)h;

    VkImageCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = VK_FORMAT_R8_UNORM;
    ci.extent = {sdfW_, sdfH_, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VK_CHECK(vkCreateImage(device_, &ci, nullptr, &sdfImage_));

    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device_, sdfImage_, &req);
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (ai.memoryTypeIndex == UINT32_MAX) return false;
    VK_CHECK(vkAllocateMemory(device_, &ai, nullptr, &sdfMemory_));
    VK_CHECK(vkBindImageMemory(device_, sdfImage_, sdfMemory_, 0));

    if (!uploadImageR8(sdfImage_, sdfW_, sdfH_, sdfPixels)) return false;

    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = sdfImage_;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = VK_FORMAT_R8_UNORM;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VK_CHECK(vkCreateImageView(device_, &vi, nullptr, &sdfView_));

    // Re-bind into the existing descriptor set
    VkDescriptorImageInfo ii{};
    ii.sampler = sampler_;
    ii.imageView = sdfView_;
    ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet wr{};
    wr.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wr.dstSet = descSet_;
    wr.dstBinding = 0;
    wr.descriptorCount = 1;
    wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wr.pImageInfo = &ii;
    vkUpdateDescriptorSets(device_, 1, &wr, 0, nullptr);

    // Rebuild vertex buffer as one centered quad sized 2x the bitmap (display zoom).
    struct V { float x, y, u, v; };
    const float scale = 2.0f;
    const float dstW = (float)w * scale;
    const float dstH = (float)h * scale;
    const float cx = (float)scExtent_.width * 0.5f;
    const float cy = (float)scExtent_.height * 0.5f;
    const float x0 = cx - dstW * 0.5f, x1 = cx + dstW * 0.5f;
    const float y0 = cy - dstH * 0.5f, y1 = cy + dstH * 0.5f;
    V verts[6] = {
        {x0, y0, 0.0f, 0.0f}, {x1, y0, 1.0f, 0.0f}, {x0, y1, 0.0f, 1.0f},
        {x1, y0, 1.0f, 0.0f}, {x1, y1, 1.0f, 1.0f}, {x0, y1, 0.0f, 1.0f},
    };

    if (vbuf_ != VK_NULL_HANDLE) {
        vkDestroyBuffer(device_, vbuf_, nullptr);
        vbuf_ = VK_NULL_HANDLE;
    }
    if (vbufMem_ != VK_NULL_HANDLE) {
        vkFreeMemory(device_, vbufMem_, nullptr);
        vbufMem_ = VK_NULL_HANDLE;
    }
    if (!createBuffer(sizeof(verts),
                      VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      vbuf_, vbufMem_)) {
        return false;
    }
    void* mapped = nullptr;
    vkMapMemory(device_, vbufMem_, 0, sizeof(verts), 0, &mapped);
    std::memcpy(mapped, verts, sizeof(verts));
    vkUnmapMemory(device_, vbufMem_);
    vertexCount_ = 6;
    LOGI("setGlyphSdf: %dx%d -> quad %.0fx%.0f at (%.0f,%.0f)", w, h, dstW, dstH, cx, cy);
    return true;
}

bool VkRenderer::setAyahAtlas(const uint8_t* sdfPixels, int w, int h,
                              const float* quads, int quadCount) {
    if (!valid()) return false;
    vkDeviceWaitIdle(device_);

    // Replace the SDF texture.
    if (sdfView_ != VK_NULL_HANDLE) {
        vkDestroyImageView(device_, sdfView_, nullptr);
        sdfView_ = VK_NULL_HANDLE;
    }
    if (sdfImage_ != VK_NULL_HANDLE) {
        vkDestroyImage(device_, sdfImage_, nullptr);
        sdfImage_ = VK_NULL_HANDLE;
    }
    if (sdfMemory_ != VK_NULL_HANDLE) {
        vkFreeMemory(device_, sdfMemory_, nullptr);
        sdfMemory_ = VK_NULL_HANDLE;
    }
    sdfW_ = (uint32_t)w;
    sdfH_ = (uint32_t)h;

    VkImageCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = VK_FORMAT_R8_UNORM;
    ci.extent = {sdfW_, sdfH_, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VK_CHECK(vkCreateImage(device_, &ci, nullptr, &sdfImage_));

    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device_, sdfImage_, &req);
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (ai.memoryTypeIndex == UINT32_MAX) return false;
    VK_CHECK(vkAllocateMemory(device_, &ai, nullptr, &sdfMemory_));
    VK_CHECK(vkBindImageMemory(device_, sdfImage_, sdfMemory_, 0));
    if (!uploadImageR8(sdfImage_, sdfW_, sdfH_, sdfPixels)) return false;

    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = sdfImage_;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = VK_FORMAT_R8_UNORM;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VK_CHECK(vkCreateImageView(device_, &vi, nullptr, &sdfView_));

    VkDescriptorImageInfo ii{};
    ii.sampler = sampler_;
    ii.imageView = sdfView_;
    ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet wr{};
    wr.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wr.dstSet = descSet_;
    wr.dstBinding = 0;
    wr.descriptorCount = 1;
    wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wr.pImageInfo = &ii;
    vkUpdateDescriptorSets(device_, 1, &wr, 0, nullptr);

    // Expand quads into a 6-vert triangle list.
    struct V { float x, y, u, v; };
    std::vector<V> verts;
    verts.reserve((size_t)quadCount * 6);
    for (int i = 0; i < quadCount; ++i) {
        const float* q = &quads[i * 8];
        float x0 = q[0], y0 = q[1];
        float x1 = q[0] + q[2], y1 = q[1] + q[3];
        float u0 = q[4], v0 = q[5];
        float u1 = q[6], v1 = q[7];
        verts.push_back({x0, y0, u0, v0});
        verts.push_back({x1, y0, u1, v0});
        verts.push_back({x0, y1, u0, v1});
        verts.push_back({x1, y0, u1, v0});
        verts.push_back({x1, y1, u1, v1});
        verts.push_back({x0, y1, u0, v1});
    }

    if (vbuf_ != VK_NULL_HANDLE) {
        vkDestroyBuffer(device_, vbuf_, nullptr);
        vbuf_ = VK_NULL_HANDLE;
    }
    if (vbufMem_ != VK_NULL_HANDLE) {
        vkFreeMemory(device_, vbufMem_, nullptr);
        vbufMem_ = VK_NULL_HANDLE;
    }

    VkDeviceSize bytes = verts.size() * sizeof(V);
    if (bytes == 0) {
        vertexCount_ = 0;
        LOGI("setAyahAtlas: 0 quads — nothing to draw");
        return true;
    }
    if (!createBuffer(bytes,
                      VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      vbuf_, vbufMem_)) {
        return false;
    }
    void* mapped = nullptr;
    vkMapMemory(device_, vbufMem_, 0, bytes, 0, &mapped);
    std::memcpy(mapped, verts.data(), bytes);
    vkUnmapMemory(device_, vbufMem_);
    vertexCount_ = (uint32_t)verts.size();
    LOGI("setAyahAtlas: atlas=%dx%d quads=%d verts=%u", w, h, quadCount, vertexCount_);
    return true;
}

void VkRenderer::recordCommandBuffer(uint32_t imageIndex) {
    VkCommandBuffer cb = cmdBuffers_[frameSlot_];
    vkResetCommandBuffer(cb, 0);

    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    vkBeginCommandBuffer(cb, &bi);

    VkClearValue clear{};
    clear.color.float32[0] = clearR_;
    clear.color.float32[1] = clearG_;
    clear.color.float32[2] = clearB_;
    clear.color.float32[3] = 1.0f;

    VkRenderPassBeginInfo rb{};
    rb.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rb.renderPass = renderPass_;
    rb.framebuffer = scFramebuffers_[imageIndex];
    rb.renderArea.extent = scExtent_;
    rb.clearValueCount = 1;
    rb.pClearValues = &clear;
    vkCmdBeginRenderPass(cb, &rb, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport vp{};
    vp.x = 0.0f;
    vp.y = 0.0f;
    vp.width = (float)scExtent_.width;
    vp.height = (float)scExtent_.height;
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(cb, 0, 1, &vp);

    VkRect2D sc{};
    sc.extent = scExtent_;
    vkCmdSetScissor(cb, 0, 1, &sc);

    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 1,
                            &descSet_, 0, nullptr);

    float pc[4] = {
        (float)scExtent_.width,
        (float)scExtent_.height,
        scrollY_.load(std::memory_order_relaxed),
        0.0f,
    };
    vkCmdPushConstants(cb, pipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), pc);

    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cb, 0, 1, &vbuf_, &offset);

    vkCmdDraw(cb, vertexCount_, 1, 0, 0);

    vkCmdEndRenderPass(cb);
    vkEndCommandBuffer(cb);
}

bool VkRenderer::drawFrame() {
    if (!valid()) return false;

    vkWaitForFences(device_, 1, &inFlightFence_[frameSlot_], VK_TRUE, UINT64_MAX);

    uint32_t imageIndex = 0;
    VkResult r = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX,
                                       acquireSem_[frameSlot_], VK_NULL_HANDLE, &imageIndex);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        return recreateSwapchain();
    }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
        LOGE("acquire failed: %d", (int)r);
        return false;
    }

    vkResetFences(device_, 1, &inFlightFence_[frameSlot_]);
    recordCommandBuffer(imageIndex);

    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &acquireSem_[frameSlot_];
    si.pWaitDstStageMask = &waitStage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmdBuffers_[frameSlot_];
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &renderSem_[frameSlot_];
    VK_CHECK(vkQueueSubmit(queue_, 1, &si, inFlightFence_[frameSlot_]));

    VkPresentInfoKHR pi{};
    pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &renderSem_[frameSlot_];
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain_;
    pi.pImageIndices = &imageIndex;
    VkResult pr = vkQueuePresentKHR(queue_, &pi);
    if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR) {
        recreateSwapchain();
    } else if (pr != VK_SUCCESS) {
        LOGE("present failed: %d", (int)pr);
    }

    frameSlot_ = (frameSlot_ + 1) % kFramesInFlight;
    return true;
}

bool VkRenderer::recreateSwapchain() {
    if (device_ == VK_NULL_HANDLE) return false;
    vkDeviceWaitIdle(device_);
    for (auto fb : scFramebuffers_) vkDestroyFramebuffer(device_, fb, nullptr);
    for (auto v : scViews_) vkDestroyImageView(device_, v, nullptr);
    scFramebuffers_.clear();
    scViews_.clear();
    if (swapchain_ != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(device_, swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
    }
    if (!createSwapchain()) return false;
    if (!createFramebuffers()) return false;
    return true;
}

void VkRenderer::destroySwapchainResources() {
    if (device_ == VK_NULL_HANDLE) return;
    for (auto fb : scFramebuffers_) vkDestroyFramebuffer(device_, fb, nullptr);
    for (auto v : scViews_) vkDestroyImageView(device_, v, nullptr);
    scFramebuffers_.clear();
    scViews_.clear();
    scImages_.clear();
    if (swapchain_ != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(device_, swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
    }
}

void VkRenderer::destroyPipelineResources() {
    if (device_ == VK_NULL_HANDLE) return;
    if (pipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(device_, pipeline_, nullptr);
        pipeline_ = VK_NULL_HANDLE;
    }
    if (pipelineLayout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr);
        pipelineLayout_ = VK_NULL_HANDLE;
    }
    if (descPool_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device_, descPool_, nullptr);
        descPool_ = VK_NULL_HANDLE;
        descSet_ = VK_NULL_HANDLE;
    }
    if (vbuf_ != VK_NULL_HANDLE) {
        vkDestroyBuffer(device_, vbuf_, nullptr);
        vbuf_ = VK_NULL_HANDLE;
    }
    if (vbufMem_ != VK_NULL_HANDLE) {
        vkFreeMemory(device_, vbufMem_, nullptr);
        vbufMem_ = VK_NULL_HANDLE;
    }
    if (sdfView_ != VK_NULL_HANDLE) {
        vkDestroyImageView(device_, sdfView_, nullptr);
        sdfView_ = VK_NULL_HANDLE;
    }
    if (sdfImage_ != VK_NULL_HANDLE) {
        vkDestroyImage(device_, sdfImage_, nullptr);
        sdfImage_ = VK_NULL_HANDLE;
    }
    if (sdfMemory_ != VK_NULL_HANDLE) {
        vkFreeMemory(device_, sdfMemory_, nullptr);
        sdfMemory_ = VK_NULL_HANDLE;
    }
    if (sampler_ != VK_NULL_HANDLE) {
        vkDestroySampler(device_, sampler_, nullptr);
        sampler_ = VK_NULL_HANDLE;
    }
    if (dsLayout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device_, dsLayout_, nullptr);
        dsLayout_ = VK_NULL_HANDLE;
    }
}

void VkRenderer::destroyAll() {
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
    }
    destroyPipelineResources();
    destroySwapchainResources();
    if (surface_ != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(instance_, surface_, nullptr);
        surface_ = VK_NULL_HANDLE;
    }
    if (device_ != VK_NULL_HANDLE) {
        for (int i = 0; i < kFramesInFlight; ++i) {
            if (acquireSem_[i] != VK_NULL_HANDLE) vkDestroySemaphore(device_, acquireSem_[i], nullptr);
            if (renderSem_[i] != VK_NULL_HANDLE) vkDestroySemaphore(device_, renderSem_[i], nullptr);
            if (inFlightFence_[i] != VK_NULL_HANDLE) vkDestroyFence(device_, inFlightFence_[i], nullptr);
            acquireSem_[i] = renderSem_[i] = VK_NULL_HANDLE;
            inFlightFence_[i] = VK_NULL_HANDLE;
        }
        if (cmdPool_ != VK_NULL_HANDLE) {
            vkDestroyCommandPool(device_, cmdPool_, nullptr);
            cmdPool_ = VK_NULL_HANDLE;
        }
        if (renderPass_ != VK_NULL_HANDLE) {
            vkDestroyRenderPass(device_, renderPass_, nullptr);
            renderPass_ = VK_NULL_HANDLE;
        }
        vkDestroyDevice(device_, nullptr);
        device_ = VK_NULL_HANDLE;
    }
    if (window_ != nullptr) {
        ANativeWindow_release(window_);
        window_ = nullptr;
    }
    if (instance_ != VK_NULL_HANDLE) {
        vkDestroyInstance(instance_, nullptr);
        instance_ = VK_NULL_HANDLE;
    }
}

}  // namespace baqarah
