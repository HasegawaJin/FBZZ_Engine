# FBZZ Engine - Claude への指示

詳細設計は `docs/Design.md` を参照。以下の方針から外れないようにすること。

---

## 絶対に守ること

- **数学ライブラリは自作** (GLM 不使用)
- **物理エンジンは自作** (Bullet / PhysX 不使用)
- **ヘッダファイルの拡張子は `.hpp`**
- **C++20** を使用
- **名前空間は `fbzz::`** (例: `fbzz::physics`, `fbzz::renderer`)
- **`#pragma once`** をすべてのヘッダファイル先頭に記述

---

## アーキテクチャ方針

- レンダラーは `IRenderer` インターフェース経由。DX11 具体実装に直接依存しない
- シーン管理: Unity 同様の GameObject / Component パターン (OOP)
- 依存方向: `sandbox → engine → physics → math` (逆転禁止)

---

## サードパーティライブラリ

OK: Assimp / DirectX 11 SDK / DirectX 12 SDK / `Microsoft::WRL::ComPtr`  
NG: GLM / GLFW / Bullet / PhysX / Box2D

---

## コーディング規約

| 対象 | 規則 | 例 |
|------|------|-----|
| クラス名 | `PascalCase` | `RigidBody`, `ShaderManager` |
| 関数名 | `PascalCase` | `ApplyForce()`, `GetComponent()` |
| 変数名 | `lowerCamelCase` | `deltaTime`, `vertexCount` |
| メンバ変数 | `m_` + `lowerCamelCase` | `m_position`, `m_isStatic` |
| 定数 / enum | `UPPER_SNAKE_CASE` | `MAX_LIGHTS`, `KEY_ESCAPE` |
| インターフェース | `I` + `PascalCase` | `IRenderer`, `IBuffer` |
| 名前空間 | `snake_case` | `fbzz::physics`, `fbzz::renderer` |
| ソース・ヘッダファイル | `PascalCase` | `RigidBody.cpp`, `IRenderer.hpp` |
| ディレクトリ | `PascalCase` | `Renderer/`, `Core/`, `DX11/` |

コメントは WHY が自明でないときのみ書く。WHAT は書かない。

### ファイルヘッダーコメント

すべての `.hpp` / `.cpp` の先頭、`#pragma once` の**直前**に記述する。

```cpp
// FBZZ Engine
// Vector3.hpp | fbzz::math
// 3次元ベクトルの演算と定数
```

---

## 所有権モデル (詳細: `docs/conventions/ownership.md`)

| スマートポインタ | 使う場面 |
|----------------|---------|
| `std::unique_ptr` | デフォルト。1 箇所が所有するリソース (Component, GameObject 等) |
| `std::shared_ptr` | 複数システム共有リソース (IBuffer, IShader, RigidBody) |
| raw pointer (`T*`) | 非所有の参照のみ (m_owner, m_parent 等) |

`new` / `delete` 直接使用禁止。`make_unique` / `make_shared` を使う。

---

## エラーハンドリング (詳細: `docs/conventions/error_handling.md`)

- 回復不可能エラー: `assert()`
- 回復可能エラー: `bool` 戻り値
- DX11 HRESULT: `FBZZ_HR_CHECK(hr)` マクロ
- `throw` / `std::exception` は**禁止**

---

## スレッドモデル (詳細: `docs/conventions/threading.md`)

Step 1〜5 はシングルスレッド。`std::thread` / `std::mutex` / `std::atomic` を engine / physics / math に持ち込まない。

---

## C++20

OK: `std::span` / Concepts / Designated initializers / `[[nodiscard]]` / `constexpr` / `consteval`  
NG: Modules / Coroutines / Ranges

---

## 禁止パターン

| パターン | 理由 |
|---------|------|
| `dynamic_cast` | RTTI コスト。`GetComponent<T>()` で代替 |
| `reinterpret_cast` | シェーダー定数バッファ転送以外で禁止 |
| グローバル変数 | `Application` シングルトン以外禁止 |
| `new` / `delete` 直接使用 | `make_unique` / `make_shared` を使う |
| `throw` / `std::exception` | エンジンコード内では禁止 |
| `#include` の循環依存 | 前方宣言 (`class Foo;`) で解決 |
| `DX11Renderer*` へのダウンキャスト | 上位レイヤーは `IRenderer&` のみ参照 |

---

## Git 運用 (詳細: `docs/conventions/git.md`)

```
main → develop → feature/<name>
```

コミット形式: `[Feature] / [Fix] / [Design] / [Build] / [Refactor] / [Release] + 動詞 + 概要`

コミットの Description (本文) は **Markdown で記述する**。
見出し (`##`) と箇条書き (`-`) を使って構造化すること。

---

## Claude への作業指針

### コードを書く前に必ずやること

1. `docs/` 以下の対応する設計ドキュメントを読む
2. `docs/design.md` のロードマップで現在の Step を確認する。未着手 Step は実装しない
3. 複数ファイルにまたがる変更は、先にユーザーへ列挙する

### してはいけないこと

- 設計書に記載のないクラス・関数をユーザーの確認なしに追加しない
- Step を飛び越えて実装しない
- 設計書とコードが矛盾している場合、自分で判断して直さず報告してから対処する
- `docs/` 以下の設計ドキュメントをコード実装のついでに書き換えない

### 曖昧な指示を受けたとき

対応する設計ドキュメントが存在すれば従う。なければ実装せずに選択肢を提示する。

### ビルド確認

**ビルドは必ず Visual Studio 2026 から行う。**  
ターミナル (PowerShell / Bash) から `ninja` や `cmake --build` を実行しない。  
MSVC の環境変数 (vcvarsall.bat) が設定されていないためコンパイルエラーになる。

- ビルド: VS 2026 で `Ctrl+Shift+B` (ソリューション全体のリビルド)
- 実行: VS のデバッガー or 生成された `.exe` を直接起動
- Claude がビルド結果を確認する必要がある場合は、ユーザーにビルドを依頼してエラー出力を貼ってもらう

#### VS 更新後に CMake Configure が失敗する場合

`CMAKE_CXX_COMPILER` のフルパスが cmake キャッシュに残るため、VS (MSVC ツールセット) を更新すると古いパスを参照してエラーになる。  
対処: `build/debug/` と `build/release/` を削除してから VS Code で再 Configure する。  
VS Code CMake Tools の場合は `...` → **Delete Cache and Reconfigure**。

### トークン節約ルール

- ファイルを読む前に Grep / Glob でファイルを特定する
- 修正が局所的なら Write より **Edit** を使う (差分のみ送信)
- 変更していないコードブロックを応答に再掲しない
- 大きな設計書は `limit` / `offset` で関連セクションのみ読む
- 独立した調査・ファイル操作は並行ツール呼び出しでまとめる

---

## ロードマップ (現在地を把握すること)

```
Step 0  ビルド環境・Application ループ         完了
Step 1  Win32 ウィンドウ表示 + DX11 初期化     完了
Step 2  三角形描画 (頂点バッファ, シェーダー)   完了
Step 3  デバッグ描画 (線, 矩形, 円)             完了
Step 4  物理エンジン (重力, 衝突)               完了
Step 5  シーン管理 (GameObject / Component)     完了
Step 6  DX12 / レイトレーシング移行             延期
Step 7  ImGui Editor
```
