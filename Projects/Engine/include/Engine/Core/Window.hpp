// FBZZ Engine
// Window.hpp | fbzz::core
// Win32 ウィンドウの生成・イベント処理
// Renderer のリサイズ通知と ImGui の WndProc フックをつなぐ境界。
// HWND は必要なバックエンドへ渡すために公開する。
#pragma once
#include <string>
#include <cstdint>
#include <functional>
#include <Windows.h>

namespace fbzz::core
{
    class Window
    {
    public:
        struct Config
        {
            std::wstring title      = L"FBZZ Engine";
            uint32_t     width      = 1920;
            uint32_t     height     = 1080;
            // Standalone モードでフルスクリーン起動する場合に true にする。
            // エディタは常に false (ウィンドウモード)。
            bool         fullscreen = false;
        };

        bool Initialize(const Config& config);
        void Shutdown();

        // OS のタイトルバー文字列を実行時に差し替える。
        // WHY: 起動後にシーン名や使用中の描画バックエンド (DirectX 11/12) を反映させたい上位が、
        //      生の Win32 (SetWindowTextW) を直接叩かずに済むよう Window 抽象へ集約する。
        void SetTitle(const std::wstring& title);

        // メッセージポンプ。WM_QUIT を受け取ったら ShouldClose() が true になる
        void PollEvents();

        bool     ShouldClose() const { return m_shouldClose; }
        HWND     GetHandle()   const { return m_hwnd; }
        uint32_t GetWidth()    const { return m_width; }
        uint32_t GetHeight()   const { return m_height; }

        // リサイズコールバック (DX11Renderer が登録する)
        using ResizeCallback = std::function<void(uint32_t, uint32_t)>;
        void SetResizeCallback(ResizeCallback cb) { m_resizeCallback = std::move(cb); }

        // WndProc フック (ImGui 等が登録する。true を返すとそれ以降の処理をスキップ)
        using WndProcHook = std::function<bool(HWND, UINT, WPARAM, LPARAM)>;
        void SetWndProcHook(WndProcHook hook) { m_wndProcHook = std::move(hook); }

    private:
        static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg,
                                        WPARAM wParam, LPARAM lParam);

        HWND     m_hwnd        = nullptr;
        uint32_t m_width       = 0;
        uint32_t m_height      = 0;
        bool     m_shouldClose = false;

        ResizeCallback m_resizeCallback;
        WndProcHook    m_wndProcHook;
    };
} // namespace fbzz::core
