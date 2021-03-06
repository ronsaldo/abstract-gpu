#ifndef AGPU_D3D12_SWAP_CHAIN_HPP
#define AGPU_D3D12_SWAP_CHAIN_HPP

#include <vector>
#include "device.hpp"
#include "../Common/overlay_window.hpp"

namespace AgpuD3D12
{

class ADXSwapChain : public agpu::swap_chain
{
public:
    ADXSwapChain(const agpu::device_ref &cdevice);
    ~ADXSwapChain();

    static agpu::swap_chain_ref create(const agpu::device_ref &device, const agpu::command_queue_ref &queue, agpu_swap_chain_create_info *createInfo);
    static agpu::swap_chain_ref createNewSwapChain(const agpu::device_ref& device, const agpu::command_queue_ref& queue, agpu_swap_chain_create_info* createInfo);
    static agpu::swap_chain_ref createResizedSwapChain(const agpu::device_ref& device, const agpu::command_queue_ref& queue, agpu_swap_chain_create_info* createInfo);

    virtual agpu_error swapBuffers() override;
    virtual agpu::framebuffer_ptr getCurrentBackBuffer() override;
    virtual agpu::framebuffer_ptr getCurrentBackBufferForLayer(agpu_uint layer) override;
    virtual agpu_size getCurrentBackBufferIndex() override;
    virtual agpu_size getFramebufferCount() override;
    virtual agpu_uint getWidth() override;
    virtual agpu_uint getHeight() override;
    virtual agpu_uint getLayerCount() override;

    virtual agpu_error setOverlayPosition(agpu_int x, agpu_int y) override;

public:
    void disconnect();
    bool createFrameBuffers(agpu_swap_chain_create_info* createInfo);

    agpu::device_ref device;

#if WINAPI_PARTITION_DESKTOP
    HWND window;
#else
    IUnknown *window;
#endif
    // Frame buffers
    ComPtr<IDXGISwapChain3> swapChain;
    std::vector<agpu::texture_ref> colorBuffers;
    std::vector<agpu::framebuffer_ref> framebuffers;

    size_t frameIndex;
    size_t frameCount;
    uint32_t windowWidth, windowHeight;
	AgpuCommon::OverlaySwapChainWindowPtr overlayWindow;
    UINT syncInterval;
};

} // End of namespace AgpuD3D12

#endif //AGPU_D3D12_VERTEX_LAYOUT_HPP
