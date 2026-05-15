# Engine / Input

`fbzz::input::Input`。キーボード・マウスの入力状態を管理する静的クラス。
Win32 メッセージ (`WM_KEYDOWN` 等) を `Window::WndProc` から受け取って状態を更新する。

---

## KeyCode

```cpp
namespace fbzz::input {

enum class KeyCode : uint32_t {
    // 文字キー
    A = 'A', B = 'B', /* ... */ Z = 'Z',

    // 数字
    KEY_0 = '0', KEY_1 = '1', /* ... */ KEY_9 = '9',

    // 特殊キー (Win32 仮想キーコードをそのまま使う)
    ESCAPE    = VK_ESCAPE,
    SPACE     = VK_SPACE,
    ENTER     = VK_RETURN,
    BACKSPACE = VK_BACK,
    TAB       = VK_TAB,
    SHIFT     = VK_SHIFT,
    CTRL      = VK_CONTROL,
    ALT       = VK_MENU,
    LEFT      = VK_LEFT,
    RIGHT     = VK_RIGHT,
    UP        = VK_UP,
    DOWN      = VK_DOWN,
    F1        = VK_F1, /* ... */ F12 = VK_F12,
};

} // namespace fbzz::input
```

---

## Input クラス

```cpp
namespace fbzz::input {

class Input {
public:
    // 初期化 (Window 生成後に呼ぶ)
    static void Init(HWND hwnd);

    // ゲームループ先頭で呼ぶ。前フレームの状態を保存する
    static void Update();

    // キーボード
    static bool KeyDown(KeyCode key);    // 押した瞬間 (1 フレームのみ true)
    static bool KeyHeld(KeyCode key);    // 押し続け
    static bool KeyUp(KeyCode key);      // 離した瞬間 (1 フレームのみ true)

    // マウスボタン (0=左, 1=右, 2=中)
    static bool MouseButton(int button);
    static bool MouseButtonDown(int button);
    static bool MouseButtonUp(int button);

    // マウス座標 (スクリーン座標)
    static math::Vector2 MousePosition();
    static math::Vector2 MouseDelta();      // 前フレームからの移動量

    // WndProc から呼ばれる内部関数
    static void HandleKeyMessage(UINT msg, WPARAM wParam);
    static void HandleMouseMove(int x, int y);
    static void HandleMouseButton(UINT msg, WPARAM wParam);

private:
    static constexpr int KEY_COUNT = 256;

    static std::array<bool, KEY_COUNT> s_currentKeys;
    static std::array<bool, KEY_COUNT> s_previousKeys;
    static std::array<bool, 3>         s_currentMouse;
    static std::array<bool, 3>         s_previousMouse;
    static math::Vector2                  s_mousePos;
    static math::Vector2                  s_prevMousePos;
};

} // namespace fbzz::input
```

---

## 状態遷移

```
フレーム N:  キー押下   → s_currentKeys[key] = true
フレーム N:  Update()  → s_previousKeys = s_currentKeys のコピー

KeyDown  = current && !previous
KeyHeld  = current
KeyUp    = !current && previous
```

---

## 使用例

```cpp
// Application::Run() 内
Input::Update();

if (Input::KeyDown(KeyCode::ESCAPE)) {
    Quit();
}

if (Input::KeyHeld(KeyCode::W)) {
    camera.m_position += camera.GetForward() * speed * dt;
}

math::Vector2 delta = Input::MouseDelta();
yaw   += delta.x * sensitivity;
pitch += delta.y * sensitivity;
```

---

## ファイル構成

```
engine/
├── include/engine/
│   └── Input/
│       ├── Input.hpp
│       └── KeyCode.hpp
└── src/
    └── Input/
        └── Input.cpp
```

---

## 参考ドキュメント

- [仮想キーコード一覧](https://learn.microsoft.com/en-us/windows/win32/inputdev/virtual-key-codes) — VK_ESCAPE / VK_SPACE 等の定数
- [WM_KEYDOWN](https://learn.microsoft.com/en-us/windows/win32/inputdev/wm-keydown) — キー押下メッセージ
- [WM_KEYUP](https://learn.microsoft.com/en-us/windows/win32/inputdev/wm-keyup) — キー離しメッセージ
- [WM_MOUSEMOVE](https://learn.microsoft.com/en-us/windows/win32/inputdev/wm-mousemove) — マウス移動メッセージ
- [WM_LBUTTONDOWN](https://learn.microsoft.com/en-us/windows/win32/inputdev/wm-lbuttondown) — マウスボタンメッセージ
- [GET_X_LPARAM / GET_Y_LPARAM](https://learn.microsoft.com/en-us/windows/win32/api/windowsx/nf-windowsx-get_x_lparam) — lParam からマウス座標を取得するマクロ
