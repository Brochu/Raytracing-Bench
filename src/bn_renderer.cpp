#include "bn_renderer.hpp"
#include "bn_shaders.hpp"

#include <cassert>
#include <cstdio>

#define RESOURCE_VIEWS_MAX_COUNT 128
#define CHECKHR(hr, msg) if (FAILED(hr)) { printf("[ERR] %s (0x%08X)\n", msg, (unsigned)hr); return; }

template<typename T>
inline T Align(T value, T align) {
    return (value + align - 1) & ~(align-1);
};

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
    r->device->SetName(L"Dx12_Device");

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
    r->cmd_queue->SetName(L"CmdQueue");

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
    r->rtv_heap->SetName(L"RTV_Heap");

    r->rtv_descriptor_size = r->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    // Main bindless heap
    D3D12_DESCRIPTOR_HEAP_DESC main_heap_desc = {};
    main_heap_desc.NumDescriptors = RESOURCE_VIEWS_MAX_COUNT;
    main_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    main_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    main_heap_desc.NodeMask = 0;
    hr = r->device->CreateDescriptorHeap(&main_heap_desc, IID_PPV_ARGS(&r->main_heap));
    CHECKHR(hr, "CreateDescriptorHeap (MAIN - Bindless)");
    r->main_heap->SetName(L"Main_Heap");

    r->main_descriptor_size = r->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    // Back buffer RTVs + per-frame resources
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle = r->rtv_heap->GetCPUDescriptorHandleForHeapStart();
    for (int32_t i = 0; i < FRAME_COUNT; i++) {
        frame_resources *frame = &r->frame_res[i];

        hr = r->device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&frame->cmd_alloc));
        CHECKHR(hr, "CreateCommandAllocator");
        frame->cmd_alloc->SetName(L"CmdAllocator");

        // DXR output UAV (one per frame in flight)
        D3D12_HEAP_PROPERTIES rt_out_heap_props = {};
        rt_out_heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC rt_out_desc = {};
        rt_out_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rt_out_desc.Width = width;
        rt_out_desc.Height = height;
        rt_out_desc.DepthOrArraySize = 1;
        rt_out_desc.MipLevels = 1;
        rt_out_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        rt_out_desc.SampleDesc.Count = 1;
        rt_out_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        hr = r->device->CreateCommittedResource(
            &rt_out_heap_props, D3D12_HEAP_FLAG_NONE,
            &rt_out_desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            nullptr, IID_PPV_ARGS(&frame->rt_out));
        CHECKHR(hr, "CreateCommittedResource (rt_out)");
        frame->rt_out->SetName(L"RT_Output");

        // UAV descriptor in main bindless heap (frame 0 = index 0, frame 1 = index 1)
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav_desc = {};
        uav_desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        D3D12_CPU_DESCRIPTOR_HANDLE uav_handle = r->main_heap->GetCPUDescriptorHandleForHeapStart();
        uav_handle.ptr += i * r->main_descriptor_size;
        r->device->CreateUnorderedAccessView(frame->rt_out, nullptr, &uav_desc, uav_handle);

        hr = r->swapchain->GetBuffer(i, IID_PPV_ARGS(&frame->back_buffer));
        CHECKHR(hr, "GetBuffer");
        frame->back_buffer->SetName(L"BackBuffer");
        r->device->CreateRenderTargetView(frame->back_buffer, nullptr, rtv_handle);
        rtv_handle.ptr += r->rtv_descriptor_size;

        frame->fence_value = 0;
    }

    // Acceleration structure buffers (per frame)
    D3D12_RAYTRACING_GEOMETRY_DESC geo_desc = {};
    geo_desc.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_PROCEDURAL_PRIMITIVE_AABBS;
    geo_desc.AABBs.AABBCount = RESOURCE_VIEWS_MAX_COUNT;
    geo_desc.AABBs.AABBs.StrideInBytes = sizeof(D3D12_RAYTRACING_AABB);
    geo_desc.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS blas_inputs = {};
    blas_inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    blas_inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    blas_inputs.NumDescs = 1;
    blas_inputs.pGeometryDescs = &geo_desc;
    blas_inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO blas_info = {};
    r->device->GetRaytracingAccelerationStructurePrebuildInfo(&blas_inputs, &blas_info);

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS tlas_inputs = {};
    tlas_inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    tlas_inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    tlas_inputs.NumDescs = 1;
    tlas_inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO tlas_info = {};
    r->device->GetRaytracingAccelerationStructurePrebuildInfo(&tlas_inputs, &tlas_info);

    UINT64 scratch_size = blas_info.ScratchDataSizeInBytes > tlas_info.ScratchDataSizeInBytes
        ? blas_info.ScratchDataSizeInBytes : tlas_info.ScratchDataSizeInBytes;

    D3D12_HEAP_PROPERTIES default_heap = {};
    default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    for (int32_t i = 0; i < FRAME_COUNT; i++) {
        frame_resources *frame = &r->frame_res[i];

        D3D12_RESOURCE_DESC buf_desc = {};
        buf_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buf_desc.Width = blas_info.ResultDataMaxSizeInBytes;
        buf_desc.Height = 1;
        buf_desc.DepthOrArraySize = 1;
        buf_desc.MipLevels = 1;
        buf_desc.SampleDesc.Count = 1;
        buf_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        buf_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        hr = r->device->CreateCommittedResource(
            &default_heap, D3D12_HEAP_FLAG_NONE,
            &buf_desc, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
            nullptr, IID_PPV_ARGS(&frame->rt_blas));
        CHECKHR(hr, "CreateCommittedResource (rt_blas)");
        frame->rt_blas->SetName(L"RT_BLAS");

        buf_desc.Width = tlas_info.ResultDataMaxSizeInBytes;
        hr = r->device->CreateCommittedResource(
            &default_heap, D3D12_HEAP_FLAG_NONE,
            &buf_desc, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
            nullptr, IID_PPV_ARGS(&frame->rt_tlas));
        CHECKHR(hr, "CreateCommittedResource (rt_tlas)");
        frame->rt_tlas->SetName(L"RT_TLAS");

        buf_desc.Width = scratch_size;
        hr = r->device->CreateCommittedResource(
            &default_heap, D3D12_HEAP_FLAG_NONE,
            &buf_desc, D3D12_RESOURCE_STATE_COMMON,
            nullptr, IID_PPV_ARGS(&frame->rt_scratch));
        CHECKHR(hr, "CreateCommittedResource (rt_scratch)");
        frame->rt_scratch->SetName(L"RT_Scratch");

        D3D12_HEAP_PROPERTIES upload_heap = {};
        upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;

        D3D12_RESOURCE_DESC upload_desc = {};
        upload_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        upload_desc.Height = 1;
        upload_desc.DepthOrArraySize = 1;
        upload_desc.MipLevels = 1;
        upload_desc.SampleDesc.Count = 1;
        upload_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        upload_desc.Width = RESOURCE_VIEWS_MAX_COUNT * sizeof(D3D12_RAYTRACING_AABB);
        hr = r->device->CreateCommittedResource(
            &upload_heap, D3D12_HEAP_FLAG_NONE,
            &upload_desc, D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr, IID_PPV_ARGS(&frame->rt_aabbs));
        CHECKHR(hr, "CreateCommittedResource (rt_aabbs)");
        frame->rt_aabbs->SetName(L"RT_AABBs");

        upload_desc.Width = sizeof(D3D12_RAYTRACING_INSTANCE_DESC);
        hr = r->device->CreateCommittedResource(
            &upload_heap, D3D12_HEAP_FLAG_NONE,
            &upload_desc, D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr, IID_PPV_ARGS(&frame->rt_tlas_inst));
        CHECKHR(hr, "CreateCommittedResource (rt_tlas_inst)");
        frame->rt_tlas_inst->SetName(L"RT_TLAS_Instance");

        upload_desc.Width = Align(sizeof(render_cbuffer), (size_t)D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
        hr = r->device->CreateCommittedResource(
            &upload_heap, D3D12_HEAP_FLAG_NONE,
            &upload_desc, D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr, IID_PPV_ARGS(&frame->scene_cb));
        CHECKHR(hr, "CreateCommittedResource (scene_cb)");
        frame->scene_cb->SetName(L"Scene_CB");

        D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
        srv_desc.Format = DXGI_FORMAT_UNKNOWN;
        srv_desc.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
        srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv_desc.RaytracingAccelerationStructure.Location = frame->rt_tlas->GetGPUVirtualAddress();

        D3D12_CPU_DESCRIPTOR_HANDLE srv_handle = r->main_heap->GetCPUDescriptorHandleForHeapStart();
        srv_handle.ptr += (2 + i) * r->main_descriptor_size;
        r->device->CreateShaderResourceView(nullptr, &srv_desc, srv_handle);
    }

    // Command list
    hr = r->device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, r->frame_res[0].cmd_alloc, nullptr, IID_PPV_ARGS(&r->cmd_list));
    CHECKHR(hr, "CreateCommandList");
    r->cmd_list->SetName(L"CmdList");
    r->cmd_list->Close();

    // Fence
    hr = r->device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&r->fence));
    CHECKHR(hr, "CreateFence");
    r->fence->SetName(L"Fence");
    r->fence_event = CreateEvent(nullptr, FALSE, FALSE, nullptr);

    // Root signature
    D3D12_ROOT_PARAMETER1 param[1];
    param[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    param[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    param[0].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_NONE;
    param[0].Descriptor.ShaderRegister = 0;
    param[0].Descriptor.RegisterSpace = 0;

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
    CHECKHR(hr, "CreateRootSignature");
    r->root_sig->SetName(L"RootSignature");
    sig_blob->Release();
    if (err_blob) {
        err_blob->Release();
    }

    // Shaders
    const wchar_t *shader_path = L".\\src\\shaders\\raytrace.hlsl";
    r->rt_shaders_blob = shaders_compile_file(shader_path, L"lib_6_8");

    // Pipeline state object
    constexpr int32_t num_subobjects = 5;
    D3D12_STATE_SUBOBJECT sub_objects[num_subobjects];

    D3D12_DXIL_LIBRARY_DESC lib_desc = {};
    lib_desc.DXILLibrary = D3D12_SHADER_BYTECODE { r->rt_shaders_blob->GetBufferPointer(), r->rt_shaders_blob->GetBufferSize() };
    sub_objects[0].Type = D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY;
    sub_objects[0].pDesc = &lib_desc;

    D3D12_HIT_GROUP_DESC hit_group = {};
    hit_group.HitGroupExport = L"SphereHitGroup";
    hit_group.Type = D3D12_HIT_GROUP_TYPE_PROCEDURAL_PRIMITIVE;
    hit_group.IntersectionShaderImport = L"sphere_intersection";
    hit_group.ClosestHitShaderImport = L"closest_main";
    hit_group.AnyHitShaderImport = nullptr;
    sub_objects[1].Type = D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP;
    sub_objects[1].pDesc = &hit_group;

    D3D12_GLOBAL_ROOT_SIGNATURE global_rs = {};
    global_rs.pGlobalRootSignature = r->root_sig;
    sub_objects[2].Type = D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE;
    sub_objects[2].pDesc = &global_rs;

    D3D12_RAYTRACING_PIPELINE_CONFIG1 rt_config = {};
    rt_config.MaxTraceRecursionDepth = 5;
    rt_config.Flags = D3D12_RAYTRACING_PIPELINE_FLAG_NONE;
    sub_objects[3].Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG1;
    sub_objects[3].pDesc = &rt_config;

    D3D12_RAYTRACING_SHADER_CONFIG config = {};
    config.MaxPayloadSizeInBytes = 32;
    config.MaxAttributeSizeInBytes = 32;
    sub_objects[4].Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG;
    sub_objects[4].pDesc = &config;

    D3D12_STATE_OBJECT_DESC state_desc = {};
    state_desc.Type = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE;
    state_desc.NumSubobjects = num_subobjects;
    state_desc.pSubobjects = sub_objects;
    hr = r->device->CreateStateObject(&state_desc, IID_PPV_ARGS(&r->rt_state));
    CHECKHR(hr, "CreateStateObject");
    r->rt_state->SetName(L"RT_StateObject");

    // Shader table
    ID3D12StateObjectProperties *rt_props = nullptr;
    hr = r->rt_state->QueryInterface(IID_PPV_ARGS(&rt_props));
    CHECKHR(hr, "QueryInterface (StateObjectProperties)");

    void *raygen_id    = rt_props->GetShaderIdentifier(L"raygen_main");
    void *miss_id      = rt_props->GetShaderIdentifier(L"miss_background");
    void *hit_group_id = rt_props->GetShaderIdentifier(L"SphereHitGroup");

    // Layout: raygen at 0, miss at 64, hit group at 128 (64-byte aligned sections)
    UINT record_size = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES; // 32 bytes
    UINT table_size  = D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT * 2 + record_size; // 160 bytes

    D3D12_HEAP_PROPERTIES upload_heap = {};
    upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC table_desc = {};
    table_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    table_desc.Width = table_size;
    table_desc.Height = 1;
    table_desc.DepthOrArraySize = 1;
    table_desc.MipLevels = 1;
    table_desc.SampleDesc.Count = 1;
    table_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    for (int32_t i = 0; i < FRAME_COUNT; i++) {
        frame_resources *frame = &r->frame_res[i];

        hr = r->device->CreateCommittedResource(
            &upload_heap, D3D12_HEAP_FLAG_NONE,
            &table_desc, D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr, IID_PPV_ARGS(&frame->rt_shader_table));
        CHECKHR(hr, "CreateCommittedResource (rt_shader_table)");
        frame->rt_shader_table->SetName(L"RT_ShaderTable");

        uint8_t *mapped = nullptr;
        frame->rt_shader_table->Map(0, nullptr, (void **)&mapped);
        memcpy(mapped, raygen_id, record_size);
        memcpy(mapped + D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT, miss_id, record_size);
        memcpy(mapped + D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT * 2, hit_group_id, record_size);
        frame->rt_shader_table->Unmap(0, nullptr);
    }

    rt_props->Release();

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

    // Upload AABBs from spheres
    D3D12_RAYTRACING_AABB *aabbs = nullptr;
    frame->rt_aabbs->Map(0, nullptr, (void **)&aabbs);
    for (int32_t i = 0; i < scene->num_spheres; i++) {
        float cx = scene->spheres[i].x;
        float cy = scene->spheres[i].y;
        float cz = scene->spheres[i].z;
        float rad = scene->spheres[i].w;
        aabbs[i].MinX = cx - rad;
        aabbs[i].MinY = cy - rad;
        aabbs[i].MinZ = cz - rad;
        aabbs[i].MaxX = cx + rad;
        aabbs[i].MaxY = cy + rad;
        aabbs[i].MaxZ = cz + rad;
    }
    frame->rt_aabbs->Unmap(0, nullptr);

    // Upload frame CBV data
    render_cbuffer *cbuffer = nullptr;
    frame->scene_cb->Map(0, nullptr, (void**)&cbuffer);
    cbuffer->width = r->width;
    cbuffer->height = r->height;
    cbuffer->cam_position = scene->camera.position;
    cbuffer->num_spheres = scene->num_spheres;
    cbuffer->frame_index = r->frame_index;
    memcpy_s(&cbuffer->spheres, 3*128*sizeof(DirectX::XMFLOAT4), &scene->spheres, 3*128*sizeof(DirectX::XMFLOAT4));

    DirectX::XMVECTOR eye    = DirectX::XMLoadFloat4(&scene->camera.position);
    DirectX::XMVECTOR target = DirectX::XMLoadFloat4(&scene->camera.target);
    DirectX::XMVECTOR up     = DirectX::XMVectorSet(0.f, 1.f, 0.f, 0.f);

    DirectX::XMMATRIX view = DirectX::XMMatrixLookAtLH(eye, target, up);
    DirectX::XMMATRIX proj = DirectX::XMMatrixPerspectiveFovLH(
        DirectX::XMConvertToRadians(scene->camera.fov),
        (float)r->width / (float)r->height,
        scene->camera.near_z, scene->camera.far_z
    );

    DirectX::XMMATRIX vp     = view * proj;
    DirectX::XMMATRIX inv_vp = DirectX::XMMatrixInverse(nullptr, vp);

    DirectX::XMStoreFloat4x4(&cbuffer->view_proj, DirectX::XMMatrixTranspose(vp));
    DirectX::XMStoreFloat4x4(&cbuffer->inv_view_proj, DirectX::XMMatrixTranspose(inv_vp));
    frame->scene_cb->Unmap(0, nullptr);
    cbuffer = nullptr;

    // Render commands
    frame->cmd_alloc->Reset();
    r->cmd_list->Reset(frame->cmd_alloc, nullptr);

    r->cmd_list->SetDescriptorHeaps(1, &r->main_heap);
    r->cmd_list->SetComputeRootSignature(r->root_sig);
    r->cmd_list->SetComputeRootConstantBufferView(0, frame->scene_cb->GetGPUVirtualAddress());

    r->cmd_list->SetPipelineState1(r->rt_state);

    // Build BLAS
    D3D12_RAYTRACING_GEOMETRY_DESC geo_desc = {};
    geo_desc.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_PROCEDURAL_PRIMITIVE_AABBS;
    geo_desc.AABBs.AABBCount = scene->num_spheres;
    geo_desc.AABBs.AABBs.StartAddress = frame->rt_aabbs->GetGPUVirtualAddress();
    geo_desc.AABBs.AABBs.StrideInBytes = sizeof(D3D12_RAYTRACING_AABB);
    geo_desc.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS blas_inputs = {};
    blas_inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    blas_inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    blas_inputs.NumDescs = 1;
    blas_inputs.pGeometryDescs = &geo_desc;
    blas_inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC blas_desc = {};
    blas_desc.DestAccelerationStructureData = frame->rt_blas->GetGPUVirtualAddress();
    blas_desc.ScratchAccelerationStructureData = frame->rt_scratch->GetGPUVirtualAddress();
    blas_desc.Inputs = blas_inputs;

    r->cmd_list->BuildRaytracingAccelerationStructure(&blas_desc, 0, nullptr);

    // UAV barriers between BLAS and TLAS builds (result + shared scratch)
    D3D12_RESOURCE_BARRIER uav_barriers[2] = {};
    uav_barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uav_barriers[0].UAV.pResource = frame->rt_blas;
    uav_barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uav_barriers[1].UAV.pResource = frame->rt_scratch;
    r->cmd_list->ResourceBarrier(2, uav_barriers);

    // Upload TLAS instance desc
    D3D12_RAYTRACING_INSTANCE_DESC *instance = nullptr;
    frame->rt_tlas_inst->Map(0, nullptr, (void **)&instance);
    *instance = {};
    instance->Transform[0][0] = 1.0f;
    instance->Transform[1][1] = 1.0f;
    instance->Transform[2][2] = 1.0f;
    instance->InstanceMask = 0xFF;
    instance->AccelerationStructure = frame->rt_blas->GetGPUVirtualAddress();
    frame->rt_tlas_inst->Unmap(0, nullptr);

    // Build TLAS
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS tlas_inputs = {};
    tlas_inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    tlas_inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    tlas_inputs.NumDescs = 1;
    tlas_inputs.InstanceDescs = frame->rt_tlas_inst->GetGPUVirtualAddress();
    tlas_inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC tlas_desc = {};
    tlas_desc.DestAccelerationStructureData = frame->rt_tlas->GetGPUVirtualAddress();
    tlas_desc.ScratchAccelerationStructureData = frame->rt_scratch->GetGPUVirtualAddress();
    tlas_desc.Inputs = tlas_inputs;

    r->cmd_list->BuildRaytracingAccelerationStructure(&tlas_desc, 0, nullptr);

    // UAV barrier: ensure TLAS build completes before DispatchRays
    D3D12_RESOURCE_BARRIER tlas_barrier = {};
    tlas_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    tlas_barrier.UAV.pResource = frame->rt_tlas;
    r->cmd_list->ResourceBarrier(1, &tlas_barrier);

    // DispatchRays
    D3D12_GPU_VIRTUAL_ADDRESS table_base = frame->rt_shader_table->GetGPUVirtualAddress();

    D3D12_DISPATCH_RAYS_DESC dispatch_desc = {};
    dispatch_desc.RayGenerationShaderRecord.StartAddress  = table_base;
    dispatch_desc.RayGenerationShaderRecord.SizeInBytes   = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;
    dispatch_desc.MissShaderTable.StartAddress  = table_base + D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT;
    dispatch_desc.MissShaderTable.SizeInBytes   = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;
    dispatch_desc.MissShaderTable.StrideInBytes = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;
    dispatch_desc.HitGroupTable.StartAddress  = table_base + D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT * 2;
    dispatch_desc.HitGroupTable.SizeInBytes   = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;
    dispatch_desc.HitGroupTable.StrideInBytes = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;
    dispatch_desc.Width  = r->width;
    dispatch_desc.Height = r->height;
    dispatch_desc.Depth  = 1;

    r->cmd_list->DispatchRays(&dispatch_desc);

    // Transition rt_output -> COPY_SOURCE, then copy to back buffer, then restore
    D3D12_RESOURCE_BARRIER copy_barriers[2] = {};
    copy_barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    copy_barriers[0].Transition.pResource = frame->rt_out;
    copy_barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    copy_barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    copy_barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    copy_barriers[1].Transition.pResource = frame->back_buffer;
    copy_barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    copy_barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    copy_barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    r->cmd_list->ResourceBarrier(2, copy_barriers);

    r->cmd_list->CopyResource(frame->back_buffer, frame->rt_out);

    // Restore rt_output -> UAV, back buffer -> PRESENT
    D3D12_RESOURCE_BARRIER restore_barriers[2] = {};
    restore_barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    restore_barriers[0].Transition.pResource = frame->rt_out;
    restore_barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    restore_barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    restore_barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    restore_barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    restore_barriers[1].Transition.pResource = frame->back_buffer;
    restore_barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    restore_barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    restore_barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    r->cmd_list->ResourceBarrier(2, restore_barriers);

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
    if (r->rt_shaders_blob) r->rt_shaders_blob->Release();

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
        if (r->frame_res[i].rt_aabbs) r->frame_res[i].rt_aabbs->Release();
        if (r->frame_res[i].rt_tlas_inst) r->frame_res[i].rt_tlas_inst->Release();
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
