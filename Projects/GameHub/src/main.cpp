// FBZZ Engine
// main.cpp | fbzz::hub
// FBZZ Hub の Win32 / ImGui エントリポイント
//
// WHY: 以前は本ファイルが D3D11 デバイス・スワップチェーンを直接生成し、ImGui の DX11 バックエンドも
//      直叩きしていた。DX12 移行に備え、GPU バックエンドの生成と描画を Engine の抽象層
//      (RendererFactory / IRenderer / IImGuiRenderer) へ寄せ、GameHub からは DX11 具象型を排除する。
//      ウィンドウ枠 (アイコン・ダークタイトルバー・DPI) は Hub 固有の見た目なので Win32 で維持する。
#include "HubApp.hpp"

#include <Engine/Renderer/IImGuiRenderer.hpp>
#include <Engine/Renderer/IRenderer.hpp>
#include <Engine/Renderer/RendererFactory.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Math/Vector4.hpp>

#include <Windows.h>
#include <dwmapi.h>
#include <imgui.h>
#include <imgui_impl_win32.h>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <thread>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

#ifndef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 ((DPI_AWARENESS_CONTEXT)-4)
#endif

namespace {

constexpr wchar_t WINDOW_CLASS_NAME[] = L"FBZZHubWindowClass";
constexpr int INITIAL_WIDTH = 960;
constexpr int INITIAL_HEIGHT = 640;
constexpr int APP_ICON_ID = 101;

// WndProc からのリサイズ通知を届けるための非所有ポインタ。
// WHY: WM_SIZE は自由関数の WndProc に届くため、バックバッファを持つ IRenderer をグローバルで参照する。
//      デバイス自体の寿命は wWinMain 内の RendererBundle が所有する。
fbzz::renderer::IRenderer* g_renderer = nullptr;
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
        if (wParam != SIZE_MINIMIZED && g_renderer) {
            // スワップチェーン・バックバッファの再構築はバックエンドに委譲する。
            g_renderer->Resize(static_cast<uint32_t>(LOWORD(lParam)),
                               static_cast<uint32_t>(HIWORD(lParam)));
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

// ImGui コンテキスト・フォント・スタイルを構築する (GPU / Win32 バックエンド初期化は含まない)。
// WHY: バックエンドの初期化は IImGuiRenderer::ImGuiInit に委譲するため、ここでは Hub 固有の
//      日本語フォントや ini ファイル名など、コンテキスト側の設定だけを行う。
void SetupImGuiContext()
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
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
    EnableDpiAwareness();

    HWND hwnd = CreateHubWindow(instance);
    if (!hwnd)
        return 1;

    // クライアント領域の実サイズでバックエンドのスワップチェーンを初期化する。
    RECT clientRect{};
    GetClientRect(hwnd, &clientRect);
    const uint32_t clientWidth  = static_cast<uint32_t>(clientRect.right - clientRect.left);
    const uint32_t clientHeight = static_cast<uint32_t>(clientRect.bottom - clientRect.top);

    // バックエンド具象の選択は RendererFactory に集約し、GameHub自身もDX12を既定にする。
    auto bundle = fbzz::renderer::CreateRenderer(
        fbzz::renderer::RendererBackend::DX12, hwnd, clientWidth, clientHeight);
    if (!bundle.renderer || !bundle.imguiRenderer) {
        DestroyWindow(hwnd);
        UnregisterClassW(WINDOW_CLASS_NAME, instance);
        return 1;
    }
    g_renderer = bundle.renderer.get();

    // GPU リソース (サムネイルテクスチャ) の所有窓口。
    fbzz::renderer::ResourceManager resources(*bundle.renderer);

    // ImGui: コンテキスト設定 → バックエンド (Win32 / GPU) 初期化の順で立ち上げる。
    SetupImGuiContext();
    bundle.imguiRenderer->ImGuiInit(hwnd);

    fbzz::hub::HubApp app;
    app.Init(resources, *bundle.imguiRenderer);

    const fbzz::math::Vector4 clearColor(0.08f, 0.09f, 0.10f, 1.0f);

    // WHY: IRenderer::EndFrame は vsync を指定できず SyncInterval=0 で Present するため、
    //      ランチャーがフレームを無制限に回して CPU/GPU を占有しないよう ~60 FPS に緩く制限する。
    constexpr auto FRAME_BUDGET = std::chrono::microseconds(16'666);

    while (g_running) {
        const auto frameStart = std::chrono::steady_clock::now();

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

        bundle.renderer->BeginFrame();

        bundle.imguiRenderer->ImGuiNewFrame();
        ImGui::NewFrame();

        app.Render();

        ImGui::Render();

        bundle.renderer->Clear(clearColor);
        bundle.imguiRenderer->ImGuiRenderDrawData();
        bundle.renderer->EndFrame();

        std::this_thread::sleep_until(frameStart + FRAME_BUDGET);
    }

    // WHY: GPU リソースはデバイス破棄前に解放する。app (サムネイル) → ImGui → ResourceManager の順に
    //      畳んでから IRenderer を Shutdown し、最後に RendererBundle を破棄する。
    app.Shutdown();
    bundle.imguiRenderer->ImGuiShutdown();
    ImGui::DestroyContext();
    resources.Reset();
    g_renderer = nullptr;
    bundle.renderer->Shutdown();
    bundle.imguiRenderer.reset();
    bundle.renderer.reset();

    if (IsWindow(hwnd)) {
        DestroyWindow(hwnd);
    }
    UnregisterClassW(WINDOW_CLASS_NAME, instance);
    return 0;
}
