#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <cmath>

#include <windows.h>
#include <d3dcompiler.h>

#include "bn_renderer.hpp"

render_scene g_scene;

static bool  g_mouse_down  = false;
static POINT g_mouse_last  = {};

static void update_orbit_camera() {
    render_cam *cam = &g_scene.camera;
    float x = cam->orbit_dist * cosf(cam->orbit_pitch) * sinf(cam->orbit_yaw);
    float y = cam->orbit_dist * sinf(cam->orbit_pitch);
    float z = cam->orbit_dist * cosf(cam->orbit_pitch) * cosf(cam->orbit_yaw);
    cam->position = { x, y, z, 1.f };
    cam->target   = { 0.f, 0.f, 0.f, 1.f };
}

LRESULT CALLBACK window_proc(HWND hWindow, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {

    case WM_CREATE:
        {
            CREATESTRUCT *cs = reinterpret_cast<CREATESTRUCT*>(lparam);
            SetWindowLongPtr(hWindow, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        }
        return 0;

    case WM_DESTROY:
    case WM_CLOSE:
        PostQuitMessage(0);
        return 0;

    case WM_LBUTTONDOWN:
        g_mouse_down = true;
        g_mouse_last = { (LONG)LOWORD(lparam), (LONG)HIWORD(lparam) };
        SetCapture(hWindow);
        return 0;

    case WM_LBUTTONUP:
        g_mouse_down = false;
        ReleaseCapture();
        return 0;

    case WM_MOUSEMOVE:
        if (g_mouse_down) {
            POINT cur = { (LONG)LOWORD(lparam), (LONG)HIWORD(lparam) };
            float dx = (float)(cur.x - g_mouse_last.x);
            float dy = (float)(cur.y - g_mouse_last.y);
            g_mouse_last = cur;

            g_scene.camera.orbit_yaw   += dx * 0.005f;
            g_scene.camera.orbit_pitch += dy * 0.005f;

            // Clamp pitch to avoid flipping
            float limit = 1.5f;
            if (g_scene.camera.orbit_pitch >  limit) g_scene.camera.orbit_pitch =  limit;
            if (g_scene.camera.orbit_pitch < -limit) g_scene.camera.orbit_pitch = -limit;

            update_orbit_camera();
        }
        return 0;

    case WM_PAINT:
        {
            renderer *r = reinterpret_cast<renderer*>(GetWindowLongPtr(hWindow, GWLP_USERDATA));
            if (r) {
                render_draw(r, &g_scene);
            }
        }
        return 0;
    }
    return DefWindowProc(hWindow, msg, wparam, lparam);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR cmdline, int nCmdShow) {
    AllocConsole();
    FILE* fp;
    freopen_s(&fp, "CONOUT$", "w", stdout);
    freopen_s(&fp, "CONOUT$", "w", stderr);
    freopen_s(&fp, "CONIN$", "r", stdin);;

    constexpr uint32_t win_width = 800;
    constexpr uint32_t win_height = 600;

    HICON icon = LoadIcon(hInstance, "g_icon");

    WNDCLASSEX wclass = { 0 };
    wclass.cbSize = sizeof(WNDCLASSEX);
    wclass.style = CS_HREDRAW | CS_VREDRAW;
    wclass.lpfnWndProc = window_proc;
    wclass.hInstance = hInstance;
    wclass.lpszClassName = "rtbench-win";
    wclass.hIcon = icon;
    wclass.hIconSm = icon;

    if (!RegisterClassEx(&wclass)) {
        printf("[ERR] Failed to register window class.");
        return -1;
    }

    renderer render;

    HWND window = CreateWindowEx(0,
        "rtbench-win", "RT Bench - ALPHA",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT, win_width, win_height,
        NULL, NULL, hInstance, &render
    );

    if (!window) {
        printf("[ERR] Failed to window.");
        return -1;
    }

    render_init(&render, window, win_width, win_height, {});

    //Create Scene ------------
    constexpr int32_t sphere_count = 64;
    constexpr float min_coord = -10.f;
    constexpr float max_coord = 10.f;
    constexpr float min_radius = 0.1f;
    constexpr float max_radius = 2.f;

    srand((unsigned)time(nullptr));
    auto randf = [](float lo, float hi) {
        return lo + (float)rand() / (float)RAND_MAX * (hi - lo);
    };

    for (int32_t i = 0; i < sphere_count; i++) {
        g_scene.types[i] = 0;
        g_scene.spheres[i] = {
            randf(min_coord, max_coord),
            randf(min_coord, max_coord),
            randf(min_coord, max_coord),
            randf(min_radius, max_radius)
        };
        g_scene.colors[i] = { randf(0.f, 1.f), randf(0.f, 1.f), randf(0.f, 1.f), 1.f };
    }
    g_scene.num_spheres = sphere_count;

    update_orbit_camera();
    //-------------------------

    for (bool should_quit = false; !should_quit; ) {
        MSG m;
        PeekMessage(&m, NULL, 0, 0, PM_REMOVE);

        if (m.message == WM_QUIT) {
            should_quit = true;
        } else {
            TranslateMessage(&m);
            DispatchMessage(&m);
        }
    }

    // CLOSE ---------------------------
    render_stop(&render);
    // ---------------------------------

    FreeConsole();
    return 0;
}
