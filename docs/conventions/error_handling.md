# エラーハンドリング規約

FBZZ Engine 全モジュール共通のエラー処理方針。

---

## 基本方針

| エラーの種類 | 処理方法 |
|-------------|---------|
| プログラミングミス (nullptrアクセス、前提条件違反) | `assert()` |
| 回復可能な実行時エラー (ファイル不在、リソース確保失敗) | `bool` 戻り値 + ログ出力 |
| DX11 HRESULT | `FBZZ_HR_CHECK` マクロ |

**`throw` / `std::exception` はエンジンコード内で使用禁止。**

---

## assert の使い方

`assert` はプログラマーが「絶対にこうなるはず」と保証する不変条件に使う。
ユーザー入力やファイル読み込み結果には使わない。

```cpp
// 良い例: 内部的に保証されているはずのケース
assert(m_device != nullptr && "Init() を呼ばずに描画しようとしている");

// 悪い例: 外部リソースの読み込み失敗に assert を使う
assert(SUCCEEDED(hr) && "シェーダー読み込み失敗");  // NG: ファイルが存在しないだけで落ちる
```

---

## bool 戻り値パターン

回復可能なエラーは `bool` で返し、呼び出し元でハンドリングする。
失敗時は `false` を返す前に原因をログ出力する。

```cpp
bool DX11Shader::Init(ID3D11Device* device, const std::string& path) {
    HRESULT hr = D3DCompileFromFile(...);
    FBZZ_HR_CHECK(hr);  // 失敗なら false を返す

    // ... 続きの初期化
    return true;
}

// 呼び出し側
if (!shader->Init(device, "shaders/Basic.hlsl")) {
    // フォールバック or 起動を中断
    return false;
}
```

---

## FBZZ_HR_CHECK マクロ

DX11 の HRESULT チェック専用マクロ。`FAILED(hr)` のとき即 `false` を返す。
デバッグビルドではブレークポイントを仕込む。

```cpp
// engine/include/engine/Core/HResult.hpp

#pragma once
#include <cassert>

#ifdef NDEBUG
    #define FBZZ_HR_CHECK(hr)                                      \
        do {                                                        \
            if (FAILED(hr)) { return false; }                       \
        } while (0)
#else
    #define FBZZ_HR_CHECK(hr)                                      \
        do {                                                        \
            if (FAILED(hr)) { __debugbreak(); return false; }       \
        } while (0)
#endif
```

戻り値が `bool` でない関数内で使う場合は専用のマクロを追加する。
`void` 関数では `FBZZ_HR_CHECK_VOID(hr)` を定義して `return;` とする。

---

## Init / Shutdown パターン

DX11 リソースを扱うクラスはコンストラクタでリソース確保をしない。
`bool Init(...)` と `void Shutdown()` に分離する。

```cpp
class DX11Renderer {
public:
    bool Init(HWND hwnd, uint32_t w, uint32_t h);  // 失敗なら false
    void Shutdown();                                 // noexcept, 冪等

    // コンストラクタ・デストラクタは最小限
    DX11Renderer()  = default;
    ~DX11Renderer() { Shutdown(); }
};
```

コンストラクタで `Init` を呼ばない理由: HRESULT をコンストラクタから返せないため。

---

## ログ出力 (将来実装)

現時点では `OutputDebugStringA` / `printf` で代替する。
将来 `fbzz::core::Logger` を実装したら置き換える。

```cpp
// 暫定ログ (Step 1〜2 まで)
OutputDebugStringA("[FBZZ] シェーダー読み込み失敗\n");
```

---

## ファイル構成

```
engine/
└── include/engine/
    └── Core/
        └── HResult.hpp    FBZZ_HR_CHECK マクロの定義
```
