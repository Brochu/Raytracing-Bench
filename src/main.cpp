#include <cstdint>
#include <cstdio>

#include <windows.h>
#include <d3dcompiler.h>

LRESULT CALLBACK window_proc(HWND hWindow, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {

    case WM_CREATE:
        // Any window creation logic
        return 0;

    case WM_DESTROY:
    case WM_CLOSE:
        PostQuitMessage(0);
        return 0;

    case WM_PAINT:
        {
            // DRAW CALL STARTING POINT
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

    WNDCLASSEX wclass = { 0 };
    wclass.cbSize = sizeof(WNDCLASSEX);
    wclass.style = CS_HREDRAW | CS_VREDRAW;
    wclass.lpfnWndProc = window_proc;
    wclass.hInstance = hInstance;
    wclass.lpszClassName = "rtbench-win";

    if (!RegisterClassEx(&wclass)) {
        printf("[ERR] Failed to register window class.");
        return -1;
    }

    static uint32_t win_width = 800;
    static uint32_t win_height = 600;

    HWND window = CreateWindowEx(0,
        "rtbench-win", "RT Bench - ALPHA",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT, win_width, win_height,
        NULL, NULL, hInstance, NULL
    );

    if (!window) {
        printf("[ERR] Failed to window.");
        return -1;
    }

    // INIT ---------------------------
    // --------------------------------

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
    // ---------------------------------

    FreeConsole();
    return 0;
}
