#pragma once

#include <cstdint>
#include <initializer_list>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <DirectXMath.h>

constexpr uint32_t FRAME_COUNT = 2;
constexpr uint32_t PASS_MAX_COUNT = 16;

struct renderer;

struct frame_resources {
    ID3D12CommandAllocator *cmd_alloc;
    ID3D12Resource *back_buffer;

    ID3D12Resource *scene_cb;

    ID3D12Resource *rt_shader_table;
    ID3D12Resource *rt_blas;
    ID3D12Resource *rt_tlas;
    ID3D12Resource *rt_scratch;
    ID3D12Resource *rt_out;

    uint64_t fence_value;
};

struct render_pass {
    void (*init)(render_pass *pass, renderer *r);
    void (*exec)(render_pass *pass, renderer *r, frame_resources *frame, ID3D12GraphicsCommandList4 *cmd);
    void (*stop)(render_pass *pass, renderer *r);
    void *data;
};

struct renderer {
    IDXGIFactory6 *factory;
    ID3D12Device5 *device;
    IDXGISwapChain3 *swapchain;
    ID3D12Fence *fence;
    HANDLE fence_event;

    ID3D12CommandQueue *cmd_queue;
    ID3D12GraphicsCommandList4 *cmd_list;
    ID3D12RootSignature *root_sig;
    ID3D12StateObject *rt_state;

    ID3D12DescriptorHeap *rtv_heap;
    uint32_t rtv_descriptor_size;
    ID3D12DescriptorHeap *main_heap;
    uint32_t main_descriptor_size;

    uint32_t frame_index;
    uint32_t width;
    uint32_t height;

    frame_resources frame_res[FRAME_COUNT];
    render_pass passes[PASS_MAX_COUNT];
    uint32_t num_passes = 0;
    uint32_t current_pass_id = 0;
};

struct render_cam {
    DirectX::XMFLOAT4 position;
    DirectX::XMFLOAT4 target;
};

struct render_scene {
    DirectX::XMFLOAT4 spheres[128];
    DirectX::XMFLOAT4 colors[128];
    int32_t types[128];

    int32_t num_spheres = 0;

    render_cam camera;
};

void render_init(renderer *r, HWND hwnd, uint32_t width, uint32_t height, std::initializer_list<render_pass*>);
void render_draw(renderer *r, render_scene *scene);
void render_stop(renderer *r);
