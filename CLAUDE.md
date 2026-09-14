# Claude Instructions

作業規約の詳細は `AGENTS.md`。このファイルはセッション開始時のクイックオリエンテーション。

---

## プロジェクト概要

**FBZZ Engine** — C++20 自作 3D ゲームエンジン (Windows / DirectX 11 + DirectX 12)。GLM・Bullet・PhysX を使わず数学・物理をゼロ実装。ImGui フルエディター・RenderGraph・NavMesh・地形・水面・VFX・スクリプト DLL ホットリロードを備えたフルセット開発環境。**ポートフォリオ公開リポジトリ**。

名前空間: `fbzz::` | 依存方向: `Editor / GameHub / Sandbox / GreenWare → Engine → Physics → Math`

エンジン本体 (`Projects/`) と、それを使う**ゲームプロジェクト** (`GreenWare/`) は別物。ゲーム側の作業はほぼ `GreenWare/Assets/` の中で完結する。

---

## ディレクトリマップ

### エンジン本体

| パス | 役割 |
|------|------|
| `Projects/Math/include/Math/` | 自作数学ライブラリ (Vector/Matrix/Quaternion/Ray/Frustum/Plane) |
| `Projects/Physics/include/Physics/` | 自作物理エンジン (GJK/EPA/BVH/RigidBody/Constraint/CCD) |
| `Projects/Engine/include/Engine/Core/` | Application / Window / Time / Logger / Cursor / Signal / IModule |
| `Projects/Engine/include/Engine/Core/Memory/` | Frame / Linear / Pool / Stack アロケーター |
| `Projects/Engine/include/Engine/Core/Scheduler/` | SystemScheduler / Phase / ComponentAccess |
| `Projects/Engine/include/Engine/Core/Concurrency/` | TaskSystem など並行処理基盤 |
| `Projects/Engine/include/Engine/Renderer/` | `IRenderer` / `IBuffer` / `ITexture` / RenderGraph / Material 等 |
| `Projects/Engine/src/Renderer/Platform/DX11/` | DX11 実装 (モジュール `FBZZRenderDX11`。外から include 不可) |
| `Projects/Engine/src/Renderer/Platform/DX12/` | DX12 実装 (`FBZZRenderDX12`。`FBZZ_ENABLE_DX12` で切れる) |
| `Projects/Engine/include/Engine/Scene/` | Scene / GameObject / Transform / Script / Reflection / SceneSerializer |
| `Projects/Engine/include/Engine/Scene/Components/` | 全コンポーネント (68 ヘッダー) |
| `Projects/Engine/include/Engine/Scene/Systems/` | 全システム (33 ヘッダー) + `RenderPasses/` |
| `Projects/Engine/include/Engine/Scene/ScriptProxy/` | **51 種**のスクリプトプロキシ (DLL 境界を越えた型安全アクセス) |
| `Projects/Engine/include/Engine/Asset/` | AssetManager / AssetDatabase / 各 Importer / Baker |
| `Projects/Engine/include/Engine/Format/` | `FzAssetFormat.hpp` (汎用アセットバイナリの定義) |
| `Projects/Engine/include/Engine/Reflection/` | `TypeSchema.hpp` (型スキーマ。Inspector / AI バスが読む) |
| `Projects/Engine/include/Engine/{AI,Audio,Input,Profiler,Util}/` | 各ドメイン |

### ツール・ゲーム

| パス | 役割 |
|------|------|
| `Projects/Editor/include/Editor/Panels/` | ImGui パネル群 (37 ヘッダー) |
| `Projects/Editor/src/Panels/Inspector/` | Inspector のカテゴリ別実装 |
| `Projects/Editor/include/Editor/Op/` | EditorOperator — 操作の登録簿 (メニュー/ホットキー/パレット/AI バスの共通実体) |
| `Projects/Editor/include/Editor/Ai/` | AI コマンドバス (Named Pipe サーバー + プロトコル) |
| `Projects/Editor/src/Tools/` | TerrainTool 等のエディターツール |
| `Projects/EditorMcp/` | **TypeScript/Node** の MCP サーバー。Named Pipe 経由でエディターを操作 |
| `Projects/EditorLauncher/` | スタンドアロン起動ラッパー |
| `Projects/GameHub/src/` | プロジェクト管理ランチャー (Unity Hub 相当) |
| `Projects/Sandbox/src/` | エンジン検証用の最小アプリ |
| `Projects/Tests/` | GoogleTest 一式 (`TestKit/` + ドメイン別 + `Bench/`) |
| `GreenWare/` | **サンプルゲーム本体**。`Assets/` `Src/` `ProjectSettings/` を持つ独立プロジェクト |
| `GreenWare/Assets/Scripts/` | ゲームスクリプト (ヘッダーのみ。namespace は `sandbox`) |
| `Assets/` | エディター既定プロジェクトのアセット (Shaders / Models / Fonts 等) |
| `Docs/conventions/` | build-performance / git / test の 3 本 |
| `Docs/design/` | 機能単位の設計文書 |
| `Tools/` | VcBuild.ps1 (ビルド入口) / カバレッジ / Blender エクスポート / 各種ジェネレーター |

---

## タスク別の主要ファイル

| タスク | 主要ファイル |
|-------|------------|
| 新コンポーネント追加 | `Engine/Scene/Components/XxxComponent.hpp` (+ 必要なら `ScriptProxy/ScriptXxxProxy.hpp`) |
| 新システム追加 | `Engine/Scene/Systems/XxxSystem.hpp` + `Engine/src/Core/Scheduler/SystemScheduler.cpp` に Phase / ComponentAccess を登録 |
| Inspector UI 追加 | `Editor/src/Panels/Inspector/InspectorXxx.cpp` (まず `FBZZ_FIELD_*` で足りないか確認する) |
| エディター操作の追加 | `Editor/src/Op/` の登録簿へ 1 件足す (メニュー・ホットキー・パレット・AI バスに同時に出る) |
| 新シェーダー追加 | `Assets/Shaders/<Category>/Xxx.hlsl` へ保存 (Editor/CMake が自動収集・差分コンパイル) |
| アセット形式追加 | `Engine/Format/FzAssetFormat.hpp` + `Engine/Asset/` に Importer/Serializer |
| シーンシリアライズ変更 | 対象コンポーネントの `Reflect()` (旧: `SceneSerializer.cpp` の手書き分岐) |
| エディターパネル追加 | `Editor/include/Editor/Panels/XxxPanel.hpp` + `Editor/src/EditorApp.cpp` に登録 |
| ゲームスクリプト追加 | `GreenWare/Assets/Scripts/<領域>/XxxComponent.hpp` 1 ファイル (`.cpp` も `.generated.hpp` も不要) |

---

## 最重要制約 (詳細は AGENTS.md)

- GLM / GLFW / Bullet / PhysX / Box2D **使用禁止**
- `DX11Renderer*` / `DX12Renderer*` へのダウンキャスト **禁止** — 上位レイヤーは `IRenderer&` のみ。DX ヘッダーは各バックエンドの PRIVATE include にしかなく、CMake が強制する
- `new` / `delete` 直接使用 **禁止** — `make_unique` / `make_shared`
- `throw` / `std::exception` **禁止** — 回復可能は `bool`、回復不可能は `assert()`
- スクリプトから Engine 実装型を直接 include **禁止** — `ScriptProxy` (`transform` / `input` / `physics` … のメンバー) 経由
- **`.generated.hpp` は廃止された**。リフレクションは `FBZZ_FIELD*` + `FBZZ_REFLECT(T)` でヘッダー内に閉じる
- ビルドはターミナルから **行わない** — VSCode / Visual Studio のタスクから行う
- **コメントは「コードから読み取れないこと」だけ** — `.hpp` は Doxygen (`///`) で必要な API のみ、`.cpp` は WHY のみ。処理をなぞる説明は**書かない** (既存コードの旧スタイルを真似しない)
- **ファイルヘッダーは全ファイル必須** — `@file` / `@brief` / `@author Hasegawa Jin` / `@date` (作成日) の 4 行
- コミットに **Co-Authored-By / Generated with の類を入れない**。`git checkout -- <path>` は**使わない**
