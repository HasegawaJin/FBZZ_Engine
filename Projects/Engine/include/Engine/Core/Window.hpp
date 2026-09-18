/// @file    Window.hpp
/// @brief   Win32 ウィンドウの生成・イベント処理。
/// @author  Hasegawa Jin
/// @date    2026-05-21
///
/// Renderer のリサイズ通知と ImGui の WndProc フックをつなぐ境界。
/// HWND は必要なバックエンドへ渡すために公開する。
#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <functional>
#include <utility>
#include <Windows.h>

/// @note COM の IDropTarget は `<oleidl.h>` で定義される。ヘッダに OLE 依存を波及させないよう前方宣言に留める。
struct IDropTarget;

namespace fbzz::core
{
/// @brief ウィンドウの表示形態。
/// @note 排他フルスクリーン (DXGI SetFullscreenState) はスワップチェーンの作り直しとデバイスロスト処理を DX11/DX12 両方へ要求するが、得られる「モニター解像度を変える」効果は描画スケールで代替できるため持たない。ボーダーレスなら Alt+Tab も壊れない。
enum class WindowMode : uint8_t {
    Windowed,
    BorderlessFullscreen,
};

class Window
{
public:
    /// @brief OS のメニューバーへ移す項目。Editor 層が Win32 HMENU を直接扱わないための境界。
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
            bool         fullscreen = false;   ///< Standalone モードでフルスクリーン起動時に true。エディタは常に false (ウィンドウモード)。
        };

        bool Initialize(const Config& config);
        void Shutdown();

        /// @brief OS のタイトルバー文字列を実行時に差し替える。
        /// @note 起動後にシーン名や描画バックエンド (DirectX 11/12) を反映させたい上位が、生の Win32 (SetWindowTextW) を直接叩かずに済むよう Window 抽象へ集約する。
        void SetTitle(const std::wstring& title);
        /// @brief Unity のようなネイティブメニューバーを設定する。Window が HMENU の所有権を持つ。
        void SetNativeMenu(std::vector<NativeMenuItem> menus, NativeMenuCommandCallback callback);
        void ClearNativeMenu();

        /// @brief メッセージポンプ。WM_QUIT を受け取ったら ShouldClose() が true になる。
        void PollEvents();

        bool     ShouldClose() const { return m_shouldClose; }
        HWND     GetHandle()   const { return m_hwnd; }
        uint32_t GetWidth()    const { return m_width; }
        uint32_t GetHeight()   const { return m_height; }

        /// @name 表示形態 (Option 画面から切り替える)
        /// @note ボーダーレス化はウィンドウスタイルの差し替えと SetWindowPos だけで済み、生じる WM_SIZE が既存の ResizeCallback 経由でスワップチェーンを合わせる。
        /// @{
        void       SetWindowMode(WindowMode mode);
        WindowMode GetWindowMode() const { return m_windowMode; }
        /// @}

        /// @brief ウィンドウモード時のクライアント寸法を変える。
        /// @note BorderlessFullscreen 中は「次にウィンドウへ戻したときの寸法」として覚えるだけで、画面はモニター解像度のまま変わらない。
        void SetClientSize(uint32_t width, uint32_t height);

        /// @brief ウィンドウが載っているモニターの表示領域 (物理ピクセル)。解像度ドロップダウンの上限や、フルスクリーン時の寸法予測に使う。
        void GetMonitorSize(uint32_t& outWidth, uint32_t& outHeight) const;

        /// @brief ウィンドウが載っているモニターが対応する解像度。重複を畳んで降順で返す。
        /// @note 固定表を持つとウルトラワイドや縦置きのモニターで選べない解像度が並ぶため、OS へ問い合わせ実際に存在するモードだけを Option へ出す。
        [[nodiscard]] std::vector<std::pair<uint32_t, uint32_t>> EnumerateResolutions() const;

        /// @brief リサイズコールバック。WM_SIZE で実クライアント寸法 (物理ピクセル) を通知する。登録するのは合成ルートである Application で、IRenderer::Resize へ橋渡しする。
        /// @note Window は描画バックエンドを知らないため、ここでは std::function に留める。
        using ResizeCallback = std::function<void(uint32_t, uint32_t)>;
        void SetResizeCallback(ResizeCallback cb) { m_resizeCallback = std::move(cb); }

        /// @brief WndProc フック (ImGui 等が登録する)。true を返すとそれ以降の処理をスキップ。
        using WndProcHook = std::function<bool(HWND, UINT, WPARAM, LPARAM)>;
        void SetWndProcHook(WndProcHook hook) { m_wndProcHook = std::move(hook); }

        /// @brief Explorer からファイルがドロップされた時のコールバック (OLE IDropTarget::Drop)。
        /// @note OS レベルのドロップを Editor (AssetBrowser) へ橋渡しする。パスは UTF-8、x/y はクライアント座標でのドロップ位置 (ドロップ先フォルダの判定に使う)。
        using FileDropCallback =
            std::function<void(const std::vector<std::string>&, int x, int y)>;
        void SetFileDropCallback(FileDropCallback cb) { m_fileDropCallback = std::move(cb); }

        /// @brief ドラッグ中 (DragEnter/DragOver) にファイルがウィンドウ上を移動した時のコールバック。x/y はクライアント座標。
        /// @note OLE ドロップターゲットはドロップ確定前にカーソル位置を通知できるため、ドロップ先フォルダをリアルタイムでハイライトするために使う。
        using FileDragOverCallback = std::function<void(int x, int y)>;
        void SetFileDragOverCallback(FileDragOverCallback cb) { m_fileDragOverCallback = std::move(cb); }

        /// @brief ドラッグがウィンドウ外へ出た / ドロップで終わった時のコールバック (ハイライト解除用)。
        using FileDragLeaveCallback = std::function<void()>;
        void SetFileDragLeaveCallback(FileDragLeaveCallback cb) { m_fileDragLeaveCallback = std::move(cb); }

        /// @brief FileDropTarget (OLE 実装) からコールバックを叩くための内部アクセサ。
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

        /// @brief クライアント寸法から枠込みのウィンドウ寸法を逆算して適用する。位置は動かさない (解像度を変えるたびにウィンドウが飛ぶのを避ける)。
        void ApplyWindowedClientSize(uint32_t width, uint32_t height);

        HWND     m_hwnd        = nullptr;
        uint32_t m_width       = 0;
        uint32_t m_height      = 0;
        bool     m_shouldClose = false;

        WindowMode m_windowMode = WindowMode::Windowed;
        /// @note WS_OVERLAPPEDWINDOW を戻すだけでは位置とサイズがフルスクリーン時のまま残り、ウィンドウへ戻した瞬間に画面いっぱいの枠付きウィンドウになるため、ボーダーレスへ入る直前のウィンドウ配置を覚える。
        WINDOWPLACEMENT m_windowedPlacement{ sizeof(WINDOWPLACEMENT) };
        bool            m_hasWindowedPlacement = false;
        /// @note ウィンドウモードで狙うクライアント寸法。フルスクリーン中に SetClientSize を呼ばれたら、ここへ溜めて復帰時に適用する。
        uint32_t m_windowedWidth  = 0;
        uint32_t m_windowedHeight = 0;

        ResizeCallback        m_resizeCallback;
        WndProcHook           m_wndProcHook;
        FileDropCallback      m_fileDropCallback;
        FileDragOverCallback  m_fileDragOverCallback;
        FileDragLeaveCallback m_fileDragLeaveCallback;
        HMENU m_nativeMenu = nullptr;
        NativeMenuCommandCallback m_nativeMenuCommandCallback;
        IDropTarget*          m_dropTarget = nullptr;   ///< OLE ドロップターゲット (RegisterDragDrop に登録)。Shutdown で Revoke + Release する。
        bool                  m_oleInitialized = false;
    };
} // namespace fbzz::core
