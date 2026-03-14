#include "bn_renderer.hpp"
#include "bn_shaders.hpp"

#include <cassert>
#include <cstdio>

#define RESOURCE_VIEWS_MAX_COUNT 128

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

    // Main bindless heap
    D3D12_DESCRIPTOR_HEAP_DESC main_heap_desc = {};
    main_heap_desc.NumDescriptors = RESOURCE_VIEWS_MAX_COUNT;
    main_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    main_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    main_heap_desc.NodeMask = 0;
    hr = r->device->CreateDescriptorHeap(&main_heap_desc, IID_PPV_ARGS(&r->main_heap));
    CHECKHR(hr, "CreateDescriptorHeap (MAIN - Bindless)");

    r->main_descriptor_size = r->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    // Command list
    hr = r->device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, r->frame_res[0].cmd_alloc, nullptr, IID_PPV_ARGS(&r->cmd_list));
    CHECKHR(hr, "CreateCommandList");
    r->cmd_list->Close();

    // Fence
    hr = r->device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&r->fence));
    CHECKHR(hr, "CreateFence");
    r->fence_event = CreateEvent(nullptr, FALSE, FALSE, nullptr);

    // Root signature
    D3D12_ROOT_PARAMETER1 param[1];
    param[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    param[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    param[0].Constants.ShaderRegister = 0;
    param[0].Constants.RegisterSpace = 0;
    param[0].Constants.Num32BitValues = 4;

    ID3DBlob *sig_blob;
    ID3DBlob *err_blob;
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC root_sig_desc = {};
    root_sig_desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_2;
    root_sig_desc.Desc_1_2.Flags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED/* | D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED*/;
    root_sig_desc.Desc_1_2.NumParameters = 1;
    root_sig_desc.Desc_1_2.pParameters = param;

    hr = D3D12SerializeVersionedRootSignature(&root_sig_desc, &sig_blob, &err_blob);
    if (err_blob != nullptr) {
        printf("[RENDER] RootSignature ERR: '%s'\n", (char *)err_blob->GetBufferPointer());
        assert(false);
    }
    hr = r->device->CreateRootSignature(0, sig_blob->GetBufferPointer(), sig_blob->GetBufferSize(), IID_PPV_ARGS(&r->root_sig));

    // Pipeline state object

    D3D12_STATE_SUBOBJECT sub_objects[2];
    D3D12_GLOBAL_ROOT_SIGNATURE global_rs;
    global_rs.pGlobalRootSignature = r->root_sig;
    sub_objects[0].Type = D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE;
    sub_objects[0].pDesc = &global_rs;

    D3D12_RAYTRACING_PIPELINE_CONFIG1 rt_config = {};
    rt_config.MaxTraceRecursionDepth = 5;
    rt_config.Flags = D3D12_RAYTRACING_PIPELINE_FLAG_NONE;
    sub_objects[1].Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG1;
    sub_objects[1].pDesc = &rt_config;

    D3D12_STATE_OBJECT_DESC state_desc = {};
    state_desc.Type = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE;
    state_desc.NumSubobjects = 2;
    state_desc.pSubobjects = sub_objects;
    hr = r->device->CreateStateObject(&state_desc, IID_PPV_ARGS(&r->rt_state));

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

void render_draw(renderer *r, render_scene *scene) {
    frame_resources *frame = &r->frame_res[r->frame_index];

    frame->cmd_alloc->Reset();
    r->cmd_list->Reset(frame->cmd_alloc, nullptr);
    r->cmd_list->SetDescriptorHeaps(1, &r->main_heap);

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

    r->cmd_list->SetGraphicsRootSignature(r->root_sig);
    r->cmd_list->SetPipelineState1(r->rt_state);

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

    shaders_release();

    if (r->fence_event) CloseHandle(r->fence_event);
    if (r->fence) r->fence->Release();
    if (r->cmd_list) r->cmd_list->Release();
    for (int32_t i = 0; i < FRAME_COUNT; i++) {
        if (r->frame_res[i].cmd_alloc) r->frame_res[i].cmd_alloc->Release();
        if (r->frame_res[i].back_buffer) r->frame_res[i].back_buffer->Release();
        if (r->frame_res[i].scene_cb) r->frame_res[i].scene_cb->Release();
        if (r->frame_res[i].rt_shader_table) r->frame_res[i].rt_shader_table->Release();
        if (r->frame_res[i].rt_blas) r->frame_res[i].rt_blas->Release();
        if (r->frame_res[i].rt_tlas) r->frame_res[i].rt_tlas->Release();
        if (r->frame_res[i].rt_scratch) r->frame_res[i].rt_scratch->Release();
        if (r->frame_res[i].rt_out) r->frame_res[i].rt_out->Release();
    }
    if (r->rt_state) r->rt_state->Release();
    if (r->root_sig) r->root_sig->Release();
    if (r->main_heap) r->main_heap->Release();
    if (r->rtv_heap) r->rtv_heap->Release();
    if (r->swapchain) r->swapchain->Release();
    if (r->cmd_queue) r->cmd_queue->Release();
    if (r->device) r->device->Release();
    if (r->factory) r->factory->Release();

    printf("[OK] Renderer stopped\n");
}
