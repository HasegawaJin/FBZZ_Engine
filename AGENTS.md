# FBZZ Engine - Claude / Codex への指示

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

**このリポジトリはポートフォリオとして公開する。コードを読む採用担当者・レビュアーが設計意図を理解できるよう、コメントは自明か否かに関わらず積極的に書くこと。**

- **言語**: 日本語で書く
- **WHY (なぜその設計か)**: 制約・トレードオフ・前提をすべて記述する
- **WHAT (何をしているか)**: 処理の概要・アルゴリズム・数式を説明する
- **関数・クラス**: 目的と責務を必ず一行以上書く

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

## メモリ管理
    スマートポインタ + MemoryDebug  (CustomMemoryAllocator)
---

## C++20

OK: `std::span` / Concepts / Designated initializers / `[[nodiscard]]` / `constexpr` / `consteval`  
NG: Modules / Coroutines / Ranges

---

## Script 便利 API (Engine/Scene/Script.hpp)

`Script` 基底クラスは Unity の `MonoBehaviour` に相当する便利メソッドを提供する。
ユーザースクリプトでは `m_gameObject->` / `m_scene->` の代わりにこれらを使う。

| メソッド | 説明 |
|---|---|
| `GetComponent<T>()` | `m_gameObject->GetComponent<T>()` の短縮形 |
| `transform->position` | 自 GO の Transform への直接ポインタ (SetContext で設定) |
| `Find(name)` / `FindWithTag(tag)` | `m_scene->Find / FindWithTag` の短縮形 |
| `GetGameObject(id)` | EntityID から GO を取得 |
| `CreateGameObject(name)` | `m_scene->CreateGameObject` の短縮形 |
| `GetMainCameraObject()` | シーン内のメインカメラ GO を返す |
| `Destroy(go, delay)` | `GameObject::Destroy` の短縮形 |
| `SetAnimatorFloat/Int/Bool/Trigger` | 自 GO の AnimatorComponent に転送 |
| `IsAnimatorInState(name)` | 現在のアニメーターステートを確認 |

`GetComponent<T>()` の template 定義は循環依存回避のため `Scene.hpp` 末尾に置く。

---

## Sandbox スクリプトの using namespace 規則

`Projects/Sandbox/src/Scripts/` 以下のヘッダファイルに限り、以下の `using` を許可する。

```cpp
using namespace fbzz::scene;
using namespace fbzz::math;
using namespace fbzz::input;
```

**WHY**: Sandbox スクリプトはエンジン層からインクルードされない末端ヘッダであり、
名前空間汚染が生じない。エンジン側の `.hpp` での `using namespace` は引き続き禁止。

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

コミット形式: `[Feature] / [Fix] / [Design] / [Build] / [Refactor] / [Chore] / [Release] + 動詞 + 概要`

コミットの Description (本文) は **Markdown で記述する**。
見出し (`##`) と箇条書き (`-`) を使って構造化すること。

---

### 曖昧な指示を受けたとき

対応する設計ドキュメントが存在すれば従う。なければ実装せずに選択肢を提示する。

### ビルド確認

**ビルドは　VSCode / Visual Studio 2026 から行う。**  
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
