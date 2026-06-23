# Claude Instructions

詳細な作業規約は `AGENTS.md` を参照。このファイルはセッション開始時のクイックオリエンテーション。

---

## プロジェクト概要

**FBZZ Engine** — C++20 自作 3D ゲームエンジン (Windows / DirectX 11)。GLM・Bullet・PhysX を使わず数学・物理エンジンをゼロ実装。ImGui フルエディター・NavMesh・地形・フォリッジ・水面・スクリプト DLL ホットリロードを備えたフルセット開発環境。**ポートフォリオ公開リポジトリ**。

名前空間: `fbzz::` | 依存方向: `Sandbox / Editor / GameHub → Engine → Physics → Math`

---

## ディレクトリマップ

| パス | 役割 |
|------|------|
| `Projects/Math/include/Math/` | 自作数学ライブラリ (Vector/Matrix/Quaternion/Ray/Frustum/Plane) |
| `Projects/Physics/include/Physics/` | 自作物理エンジン (GJK/EPA/BVH/RigidBody/Constraint) |
| `Projects/Engine/include/Engine/Core/` | Application / Window / Time / Logger / Input / EventBus / TaskSystem |
| `Projects/Engine/include/Engine/Core/Memory/` | Frame / Linear / Pool / Stack アロケーター |
| `Projects/Engine/include/Engine/Core/Scheduler/` | SystemScheduler / Phase / ComponentAccess |
| `Projects/Engine/include/Engine/Renderer/` | IRenderer / IBuffer / ITexture / IShader 等インターフェース |
| `Projects/Engine/src/Renderer/Platform/DX11/` | DX11 具体実装 (上位レイヤーから直接参照禁止) |
| `Projects/Engine/include/Engine/Scene/` | Scene / GameObject / Transform / Script |
| `Projects/Engine/include/Engine/Scene/Components/` | 全コンポーネントヘッダー |
| `Projects/Engine/include/Engine/Scene/Systems/` | 全システムヘッダー |
| `Projects/Engine/include/Engine/Scene/ScriptProxy/` | 18 種のスクリプトプロキシ (DLL 境界を越えた型安全アクセス) |
| `Projects/Engine/include/Engine/Asset/` | AssetManager / AssetHandle / FzAssetFormat / FzModelFormat |
| `Projects/Editor/include/Editor/Panels/` | ImGui パネル群 |
| `Projects/Editor/src/Panels/Inspector/` | Inspector カテゴリ別実装 |
| `Projects/Editor/src/Tools/` | TerrainTool / FoliageTool / WaterTool / DetailTool |
| `Projects/GameHub/src/` | プロジェクト管理ランチャー (Unity Hub 相当) |
| `Assets/Shaders/` | HLSL ソース (`compiled/` に事前コンパイル済み .cso) |
| `Assets/Scripts/` | ユーザースクリプト + コード生成済み `.generated.hpp` |
| `Docs/conventions/` | 所有権・エラー処理・Git・スレッド規約 |

---

## タスク別の主要ファイル

| タスク | 主要ファイル |
|-------|------------|
| 新コンポーネント追加 | `Engine/include/Engine/Scene/Components/XxxComponent.hpp` + `ScriptProxy/ScriptXxxProxy.hpp` |
| 新システム追加 | `Engine/include/Engine/Scene/Systems/XxxSystem.hpp` + `Engine/src/Core/Scheduler/SystemScheduler.cpp` に登録 |
| Inspector UI 追加 | `Editor/src/Panels/Inspector/InspectorXxx.cpp/.hpp` |
| 新シェーダー追加 | `Assets/Shaders/<Category>/Xxx.hlsl` → `compile_shaders.bat` 実行 |
| アセット形式追加 | `Engine/include/Engine/Asset/FzAssetFormat.hpp` |
| シーンシリアライズ変更 | `Engine/src/Scene/SceneSerializer.cpp` |
| エディターパネル追加 | `Editor/include/Editor/Panels/XxxPanel.hpp` + `Editor/src/EditorApp.cpp` に登録 |
| スクリプト追加 | `Assets/Scripts/XxxComponent.hpp` → ScriptCodeGen で `.generated.hpp` 自動生成 |

---

## 最重要制約 (詳細は AGENTS.md)

- GLM / GLFW / Bullet / PhysX / Box2D **使用禁止**
- `DX11Renderer*` へのダウンキャスト **禁止** — 上位レイヤーは `IRenderer&` のみ参照
- `new` / `delete` 直接使用 **禁止** — `make_unique` / `make_shared` を使う
- `throw` / `std::exception` **禁止**
- スクリプトから Engine 実装に直接依存 **禁止** — `ScriptProxy` 経由でアクセス
- ビルドはターミナルから **行わない** — VSCode / Visual Studio からビルドすること
- **コメントは積極的に書く** — ポートフォリオ公開リポジトリのため設計意図を必ず残す
