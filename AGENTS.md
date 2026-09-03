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

**大原則: コメントは「コードを読んでも分からないこと」だけを書く。** 処理をなぞる説明・名前の言い換え・自明な記述は書かない。
言語は日本語。

> **旧規約 (自明か否かに関わらず積極的に書く / 旧形式の `// FBZZ Engine` バナー) は廃止した。過去のコードに残る旧スタイルを参考にしてはいけない。**

### ファイルヘッダー — 全ファイル必須

`.hpp` / `.cpp` の**一番上** (`#pragma once` より前) に、Doxygen 形式で以下を書く。これは「必要なものだけ」の例外で、**すべてのファイルに統一して付ける**。

```cpp
/// @file    Vector3.hpp
/// @brief   3次元ベクトルの演算と定数。
/// @author  Hasegawa Jin
/// @date    2026-08-21

#pragma once
```

- `@file`: 拡張子込みのファイル名。ディレクトリは書かない。
- `@brief`: そのファイルの責務を**一行**で。ファイル名の言い換えにならないよう、何を担うファイルかを書く。長い設計説明は `Docs/design/` に置いてリンクする。
- `@author`: `Hasegawa Jin`。
- `@date`: **ファイル作成日** (`YYYY-MM-DD`)。更新のたびに書き換えない — 更新履歴は git が持つので二重管理しない。
- 縦位置を揃える (タグの後を空白で桁揃え)。
- 3 行以上の設計解説をここに書かない。ファイルヘッダーは索引であって設計文書ではない。

既存ファイルの旧バナー (`// FBZZ Engine` / `// Vector3.hpp | fbzz::math`) は、そのファイルを触ったときにこの形式へ置き換える。

### `.hpp` — Doxygen 形式で、必要なものだけ

公開 API (型・関数・非自明なメンバ) に `///` の Doxygen コメントを付ける。**必要なものだけ**。

書く価値があるもの:

- 型・関数の**責務**を一行で (名前から読み取れない場合のみ)
- 前提条件・事後条件、呼び出し順序の制約
- **単位・座標系・値域** (ラジアン/度、ワールド/ローカル、0-1 正規化 など)
- **所有権・寿命** (返り値のポインタを誰が持つか、参照がいつまで有効か)
- **スレッド安全性** (呼び出して良いスレッド・フェーズ)
- 失敗時の戻り値の意味、`nullptr` を返す条件

書かないもの:

- 名前から自明なゲッター・セッター・単純なコンストラクタ
- 引数名を言い換えただけの `@param` / `@return`
- 自明な構造体メンバへの一行コメント
- ファイルヘッダーの `@brief` と同じ内容を、直下のクラスにもう一度書くこと

```cpp
/// レイと BVH の最近接交差を求める。
/// @param ray 原点はワールド空間、方向は正規化済みであること。
/// @return ヒットなしなら false。out は未変更。
[[nodiscard]] bool Raycast(const Ray& ray, RaycastHit& out) const;

/// 補間済みのワールド行列。物理ステップ間の描画用で、FixedUpdate 中は古い値を返す。
[[nodiscard]] const Matrix4& GetRenderMatrix() const;

// NG: 何も足していない
/// 位置を取得する
/// @param なし
/// @return 位置
const Vector3& GetPosition() const;
```

### `.cpp` — 原則コメント無し。補足が要る箇所だけ

実装は基本的に無コメントで読める状態にする (関数分割・命名で解決する)。コメントを書くのは **WHY** に限る:

- 非自明なアルゴリズム・数式の根拠や出典
- 順序依存・タイミング依存 (「この前に呼ぶと〜が未初期化」など)
- API / ドライバ / 外部ライブラリの制約に対する回避策
- 意図的なパフォーマンス上の選択、あえて素朴に書いていない理由
- マジックナンバーの出所

書かないもの: 処理の見出し (`// 位置を更新`)、区切り線バナー (`// ===== 初期化 =====`)、コメントアウトされた旧コード、作業メモ (`TODO:` / `FIXME:` は可)。

```cpp
// NG                                    // OK
// 速度に重力を加算する                    // 半陰的オイラー。速度を先に更新しないと
m_velocity += gravity * dt;              // 減衰が 1 フレーム遅れて振動する。
                                         m_velocity += gravity * dt;
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

## 編集中も実行するスクリプト (`FBZZ_EXECUTE_ALWAYS`)

Script は既定では Play 中しか動かない (`ScriptSystem` が `ctx.simulating` で選別する)。HUD の配色やレイアウトのように**見た目を組み立てるスクリプト**は、Play を押さずにビューポートで確かめたい。クラス本体で宣言する。

```cpp
class PolarityGunHudComponent : public Script {
    FBZZ_SCRIPT(PolarityGunHudComponent)
    FBZZ_EXECUTE_ALWAYS()
```

**編集中に呼ばれるもの** — `OnAwake` / `OnStart` / `OnEnable` / `OnDisable` / `OnUpdate` / `OnLateUpdate` / `OnDestroy`、`Invoke` と Coroutine。

**呼ばれないもの** — `OnFixedUpdate` と衝突・トリガー系。`PhysicsSystem` は `RunMode::SimOnly` のままで、編集中に力を加えても積分する相手が居ない。`physics` プロキシも編集中は World が外れている。

**Play をまたぐとライフサイクルは張り直される。** Play の開始・停止では Script インスタンスが作り直されないため、編集中に立った `m_awoken` / `m_started` をそのまま持ち込むと Play で `OnStart` が二度と呼ばれない。モードが切り替わった瞬間に `OnDisable → OnDestroy` まで通してから `OnAwake → OnStart` をやり直す (`ScriptSystem::CollapseLifecycleAcrossModes`)。編集中に積んだ状態は Play へ持ち越さない。

**編集中の書き込みはシーンの中身になる。** Unity の `[ExecuteAlways]` と同じで、書いた値はそのままシーンに残り、保存すれば `.fbzz` へ入る。副作用として未保存マークも立つ。**ゲーム進行を持つ状態 (体力・スコア・座標) を触るスクリプトには付けないこと。** 付けてよいのは、シーンの値から見た目を導出するだけで、二度走らせても同じ結果になるもの。

**モードの分岐は `app.IsPlaying()` / `app.IsEditMode()`。** 編集中は入力・物理・音が動いていないため、それらを触る処理はこれで囲う。

> ABI 注記: `ExecuteInEditMode()` は `Script` の仮想関数列の末尾にある。仮想関数を足すときは末尾へ置き、`ScriptDllAbi.hpp` の `kScriptVtableAbiVersion` を必ずインクリメントすること。

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

### Engine 内部のモジュール (詳細: `Docs/conventions/build-performance.md` §4-4)

```
FBZZCore → FBZZRHI → FBZZRenderPlatform → FBZZRenderDX11 / FBZZRenderDX12 → FBZZEngine
```

すべて OBJECT ライブラリで、`FBZZEngine.dll` が `$<TARGET_OBJECTS:>` で飲み込む。
出荷する DLL は増えない。

- **新しいディレクトリを `Engine/src/` に作ったら `Projects/Engine/CMakeLists.txt` の
  モジュール定義へ足す。** 足し忘れは configure 時に FATAL_ERROR で落ちる
- **DX11 / DX12 のヘッダーは各モジュールの外から include できない。** バックエンドが
  外へ出すのは `Engine/Renderer/BackendEntry.hpp` の生成関数 1 つだけ
- 低層 (Core / RHI) が Asset や Scene を必要としたら、依存を逆転させる。
  実例は `Engine/Renderer/AssetPathService.hpp`（パス解決の規則を Asset 層が差し込む）
- STATIC にしないこと。自己登録の静的初期化子をリンカが捨てる

---

## テスト (詳細: `Docs/conventions/test.md`)

GoogleTest / GoogleMock は `ThirdParty/GoogleTest/` に vendor 済み。FetchContent は使わない。

| 層 | 置き場所 | CTest 登録 |
|---|---|---|
| Auto | `Projects/Tests/<Domain>/Auto/` | する |
| Manual | `Projects/Tests/<Domain>/ManualTest/` | しない (OS 状態を触る) |

- 素の `TEST` を書かず **`TEST_F` + TestKit の fixture** を使う
- float の比較は `EXPECT_EQ` ではなく `EXPECT_VEC3_NEAR` 等 (`TestKit/Approx.hpp`)
- 乱数・時刻・`sleep` を持ち込まない (`TestKit/Deterministic.hpp`)
- 新規テストは `Projects/Tests/CMakeLists.txt` の `SOURCES` へ**手で 1 行足す** (`file(GLOB)` 禁止)

---

## ビルド時間 (詳細: `Docs/conventions/build-performance.md`)

ビルド時間は **TU 数 × 1 TU あたりの前処理行数**で決まる。自作ヘッダーは
前処理量の 3〜4% しかなく、残りは標準ヘッダーと `<Windows.h>`。

- **ヘッダーに `<Windows.h>` を書かない** — 1 本で 35 万行。`.cpp` に書く。
  OS 定数を列挙へ写すときは数値リテラル + Win32 境界の `.cpp` で `static_assert`
- `.cpp` で Win32 が要るときは `WIN32_LEAN_AND_MEAN` を先に定義する
- **`#define NOMINMAX` をソースに書かない** — ルートの CMakeLists が全構成へ定義済み。
  include 順に依存した防御はヘッダーを 1 つ整理しただけで崩れる (C2589 の実例あり)
- 標準ヘッダーは `fbzz_use_std_pch(<target>)` に任せる。**共有 PCH に `<Windows.h>` は入れない**
  (DX バックエンドのように全 TU が Win32/D3D を使うモジュールだけ `PCH_EXTRA` で足す)
- `-DFBZZ_UNITY_BUILD=ON` で翻訳単位を統合できる (CI・配布向け。増分ビルドは遅くなる)
- `toml++` のようなヘッダーオンリー実装を公開ヘッダーへ載せない

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
