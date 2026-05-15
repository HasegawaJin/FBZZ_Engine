# Engine / Window

`fbzz::core::Window`。Win32 API でウィンドウを生成・管理する。
GLFW は使用しない (CLAUDE.md 参照)。

---

## クラス定義

```cpp
namespace fbzz::core {

class Window {
public:
    struct Desc {
        std::wstring title  = L"FBZZ Engine";
        uint32_t     width  = 1280;
        uint32_t     height = 720;
    };

    bool Init(const Desc& desc);
    void Shutdown();

    // メッセージポンプ。WM_QUIT を受け取ったら ShouldClose() が true になる
    void PollEvents();

    bool     ShouldClose() const { return m_shouldClose; }
    HWND     GetHandle()   const { return m_hwnd; }
    uint32_t GetWidth()    const { return m_width; }
    uint32_t GetHeight()   const { return m_height; }

    // リサイズコールバック (DX11Renderer が登録する)
    using ResizeCallback = std::function<void(uint32_t, uint32_t)>;
    void SetResizeCallback(ResizeCallback cb) { m_resizeCallback = cb; }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg,
                                    WPARAM wParam, LPARAM lParam);

    HWND     m_hwnd        = nullptr;
    uint32_t m_width       = 0;
    uint32_t m_height      = 0;
    bool     m_shouldClose = false;

    ResizeCallback m_resizeCallback;
};

} // namespace fbzz::core
```

---

## Win32 初期化フロー

```cpp
bool Window::Init(const Desc& desc) {
    // 1. WNDCLASSEX を登録
    WNDCLASSEXW wc = {};
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = GetModuleHandle(nullptr);
    wc.lpszClassName = L"FBZZEngineWindow";
    RegisterClassExW(&wc);

    // 2. ウィンドウ生成
    m_hwnd = CreateWindowExW(
        0, L"FBZZEngineWindow", desc.title.c_str(),
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        desc.width, desc.height,
        nullptr, nullptr, wc.hInstance, this
    );

    ShowWindow(m_hwnd, SW_SHOW);
    UpdateWindow(m_hwnd);
    return m_hwnd != nullptr;
}
```

---

## メッセージポンプ

```cpp
void Window::PollEvents() {
    MSG msg = {};
    while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            m_shouldClose = true;
        }
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
}
```

---

## WndProc での入力イベント

キーボード・マウスのメッセージは `Input` モジュールに転送する。

```cpp
LRESULT CALLBACK Window::WndProc(HWND hwnd, UINT msg,
                                  WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;

        case WM_SIZE: {
            uint32_t w = LOWORD(lParam), h = HIWORD(lParam);
            auto* wnd = reinterpret_cast<Window*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
            if (wnd && wnd->m_resizeCallback) wnd->m_resizeCallback(w, h);
            return 0;
        }

        case WM_KEYDOWN:
        case WM_KEYUP:
            Input::HandleKeyMessage(msg, wParam);
            return 0;

        case WM_MOUSEMOVE:
            Input::HandleMouseMove(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}
```

---

## ファイル構成

```
engine/
├── include/engine/
│   └── Core/
│       └── Window.hpp
└── src/
    └── Core/
        └── Window.cpp
```
