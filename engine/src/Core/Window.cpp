// FBZZ Engine
// Window.cpp | fbzz::core
// Win32 ウィンドウの生成・メッセージ処理

#include "engine/Core/Window.hpp"

#include <dwmapi.h>
#include <cassert>

#pragma comment(lib, "dwmapi.lib")

using namespace fbzz::core;

namespace
{
    constexpr wchar_t kWindowClassName[] = L"FBZZWindowClass";
}

bool Window::Initialize(const Config& config)
{
    m_width  = config.width;
    m_height = config.height;

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(WNDCLASSEXW);
    wc.style         = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.hCursor       = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
    wc.lpszClassName = kWindowClassName;

    RegisterClassExW(&wc);

    RECT rect = { 0, 0, static_cast<LONG>(config.width), static_cast<LONG>(config.height) };
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);

    m_hwnd = CreateWindowExW(
        0,
        kWindowClassName,
        config.title.c_str(),
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rect.right - rect.left,
        rect.bottom - rect.top,
        nullptr, nullptr,
        GetModuleHandleW(nullptr),
        this);

    assert(m_hwnd && "Window creation failed");

    // ダークモード有効化
    BOOL darkMode = TRUE;
    DwmSetWindowAttribute(m_hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkMode, sizeof(darkMode));

    ShowWindow(m_hwnd, SW_SHOW);
    UpdateWindow(m_hwnd);

    return m_hwnd != nullptr;
}

void Window::Shutdown()
{
    if (!m_hwnd)
        return;

    DestroyWindow(m_hwnd);
    m_hwnd = nullptr;
}

void Window::PollEvents()
{
    MSG msg{};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
    {
        if (msg.message == WM_QUIT)
            m_shouldClose = true;

        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

LRESULT CALLBACK Window::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_NCCREATE)
    {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    auto* window = reinterpret_cast<Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg)
    {
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    case WM_SIZE:
    {
        if (!window) return 0;
        const uint32_t w = LOWORD(lParam);
        const uint32_t h = HIWORD(lParam);
        if (w == 0 || h == 0) return 0;
        window->m_width  = w;
        window->m_height = h;
        if (window->m_resizeCallback)
            window->m_resizeCallback(w, h);
        return 0;
    }

    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_MOUSEMOVE:
        // InputSystem に渡す予定
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}
