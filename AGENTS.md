# FBZZ Engine - AI エージェントへの指示

以下の方針から外れないようにすること。

---

## 絶対に守ること

- **数学ライブラリは自作** (GLM 不使用)
- **物理エンジンは自作** (Bullet / PhysX 不使用)
- **ヘッダファイルの拡張子は `.hpp`**
- **C++20** を使用
- **名前空間は `fbzz::`** (例: `fbzz::physics`, `fbzz::renderer`, `fbzz::scene`)
- **`#pragma once`** をすべてのヘッダファイル先頭に記述

---

## アーキテクチャ方針

- レンダラーは `IRenderer` インターフェース経由。`DX11Renderer*` へのダウンキャスト禁止
- シーン管理: Unity 同様の GameObject / Component パターン
- 依存方向: `Sandbox / Editor / GameHub → Engine → Physics → Math` (逆転禁止)
- スクリプトは `ScriptProxy` 経由でエンジン機能にアクセス。DLL 境界を超えて Engine 実装に直接依存しない
- 新しい Engine 機能をスクリプトに公開する場合は `Engine/include/Engine/Scene/ScriptProxy/ScriptXxxProxy.hpp` を追加する

---

## サードパーティライブラリ

OK: `Assimp` / `DirectX 11 SDK` / `DirectX 12 SDK` / `Microsoft::WRL::ComPtr` / `ImGui` / `ImGuizmo` / `ImNodes` / `toml++` / `stb_image` / `stb_truetype` / `stb_rect_pack` / `DirectXTex` / `XAudio2`

NG: `GLM` / `GLFW` / `Bullet` / `PhysX` / `Box2D`

`stb_truetype` / `stb_rect_pack` はフォントの実行時ラスタライズとアトラス配置に使う (`Docs/design/font-system.md`)。
**WHY**: 「数学・物理は自作」という方針は保つが、TrueType の字形解釈は別ドメインであり、
glyf / loca / cmap / hmtx の自前パースはフォント機能とは別スケールの投資になる。
`stb_image` を既に許可している方針と整合させ、public domain の単一ヘッダを `ThirdParty/Stb/` へベンダーする。

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

---

## コメント規約

**このリポジトリはポートフォリオとして公開する。採用担当者・レビュアーが設計意図を理解できるよう、コメントは自明か否かに関わらず積極的に書くこと。**

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

## 所有権モデル (詳細: `Docs/conventions/ownership.md`)

| スマートポインタ | 使う場面 |
|----------------|---------|
| `std::unique_ptr` | デフォルト。1 箇所が所有するリソース (Component, GameObject 等) |
| `std::shared_ptr` | 複数システム共有リソース (IBuffer, IShader, RigidBody) |
| raw pointer (`T*`) | 非所有の参照のみ (m_owner, m_parent 等) |

`new` / `delete` 直接使用禁止。`make_unique` / `make_shared` を使う。

---

## エラーハンドリング (詳細: `Docs/conventions/error_handling.md`)

- 回復不可能エラー: `assert()`
- 回復可能エラー: `bool` 戻り値
- DX11 HRESULT: `FBZZ_HR_CHECK(hr)` マクロ
- `throw` / `std::exception` は**禁止**

---

## メモリシステム (`Engine/include/Engine/Core/Memory/`)

4 種のカスタムアロケーターを用途に応じて使い分ける。通常のヒープ確保には `make_unique` / `make_shared` を使い、カスタムアロケーターは高頻度・大量確保の場合にのみ使う。

| アロケーター | 用途 |
|------------|------|
| `FrameAllocator` | フレームごとにリセットされる一時バッファ (毎フレームの描画コマンド等) |
| `LinearAllocator` | 前端から順番に確保。解放は一括のみ |
| `PoolAllocator` | 同サイズオブジェクトを大量生成する場合 (パーティクル・コライダー等) |
| `StackAllocator` | マーカーによる部分解放が必要な場合 |

リークは `MemoryTracker` / `MemoryDebug` で検出できる。

---

## C++20

OK: `std::span` / Concepts / Designated initializers / `[[nodiscard]]` / `constexpr` / `consteval`

NG: Modules / Coroutines / Ranges

---

## システムスケジューラー (`Engine/include/Engine/Core/Scheduler/`)

新しいシステムを追加する際は `SystemScheduler.cpp` に `Phase` と `ComponentAccess` を登録する。

- **Phase**: システムの実行フェーズ (PrePhysics / Physics / PostPhysics / Render / UI / Script)
- **ComponentAccess**: 読み書きするコンポーネント型を宣言する (並列実行の安全性確保)

---

## アセット形式 (`Engine/include/Engine/Asset/`)

| 拡張子 | 定義ファイル | 内容 |
|--------|------------|------|
| `.fzasset` | `FzAssetFormat.hpp` | 汎用アセットバイナリ (メタデータ + ペイロード) |
| `.mesh` | `FzModelFormat.hpp` | メッシュサブアセット (FBX からエクスポート) |
| `.scene` | `SceneSerializer.hpp` | TOML ベースのシーンファイル |
| `.terrain` | `FzTerrainFormat.hpp` | テレインアセット (ハイトマップ + レイヤー情報) |
| `.mat` | `MatAssetImporter.hpp` | マテリアルアセット |
| `.tex` | `TexDescSerializer.hpp` | テクスチャデスクリプター |
| `.animcontroller` | `AnimatorControllerAsset.hpp` | アニメーションステートマシン |

---

## スクリプティングシステム

### .generated.hpp パターン

`Assets/Scripts/XxxComponent.hpp` にコンポーネント定義を書くと、エディターの `ScriptCodeGen` が `XxxComponent.generated.hpp` を自動生成する。**手動で `.generated.hpp` を編集しない**。

### ScriptProxy の追加手順

スクリプトから新しいエンジン機能を使えるようにする場合:

1. `Engine/include/Engine/Scene/ScriptProxy/ScriptXxxProxy.hpp` を追加
2. `Engine/src/Scene/ScriptProxies.cpp` に実装を追加
3. `Engine/include/Engine/Scene/Script.hpp` のプロキシ一覧に追加

DLL 境界を越えるため、プロキシのインターフェースには Engine 内部型を直接露出しないこと。

### 既存の ScriptProxy 一覧 (`Engine/include/Engine/Scene/ScriptProxy/`)

| プロキシ | 提供機能 |
|---------|---------|
| `ScriptTransformProxy` | 位置・回転・スケールの取得・設定 |
| `ScriptPhysicsProxy` | 力の印加・RigidBody 操作 |
| `ScriptAnimatorProxy` | アニメーターパラメーターの読み書き |
| `ScriptCameraProxy` | カメラ FOV・ターゲット設定 |
| `ScriptInputProxy` | キー・マウス入力の取得 |
| `ScriptSceneProxy` | GameObject 検索・生成・破棄 |
| `ScriptAudioProxy` | サウンドの再生・停止 |
| `ScriptLightProxy` | ライトパラメーターの変更 |
| `ScriptMaterialProxy` | マテリアルプロパティの動的書き換え |
| `ScriptParticleProxy` | パーティクルの発生制御 |
| `ScriptNavigationProxy` | NavMesh エージェントの目標設定 |
| `ScriptTrailProxy` / `ScriptMeshTrailProxy` | トレイルエフェクト制御 |
| `ScriptUIProxy` | UI テキスト・画像の更新 |
| `ScriptPostProcessProxy` | ポストプロセスパラメーターの変更 |
| `ScriptDebugProxy` | デバッグ描画 |
| `ScriptMemoryProxy` | カスタムアロケーター経由のメモリ確保 |
| `GizmoProxy` | Gizmo のエディター描画 |

### Script 基底クラスの便利 API (`Engine/include/Engine/Scene/Script.hpp`)

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

**WHY**: Sandbox スクリプトはエンジン層からインクルードされない末端ヘッダであり、名前空間汚染が生じない。エンジン側の `.hpp` での `using namespace` は引き続き禁止。

---

## Assets/ に置く 3 種類のヘッダ

`Assets/**/*.hpp` は CMake の GLOB がすべて Script DLL のビルド対象へ入れるが、**登録されるかどうかはマクロで決まる**。ScriptCodeGen が `FBZZ_SCRIPT(` / `FBZZ_DATA_ASSET(` を走査して `ScriptList.inl` / `DataAssetList.inl` を同期する。

| 種類 | マクロ | 登録先 | Add Script に出るか |
|------|--------|--------|--------------------|
| スクリプト (アタッチする) | `FBZZ_SCRIPT(T)` | `ScriptList.inl` | 出る |
| ユーティリティ (アタッチしない) | **なし** | されない | 出ない |
| 共有調整値 (`.fzdata`) | `FBZZ_DATA_ASSET(T)` | `DataAssetList.inl` | 出ない |

**ユーティリティは登録マクロを付けないだけでよい。** Unity で `MonoBehaviour` を継承しない普通のクラスに相当し、ファイル分割・ヘルパー関数・データ構造はこちらで書く。テンプレートは AssetBrowser の `Create > C++...` に 3 種類とも並ぶ。

---

## 必須コンポーネントの宣言 (`FBZZ_REQUIRE_COMPONENT`)

スクリプトが同じ GameObject に必要とするコンポーネントは、クラス本体で宣言する。

```cpp
class EnemyComponent : public Script {
    FBZZ_SCRIPT(EnemyComponent)
    FBZZ_REQUIRE_COMPONENT(RigidBodyComponent, CharacterControllerComponent)
    FBZZ_OPTIONAL_COMPONENT(AnimatorComponent)   // 無くても縮退動作するもの
```

**WHY**: `scene.GetComponent<T>()` は無ければ `nullptr` を返して早期 return し、`animator.SetFloat()` などのプロキシも対象が無ければ黙って何もしない。宣言が無いと、付け忘れは「動かないのにエラーも出ない」形でしか現れない。宣言すると 3 箇所が同じ情報で名指しする:

- **Inspector** — 不足を赤帯で表示。`Fix` ボタンで既定値付きの一括追加
- **Play 開始時** — シーン全体を検証して Console へエラー出力 (Play は止めない)
- **ScriptSystem** — 実行時に一度だけ警告。Standalone ビルドでも出る

また Hierarchy の `Add Object > Script Object > <型名>` が、この宣言をそのまま組み立て手順として使う (スクリプト + 要求コンポーネント一式の GameObject を生成)。そのまま `Save As Prefab` すれば、以降はプレファブ 1 個のドラッグで配置できる。

型名は**型そのもの**で書く (文字列ではない)。綴り違いや `#include` 漏れはコンパイルエラーになる。対象の型ヘッダを `#include` すること。

> **自動追加にはしない。** Animator は Controller 未設定なら足しても動かず、Collider は寸法が決まらない。黙って増やすと「揃っているのに動かない」一段深い迷子を作るため、不足を名指しして判断は人に残す。

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
| スクリプトから Engine 実装型を直接インクルード | `ScriptProxy` 経由でアクセス |

---

## Git 運用 (詳細: `Docs/conventions/git.md`)

```
main → develop → feature/<name>
```

コミット形式: `[Feature] / [Fix] / [Design] / [Build] / [Refactor] / [Chore] / [Release] + 動詞 + 概要`

コミットの Description (本文) は **Markdown で記述する**。見出し (`##`) と箇条書き (`-`) を使って構造化すること。

---

## ビルド方法

**ビルドは VSCode / Visual Studio 2022 以降から行う。**
ターミナル (PowerShell / Bash) から `ninja` や `cmake --build` を実行しない。
MSVC の環境変数 (vcvarsall.bat) が設定されていないためコンパイルエラーになる。

- ビルド: VS で `Ctrl+Shift+B` (ソリューション全体のリビルド)
- 実行: VS のデバッガー or 生成された `.exe` を直接起動
- ビルド結果を確認する必要がある場合は、ユーザーにビルドを依頼してエラー出力を貼ってもらう

### VS 更新後に CMake Configure が失敗する場合

`CMAKE_CXX_COMPILER` のフルパスが cmake キャッシュに残るため、VS (MSVC ツールセット) を更新すると古いパスを参照してエラーになる。
対処: `build/Debug/` と `build/Release/` を削除してから VS Code で再 Configure する。
VS Code CMake Tools の場合は `...` → **Delete Cache and Reconfigure**。

---

## 曖昧な指示を受けたとき

設計ドキュメントが `Docs/` に存在すれば従う。なければ実装せずに選択肢を提示する。

---

## トークン節約ルール

- ファイルを読む前に Grep / Glob でファイルを特定する
- 修正が局所的なら Write より **Edit** を使う (差分のみ送信)
- 変更していないコードブロックを応答に再掲しない
- 大きなファイルは関連セクションのみ読む (`lines:N-M` モード)
- 独立した調査・ファイル操作は並行ツール呼び出しでまとめる
