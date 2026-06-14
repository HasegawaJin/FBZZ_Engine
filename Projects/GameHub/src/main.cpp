// FBZZ Engine
// main.cpp | fbzz::hub
// FBZZ Hub の Win32 / DX11 / ImGui エントリポイント
#include "HubApp.hpp"

#include <Windows.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <wrl/client.h>
#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>
#include <filesystem>
#include <string>
#include <system_error>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

#ifndef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 ((DPI_AWARENESS_CONTEXT)-4)
#endif

namespace {

using Microsoft::WRL::ComPtr;

constexpr wchar_t WINDOW_CLASS_NAME[] = L"FBZZHubWindowClass";
constexpr int INITIAL_WIDTH = 960;
constexpr int INITIAL_HEIGHT = 640;
constexpr int APP_ICON_ID = 101;

struct D3DState {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain> swapChain;
    ComPtr<ID3D11RenderTargetView> renderTargetView;
};

D3DState g_d3d;
bool g_running = true;

std::string ResolveJapaneseFontPath()
{
    // WHY: GameHub も ImGui を直接初期化するため、EditorTheme を通らない。
    //      Windows 標準日本語フォントを使い、プロジェクト名やログの文字化けを防ぐ。
    static constexpr const char* CANDIDATES[] = {
        "C:/Windows/Fonts/YuGothM.ttc",
        "C:/Windows/Fonts/meiryo.ttc",
        "C:/Windows/Fonts/msgothic.ttc",
    };

    std::error_code ec;
    for (const char* path : CANDIDATES) {
        if (std::filesystem::is_regular_file(path, ec) && !ec) {
            return path;
        }
        ec.clear();
    }
    return {};
}

HICON LoadApplicationIcon(HINSTANCE instance, int size)
{
    // WHY: exe に埋め込んだ Hub 専用アイコンを、Explorer だけでなくタイトルバーと Alt+Tab にも反映する。
    // LR_SHARED により HICON の寿命を OS 管理にして、Hub 側の解放責務を持たない。
    // WHAT: rc ファイルと同じ ID 101 から、Win32 が要求する大/小サイズの HICON を取得する。
    return static_cast<HICON>(LoadImageW(
        instance,
        MAKEINTRESOURCEW(APP_ICON_ID),
        IMAGE_ICON,
        size,
        size,
        LR_DEFAULTCOLOR | LR_SHARED));
}

void EnableDpiAwareness()
{
    if (SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
        return;

    SetProcessDPIAware();
}

bool CreateRenderTarget()
{
    ComPtr<ID3D11Texture2D> backBuffer;
    if (FAILED(g_d3d.swapChain->GetBuffer(0, IID_PPV_ARGS(backBuffer.GetAddressOf()))))
        return false;

    return SUCCEEDED(g_d3d.device->CreateRenderTargetView(
        backBuffer.Get(),
        nullptr,
        g_d3d.renderTargetView.GetAddressOf()));
}

void CleanupRenderTarget()
{
    if (g_d3d.context) {
        g_d3d.context->OMSetRenderTargets(0, nullptr, nullptr);
    }
    g_d3d.renderTargetView.Reset();
}

bool InitD3D(HWND hwnd)
{
    DXGI_SWAP_CHAIN_DESC desc{};
    desc.BufferCount = 2;
    desc.BufferDesc.Width = 0;
    desc.BufferDesc.Height = 0;
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferDesc.RefreshRate.Numerator = 0;
    desc.BufferDesc.RefreshRate.Denominator = 1;
    desc.Flags = 0;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.OutputWindow = hwnd;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    UINT flags = 0;
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    D3D_FEATURE_LEVEL featureLevel{};
    constexpr D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_0
    };

    const HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        flags,
        featureLevels,
        2,
        D3D11_SDK_VERSION,
        &desc,
        g_d3d.swapChain.GetAddressOf(),
        g_d3d.device.GetAddressOf(),
        &featureLevel,
        g_d3d.context.GetAddressOf());

    if (FAILED(hr))
        return false;

    return CreateRenderTarget();
}

void CleanupD3D()
{
    CleanupRenderTarget();
    g_d3d.swapChain.Reset();
    g_d3d.context.Reset();
    g_d3d.device.Reset();
}

void ResizeD3D(UINT width, UINT height)
{
    if (!g_d3d.swapChain || width == 0 || height == 0)
        return;

    CleanupRenderTarget();
    g_d3d.swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
    CreateRenderTarget();
}

RECT CalculateWindowRect()
{
    RECT rect{ 0, 0, INITIAL_WIDTH, INITIAL_HEIGHT };
    AdjustWindowRectEx(&rect, WS_OVERLAPPEDWINDOW, FALSE, 0);

    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    const int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    const int screenHeight = GetSystemMetrics(SM_CYSCREEN);
    const int x = (screenWidth - width) / 2;
    const int y = (screenHeight - height) / 2;

    return { x, y, x + width, y + height };
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam))
        return true;

    switch (msg) {
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) {
            ResizeD3D(static_cast<UINT>(LOWORD(lParam)), static_cast<UINT>(HIWORD(lParam)));
        }
        return 0;
    case WM_DESTROY:
        g_running = false;
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

HWND CreateHubWindow(HINSTANCE instance)
{
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.hIcon = LoadApplicationIcon(instance, GetSystemMetrics(SM_CXICON));
    wc.hIconSm = LoadApplicationIcon(instance, GetSystemMetrics(SM_CXSMICON));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = WINDOW_CLASS_NAME;
    RegisterClassExW(&wc);

    const RECT rect = CalculateWindowRect();
    HWND hwnd = CreateWindowExW(
        0,
        WINDOW_CLASS_NAME,
        L"FBZZ Hub",
        WS_OVERLAPPEDWINDOW,
        rect.left,
        rect.top,
        rect.right - rect.left,
        rect.bottom - rect.top,
        nullptr,
        nullptr,
        instance,
        nullptr);

    if (!hwnd)
        return nullptr;

    BOOL darkMode = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkMode, sizeof(darkMode));
    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);
    return hwnd;
}

void InitImGui(HWND hwnd)
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = "hub_imgui.ini";

    ImFontConfig fontCfg;
    fontCfg.OversampleH = 2;
    fontCfg.OversampleV = 2;
    fontCfg.PixelSnapH  = false;
    const std::string japaneseFontPath = ResolveJapaneseFontPath();
    if (japaneseFontPath.empty() ||
        !io.Fonts->AddFontFromFileTTF(
            japaneseFontPath.c_str(), 15.0f, &fontCfg, io.Fonts->GetGlyphRangesJapanese())) {
        io.Fonts->AddFontDefault();
    }

    ImGui::StyleColorsDark();
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_d3d.device.Get(), g_d3d.context.Get());
}

void ShutdownImGui()
{
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
    EnableDpiAwareness();

    HWND hwnd = CreateHubWindow(instance);
    if (!hwnd)
        return 1;

    if (!InitD3D(hwnd)) {
        DestroyWindow(hwnd);
        UnregisterClassW(WINDOW_CLASS_NAME, instance);
        return 1;
    }

    InitImGui(hwnd);

    fbzz::hub::HubApp app;
    app.Init(g_d3d.device.Get());

    while (g_running) {
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) {
                g_running = false;
            }
        }
        if (!g_running)
            break;

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        app.Render();

        ImGui::Render();

        constexpr float clearColor[4] = { 0.08f, 0.09f, 0.10f, 1.0f };
        g_d3d.context->OMSetRenderTargets(1, g_d3d.renderTargetView.GetAddressOf(), nullptr);
        g_d3d.context->ClearRenderTargetView(g_d3d.renderTargetView.Get(), clearColor);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_d3d.swapChain->Present(1, 0);
    }

    app.Shutdown();
    ShutdownImGui();
    CleanupD3D();
    if (IsWindow(hwnd)) {
        DestroyWindow(hwnd);
    }
    UnregisterClassW(WINDOW_CLASS_NAME, instance);
    return 0;
}
