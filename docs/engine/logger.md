# Engine / Logger

`fbzz::core::Logger`。ログレベル付きのデバッグ出力を管理する静的クラス。
Release ビルドではマクロごと除去される。

---

## ログレベル

```cpp
namespace fbzz::core {

enum class LogLevel {
    INFO      = 0,
    WARNING   = 1,
    LOG_ERROR = 2,   // ERROR は Win32 マクロと衝突するため LOG_ERROR を使う
};

} // namespace fbzz::core
```

---

## Logger クラス

```cpp
namespace fbzz::core {

class Logger {
public:
    static void Info (const char* fmt, ...);
    static void Warn (const char* fmt, ...);
    static void Error(const char* fmt, ...);

    // INFO 未満のログを抑制する (デフォルト: INFO)
    static void SetMinLevel(LogLevel level);

private:
    static LogLevel s_minLevel;
    static void     Log(LogLevel level, const char* fmt, va_list args);
};

} // namespace fbzz::core
```

---

## マクロ

ファイル名・行番号を自動付与する。Release ビルド (`NDEBUG`) では何もしない。

```cpp
// engine/include/engine/Core/Logger.hpp

#ifdef NDEBUG
    #define FBZZ_LOG_INFO(fmt, ...)  ((void)0)
    #define FBZZ_LOG_WARN(fmt, ...)  ((void)0)
    #define FBZZ_LOG_ERROR(fmt, ...) ((void)0)
#else
    #define FBZZ_LOG_INFO(fmt, ...)  ::fbzz::core::Logger::Info ("[%s:%d] " fmt, __FILE__, __LINE__, ##__VA_ARGS__)
    #define FBZZ_LOG_WARN(fmt, ...)  ::fbzz::core::Logger::Warn ("[%s:%d] " fmt, __FILE__, __LINE__, ##__VA_ARGS__)
    #define FBZZ_LOG_ERROR(fmt, ...) ::fbzz::core::Logger::Error("[%s:%d] " fmt, __FILE__, __LINE__, ##__VA_ARGS__)
#endif
```

---

## 出力形式

Visual Studio のデバッグ出力ウィンドウ (`OutputDebugStringA`) に出力する。

```
[FBZZ INFO]  [Window.cpp:42] ウィンドウ生成完了: 1280x720
[FBZZ WARN]  [DX11Shader.cpp:88] シェーダーキャッシュにヒットしませんでした: Phong.hlsl
[FBZZ ERROR] [DX11Renderer.cpp:55] デバイス生成失敗 (HRESULT: 0x80004005)
```

---

## FBZZ_HR_CHECK との連携

`HResult.hpp` の `FBZZ_HR_CHECK` に Logger を組み込む。

```cpp
#define FBZZ_HR_CHECK(hr)                                              \
    do {                                                                \
        if (FAILED(hr)) {                                               \
            FBZZ_LOG_ERROR("HRESULT 失敗: 0x%08X", (unsigned)(hr));    \
            __debugbreak();                                             \
            return false;                                               \
        }                                                               \
    } while (0)
```

---

## 使用例

```cpp
bool DX11Renderer::Init(HWND hwnd, uint32_t w, uint32_t h) {
    HRESULT hr = D3D11CreateDeviceAndSwapChain(...);
    FBZZ_HR_CHECK(hr);

    FBZZ_LOG_INFO("DX11 デバイス生成完了: %ux%u", w, h);
    return true;
}

bool DX11Shader::Init(ID3D11Device* device, const std::string& path) {
    if (!std::filesystem::exists(path)) {
        FBZZ_LOG_WARN("シェーダーファイルが見つかりません: %s", path.c_str());
        return false;
    }
    // ...
}
```

---

## ファイル構成

```
engine/
├── include/engine/
│   └── Core/
│       └── Logger.hpp
└── src/
    └── Core/
        └── Logger.cpp
```

---

## 参考ドキュメント

- [OutputDebugStringA](https://learn.microsoft.com/en-us/windows/win32/api/debugapi/nf-debugapi-outputdebugstringa) — VS デバッグ出力ウィンドウへの出力
- [va_list / va_start (cppreference)](https://en.cppreference.com/w/cpp/utility/variadic) — 可変長引数の処理
- [vsprintf_s](https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/vsprintf-s-vsprintf-s-l-vswprintf-s-vswprintf-s-l) — フォーマット文字列をバッファに書き出す
