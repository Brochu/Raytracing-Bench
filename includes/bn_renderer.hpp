#pragma once

#include <cstdint>
#include <initializer_list>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <DirectXMath.h>

#include "dxcapi.h"

constexpr int32_t PRIMITIVE_MAX_COUNT = 128;
constexpr int32_t FRAME_COUNT = 2;
constexpr int32_t PASS_MAX_COUNT = 16;

struct renderer;

struct frame_resources {
    ID3D12CommandAllocator *cmd_alloc;
    ID3D12Resource *back_buffer;

    ID3D12Resource *scene_cb;

    ID3D12Resource *rt_shader_table;
    ID3D12Resource *rt_blas;
    ID3D12Resource *rt_tlas;
    ID3D12Resource *rt_tlas_inst;
    ID3D12Resource *rt_scratch;
    ID3D12Resource *rt_out;
    ID3D12Resource *rt_aabbs;

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

    IDxcBlob *rt_shaders_blob;

    int32_t frame_index;
    int32_t width;
    int32_t height;

    frame_resources frame_res[FRAME_COUNT];
    render_pass passes[PASS_MAX_COUNT];
    int32_t num_passes = 0;
    int32_t current_pass_id = 0;
};

struct render_cbuffer {
    DirectX::XMFLOAT4X4 view_proj;
    DirectX::XMFLOAT4X4 inv_view_proj;
    DirectX::XMFLOAT4 cam_position;

    DirectX::XMFLOAT4 spheres[PRIMITIVE_MAX_COUNT];
    DirectX::XMFLOAT4 colors[PRIMITIVE_MAX_COUNT];
    DirectX::XMUINT4 materials[PRIMITIVE_MAX_COUNT];

    DirectX::XMFLOAT4 ground_color;
    float ground_y;

    int32_t frame_index;
    int32_t num_spheres;
    int32_t rays_per_pixel;

    int32_t width;
    int32_t height;
    uint32_t _pad[2];
};

struct render_cam {
    DirectX::XMFLOAT4 position;
    DirectX::XMFLOAT4 target;
    float fov = 60.f;
    float near_z = 0.1f;
    float far_z = 1000.f;

    float orbit_yaw   = 3.14159265f;
    float orbit_pitch = 0.4f;
    float orbit_dist  = 25.f;
};

struct render_scene {
    DirectX::XMFLOAT4 spheres[PRIMITIVE_MAX_COUNT];
    DirectX::XMFLOAT4 colors[PRIMITIVE_MAX_COUNT];
    DirectX::XMUINT4 materials[PRIMITIVE_MAX_COUNT];

    DirectX::XMFLOAT4 ground_color = { 0.8f, 0.8f, 0.4f, 1.0f };
    float ground_y = 0.0f;

    int32_t num_spheres = 0;
    int32_t rays_per_pixel = 8;

    render_cam camera;
};

void render_init(renderer *r, HWND hwnd, int32_t width, int32_t height, std::initializer_list<render_pass*>);
void render_draw(renderer *r, render_scene *scene);
void render_stop(renderer *r);
