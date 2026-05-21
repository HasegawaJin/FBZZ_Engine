# TASKS — FBZZ Engine

作業開始時に確認し、作業終了時に更新すること。  
詳細設計は `docs/` 以下の各 Design.md を参照。

---

## 現在地: Step 6 — ImGui Editor

詳細: [docs/editor/Design.md](docs/editor/Design.md)

| # | タスク | 状態 |
|---|--------|------|
| 1 | Engine Util 整備 (ILogSink / FileSystem / StringUtils) | ✅ 完了 |
| 2 | ImGui → IRenderer 統合 | ✅ 完了 |
| 3 | EditorApp 骨格 (DockSpace + MenuBar) | ✅ 完了 |
| 4 | Util 各種 (MathConvert / ImGuiWidgets / UndoStack / HotkeyManager / ConsoleSink / ModalDialog / EditorSettings / FileDialog) | ✅ 完了 |
| 5 | SceneHierarchyPanel | ✅ 完了 |
| 6 | InspectorPanel — Transform 編集 | ✅ 完了 |
| 7 | ViewportPanel + EditorCamera | ✅ 完了 |
| 8 | Gizmo (ImGuizmo) + MousePicking | ✅ 完了 |
| 9 | LightPanel | ✅ 完了 |
| 10 | ConsolePanel / AssetBrowserPanel / StatusBar | ✅ 完了 |
| 11 | SceneSerializer — engine::scene::SceneSerializer Save/Load | ✅ 完了 |
| 12 | PlayModeController — Play/Pause/Stop 基本動作 | ✅ 完了 |
| 13 | InspectorPanel — MeshRenderer / LightComponent / CameraComponent / ParticleEmitter / AudioSource / RigidBody / SkyRenderer Component 編集 | ✅ 完了 |
| 14 | editor::SceneSerializer Load/Deserialize — PlayMode Stop 時のスナップショット復元 | ✅ 完了 |
| 15 | MenuBar: File > Save Scene / Open Scene | ✅ 完了 |

---

## Step 6.5 — SceneSerializer 統合

詳細: [docs/scene/SceneSerializer.md](docs/scene/SceneSerializer.md)

| # | タスク | 状態 |
|---|--------|------|
| 1 | sandbox/main.cpp を SceneSerializer::Load で短縮 (~80 行目標) | 🔄 進行中 (初回起動で assets/scenes/*.fbzz 生成後に短縮可) |
| 2 | .fbzz シーンファイルの作成 (現 main.cpp のシーン定義を移植) | 🔄 進行中 (初回起動で assets/scenes/ に自動生成) |

---

## エンジン モジュール

詳細: [docs/engine/Design.md](docs/engine/Design.md)

| モジュール | 状態 | 備考 |
|-----------|------|------|
| Core / Input / Renderer / Scene 基盤 | ✅ 完了 | |
| Asset / Audio / Util | ✅ 完了 | |
| LightComponent / CameraComponent / AudioSourceComponent | ✅ 完了 | docs の「未実装」表記は誤り |
| AudioSystem free function (scene::AudioSystem) | ✅ 完了 | docs の「未実装」表記は誤り |
| RenderSystem LightComponent 収集 (LightSystem 引数削除) | ✅ 完了 | docs の「未実装」表記は誤り |

---

## 物理エンジン (Step 4 以降)

詳細: [docs/physics/Design.md](docs/physics/Design.md)

| 機能 | 状態 |
|------|------|
| CapsuleCollider | ✅ 完了 |
| Unity-style ColliderComponent / VolumeComponent 化 | ✅ 完了 |
| Volume 系 (6 種) | ✅ 完了 (ColliderVolume + VolumeComponent に統合) |
| Constraint 系 (5 種) | ✅ 完了 |

---

## Step 7 以降

| Step | 内容 | 状態 |
|------|------|------|
| 7 | DX12 移行 + RenderGraph | ❌ 未着手 |
| 7 | DXR (Ray Tracing) | ❌ 未着手 |
| — | Script / Reflect システム | ❌ 未着手 ([docs/scene/Script.md](docs/scene/Script.md)) |

---

## Script / Reflect

| # | Task | Status |
|---|------|--------|
| 1 | Script base / IReflector / ScriptComponent | Done |
| 2 | Scene registration / GameObject AddScript / GetScript | Done |
| 3 | ScriptSystem and SceneManager Update integration | Done |
| 4 | ImGuiReflector and InspectorPanel script section | Done |
| 5 | Sandbox PlayerController sample | Done |
| 6 | SceneSerializer v2 ScriptFactory / TOML reflector | Done |

---

## Step 6.5 Progress

| # | Task | Status |
|---|------|--------|
| 1 | sandbox/main.cpp loads a .fbzz scene through SceneSerializer | Done |
| 2 | Physics verification scene moved to assets/scenes/PhysicsTest.fbzz | Done |
| 3 | Runtime RigidBodyComponent bootstrap for PhysicsTest | Done |
