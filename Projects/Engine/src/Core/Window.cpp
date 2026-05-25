// FBZZ Engine
// Window.cpp | fbzz::core
// Win32 ウィンドウの生成とメッセージ処理
// Input へのメッセージ転送、リサイズ通知、WndProc フックをまとめる。
// Renderer / ImGui とはコールバックで疎結合に接続する。

#include "Engine/Core/Window.hpp"
#include "Engine/Input/Input.hpp"

#include <dwmapi.h>
#include <windowsx.h>
#include <algorithm>
#include <cassert>

#pragma comment(lib, "dwmapi.lib")

#ifndef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 ((DPI_AWARENESS_CONTEXT)-4)
#endif

using namespace fbzz::core;

namespace
{
    constexpr wchar_t kWindowClassName[] = L"FBZZWindowClass";

    void EnableDpiAwareness()
    {
        if (SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
            return;

        SetProcessDPIAware();
    }

    bool AdjustWindowRectForDpi(RECT& rect, DWORD style, DWORD exStyle, UINT dpi)
    {
        if (AdjustWindowRectExForDpi(&rect, style, FALSE, exStyle, dpi))
            return true;

        return AdjustWindowRectEx(&rect, style, FALSE, exStyle) != FALSE;
    }

    RECT GetPrimaryWorkArea()
    {
        POINT origin{ 0, 0 };
        HMONITOR monitor = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);

        MONITORINFO monitorInfo{};
        monitorInfo.cbSize = sizeof(MONITORINFO);
        if (!GetMonitorInfoW(monitor, &monitorInfo))
            return { 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };

        return monitorInfo.rcWork;
    }

    RECT CalculateInitialWindowRect(uint32_t desiredClientWidth, uint32_t desiredClientHeight,
                                    DWORD style, DWORD exStyle, UINT dpi)
    {
        const RECT work = GetPrimaryWorkArea();
        const int workWidth  = work.right - work.left;
        const int workHeight = work.bottom - work.top;

        RECT frameRect = { 0, 0, 0, 0 };
        AdjustWindowRectForDpi(frameRect, style, exStyle, dpi);

        const int horizontalFrame = (frameRect.right - frameRect.left);
        const int verticalFrame   = (frameRect.bottom - frameRect.top);

        const int clientWidth = (std::max)(1, (std::min)(
            static_cast<int>(desiredClientWidth),
            workWidth - horizontalFrame));
        const int clientHeight = (std::max)(1, (std::min)(
            static_cast<int>(desiredClientHeight),
            workHeight - verticalFrame));

        RECT windowRect = { 0, 0, clientWidth, clientHeight };
        AdjustWindowRectForDpi(windowRect, style, exStyle, dpi);

        const int windowWidth  = windowRect.right - windowRect.left;
        const int windowHeight = windowRect.bottom - windowRect.top;

        const int x = work.left + (std::max)(0, (workWidth  - windowWidth)  / 2);
        const int y = work.top  + (std::max)(0, (workHeight - windowHeight) / 2);

        return {
            static_cast<LONG>(x),
            static_cast<LONG>(y),
            static_cast<LONG>(x + windowWidth),
            static_cast<LONG>(y + windowHeight)
        };
    }
}

bool Window::Initialize(const Config& config)
{
    EnableDpiAwareness();

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

    const DWORD style   = WS_OVERLAPPEDWINDOW;
    const DWORD exStyle = 0;

    const UINT dpi = GetDpiForSystem();
    const RECT windowRect = CalculateInitialWindowRect(config.width, config.height, style, exStyle, dpi);

    m_hwnd = CreateWindowExW(
        exStyle,
        kWindowClassName,
        config.title.c_str(),
        style,
        windowRect.left, windowRect.top,
        windowRect.right - windowRect.left,
        windowRect.bottom - windowRect.top,
        nullptr, nullptr,
        GetModuleHandleW(nullptr),
        this);

    assert(m_hwnd && "Window creation failed");

    // ダークモード有効化
    BOOL darkMode = TRUE;
    DwmSetWindowAttribute(m_hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkMode, sizeof(darkMode));

    ShowWindow(m_hwnd, SW_SHOW);
    UpdateWindow(m_hwnd);

    RECT clientRect{};
    GetClientRect(m_hwnd, &clientRect);
    m_width  = static_cast<uint32_t>(clientRect.right - clientRect.left);
    m_height = static_cast<uint32_t>(clientRect.bottom - clientRect.top);

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

    // 登録済みフック (ImGui 等) に先にメッセージを渡す
    if (window && window->m_wndProcHook)
        if (window->m_wndProcHook(hwnd, msg, wParam, lParam))
            return true;

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
    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP:
        fbzz::input::Input::HandleKeyMessage(msg, wParam);
        return 0;

    case WM_MOUSEMOVE:
        fbzz::input::Input::HandleMouseMove(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        return 0;

    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
        SetCapture(hwnd); // ウィンドウ外でもマウスイベントを受け取る
        fbzz::input::Input::HandleMouseButton(msg);
        return 0;

    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
    case WM_MBUTTONUP:
        ReleaseCapture();
        fbzz::input::Input::HandleMouseButton(msg);
        return 0;

    case WM_MOUSEWHEEL:
    {
        float delta = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam)) / WHEEL_DELTA;
        fbzz::input::Input::HandleMouseScroll(delta);
        return 0;
    }
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}
