#include "App.h"
#include <print>

namespace
{
    App g_app;

    LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        switch (msg)
        {
        case WM_SIZE:
            if (wp != SIZE_MINIMIZED)
                g_app.OnResize(LOWORD(lp), HIWORD(lp));
            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;

        case WM_KEYDOWN:
            if (wp == VK_ESCAPE)
                DestroyWindow(hwnd);
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nCmdShow)
{
    // 콘솔 창 열기 (디버그용)
#if defined(_DEBUG)
    AllocConsole();
    FILE* dummy;
    freopen_s(&dummy, "CONOUT$", "w", stdout);
    freopen_s(&dummy, "CONOUT$", "w", stderr);
    // 버퍼링 없이 즉시 출력
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
#endif

    const wchar_t k_className[] = L"DXRWindowClass";

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.lpszClassName = k_className;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);

    RECT rect{ 0, 0,
               static_cast<LONG>(k_defaultWidth),
               static_cast<LONG>(k_defaultHeight) };
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);

    HWND hwnd = CreateWindowExW(
        0, k_className, L"DXR Renderer",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rect.right - rect.left, rect.bottom - rect.top,
        nullptr, nullptr, hInst, nullptr);

    if (!hwnd)
    {
        std::println(stderr, "윈도우 생성 실패");
        return -1;
    }

    try
    {
        g_app.Init(hwnd, k_defaultWidth, k_defaultHeight);
        ShowWindow(hwnd, nCmdShow);

        MSG msg{};
        while (msg.message != WM_QUIT)
        {
            if (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
            {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            else
            {
                g_app.OnRender();
            }
        }

        g_app.Shutdown();
    }
    catch (const std::exception& e)
    {
        MessageBoxA(nullptr, e.what(), "치명적 오류", MB_ICONERROR | MB_OK);
        return -1;
    }

    return 0;
}
