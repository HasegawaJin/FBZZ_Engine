# Module Architecture

FBZZ Engine は依存方向を厳密に一方向に保つ **レイヤー分離モデル** を採用している。
外部ライブラリ (GLM / Bullet / PhysX 等) をゼロ依存で実装しつつ、
エディター・スタンドアロン・ランチャーの 3 実行バイナリを同一コアから派生させる設計。

---

## レイヤー構造

```
┌──────────────────────────────────────────────────┐
│  Sandbox  │  Editor (ImGui)  │  GameHub (Launcher) │  ← 実行バイナリ
└─────────────────────┬────────────────────────────┘
                      │ depends on
┌─────────────────────▼────────────────────────────┐
│                   Engine                          │  ← コアライブラリ
│  Core / Scene / Renderer / Asset / Audio / Input  │
└──────────┬───────────────────────────────────────┘
           │ depends on
┌──────────▼──────────┐   ┌──────────────────────┐
│       Physics        │   │        Math           │
│  GJK/EPA/BVH/World  │   │  Vec/Mat/Quat/Frustum │
└─────────────────────┘   └──────────────────────┘
```

**依存の方向は下向き専一。** Engine は Physics / Math に依存するが、逆方向の依存は存在しない。
DX11 具象実装 (`Projects/Engine/src/Renderer/Platform/DX11/`) は `IRenderer` インターフェース越しにのみ参照される。

---

## IModule — フレームループの抽象化

```
Engine::Application::Run()
  │
  ├─ IModule::OnInit()
  │
  └─ [frame loop]
       ├─ IModule::OnUpdate(dt)       ← Script / Input / Physics
       ├─ IModule::OnLateUpdate(dt)   ← Animator / IK / LateUpdate
       └─ IModule::OnRender()         ← BeginFrame → RenderSystem → EndFrame
```

`IModule` (`Engine/include/Engine/Core/IModule.hpp`) が `OnInit / OnUpdate / OnLateUpdate / OnRender / OnShutdown` の 5 フックを定義する。
`Application` はウィンドウ・入力・タイム・メモリ・プロファイラーの共通処理を担い、
「何を更新・描画するか」は各モジュール実装に委譲する。

| 実装クラス | バイナリ | 役割 |
|-----------|---------|------|
| `SandboxModule` | Sandbox.exe | スタンドアロン実行 + スクリプト DLL ホスト |
| `EditorModule` | Editor.exe | ImGui エディター + Play Mode |
| `HubApp` | GameHub.exe | プロジェクト管理ランチャー |

---

## SystemScheduler — ECS システムの位相管理

```
Phase::PreUpdate
  └─ TransformSystem, LifetimeSystem

Phase::Update
  └─ PhysicsSystem, NavMeshPatrolSystem, ScriptSystem

Phase::LateUpdate
  └─ AnimatorSystem, IKSystem, FootIKSystem

Phase::Render
  └─ RenderSystem, UISystem, AudioSystem
```

`SystemScheduler` (`Engine/include/Engine/Core/Scheduler/SystemScheduler.hpp`) が
各システムを位相 (Phase) と ComponentAccess 宣言に従って順序付けする。
システムは `ISystem` インターフェースを実装し、依存するコンポーネント型を宣言することで
将来的なデータ並列実行の準備ができている。

---

## Script DLL ホットリロード

```
Assets/Scripts/
  XxxComponent.hpp          ← ユーザー定義スクリプト
  XxxComponent.generated.hpp ← ScriptCodeGen が自動生成

  [MSBuild] → Scripts.dll

ScriptDllLoader
  ├─ ABI 署名検証 (GetScriptDllAbiSignature)  ← FNV-1a で型レイアウトをハッシュ化
  ├─ RegisterComponents() 呼び出し
  └─ Hot-reload: ファイル変更検知 → Unload → Compile → Load
```

スクリプトは `Script` 基底クラスを継承し、`ScriptProxy` 経由で Engine コンポーネントにアクセスする。
DLL ABI 互換性は `ScriptDllAbi.hpp` の `GetScriptDllAbiSignature()` で FNV-1a ハッシュ化された
`sizeof(Scene) / sizeof(Script) / ComponentList サイズ / _MSC_VER` によって検証される。

---

## メモリ管理

外部 `new` / `delete` は禁止。アロケーター階層は以下の通り。

| アロケーター | 用途 |
|-------------|------|
| `FrameAllocator` | フレーム終端に一括解放する一時データ |
| `LinearAllocator` | 線形確保のみの連続バッファ |
| `PoolAllocator` | 固定サイズオブジェクトの再利用プール |
| `StackAllocator` | LIFO 解放が保証されるスコープ付き確保 |

`MemoryTracker` が確保/解放を追跡し、`MemoryDebug` でリーク検出を行う。

---

## 並列処理

`TaskSystem` (`Engine/include/Engine/Core/Concurrency/TaskSystem.hpp`) がスレッドプールを管理する。
Physics ワールドの島分割・アニメーション Skinning・NavMesh ベイクなど
コンポーネント更新の一部をジョブとしてオフロードする。
`SystemScheduler` の ComponentAccess 宣言がデータ競合の静的検査に使われる。
