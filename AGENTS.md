# FBZZ Engine — AI エージェントへの指示

以下の方針から外れないようにすること。ディレクトリ地図とタスク別の入口は `CLAUDE.md`。

---

## 絶対に守ること

- **数学ライブラリは自作** (GLM 不使用)
- **物理エンジンは自作** (Bullet / PhysX 不使用)
- **ヘッダファイルの拡張子は `.hpp`**、先頭に **`#pragma once`**
- **C++20**
- **名前空間は `fbzz::`** (`fbzz::math` / `fbzz::physics` / `fbzz::renderer` / `fbzz::scene` / `fbzz::editor`)
- ゲームスクリプトの名前空間は `sandbox`。**入れ子にしない** (ScriptCodeGen が 1 段しか解釈しない)

---

## アーキテクチャ方針

- レンダラーは `IRenderer` インターフェース経由。バックエンド具象型へのダウンキャスト禁止
- シーン管理は Unity 同様の GameObject / Component パターン
- 依存方向: `Editor / GameHub / Sandbox / GreenWare → Engine → Physics → Math` (逆転禁止)
- スクリプトは `ScriptProxy` 経由でエンジン機能へ。DLL 境界を越えて Engine 実装型に直接依存しない
- エディターの「操作」は `Editor/Op/` の登録簿へ 1 件登録する。メニュー・ホットキー・コマンドパレット・AI バスはその**投影**であり、面ごとに条件や実体を書かない

---

## サードパーティライブラリ

OK: `Assimp` / `DirectX 11 SDK` / `DirectX 12 SDK` / `DXC` / `Microsoft::WRL::ComPtr` / `ImGui` / `ImGuizmo` / `ImNodes` / `toml++` / `stb_image` / `stb_truetype` / `stb_rect_pack` / `DirectXTex` / `TinyEXR` / `XAudio2` / `GoogleTest` / `GoogleMock`

NG: `GLM` / `GLFW` / `Bullet` / `PhysX` / `Box2D`

実体は `ThirdParty/` へベンダーする (FetchContent は使わない)。

ベンダーするときは、コードと一緒に**次の 3 つを必ず揃える**。
1. `ThirdParty/<Name>/LICENSE` — 上流のライセンス全文をそのまま置く
2. `ThirdParty/<Name>/VERSION` — 取得元 URL・版・取得日・取り込み範囲・更新手順 (`ThirdParty/GoogleTest/VERSION` が手本)
3. `THIRD-PARTY-NOTICES.md` へ 1 件追記する

**WHY**: MIT も BSD-3-Clause も「著作権表示とライセンス文の同梱」を再配布の条件にしている。ソース配布だけならディレクトリ内の `LICENSE` で足りるが、スタンドアロンパッケージはソースを含まないので、告知を 1 枚に集めた `THIRD-PARTY-NOTICES.md` が要る。版を記録しないと、脆弱性の報告が来たときに「今どれを積んでいるか」が誰にも分からない。

`stb_truetype` / `stb_rect_pack` はフォントの実行時ラスタライズとアトラス配置に使う。
**WHY**: 「数学・物理は自作」は保つが、TrueType の字形解釈は別ドメインであり、glyf / loca / cmap / hmtx の自前パースはフォント機能とは別スケールの投資になる。`stb_image` を既に許可している方針と整合させる。

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
| 名前空間 | `snake_case` | `fbzz::physics` |
| ソース・ヘッダ | `PascalCase` | `RigidBody.cpp`, `IRenderer.hpp` |
| ディレクトリ | `PascalCase` | `Renderer/`, `Core/`, `DX11/` |

例外: スクリプトの**公開フィールド**は Inspector に出る値なので `m_` を付けず `lowerCamelCase` で書く (`FBZZ_FIELD` が表示名を自動生成する)。

---

## コメント規約

**大原則: コメントは「コードを読んでも分からないこと」だけを書く。** 処理をなぞる説明・名前の言い換え・自明な記述は書かない。言語は日本語。

> **旧規約 (自明か否かに関わらず積極的に書く / `// FBZZ Engine` バナー) は廃止した。過去のコードに残る旧スタイルを参考にしてはいけない。**

### ファイルヘッダー — 全ファイル必須

`.hpp` / `.cpp` の**一番上** (`#pragma once` より前) に Doxygen 形式で書く。これは「必要なものだけ」の例外で、**すべてのファイルに統一して付ける**。

```cpp
/// @file    Vector3.hpp
/// @brief   3次元ベクトルの演算と定数。
/// @author  Hasegawa Jin
/// @date    2026-08-21

#pragma once
```

- `@file`: 拡張子込みのファイル名。ディレクトリは書かない
- `@brief`: 責務を**一行**で。ファイル名の言い換えにしない
- `@author`: `Hasegawa Jin`
- `@date`: **ファイル作成日** (`YYYY-MM-DD`)。更新のたびに書き換えない — 履歴は git が持つ
- タグの後を空白で桁揃えする

4 行の後に、設計判断が絡むファイルに限り `WHY:` ブロックを続けてよい (「なぜこの構造か」「なぜ前の方式を捨てたか」)。長い設計解説は `Docs/design/` に置いてリンクする。

既存ファイルの旧バナー (`// FBZZ Engine` / `// Vector3.hpp | fbzz::math`) は、そのファイルを触ったときにこの形式へ置き換える。

### `.hpp` — Doxygen 形式で、必要なものだけ

書く価値があるもの:

- 型・関数の**責務**を一行で (名前から読み取れない場合のみ)
- 前提条件・事後条件、呼び出し順序の制約
- **単位・座標系・値域** (ラジアン/度、ワールド/ローカル、0-1 正規化)
- **所有権・寿命** (返り値のポインタを誰が持つか、参照がいつまで有効か)
- **スレッド安全性** (呼び出して良いスレッド・フェーズ)
- 失敗時の戻り値の意味、`nullptr` を返す条件

書かないもの: 名前から自明なゲッター・セッター / 引数名を言い換えただけの `@param` `@return` / 自明なメンバへの一行コメント / ファイルヘッダーの `@brief` をクラスにもう一度書くこと。

```cpp
/// レイと BVH の最近接交差を求める。
/// @param ray 原点はワールド空間、方向は正規化済みであること。
/// @return ヒットなしなら false。out は未変更。
[[nodiscard]] bool Raycast(const Ray& ray, RaycastHit& out) const;

// NG: 何も足していない
/// 位置を取得する
const Vector3& GetPosition() const;
```

### `.cpp` — 原則コメント無し。補足が要る箇所だけ

実装は無コメントで読める状態にする (関数分割・命名で解決)。書くのは **WHY** に限る:

- 非自明なアルゴリズム・数式の根拠や出典
- 順序依存・タイミング依存 (「この前に呼ぶと〜が未初期化」)
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

## 所有権モデル

| スマートポインタ | 使う場面 |
|----------------|---------|
| `std::unique_ptr` | デフォルト。1 箇所が所有するリソース (Component, GameObject 等) |
| `std::shared_ptr` | 複数システムで共有するリソース (IBuffer, IShader, RigidBody) |
| raw pointer (`T*`) | 非所有の参照のみ (`m_owner`, `m_parent` 等) |

`new` / `delete` 直接使用禁止。`make_unique` / `make_shared` を使う。
GPU リソースは原則 `unique_ptr` の単独所有で返す。生ハンドルを複製して持ち回らない。

---

## エラーハンドリング

- 回復不可能エラー: `assert()`
- 回復可能エラー: `bool` 戻り値 (失敗を黙って握り潰さず、Logger へ理由を残す)
- DX の HRESULT: `FBZZ_HR_CHECK(hr)` (`Engine/Core/HResult.hpp`)
- 数学の契約違反 (長さ 0 の正規化など): `assert` ではなく `FBZZ_MATH_CONTRACT` で通報し、定義済みの値を返す
- `throw` / `std::exception` は**禁止**

---

## メモリシステム (`Engine/include/Engine/Core/Memory/`)

通常のヒープ確保は `make_unique` / `make_shared`。カスタムアロケーターは**高頻度・大量確保のときだけ**使う。

| アロケーター | 用途 |
|------------|------|
| `FrameAllocator` | フレームごとにリセットされる一時バッファ |
| `LinearAllocator` | 前端から順に確保。解放は一括のみ |
| `PoolAllocator` | 同サイズオブジェクトの大量生成 (パーティクル・コライダー等) |
| `StackAllocator` | マーカーによる部分解放が要る場合 |

リークは `MemoryTracker` / `MemoryDebug` で検出できる。

---

## C++20

OK: `std::span` / Concepts / Designated initializers / `[[nodiscard]]` / `constexpr` / `consteval`

NG: Modules / `std::ranges` / C++20 コルーチン
(スクリプトのコルーチンは `Engine/Scene/Coroutine.hpp` の自作実装を使う)

---

## システムスケジューラー (`Engine/include/Engine/Core/Scheduler/`)

新しいシステムは `ISystem` を継承し、`GetPhase()` / `GetRunMode()` / `ComponentAccess` を宣言して `SystemScheduler` へ登録する。

- **Phase** (`Core/Scheduler/Phase.hpp` の宣言順に実行):
  `PreScript` → `Script` → `PrePhysics` → `Physics` (固定ステップ) → `PostPhysics` → `Navigation` → `LateScript` → `Cleanup` → `LateUpdate`
- **RunMode**: `Always` / `SimOnly` (Play 中のみ) / `EditorOnly` (編集中のみ)
- **ComponentAccess**: 読み書きするコンポーネント型を宣言する (並列実行の安全性)

描画と UI (`RenderSystem` / `UISystem`) はスケジューラーの Phase ではなく描画パスから駆動する。

`worldPosition` を書いても `PrePhysics` が local から組み直すため、位置を置くときは `position` (ローカル) も書く。

---

## アセットとプロジェクト構成

### プロジェクトの形

エンジンは**プロジェクト単位**で動く (`GreenWare/` が実例)。

```
<Project>/Assets/          オーサリングする実体 + 各ファイルに .meta (GUID)
<Project>/Library/         インポート済みキャッシュ (生成物。git 管理外)
<Project>/ProjectSettings/ ProjectSettings.toml / Input.inputactions
<Project>/Src/             EXE のエントリ (AppMain.cpp / GameMain.cpp / ScriptsDll.cpp)
```

- **参照は GUID (`guid:`) で書く。** `Assets/` のファイルには `.meta` が随伴し、改名・移動しても参照は切れない
- **ディスク上のアセットが正本。** シーンやプレファブを生成スクリプトで作り直さない。既存ファイルへ**差分で**当てる
- エディター起動中に git でツリーを巻き戻さない (`.meta` が GUID ごと作り直されて参照が壊れる)

### 主なアセット形式

| 拡張子 | 定義 | 内容 |
|--------|------|------|
| `.fzasset` | `Engine/Format/FzAssetFormat.hpp` | 汎用アセットバイナリ (メタデータ + ペイロード) |
| `.mesh` | `Asset/FzModelFormat.hpp` | メッシュサブアセット |
| `.scene` / `.prefab` | `Scene/SceneSerializer.hpp` | TOML ベースのシーン / プレファブ |
| `.mat` | `Asset/MatAssetImporter.hpp` | マテリアル (見た目のパラメーターはここが正本) |
| `.tex` | `Asset/TexDescSerializer.hpp` | テクスチャデスクリプター |
| `.terrain` | `Asset/FzTerrainFormat.hpp` | テレイン (実体は TOML。binary 版は移行途中) |
| `.animcontroller` | `Asset/AnimatorControllerAsset.hpp` | アニメーションステートマシン |
| `.anim` / `.skel` / `.mask` | `Asset/AnimationClip.hpp` ほか | クリップ / スケルトン / アバターマスク |
| `.physmat` | `Asset/PhysicsMaterialAsset.hpp` | 物理マテリアル |
| `.sequence` | `Asset/SequenceAsset.hpp` | カットシーン / シーケンス |
| `.synth` | `Audio/` | 手続き生成 SFX |
| `.fluid` / `.vfield` | `Asset/FluidBaker.hpp` / `Asset/` | 流体ベイク / 速度場 |
| `.curve` / `.gradient` | `Asset/ParticleCurveAsset.hpp` | パーティクル用カーブ・グラデーション |
| `.fzdata` | `Asset/DataAsset.hpp` | 共有調整値 (`FBZZ_DATA_ASSET`) |

### シェーダー

`Assets/Shaders/<Category>/*.hlsl`。**同じシェーダーがエンジン側とプロジェクト側に複製されている**ので、直すときは全コピーを検索して全部直し、全部コンパイルする。

---

## スクリプティングシステム

### 1 スクリプト = 1 ヘッダー

`<Project>/Assets/Scripts/<領域>/XxxComponent.hpp` にクラスを書くだけ。`.cpp` も `.generated.hpp` も要らない。

```cpp
/// @file    EnemyComponent.hpp
/// @brief   雑魚の追跡と攻撃。
/// @author  Hasegawa Jin
/// @date    2026-09-12
#pragma once
#include <Engine/Scene/Script.hpp>

using namespace fbzz::scene;
using namespace fbzz::math;

namespace sandbox {

class EnemyComponent : public Script {
    FBZZ_SCRIPT(EnemyComponent)
    FBZZ_REQUIRE_COMPONENT(RigidBodyComponent, CharacterControllerComponent)
    FBZZ_OPTIONAL_COMPONENT(AnimatorComponent)

    FBZZ_GROUP("移動")
    FBZZ_FIELD_RANGE(float, moveSpeed, 4.0f, "移動速度 [m/s]", 0.0f, 20.0f)
    FBZZ_TOOLTIP("走りの基準速度。回避距離はこの比で決まる")
    FBZZ_FIELD_REF(EntityRef, target, "追跡対象")

public:
    void OnUpdate(float dt) override { /* transform->position, input, physics ... */ }
};

FBZZ_REFLECT(EnemyComponent)   // フィールド宣言の直後、クラス外に置く

} // namespace sandbox
```

> **`.generated.hpp` は廃止した (2026-06-29)。** 外部ツールによるコード生成ではなく、`FBZZ_FIELD*` が宣言と同じ場所から `Reflect()` を組み立てる。フィールドを 1 行足すだけで Inspector とシリアライズが追従する。旧コードに残る `.generated.hpp` の include を真似しない。

`ScriptCodeGen` が今も担うのは **`ScriptList.inl` / `DataAssetList.inl` の同期だけ** (`FBZZ_SCRIPT(` / `FBZZ_DATA_ASSET(` を走査する)。この 2 ファイルは手で編集しない。

### Assets/ に置く 3 種類のヘッダ

`Assets/**/*.hpp` は CMake の GLOB が全部 Script DLL のビルド対象へ入れるが、**登録されるかはマクロで決まる**。

| 種類 | マクロ | 登録先 | Add Script に出るか |
|------|--------|--------|--------------------|
| スクリプト (アタッチする) | `FBZZ_SCRIPT(T)` | `ScriptList.inl` | 出る |
| ユーティリティ (アタッチしない) | **なし** | されない | 出ない |
| 共有調整値 (`.fzdata`) | `FBZZ_DATA_ASSET(T)` | `DataAssetList.inl` | 出ない |

ユーティリティは登録マクロを付けないだけでよい (Unity で `MonoBehaviour` を継承しない普通のクラスに相当)。

### 主なリフレクションマクロ (`Engine/Scene/Script.hpp`)

| マクロ | 用途 |
|--------|------|
| `FBZZ_FIELD(T, name, default, "表示名")` | 基本。表示名に `""` を渡すと変数名から自動生成 |
| `FBZZ_FIELD_RANGE` / `_RANGE_INT` / `_MIN` / `_STEP` | 値域付きスライダー |
| `FBZZ_FIELD_COLOR` / `_ANGLE` / `_ENUM` / `_FLAGS` / `_LAYER_MASK` / `_TAG` | 型に応じた専用ウィジェット |
| `FBZZ_FIELD_CURVE` / `_GRADIENT` / `_AUDIO` / `_FILE` | アセット参照 |
| `FBZZ_FIELD_REF` / `FBZZ_REF` / `FBZZ_OBJECT_FIELD` | GameObject / コンポーネント参照 |
| `FBZZ_LIST_FIELD` / `FBZZ_FIXED_ARRAY_FIELD` | 配列 |
| `FBZZ_GROUP` / `FBZZ_SPACE` / `FBZZ_TOOLTIP` / `FBZZ_BUTTON` | Inspector の見た目 |
| `FBZZ_FIELD_SHOW_IF` / `_ENABLE_IF` / `_HIDDEN` / `_READ_ONLY` | 条件表示 |
| `FBZZ_REFLECT(T)` | **フィールド宣言の締め。クラス外に 1 行必須** |

### ScriptProxy

スクリプトはプロキシを**メンバー名**で呼ぶ (`transform->position`, `input.IsKeyDown(...)`, `physics.AddForce(...)`)。現在 52 種 (正本は `ScriptProxy/ScriptProxyMembers.inl`):

`transform` `input` `cursor` `app` `time` `physics` `collider` `audio` `light` `camera` `material` `particle` `particleForceField` `cloud` `sunMoon` `patrol` `wind` `trail` `meshTrail` `scene` `animator` `debug` `postprocess` `memory` `ui` `uiAnimator` `navigation` `character` `mesh` `ik` `water` `terrain` `environment` `decal` `volume` `reflectionProbe` `lifetime` `vfx` `gameplay` `save` `events` `random` `tween` `config` `display` `graphics` `motionWarp` `sequence` `objectMask` `springBone` `ragdoll` `joint`

> `lifetime` / `time` / `random` など**基底のメンバー名と同じ名前をスクリプト側で宣言しない**。名前が隠れてプロキシが呼べなくなる。

新しいエンジン機能をスクリプトへ公開する手順:

1. `Engine/include/Engine/Scene/ScriptProxy/ScriptXxxProxy.hpp` を追加
2. `Engine/src/Scene/ScriptProxies.cpp` に実装
3. `ScriptProxy/ScriptProxyMembers.inl` と `AllScriptProxies.hpp` へ追加

DLL 境界を越えるため、プロキシの引数・戻り値に Engine 内部型を直接露出しない。

### 必須コンポーネントの宣言 (`FBZZ_REQUIRE_COMPONENT`)

**WHY**: `GetComponent<T>()` は無ければ `nullptr` を返して早期 return し、プロキシも対象が無ければ黙って何もしない。宣言が無いと付け忘れは「動かないのにエラーも出ない」形でしか現れない。宣言すると 3 箇所が同じ情報で名指しする:

- **Inspector** — 不足を赤帯で表示。`Fix` ボタンで既定値付きの一括追加
- **Play 開始時** — シーン全体を検証して Console へエラー出力 (Play は止めない)
- **ScriptSystem** — 実行時に一度だけ警告。Standalone ビルドでも出る

Hierarchy の `Add Object > Script Object > <型名>` はこの宣言を組み立て手順として使う。型名は**型そのもの**で書き、対象のヘッダーを `#include` する。

> **自動追加にはしない。** Animator は Controller 未設定なら足しても動かず、Collider は寸法が決まらない。不足を名指しして判断は人に残す。

### 編集中も実行するスクリプト (`FBZZ_EXECUTE_ALWAYS`)

Script は既定では Play 中しか動かない。見た目を組み立てるスクリプトだけ宣言する。

- **編集中に呼ばれる** — `OnAwake` / `OnStart` / `OnEnable` / `OnDisable` / `OnUpdate` / `OnLateUpdate` / `OnDestroy`、`Invoke` と Coroutine
- **呼ばれない** — `OnFixedUpdate` と衝突・トリガー系 (物理は `SimOnly` のまま)
- **Play をまたぐとライフサイクルは張り直される** (`ScriptSystem::CollapseLifecycleAcrossModes`)。編集中に積んだ状態を Play へ持ち越さない
- **編集中の書き込みはシーンの中身になる。** ゲーム進行を持つ状態 (体力・スコア・座標) を触るスクリプトには付けない
- モードの分岐は `app.IsPlaying()` / `app.IsEditMode()`

> ABI 注記: `Script` に仮想関数を足すときは**仮想関数列の末尾**へ置き、`ScriptDllAbi.hpp` の `kScriptVtableAbiVersion` を必ずインクリメントする。

### ライフサイクルの落とし穴

- **破棄では `OnDisable` が呼ばれない。** 後始末は `OnDestroy` に書く
- Play の開始・停止は `SceneSerializer` の往復を通る。**シリアライズしていない値は既定値へ戻る**

### using namespace の許可範囲

`<Project>/Assets/**/*.hpp` (スクリプト) に限り、以下を許可する。

```cpp
using namespace fbzz::scene;
using namespace fbzz::math;
using namespace fbzz::input;
```

**WHY**: スクリプトはエンジン層から include されない末端ヘッダで、名前空間汚染が生じない。エンジン側 `.hpp` での `using namespace` は引き続き禁止。

---

## 禁止パターン

| パターン | 理由 |
|---------|------|
| `dynamic_cast` | RTTI コスト。`GetComponent<T>()` で代替 |
| `reinterpret_cast` | シェーダー定数バッファ転送以外で禁止 |
| グローバル変数 | `Application` シングルトン以外禁止 |
| `new` / `delete` 直接使用 | `make_unique` / `make_shared` |
| `throw` / `std::exception` | エンジンコード内では禁止 |
| `#include` の循環依存 | 前方宣言 (`class Foo;`) で解決 |
| バックエンド具象型へのダウンキャスト | 上位レイヤーは `IRenderer&` のみ |
| スクリプトから Engine 実装型を直接 include | `ScriptProxy` 経由 |
| `.generated.hpp` の新規作成・include | 廃止済み。`FBZZ_REFLECT` を使う |
| ヘッダーへの `<Windows.h>` | 1 本で 35 万行。`.cpp` に書く |

### Engine 内部のモジュール (詳細: `Docs/conventions/build-performance.md` §4-4)

```
FBZZCore → FBZZRHI → FBZZRenderPlatform → FBZZRenderDX11 / FBZZRenderDX12 → FBZZEngine
```

すべて OBJECT ライブラリで、`FBZZEngine.dll` が `$<TARGET_OBJECTS:>` で飲み込む。出荷する DLL は増えない。

- **新しいディレクトリを `Engine/src/` に作ったら `Projects/Engine/CMakeLists.txt` のモジュール定義へ足す。** 足し忘れは configure 時に FATAL_ERROR で落ちる
- **DX11 / DX12 のヘッダーは各モジュールの外から include できない。** バックエンドが外へ出すのは `Engine/Renderer/BackendEntry.hpp` の生成関数 1 つだけ
- 低層 (Core / RHI) が Asset や Scene を必要としたら依存を逆転させる (実例: `Engine/Renderer/AssetPathService.hpp`)
- **STATIC にしないこと。** 自己登録の静的初期化子をリンカが捨てる

---

## テスト (詳細: `Docs/conventions/test.md`)

GoogleTest / GoogleMock は `ThirdParty/GoogleTest/` に vendor 済み。FetchContent は使わない。

| 層 | 置き場所 | CTest 登録 |
|---|---|---|
| Auto | `Projects/Tests/<Domain>/Auto/` | する |
| Manual | `Projects/Tests/<Domain>/ManualTest/` | しない (OS 状態を触る) |
| Bench | `Projects/Tests/Bench/` | しない |

- 素の `TEST` を書かず **`TEST_F` + TestKit の fixture** を使う
- float の比較は `EXPECT_EQ` ではなく `EXPECT_VEC3_NEAR` 等 (`TestKit/Approx.hpp`)
- 乱数・時刻・`sleep` を持ち込まない (`TestKit/Deterministic.hpp`)
- 新規テストは `Projects/Tests/CMakeLists.txt` の `SOURCES` へ**手で 1 行足す** (`file(GLOB)` 禁止)

---

## ビルド時間 (詳細: `Docs/conventions/build-performance.md`)

ビルド時間は **TU 数 × 1 TU あたりの前処理行数**で決まる。自作ヘッダーは前処理量の 3〜4% しかなく、残りは標準ヘッダーと `<Windows.h>`。

- **ヘッダーに `<Windows.h>` を書かない。** OS 定数を列挙へ写すときは数値リテラル + Win32 境界の `.cpp` で `static_assert`
- `.cpp` で Win32 が要るときは `WIN32_LEAN_AND_MEAN` を先に定義する
- **`#define NOMINMAX` をソースに書かない** — ルートの CMakeLists が全構成へ定義済み
- 標準ヘッダーは `fbzz_use_std_pch(<target>)` に任せる。**共有 PCH に `<Windows.h>` は入れない** (全 TU が Win32/D3D を使うモジュールだけ `PCH_EXTRA` で足す)
- `-DFBZZ_UNITY_BUILD=ON` で TU を統合できる (CI・配布向け。増分ビルドは遅くなる)
- `toml++` のようなヘッダーオンリー実装を公開ヘッダーへ載せない

---

## ビルド方法

**ビルドは VSCode / Visual Studio から行う。ターミナルから `ninja` / `cmake --build` を直接叩かない。**
(MSVC の環境変数が無いままでは失敗する。VS の場所を知っているのは `Tools/VcBuild.ps1` だけで、VS Code のタスクはすべてそこを通る)

- VS Code: タスク `CMake: Build All (Debug)` / `Build Editor (...)` / `Tests: Build & Run Suite (Debug)` など
- 構成プリセット: `debug` / `development` / `release` / `coverage` / `sdk`
- ビルド結果が必要なときは、**ユーザーにビルドを依頼してエラー出力を貼ってもらう**
- 出力は BuildConsole に `file:line` 付きで出る (クリックでジャンプできる)

### VS 更新後に CMake Configure が失敗する場合

`CMAKE_CXX_COMPILER` のフルパスがキャッシュに残るため、MSVC ツールセットを更新すると古いパスを参照して落ちる。
対処: `build/Debug/` と `build/Release/` を削除して再 Configure (VS Code CMake Tools なら `...` → **Delete Cache and Reconfigure**)。

---

## Git 運用 (詳細: `Docs/conventions/git.md`)

```
main → develop → feature/<name>
```

- コミット形式: `[Feature] / [Fix] / [Design] / [Build] / [Refactor] / [Chore] / [Release] + 動詞 + 概要`
- Description (本文) は **Markdown**。見出し (`##`) と箇条書き (`-`) で構造化する
- **`Co-Authored-By` / `Generated with` の類を入れない** (公開リポジトリのため)
- **`git checkout -- <path>` / `git restore <path>` を使わない。** 未コミットの作業を消した事故がある。戻したいときは対象を提示して判断を仰ぐ
- コミットやプッシュはユーザーに言われたときだけ行う

---

## 作業の進め方

- **報告の前に `git status` を見る。** 作業中にツリーが動く (エディターが `.meta` やシーンを書き換える) ことがある
- **改名・削除は grep で全件を洗う。** スクリプトを消すときは生成物 (`ScriptList.inl` / `.meta` / シーン内の参照) も追う
- 設計ドキュメントが `Docs/` にあれば従う。無ければ実装せずに選択肢を提示する
- `std::string` の `c_str()` を保存する前に `reserve()` する (再確保でポインタが無効になる)

---

## トークン節約ルール

- ファイルを読む前に Grep / Glob で場所を特定する
- 修正が局所的なら Write より **Edit** を使う
- 変更していないコードブロックを応答に再掲しない
- 大きなファイルは関連セクションのみ読む (`lines:N-M`)
- 独立した調査・ファイル操作は並行ツール呼び出しでまとめる
- **圧縮された読み取り結果をそのまま編集元にしない。** Edit の `old_string` は非圧縮の読み取りから取る
