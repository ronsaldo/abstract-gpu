#include <algorithm>
#include "swap_chain.hpp"
#include "texture.hpp"
#include "texture_format.hpp"
#include "framebuffer.hpp"

#ifdef __unix__
#include <X11/Xlib.h>
#include <X11/Xlib-xcb.h>
#endif

namespace AgpuVulkan
{
using namespace AgpuCommon;

inline VkPresentModeKHR mapPresentationMode(agpu_swap_chain_presentation_mode mode)
{
    switch(mode)
    {
    case AGPU_SWAP_CHAIN_PRESENTATION_MODE_FIFO_RELAXED:
    default: return VK_PRESENT_MODE_FIFO_RELAXED_KHR;

    case AGPU_SWAP_CHAIN_PRESENTATION_MODE_IMMEDIATE: return VK_PRESENT_MODE_IMMEDIATE_KHR;
    case AGPU_SWAP_CHAIN_PRESENTATION_MODE_MAILBOX: return VK_PRESENT_MODE_MAILBOX_KHR;
    case AGPU_SWAP_CHAIN_PRESENTATION_MODE_DEFAULT:
    case AGPU_SWAP_CHAIN_PRESENTATION_MODE_FIFO: return VK_PRESENT_MODE_FIFO_KHR;
    }
}
AVkSwapChain::AVkSwapChain(const agpu::device_ref &device)
    : device(device)
{
    surface = VK_NULL_HANDLE;
    handle = VK_NULL_HANDLE;
    currentBackBufferIndex = 0;
}

AVkSwapChain::~AVkSwapChain()
{
    for(auto semaphore : imageAvailableSemaphores)
    {
        if(semaphore)
            vkDestroySemaphore(deviceForVk->device, semaphore, nullptr);
    }

    for(auto semaphore : renderingFinishedSemaphores)
    {
        if(semaphore)
            vkDestroySemaphore(deviceForVk->device, semaphore, nullptr);
    }

    if (handle)
        vkDestroySwapchainKHR(deviceForVk->device, handle, nullptr);
    if(surface)
        vkDestroySurfaceKHR(deviceForVk->vulkanInstance, surface, nullptr);
}

static VkResult createXlibSurface(const agpu::device_ref &device, const agpu::command_queue_ref &graphicsCommandQueue, agpu_swap_chain_create_info *createInfo, const OverlaySwapChainWindowPtr& overlayWindow, VkSurfaceKHR *surface)
{
#ifdef __unix__
    auto displayHandle = createInfo->display;
    if(!displayHandle)
    {
        if(!deviceForVk->displayHandle)
            deviceForVk->displayHandle = XOpenDisplay(nullptr);
        displayHandle = deviceForVk->displayHandle;
    }

    if (!displayHandle || !createInfo->window)
        return VK_INCOMPLETE;

    VkXcbSurfaceCreateInfoKHR surfaceCreateInfo;
    memset(&surfaceCreateInfo, 0, sizeof(surfaceCreateInfo));
    surfaceCreateInfo.sType = VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR;
    surfaceCreateInfo.connection = XGetXCBConnection((Display*)displayHandle);
    surfaceCreateInfo.window = (xcb_window_t)(uintptr_t)createInfo->window;

    return vkCreateXcbSurfaceKHR(deviceForVk->vulkanInstance, &surfaceCreateInfo, nullptr, surface);
#else
    return VK_INCOMPLETE;
#endif
}

static VkResult createXcbSurface(const agpu::device_ref &device, const agpu::command_queue_ref &graphicsCommandQueue, agpu_swap_chain_create_info *createInfo, const OverlaySwapChainWindowPtr& overlayWindow, VkSurfaceKHR *surface)
{
#ifdef __unix__
    auto displayHandle = createInfo->display;
    if(!displayHandle)
    {
        if(!deviceForVk->displayHandle)
            deviceForVk->displayHandle = xcb_connect(nullptr, nullptr);
        displayHandle = deviceForVk->displayHandle;
    }

    if (!displayHandle || !createInfo->window)
        return VK_INCOMPLETE;

    VkXcbSurfaceCreateInfoKHR surfaceCreateInfo;
    memset(&surfaceCreateInfo, 0, sizeof(surfaceCreateInfo));
    surfaceCreateInfo.sType = VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR;
    surfaceCreateInfo.connection = (xcb_connection_t*)displayHandle;
    surfaceCreateInfo.window = (xcb_window_t)(uintptr_t)createInfo->window;

    return vkCreateXcbSurfaceKHR(deviceForVk->vulkanInstance, &surfaceCreateInfo, nullptr, surface);
#else
    return VK_INCOMPLETE;
#endif
}

static VkResult createWin32Surface(const agpu::device_ref &device, const agpu::command_queue_ref &graphicsCommandQueue, agpu_swap_chain_create_info *createInfo, const OverlaySwapChainWindowPtr &overlayWindow, VkSurfaceKHR *surface)
{
#ifdef _WIN32
    if (!createInfo->window)
        return VK_INCOMPLETE;
    VkWin32SurfaceCreateInfoKHR surfaceCreateInfo;
    memset(&surfaceCreateInfo, 0, sizeof(surfaceCreateInfo));
    surfaceCreateInfo.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    surfaceCreateInfo.hinstance = GetModuleHandle(nullptr);
    surfaceCreateInfo.hwnd = (HWND)createInfo->window;
    if(overlayWindow)
        surfaceCreateInfo.hwnd = (HWND)overlayWindow->getWindowHandle();

    return vkCreateWin32SurfaceKHR(deviceForVk->vulkanInstance, &surfaceCreateInfo, nullptr, surface);
#else
    return VK_INCOMPLETE;
#endif
}

static VkResult createDisplaySurface(const agpu::device_ref &device, const agpu::command_queue_ref &graphicsCommandQueue, agpu_swap_chain_create_info *createInfo, VkSurfaceKHR *surface)
{
    return VK_INCOMPLETE;
}

static VkResult createDefaultSurface(const agpu::device_ref &device, const agpu::command_queue_ref &graphicsCommandQueue, agpu_swap_chain_create_info *createInfo, const OverlaySwapChainWindowPtr &overlayWindow, VkSurfaceKHR *surface)
{
#if defined(_WIN32)
    return createWin32Surface(device, graphicsCommandQueue, createInfo, overlayWindow, surface);
#elif defined(__unix__)
    return createXlibSurface(device, graphicsCommandQueue, createInfo, overlayWindow, surface);
#else
#error unsupported platform
#endif
}

agpu::swap_chain_ref AVkSwapChain::create(const agpu::device_ref &device, const agpu::command_queue_ref &graphicsCommandQueue, agpu_swap_chain_create_info *createInfo)
{
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (!graphicsCommandQueue || !createInfo)
    {
        return agpu::swap_chain_ref();
    }

	OverlaySwapChainWindowPtr overlayWindow;
    VkResult error;
    if(createInfo->old_swap_chain)
    {
        auto oldSwapChainRef = agpu::swap_chain_ref::import(createInfo->old_swap_chain);
        auto avkOldSwapChain = oldSwapChainRef.as<AVkSwapChain> ();

        // Borrow the overlay window from the old swap chain.
        if((createInfo->flags & AGPU_SWAP_CHAIN_FLAG_OVERLAY_WINDOW) != 0)
        {
            overlayWindow = avkOldSwapChain->overlayWindow;
            overlayWindow->setPositionAndSize(createInfo->x, createInfo->y, createInfo->width, createInfo->height);
        }

        // Borrow the surface from the old swap chain.
        surface = avkOldSwapChain->surface;
        avkOldSwapChain->surface = VK_NULL_HANDLE;
    }
    else
    {
        if((createInfo->flags & AGPU_SWAP_CHAIN_FLAG_OVERLAY_WINDOW) != 0)
        {
            overlayWindow = createOverlaySwapChainWindow(createInfo);
            if(!overlayWindow)
                return agpu::swap_chain_ref();
        }

        error = VK_ERROR_FEATURE_NOT_PRESENT;
        if(!createInfo->window_system_name)
            error = createDefaultSurface(device, graphicsCommandQueue, createInfo, overlayWindow , &surface);
        else if(!strcmp(createInfo->window_system_name, "xlib"))
            error = createXlibSurface(device, graphicsCommandQueue, createInfo, overlayWindow, &surface);
        else if(!strcmp(createInfo->window_system_name, "xcb"))
            error = createXcbSurface(device, graphicsCommandQueue, createInfo, overlayWindow, &surface);
        else if(!strcmp(createInfo->window_system_name, "win32"))
            error = createWin32Surface(device, graphicsCommandQueue, createInfo, overlayWindow, &surface);
        else if(!strcmp(createInfo->window_system_name, "display"))
            error = createDisplaySurface(device, graphicsCommandQueue, createInfo, &surface);

        if(error || surface == VK_NULL_HANDLE)
        {
            printError("Failed to create the swap chain surface\n");
            return agpu::swap_chain_ref();
        }
    }

    auto presentationQueue = graphicsCommandQueue;
    if (!graphicsCommandQueue.as<AVkCommandQueue> ()->supportsPresentingSurface(surface))
    {
        // TODO: Find a presentation queue.
        vkDestroySurfaceKHR(deviceForVk->vulkanInstance, surface, nullptr);
        printError("Surface presentation in different queue is not yet supported.\n");
        return agpu::swap_chain_ref();
    }

    uint32_t formatCount = 0;
    error = deviceForVk->fpGetPhysicalDeviceSurfaceFormatsKHR(deviceForVk->physicalDevice, surface, &formatCount, nullptr);
    if (error)
    {
        vkDestroySurfaceKHR(deviceForVk->vulkanInstance, surface, nullptr);
        return agpu::swap_chain_ref();
    }

    std::vector<VkSurfaceFormatKHR> surfaceFormats(formatCount);
    error = deviceForVk->fpGetPhysicalDeviceSurfaceFormatsKHR(deviceForVk->physicalDevice, surface, &formatCount, &surfaceFormats[0]);
    if (error)
    {
        vkDestroySurfaceKHR(deviceForVk->vulkanInstance, surface, nullptr);
        return agpu::swap_chain_ref();
    }

    // Create the swap chain object.
    auto result = agpu::makeObject<AVkSwapChain> (device);
    auto swapChain = result.as<AVkSwapChain> ();
    swapChain->overlayWindow = overlayWindow;
    swapChain->surface = surface;
    swapChain->graphicsQueue = graphicsCommandQueue;
    swapChain->presentationQueue = presentationQueue;

    // Set the format.
    agpu_texture_format actualFormat = createInfo->colorbuffer_format;
    if (formatCount == 1 && surfaceFormats[0].format == VK_FORMAT_UNDEFINED)
    {
        swapChain->format = mapTextureFormat(createInfo->colorbuffer_format, false);
        if (swapChain->format == VK_FORMAT_UNDEFINED)
        {
            swapChain->format = VK_FORMAT_B8G8R8A8_UNORM;
            actualFormat = AGPU_TEXTURE_FORMAT_B8G8R8A8_UNORM;
        }
        swapChain->colorSpace = surfaceFormats[0].colorSpace;
    }
    else
    {
        assert(formatCount >= 1);

        // Start selecting the first format.
        swapChain->format = surfaceFormats[0].format;
        swapChain->colorSpace = surfaceFormats[0].colorSpace;
        actualFormat = AGPU_TEXTURE_FORMAT_B8G8R8A8_UNORM;
        if(swapChain->format == VK_FORMAT_B8G8R8A8_SRGB)
            actualFormat = AGPU_TEXTURE_FORMAT_B8G8R8A8_UNORM_SRGB;

        // Try to select the expected format.
        auto wantedFormat = mapTextureFormat(createInfo->colorbuffer_format, false);
        for(size_t i = 0; i < formatCount; ++i)
        {
            auto &format = surfaceFormats[i];
            if(format.format == wantedFormat)
            {
                swapChain->format = format.format;
                swapChain->colorSpace = format.colorSpace;
                actualFormat = createInfo->colorbuffer_format;
                break;
            }
        }

    }
    swapChain->agpuFormat = actualFormat;

    // Initialize the rest of the swap chain.
    if (!swapChain->initialize(createInfo))
        return agpu::swap_chain_ref();

    return result;
}

bool AVkSwapChain::initialize(agpu_swap_chain_create_info *createInfo)
{
    swapChainWidth = createInfo->width;
    swapChainHeight = createInfo->height;

    VkSurfaceCapabilitiesKHR surfaceCapabilities;
    auto error = deviceForVk->fpGetPhysicalDeviceSurfaceCapabilitiesKHR(deviceForVk->physicalDevice, surface, &surfaceCapabilities);
    if (error)
        return false;

    uint32_t presentModeCount;
    error = deviceForVk->fpGetPhysicalDeviceSurfacePresentModesKHR(deviceForVk->physicalDevice, surface, &presentModeCount, nullptr);
    if (error)
        return false;

    std::vector<VkPresentModeKHR> presentModes(presentModeCount);
    error = deviceForVk->fpGetPhysicalDeviceSurfacePresentModesKHR(deviceForVk->physicalDevice, surface, &presentModeCount, &presentModes[0]);
    if (error)
        return false;

    VkExtent2D swapchainExtent;
    if (surfaceCapabilities.currentExtent.width == (uint32_t)-1)
    {
        swapchainExtent.width = swapChainWidth;
        swapchainExtent.height = swapChainHeight;
    }
    else
    {
        swapchainExtent = surfaceCapabilities.currentExtent;
        createInfo->width = swapChainWidth = swapchainExtent.width;
        createInfo->height = swapChainHeight = swapchainExtent.height;
    }

    createInfo->layers = swapChainLayers = std::min(std::max(1u, createInfo->layers), surfaceCapabilities.maxImageArrayLayers);

    VkPresentModeKHR swapchainPresentMode = presentModes[0];
    if(createInfo->presentation_mode == AGPU_SWAP_CHAIN_PRESENTATION_MODE_DEFAULT)
        createInfo->presentation_mode = AGPU_SWAP_CHAIN_PRESENTATION_MODE_FIFO;
    if(createInfo->fallback_presentation_mode == AGPU_SWAP_CHAIN_PRESENTATION_MODE_DEFAULT)
        createInfo->fallback_presentation_mode = AGPU_SWAP_CHAIN_PRESENTATION_MODE_FIFO;

    auto presentationMode = mapPresentationMode(createInfo->presentation_mode);
    auto fallbackPresentationMode = mapPresentationMode(createInfo->fallback_presentation_mode);
    for (size_t i = 0; i < presentModeCount; i++) {
        if (presentModes[i] == presentationMode) {
            swapchainPresentMode = presentationMode;
            break;
        }

        if (presentModes[i] == fallbackPresentationMode) {
            swapchainPresentMode = fallbackPresentationMode;
        }
    }

    uint32_t desiredNumberOfSwapchainImages = std::max(surfaceCapabilities.minImageCount, createInfo->buffer_count);
    if ((surfaceCapabilities.maxImageCount > 0) &&
        (desiredNumberOfSwapchainImages > surfaceCapabilities.maxImageCount))
        desiredNumberOfSwapchainImages = surfaceCapabilities.maxImageCount;
    VkSurfaceTransformFlagsKHR preTransform;
    if (surfaceCapabilities.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) {
        preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    }
    else {
        preTransform = surfaceCapabilities.currentTransform;
    }

    VkSwapchainCreateInfoKHR swapchainInfo = {};
    swapchainInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    swapchainInfo.surface = surface;
    swapchainInfo.minImageCount = desiredNumberOfSwapchainImages;
    swapchainInfo.imageFormat = format;
    swapchainInfo.imageColorSpace = colorSpace;
    swapchainInfo.imageExtent = swapchainExtent;
    swapchainInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    swapchainInfo.preTransform = (VkSurfaceTransformFlagBitsKHR)preTransform;
    swapchainInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    swapchainInfo.imageArrayLayers = createInfo->layers;
    swapchainInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    swapchainInfo.presentMode = swapchainPresentMode;
    swapchainInfo.oldSwapchain = VK_NULL_HANDLE;
    if(createInfo->old_swap_chain)
    {
        auto oldSwapChainRef = agpu::swap_chain_ref::import(createInfo->old_swap_chain);
        auto avkOldSwapChain = oldSwapChainRef.as<AVkSwapChain> ();
        swapchainInfo.oldSwapchain = avkOldSwapChain->handle;
    }

    error = deviceForVk->fpCreateSwapchainKHR(deviceForVk->device, &swapchainInfo, nullptr, &handle);
    if (error)
        return false;

    error = deviceForVk->fpGetSwapchainImagesKHR(deviceForVk->device, handle, &imageCount, nullptr);
    if (error)
        return false;

    std::vector<VkImage> swapChainImages(imageCount);
    error = deviceForVk->fpGetSwapchainImagesKHR(deviceForVk->device, handle, &imageCount, &swapChainImages[0]);
    if (error)
        return false;

    // Color buffers descriptions
    agpu_texture_description colorDesc = {};
    colorDesc.type = AGPU_TEXTURE_2D;
    colorDesc.width = createInfo->width;
    colorDesc.height = createInfo->height;
    colorDesc.depth = 1;
    colorDesc.layers = createInfo->layers;
    colorDesc.format = agpuFormat;
    colorDesc.usage_modes = agpu_texture_usage_mode_mask(AGPU_TEXTURE_USAGE_COLOR_ATTACHMENT | AGPU_TEXTURE_USAGE_PRESENT);
    colorDesc.main_usage_mode = AGPU_TEXTURE_USAGE_PRESENT;
    colorDesc.heap_type = AGPU_MEMORY_HEAP_TYPE_DEVICE_LOCAL;
    colorDesc.miplevels = 1;
    colorDesc.sample_count = 1;

    // Depth stencil buffer descriptions.
    agpu_texture_description depthStencilDesc = {};
    depthStencilDesc.type = AGPU_TEXTURE_2D;
    depthStencilDesc.width = createInfo->width;
    depthStencilDesc.height = createInfo->height;
    depthStencilDesc.depth = 1;
    depthStencilDesc.layers = 1;
    depthStencilDesc.format = createInfo->depth_stencil_format;
    depthStencilDesc.heap_type = AGPU_MEMORY_HEAP_TYPE_DEVICE_LOCAL;
    depthStencilDesc.miplevels = 1;
    depthStencilDesc.clear_value.depth_stencil.depth = 1.0f;
    depthStencilDesc.clear_value.depth_stencil.stencil = 0;

    bool hasDepth = hasDepthComponent(createInfo->depth_stencil_format);
    bool hasStencil = hasStencilComponent(createInfo->depth_stencil_format);
    if (hasDepth)
        depthStencilDesc.usage_modes = agpu_texture_usage_mode_mask(depthStencilDesc.usage_modes | AGPU_TEXTURE_USAGE_DEPTH_ATTACHMENT);
    if (hasStencil)
        depthStencilDesc.usage_modes = agpu_texture_usage_mode_mask(depthStencilDesc.usage_modes | AGPU_TEXTURE_USAGE_STENCIL_ATTACHMENT);
    depthStencilDesc.main_usage_mode = depthStencilDesc.usage_modes;

    // Create the semaphores
    VkSemaphoreCreateInfo semaphoreInfo;
    memset(&semaphoreInfo, 0, sizeof(semaphoreInfo));
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    imageAvailableSemaphores.resize(imageCount, VK_NULL_HANDLE);
    renderingFinishedSemaphores.resize(imageCount, VK_NULL_HANDLE);
    currentSemaphoreIndex = 0;
    for (size_t i = 0; i < imageCount; ++i)
    {
        error = vkCreateSemaphore(deviceForVk->device, &semaphoreInfo, nullptr, &imageAvailableSemaphores[i]);
        if(error)
            return false;

        error = vkCreateSemaphore(deviceForVk->device, &semaphoreInfo, nullptr, &renderingFinishedSemaphores[i]);
        if(error)
            return false;
    }

    // Since the depth stencil buffer is used exclusively on the GPU, there is
    // no need to use triple buffering on it.
    agpu::texture_ref depthStencilBuffer;
    agpu::texture_view_ref depthStencilBufferView;
    if(hasDepth || hasStencil)
    {
        depthStencilBuffer = AVkTexture::create(device, &depthStencilDesc);
        if (!depthStencilBuffer)
            return false;

        // Get the depth stencil buffer view description.
        depthStencilBufferView = agpu::texture_view_ref(depthStencilBuffer->getOrCreateFullView());
    }

    VkClearColorValue clearColor = {};
    framebuffers.resize(imageCount);
    for (size_t i = 0; i < imageCount; ++i)
    {
        auto colorImage = swapChainImages[i];
        VkImageSubresourceRange range = {};
        range.layerCount = 1;
        range.levelCount = 1;
        range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        bool clearResult = false;
        deviceForVk->withSetupCommandListDo([&](AVkImplicitResourceSetupCommandList &setupList) {
            clearResult =
                setupList.setupCommandBuffer() &&
                setupList.transitionImageUsageMode(colorImage, colorDesc.usage_modes, AGPU_TEXTURE_USAGE_NONE, AGPU_TEXTURE_USAGE_COPY_DESTINATION, range) &&
                setupList.clearImageWithColor(colorImage, range, colorDesc.usage_modes, AGPU_TEXTURE_USAGE_COPY_DESTINATION, &clearColor) &&
                setupList.transitionImageUsageMode(colorImage, colorDesc.usage_modes, AGPU_TEXTURE_USAGE_COPY_DESTINATION, AGPU_TEXTURE_USAGE_PRESENT, range) &&
                setupList.submitCommandBuffer();
        });

        agpu::texture_ref colorBuffer;

        {
            colorBuffer = AVkTexture::createFromImage(device, &colorDesc, colorImage);
            if (!colorBuffer)
                return false;

            auto avkColorBuffer = colorBuffer.as<AVkTexture> ();
            avkColorBuffer->imageAspect = VK_IMAGE_ASPECT_COLOR_BIT;
        }

        agpu_texture_view_description colorBufferViewDescription;
        colorBuffer->getFullViewDescription(&colorBufferViewDescription);
        colorBufferViewDescription.subresource_range.layer_count = 1;

        framebuffers[i].resize(swapChainLayers);
        for(size_t layerIndex = 0; layerIndex < swapChainLayers; ++layerIndex)
        {
            colorBufferViewDescription.subresource_range.base_arraylayer = agpu_uint(layerIndex);
            auto colorBufferView = agpu::texture_view_ref(colorBuffer->createView(&colorBufferViewDescription));

            auto framebuffer = AVkFramebuffer::create(device, swapChainWidth, swapChainHeight, 1, &colorBufferView, depthStencilBufferView);
            auto avkFramebuffer = framebuffer.as<AVkFramebuffer> ();
            avkFramebuffer->swapChainFramebuffer = true;
            framebuffers[i][layerIndex] = framebuffer;

            // Release the references to the buffers.
            if (!framebuffer)
                return false;
        }
    }

    auto nextBackBufferError = getNextBackBufferIndex();
    return nextBackBufferError == AGPU_OK || nextBackBufferError == AGPU_OUT_OF_DATE || nextBackBufferError == AGPU_SUBOPTIMAL;
}

agpu_error AVkSwapChain::getNextBackBufferIndex()
{
    auto semaphore = imageAvailableSemaphores[currentSemaphoreIndex];
    auto error = deviceForVk->fpAcquireNextImageKHR(deviceForVk->device, handle, UINT64_MAX, semaphore, VK_NULL_HANDLE, &currentBackBufferIndex);
    agpu_error result = AGPU_OK;
    if (error == VK_ERROR_OUT_OF_DATE_KHR)
    {
        return AGPU_OUT_OF_DATE;
    }
    else if (error == VK_SUBOPTIMAL_KHR)
    {
        result = AGPU_SUBOPTIMAL;
    }
    else if(error)
    {
        CONVERT_VULKAN_ERROR(error);
    }

    // Set the semaphore in the corresponding framebuffer.
    for(size_t layerIndex = 0; layerIndex < swapChainLayers; ++layerIndex)
    {
        auto avkFramebuffer = framebuffers[currentBackBufferIndex][layerIndex].as<AVkFramebuffer> ();
        avkFramebuffer->waitSemaphore = imageAvailableSemaphores[currentSemaphoreIndex];
        avkFramebuffer->signalSemaphore = renderingFinishedSemaphores[currentSemaphoreIndex];
    }

    return result;
}

agpu_error AVkSwapChain::swapBuffers()
{
    auto semaphore = renderingFinishedSemaphores[currentSemaphoreIndex];

    // Present.
    VkPresentInfoKHR presentInfo = {};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pSwapchains = &handle;
    presentInfo.pImageIndices = &currentBackBufferIndex;
    presentInfo.pWaitSemaphores = &semaphore;
    presentInfo.swapchainCount = 1;

    auto error = deviceForVk->fpQueuePresentKHR(presentationQueue.as<AVkCommandQueue> ()->queue, &presentInfo);
    currentSemaphoreIndex = (currentSemaphoreIndex + 1) % imageAvailableSemaphores.size();

    bool isSuboptimal = false;
    if (error == VK_ERROR_OUT_OF_DATE_KHR)
    {
        return AGPU_OUT_OF_DATE;
    }
    else if (error == VK_SUBOPTIMAL_KHR)
    {
        isSuboptimal = true;
    }
    else if (error)
    {
        CONVERT_VULKAN_ERROR(error);
    }

    auto agpuError = getNextBackBufferIndex();
    if (agpuError)
        return agpuError;

    return isSuboptimal ? AGPU_SUBOPTIMAL : AGPU_OK;
}

agpu::framebuffer_ptr AVkSwapChain::getCurrentBackBuffer()
{
    return getCurrentBackBufferForLayer(0);
}

agpu::framebuffer_ptr AVkSwapChain::getCurrentBackBufferForLayer(agpu_uint layer)
{
    if(layer >= swapChainLayers)
        return nullptr;
    return framebuffers[currentBackBufferIndex][layer].disownedNewRef();
}

agpu_size AVkSwapChain::getCurrentBackBufferIndex ( )
{
    return currentBackBufferIndex;
}

agpu_size AVkSwapChain::getFramebufferCount ( )
{
    return (agpu_size)framebuffers.size();
}

agpu_size AVkSwapChain::getWidth()
{
    return swapChainWidth;
}

agpu_size AVkSwapChain::getHeight()
{
    return swapChainHeight;
}

agpu_size AVkSwapChain::getLayerCount()
{
    return swapChainLayers;
}

agpu_error AVkSwapChain::setOverlayPosition(agpu_int x, agpu_int y)
{
    if(!overlayWindow)
        return AGPU_OK;

    return overlayWindow->setPosition(x, y);
}

} // End of namespace AgpuVulkan
