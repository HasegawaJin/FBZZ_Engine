// FBZZ Engine
// Window.cpp | fbzz::core
// Win32 ウィンドウの生成とメッセージ処理
// Input へのメッセージ転送、リサイズ通知、WndProc フックをまとめる。
// Renderer / ImGui とはコールバックで疎結合に接続する。

#include "Engine/Core/Window.hpp"
#include "Engine/Core/Logger.hpp"
#include "Engine/Input/Input.hpp"
#include "Engine/Util/FileSystem.hpp"

#include <dwmapi.h>
#include <windowsx.h>
#include <shellapi.h>
#include <ole2.h>
#include <oleidl.h>
#include <algorithm>
#include <cassert>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib") // IID_IDropTarget / IID_IUnknown

#ifndef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 ((DPI_AWARENESS_CONTEXT)-4)
#endif

// WM_DPICHANGED は Windows 8.1 SDK (WINVER >= 0x0603) 以降でのみ定義される。
// 古い SDK でビルドされても WndProc の case が消えないよう、値を明示して補う。
#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif

using namespace fbzz::core;

namespace
{
    constexpr wchar_t kWindowClassName[] = L"FBZZWindowClass";
    constexpr int kDefaultApplicationIconId = 101;

    void EnableDpiAwareness()
    {
        // 正規の宣言は CMake/FBZZApp.manifest 側。ローダーがプロセス起動時に適用するため、
        // ここへ来た時点で既に PerMonitorV2 が確定しており、この呼び出しは FALSE を返す
        // (ERROR_ACCESS_DENIED = 設定済み)。それが正常系。
        // WHY 呼び出しを残すか: マニフェストが剥がれたビルド (手製の exe、旧 SDK 経由の
        //     外部ゲーム) でも DPI 非対応のまま起動させないための保険。
        if (!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
            SetProcessDPIAware();

        // WHY 実際の値をログへ出すか: DPI 非対応のまま起動すると Windows がウィンドウ全体を
        //     ビットマップ拡大するため、100% 以外の環境で UI と文字が一律に滲む。
        //     この症状は「フォントが汚い」としか見えず、原因の切り分けに非常に手間がかかる。
        //     宣言の成否ではなく確定後の実値を残し、ログだけで判別できるようにする。
        const DPI_AWARENESS awareness =
            GetAwarenessFromDpiAwarenessContext(GetThreadDpiAwarenessContext());
        switch (awareness) {
        case DPI_AWARENESS_PER_MONITOR_AWARE:
            FBZZ_LOG_INFO("Window: DPI 認識 = Per-Monitor (system DPI=%u)", GetDpiForSystem());
            break;
        case DPI_AWARENESS_SYSTEM_AWARE:
            FBZZ_LOG_WARN("Window: DPI 認識 = System のみ。別 DPI のモニターへ移動すると滲みます");
            break;
        case DPI_AWARENESS_UNAWARE:
            FBZZ_LOG_ERROR("Window: DPI 非対応で起動しました — OS がウィンドウ全体を拡大するため "
                           "UI と文字が滲みます。マニフェスト (CMake/FBZZApp.manifest) の埋め込みを確認してください");
            break;
        default:
            FBZZ_LOG_WARN("Window: DPI 認識を判定できませんでした (awareness=%d)",
                          static_cast<int>(awareness));
            break;
        }
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

    // ── OLE ドロップターゲット ────────────────────────────────────────────────
    // WHY: WM_DROPFILES はドロップ確定時しか発火せず、ドラッグ中のカーソル位置が取れない。
    //      OLE の IDropTarget は DragEnter/DragOver でドロップ前の位置を通知できるため、
    //      Unity のように「落とす前にフォルダをハイライト」する体験を実現できる。

    // IDataObject から CF_HDROP のファイルパス群を UTF-8 で取り出す。
    std::vector<std::string> ExtractHdropPaths(IDataObject* data)
    {
        std::vector<std::string> paths;
        if (!data) return paths;
        FORMATETC fmt{ CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
        STGMEDIUM stg{};
        if (data->GetData(&fmt, &stg) != S_OK) return paths;
        if (HDROP hdrop = static_cast<HDROP>(GlobalLock(stg.hGlobal))) {
            const UINT count = DragQueryFileW(hdrop, 0xFFFFFFFFu, nullptr, 0);
            paths.reserve(count);
            for (UINT i = 0; i < count; ++i) {
                const UINT len = DragQueryFileW(hdrop, i, nullptr, 0);
                if (len == 0) continue;
                std::wstring wpath(len, L'\0');
                DragQueryFileW(hdrop, i, wpath.data(), len + 1);
                paths.push_back(fbzz::util::FileSystem::PathToUtf8(std::filesystem::path(wpath)));
            }
            GlobalUnlock(stg.hGlobal);
        }
        ReleaseStgMedium(&stg);
        return paths;
    }

    bool DataHasFiles(IDataObject* data)
    {
        if (!data) return false;
        FORMATETC fmt{ CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
        return data->QueryGetData(&fmt) == S_OK;
    }

    class FileDropTarget final : public IDropTarget
    {
    public:
        FileDropTarget(Window* window, HWND hwnd) : m_window(window), m_hwnd(hwnd) {}

        // IUnknown
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
        {
            if (riid == IID_IUnknown || riid == IID_IDropTarget) {
                *ppv = static_cast<IDropTarget*>(this);
                AddRef();
                return S_OK;
            }
            *ppv = nullptr;
            return E_NOINTERFACE;
        }
        ULONG STDMETHODCALLTYPE AddRef() override { return ++m_refCount; }
        ULONG STDMETHODCALLTYPE Release() override
        {
            const ULONG r = --m_refCount;
            if (r == 0) delete this;
            return r;
        }

        // IDropTarget
        HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data, DWORD, POINTL pt, DWORD* effect) override
        {
            m_hasFiles = DataHasFiles(data);
            if (effect) *effect = m_hasFiles ? DROPEFFECT_COPY : DROPEFFECT_NONE;
            if (m_hasFiles) NotifyOver(pt);
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL pt, DWORD* effect) override
        {
            if (effect) *effect = m_hasFiles ? DROPEFFECT_COPY : DROPEFFECT_NONE;
            if (m_hasFiles) NotifyOver(pt);
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE DragLeave() override
        {
            m_hasFiles = false;
            if (m_window) m_window->InvokeFileDragLeave();
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD, POINTL pt, DWORD* effect) override
        {
            if (effect) *effect = DROPEFFECT_COPY;
            POINT p{ pt.x, pt.y };
            ScreenToClient(m_hwnd, &p);
            const std::vector<std::string> paths = ExtractHdropPaths(data);
            if (m_window) {
                m_window->InvokeFileDrop(paths, p.x, p.y);
                m_window->InvokeFileDragLeave();
            }
            m_hasFiles = false;
            return S_OK;
        }

    private:
        void NotifyOver(POINTL pt)
        {
            POINT p{ pt.x, pt.y };
            ScreenToClient(m_hwnd, &p);
            if (m_window) m_window->InvokeFileDragOver(p.x, p.y);
        }

        Window* m_window  = nullptr;
        HWND    m_hwnd    = nullptr;
        ULONG   m_refCount = 1;
        bool    m_hasFiles = false;
    };

    HICON LoadApplicationIcon(HINSTANCE instance, int size)
    {
        // WHY: Window クラスにアイコンを設定しないと、exe に埋め込んだアイコンがタイトルバーや Alt+Tab に
        // 反映されない環境がある。LR_SHARED により HICON の寿命を OS 管理にして、Window 側の解放責務を持たない。
        return static_cast<HICON>(LoadImageW(
            instance,
            MAKEINTRESOURCEW(kDefaultApplicationIconId),
            IMAGE_ICON,
            size,
            size,
            LR_DEFAULTCOLOR | LR_SHARED));
    }

    void AppendNativeMenuItems(HMENU menu,
                               const std::vector<Window::NativeMenuItem>& items)
    {
        for (const auto& item : items) {
            if (item.separator) {
                AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
                continue;
            }

            if (!item.children.empty()) {
                HMENU submenu = CreatePopupMenu();
                AppendNativeMenuItems(submenu, item.children);
                AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(submenu), item.label.c_str());
                continue;
            }

            AppendMenuW(menu, MF_STRING, item.commandId, item.label.c_str());
        }
    }
}

bool Window::Initialize(const Config& config)
{
    EnableDpiAwareness();

    m_width  = config.width;
    m_height = config.height;

    WNDCLASSEXW wc{};
    HINSTANCE instance = GetModuleHandleW(nullptr);
    wc.cbSize        = sizeof(WNDCLASSEXW);
    wc.style         = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = instance;
    wc.hIcon         = LoadApplicationIcon(instance, GetSystemMetrics(SM_CXICON));
    wc.hIconSm       = LoadApplicationIcon(instance, GetSystemMetrics(SM_CXSMICON));
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
        instance,
        this);

    assert(m_hwnd && "Window creation failed");

    // ダークモード有効化
    BOOL darkMode = TRUE;
    DwmSetWindowAttribute(m_hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkMode, sizeof(darkMode));

    // エクスプローラーからのファイル D&D を OLE ドロップターゲットとして受け付ける。
    // WHY: OLE を使うことでドロップ確定前のドラッグオーバー位置を取得でき、取り込み先フォルダを
    //      リアルタイムでハイライトできる (WM_DROPFILES では不可能)。
    if (SUCCEEDED(OleInitialize(nullptr))) {
        m_oleInitialized = true;
        auto* target = new FileDropTarget(this, m_hwnd); // ref=1 (自分の参照)
        if (RegisterDragDrop(m_hwnd, target) == S_OK) {
            // RegisterDragDrop が AddRef 済み。自分の初期参照は手放し、OLE 側の 1 参照だけ残す。
            // Shutdown の RevokeDragDrop がその最後の参照を解放して delete させる。
            m_dropTarget = target;
            target->Release();
        } else {
            target->Release();
        }
    }

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

    // OLE ドロップターゲットを解除する。RevokeDragDrop が最後の参照を解放し FileDropTarget を delete する。
    if (m_dropTarget) {
        RevokeDragDrop(m_hwnd);
        m_dropTarget = nullptr;
    }
    if (m_oleInitialized) {
        OleUninitialize();
        m_oleInitialized = false;
    }

    ClearNativeMenu();
    DestroyWindow(m_hwnd);
    m_hwnd = nullptr;
}

void Window::SetTitle(const std::wstring& title)
{
    if (m_hwnd)
        SetWindowTextW(m_hwnd, title.c_str());
}

void Window::SetNativeMenu(std::vector<NativeMenuItem> menus,
                           NativeMenuCommandCallback callback)
{
    ClearNativeMenu();
    if (!m_hwnd) return;

    m_nativeMenu = CreateMenu();
    if (!m_nativeMenu) return;
    AppendNativeMenuItems(m_nativeMenu, menus);
    m_nativeMenuCommandCallback = std::move(callback);
    SetMenu(m_hwnd, m_nativeMenu);
    DrawMenuBar(m_hwnd);
}

void Window::ClearNativeMenu()
{
    if (m_hwnd) {
        SetMenu(m_hwnd, nullptr);
        DrawMenuBar(m_hwnd);
    }
    if (m_nativeMenu) {
        DestroyMenu(m_nativeMenu);
        m_nativeMenu = nullptr;
    }
    m_nativeMenuCommandCallback = {};
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

    if (window && msg == WM_COMMAND && HIWORD(wParam) == 0 && lParam == 0
        && window->m_nativeMenuCommandCallback) {
        if (window->m_nativeMenuCommandCallback(LOWORD(wParam)))
            return 0;
    }

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

    // DPI の異なるモニターへ移動した / 表示スケールが変更された。
    case WM_DPICHANGED:
    {
        // WHY 推奨矩形へ追従させるか: PER_MONITOR_AWARE_V2 が OS 側で自動処理するのは
        //     非クライアント領域 (タイトルバー・枠) の再スケールまで。ウィンドウ本体の
        //     寸法を新 DPI へ合わせるのはアプリの責務で、ここで何もしないと物理ピクセル数が
        //     据え置かれ、移動先モニターでエディター全体が相対的に小さく (または大きく) なる。
        // lParam = OS が算出した推奨ウィンドウ矩形 (新 DPI・フレーム込みの物理ピクセル)。
        const auto* suggested = reinterpret_cast<const RECT*>(lParam);
        if (!suggested) return 0;

        SetWindowPos(hwnd, nullptr,
                     suggested->left, suggested->top,
                     suggested->right - suggested->left,
                     suggested->bottom - suggested->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        // NOTE: クライアント寸法が変わればこの SetWindowPos から WM_SIZE が届き、
        //       そこで Application 登録のコールバックがスワップチェーンを再構築する。
        //       ここで直接 Resize を呼ぶと同じ寸法で二重に走るため、WM_SIZE に任せる。
        // NOTE: ImGui のフォント/スタイル倍率 (EditorTheme::SetUiScale) はユーザーの明示設定なので、
        //       DPI 変更で勝手に上書きしない。物理解像度への追従だけをここで担う。
        return 0;
    }

    case WM_CHAR:
        fbzz::input::Input::HandleTextInput(static_cast<wchar_t>(wParam));
        return 0;

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
    // WHY: エクスプローラーからのファイルドロップは OLE の IDropTarget (FileDropTarget) で扱う。
    //      WM_DROPFILES は使わない (ドラッグ中の位置が取れずハイライトできないため)。

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}
