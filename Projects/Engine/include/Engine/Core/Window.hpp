// FBZZ Engine
// Window.hpp | fbzz::core
// Win32 ウィンドウの生成・イベント処理
// Renderer のリサイズ通知と ImGui の WndProc フックをつなぐ境界。
// HWND は必要なバックエンドへ渡すために公開する。
#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <functional>
#include <utility>
#include <Windows.h>

// COM の IDropTarget は <oleidl.h> で定義される。ヘッダに OLE 依存を波及させないよう前方宣言に留める。
struct IDropTarget;

namespace fbzz::core
{
class Window
{
public:
    // OSのメニューバーへ移す項目。Editor層がWin32 HMENUを直接扱わないための境界。
    struct NativeMenuItem {
        std::wstring label;
        uint16_t commandId = 0;
        bool separator = false;
        std::vector<NativeMenuItem> children;
    };
    using NativeMenuCommandCallback = std::function<bool(uint16_t)>;

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
        // Unityのようなネイティブメニューバーを設定する。WindowがHMENUの所有権を持つ。
        void SetNativeMenu(std::vector<NativeMenuItem> menus, NativeMenuCommandCallback callback);
        void ClearNativeMenu();

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

        // Explorer からファイルがドロップされた時のコールバック (OLE IDropTarget::Drop)。
        // WHY: Unity のようにエクスプローラーから素材を D&D で取り込めるよう、
        //      OS レベルのドロップを Editor (AssetBrowser) へ橋渡しする。パスは UTF-8、
        //      x/y はクライアント座標でのドロップ位置 (ドロップ先フォルダの判定に使う)。
        using FileDropCallback =
            std::function<void(const std::vector<std::string>&, int x, int y)>;
        void SetFileDropCallback(FileDropCallback cb) { m_fileDropCallback = std::move(cb); }

        // ドラッグ中 (DragEnter/DragOver) にファイルがウィンドウ上を移動した時のコールバック。
        // WHY: OLE ドロップターゲットはドロップ確定前にカーソル位置を通知できる。これを使い、
        //      ドロップ先フォルダを Unity のようにリアルタイムでハイライトする。x/y はクライアント座標。
        using FileDragOverCallback = std::function<void(int x, int y)>;
        void SetFileDragOverCallback(FileDragOverCallback cb) { m_fileDragOverCallback = std::move(cb); }

        // ドラッグがウィンドウ外へ出た / ドロップで終わった時のコールバック (ハイライト解除用)。
        using FileDragLeaveCallback = std::function<void()>;
        void SetFileDragLeaveCallback(FileDragLeaveCallback cb) { m_fileDragLeaveCallback = std::move(cb); }

        // FileDropTarget (OLE 実装) からコールバックを叩くための内部アクセサ。
        void InvokeFileDrop(const std::vector<std::string>& paths, int x, int y) const {
            if (m_fileDropCallback && !paths.empty()) m_fileDropCallback(paths, x, y);
        }
        void InvokeFileDragOver(int x, int y) const {
            if (m_fileDragOverCallback) m_fileDragOverCallback(x, y);
        }
        void InvokeFileDragLeave() const {
            if (m_fileDragLeaveCallback) m_fileDragLeaveCallback();
        }

    private:
        static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg,
                                        WPARAM wParam, LPARAM lParam);

        HWND     m_hwnd        = nullptr;
        uint32_t m_width       = 0;
        uint32_t m_height      = 0;
        bool     m_shouldClose = false;

        ResizeCallback        m_resizeCallback;
        WndProcHook           m_wndProcHook;
        FileDropCallback      m_fileDropCallback;
        FileDragOverCallback  m_fileDragOverCallback;
        FileDragLeaveCallback m_fileDragLeaveCallback;
        HMENU m_nativeMenu = nullptr;
        NativeMenuCommandCallback m_nativeMenuCommandCallback;
        // OLE ドロップターゲット (RegisterDragDrop に登録)。Shutdown で Revoke + Release する。
        IDropTarget*          m_dropTarget = nullptr;
        bool                  m_oleInitialized = false;
    };
} // namespace fbzz::core
