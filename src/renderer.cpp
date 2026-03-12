#include "renderer.hpp"

#include <cassert>
#include <cstdio>

#define CHECKHR(hr, msg) if (FAILED(hr)) { printf("[ERR] %s (0x%08X)\n", msg, (unsigned)hr); return; }

static void wait_for_gpu(renderer *r) {
    uint64_t val = ++r->frame_res[r->frame_index].fence_value;
    r->cmd_queue->Signal(r->fence, val);

    if (r->fence->GetCompletedValue() < val) {
        r->fence->SetEventOnCompletion(val, r->fence_event);
        WaitForSingleObject(r->fence_event, INFINITE);
    }
}

static void move_to_next_frame(renderer *r) {
    uint64_t current_fence_value = r->frame_res[r->frame_index].fence_value;
    r->cmd_queue->Signal(r->fence, current_fence_value);

    r->frame_index = r->swapchain->GetCurrentBackBufferIndex();

    // If the next frame's work is still in flight, wait for it
    if (r->fence->GetCompletedValue() < r->frame_res[r->frame_index].fence_value) {
        r->fence->SetEventOnCompletion(r->frame_res[r->frame_index].fence_value, r->fence_event);
        WaitForSingleObject(r->fence_event, INFINITE);
    }

    r->frame_res[r->frame_index].fence_value = current_fence_value + 1;
}

void render_init(renderer *r, HWND hwnd, uint32_t width, uint32_t height, std::initializer_list<render_pass*> pass_list) {
    *r = {};
    r->width = width;
    r->height = height;

    HRESULT hr;
    UINT factory_flags = 0;

#if BUILD_DEBUG
    ID3D12Debug *debug_controller = nullptr;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug_controller)))) {
        debug_controller->EnableDebugLayer();
        debug_controller->Release();
        factory_flags = DXGI_CREATE_FACTORY_DEBUG;
        printf("[INFO] D3D12 debug layer enabled\n");
    }
#endif

    // Factory
    hr = CreateDXGIFactory2(factory_flags, IID_PPV_ARGS(&r->factory));
    CHECKHR(hr, "CreateDXGIFactory2");

    // Adapter (prefer high-performance GPU)
    IDXGIAdapter1 *adapter = nullptr;
    hr = r->factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter));
    CHECKHR(hr, "EnumAdapterByGpuPreference");

    {
        DXGI_ADAPTER_DESC1 desc;
        adapter->GetDesc1(&desc);
        printf("[INFO] Adapter: %ls\n", desc.Description);
    }

    // Device
    hr = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&r->device));
    adapter->Release();
    CHECKHR(hr, "D3D12CreateDevice");

    // Verify DXR support
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 opts5 = {};
    hr = r->device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &opts5, sizeof(opts5));
    if (FAILED(hr) || opts5.RaytracingTier == D3D12_RAYTRACING_TIER_NOT_SUPPORTED) {
        printf("[ERR] DXR not supported on this device\n");
        r->device->Release(); r->device = nullptr;
        return;
    }
    printf("[INFO] DXR Tier: %d\n", opts5.RaytracingTier);

    // Command queue
    D3D12_COMMAND_QUEUE_DESC queue_desc = {};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    hr = r->device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&r->cmd_queue));
    CHECKHR(hr, "CreateCommandQueue");

    // Swap chain
    DXGI_SWAP_CHAIN_DESC1 sc_desc = {};
    sc_desc.BufferCount = FRAME_COUNT;
    sc_desc.Width = width;
    sc_desc.Height = height;
    sc_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sc_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sc_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sc_desc.SampleDesc.Count = 1;

    IDXGISwapChain1 *sc1 = nullptr;
    hr = r->factory->CreateSwapChainForHwnd(r->cmd_queue, hwnd, &sc_desc, nullptr, nullptr, &sc1);
    CHECKHR(hr, "CreateSwapChainForHwnd");

    hr = sc1->QueryInterface(IID_PPV_ARGS(&r->swapchain));
    sc1->Release();
    CHECKHR(hr, "SwapChain QueryInterface");

    r->factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    r->frame_index = r->swapchain->GetCurrentBackBufferIndex();

    // RTV descriptor heap
    D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc = {};
    rtv_heap_desc.NumDescriptors = FRAME_COUNT;
    rtv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hr = r->device->CreateDescriptorHeap(&rtv_heap_desc, IID_PPV_ARGS(&r->rtv_heap));
    CHECKHR(hr, "CreateDescriptorHeap (RTV)");

    r->rtv_descriptor_size = r->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    // Back buffer RTVs + per-frame resources
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle = r->rtv_heap->GetCPUDescriptorHandleForHeapStart();
    for (int32_t i = 0; i < FRAME_COUNT; i++) {
        frame_resources *frame = &r->frame_res[i];

        hr = r->swapchain->GetBuffer(i, IID_PPV_ARGS(&frame->back_buffer));
        CHECKHR(hr, "GetBuffer");
        r->device->CreateRenderTargetView(frame->back_buffer, nullptr, rtv_handle);
        rtv_handle.ptr += r->rtv_descriptor_size;

        hr = r->device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&frame->cmd_alloc));
        CHECKHR(hr, "CreateCommandAllocator");

        frame->fence_value = 0;
    }

    // Command list
    hr = r->device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, r->frame_res[0].cmd_alloc, nullptr, IID_PPV_ARGS(&r->cmd_list));
    CHECKHR(hr, "CreateCommandList");
    r->cmd_list->Close();

    // Fence
    hr = r->device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&r->fence));
    CHECKHR(hr, "CreateFence");
    r->fence_event = CreateEvent(nullptr, FALSE, FALSE, nullptr);

    // Register render passes
    assert(pass_list.size() <= PASS_MAX_COUNT);
    for (render_pass *pass : pass_list) {
        r->passes[r->num_passes++] = *pass;
    }

    // Init render passes
    for (int32_t i = 0; i < r->num_passes; i++) {
        r->passes[i].init(&r->passes[i], r);
    }

    printf("[OK] Renderer initialized (%ux%u, %u back buffers, %u passes)\n", width, height, FRAME_COUNT, r->num_passes);
}

void render_draw(renderer *r) {
    frame_resources *frame = &r->frame_res[r->frame_index];

    frame->cmd_alloc->Reset();
    r->cmd_list->Reset(frame->cmd_alloc, nullptr);

    // Transition back buffer: present -> render target
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = frame->back_buffer;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    r->cmd_list->ResourceBarrier(1, &barrier);

    // Clear render target to black
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = r->rtv_heap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += r->frame_index * r->rtv_descriptor_size;
    float clear_color[4] = {0.392f, 0.584f, 0.929f, 1.f};
    r->cmd_list->ClearRenderTargetView(rtv, clear_color, 0, nullptr);

    // Transition back buffer: render target -> present
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    r->cmd_list->ResourceBarrier(1, &barrier);

    r->cmd_list->Close();

    ID3D12CommandList *cmd_lists[] = {r->cmd_list};
    r->cmd_queue->ExecuteCommandLists(1, cmd_lists);

    r->swapchain->Present(1, 0);

    move_to_next_frame(r);
}

void render_stop(renderer *r) {
    wait_for_gpu(r);

    // Stop render passes (reverse order)
    for (int32_t i = r->num_passes - 1; i >= 0; i--) {
        r->passes[i].stop(&r->passes[i], r);
    }

    if (r->fence_event) CloseHandle(r->fence_event);
    if (r->fence) r->fence->Release();
    if (r->cmd_list) r->cmd_list->Release();
    for (int32_t i = 0; i < FRAME_COUNT; i++) {
        if (r->frame_res[i].cmd_alloc) r->frame_res[i].cmd_alloc->Release();
        if (r->frame_res[i].back_buffer) r->frame_res[i].back_buffer->Release();
    }
    if (r->rtv_heap) r->rtv_heap->Release();
    if (r->swapchain) r->swapchain->Release();
    if (r->cmd_queue) r->cmd_queue->Release();
    if (r->device) r->device->Release();
    if (r->factory) r->factory->Release();

    printf("[OK] Renderer stopped\n");
}
