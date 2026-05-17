# FBZZ Engine 設計書

C++20 自作 3D ゲームエンジン。数学・物理エンジンを自作し、DirectX 11 レンダラーを抽象インターフェースで隠蔽する。  
就活ポートフォリオ目的。設計を重視し、Step 単位で段階的に実装する。

---

## ロードマップ

| Step | 内容 | 状態 |
|------|------|------|
| 0 | ビルド環境・Application ループ | **完了** |
| 1 | Win32 ウィンドウ表示 + DX11 初期化 | **完了** |
| 2 | 三角形描画 (頂点バッファ・シェーダー) | **完了** |
| 3 | デバッグ描画 (DebugDraw) | **完了** |
| 4 | 物理エンジン (重力・衝突) | **完了** |
| 5 | シーン管理 (Scene / Entity / Component / System) | **現在地** |
| 6 | DX12 / Render Graph 移行 | 未着手 |

---

## モジュール構成

```
FBZZ_Engine/
├── math/          独立ライブラリ。依存なし
├── physics/       math をリンク
├── engine/        physics / math をリンク
├── sandbox/       動作確認・サンプル。engine をリンク
├── third_party/   Assimp / DirectXTex
├── cmake/         コンパイラオプション等
└── docs/          設計ドキュメント
```

依存方向 (逆転禁止):

```
sandbox → engine → physics → math
```

engine 内の namespace 依存方向:

```
scene → renderer, physics, asset
asset → renderer
audio → (独立)
renderer → (独立)
core → (独立)
```

---

## ディレクトリ構成

### 実装済み (Step 0〜4)

```
engine/include/engine/
├── Core/        Application, Window, Logger, Time, HResult
├── Input/       Input, KeyCode
└── Renderer/    IRenderer, IBuffer, IShader, IConstantBuffer, ITexture,
                 IPipelineState, IRenderTarget, DrawCall, RenderLayer,
                 RenderState, SamplerMode, Camera, DebugDraw, ShaderManager

physics/         詳細: docs/physics/Design.md
math/
sandbox/
```

### Step 5 で追加

```
engine/include/engine/
├── Renderer/
│   ├── Mesh.hpp
│   ├── Material.hpp
│   └── LightSystem.hpp
├── Scene/
│   ├── Entity.hpp
│   ├── ComponentArray.hpp
│   ├── Transform.hpp
│   ├── Scene.hpp
│   ├── SceneManager.hpp
│   ├── Components/
│   │   ├── MeshRenderer.hpp
│   │   └── RigidBodyComponent.hpp
│   └── Systems/
│       ├── TransformSystem.hpp
│       ├── RenderSystem.hpp
│       └── PhysicsSystem.hpp
├── Asset/
│   ├── AssetManager.hpp
│   ├── MeshImporter.hpp
│   └── TextureImporter.hpp
└── Audio/
    └── AudioSystem.hpp
```

---

## アーキテクチャ原則

| 原則 | 内容 |
|------|------|
| デバッグ最優先 | 状態は Scene に一元管理。実行順序は線形かつ明示的 |
| Component はデータのみ | ロジックを持たない plain struct |
| System は free function | 副作用が明確。ブレークポイントが効く |
| 新機能は新ファイル | 既存コードを触らずに機能追加できる |
| レンダラー抽象化 | 上位レイヤーは `IRenderer&` のみ参照 |
| 数学・物理 | 自作のみ。GLM / Bullet / PhysX 禁止 |
| シングルスレッド | Step 1〜5 は `std::thread` 不使用 |

---

## 命名規則

| 対象 | 規則 | 例 |
|------|------|-----|
| クラス名 | `PascalCase` | `RigidBody`, `ShaderManager` |
| 関数名 | `PascalCase` | `ApplyForce()`, `GetComponent()` |
| 変数名 | `lowerCamelCase` | `deltaTime`, `vertexCount` |
| メンバ変数 | `m_` + `lowerCamelCase` | `m_position`, `m_isStatic` |
| 定数 / enum | `UPPER_SNAKE_CASE` | `MAX_ENTITIES`, `KEY_ESCAPE` |
| インターフェース | `I` + `PascalCase` | `IRenderer`, `IBuffer` |
| 名前空間 | `snake_case` | `fbzz::scene`, `fbzz::renderer` |
| ファイル | `PascalCase` | `RigidBody.cpp`, `IRenderer.hpp` |
| ディレクトリ | `PascalCase` | `Renderer/`, `Scene/`, `Asset/` |

ファイルヘッダーコメント形式 (全 `.hpp` / `.cpp` の先頭に記述):

```cpp
// FBZZ Engine
// Scene.hpp | fbzz::scene
// Entity・Component 配列の一元管理
```

---

## 所有権モデル

詳細: [docs/conventions/ownership.md](conventions/ownership.md)

| スマートポインタ | 使う場面 |
|----------------|---------|
| `std::unique_ptr` | デフォルト。1 箇所が所有するリソース |
| `std::shared_ptr` | 複数システム共有リソース (Mesh, Material, RigidBody) |
| raw pointer (`T*`) | 非所有の参照のみ (親 Entity 参照等) |

`new` / `delete` 直接使用禁止。`make_unique` / `make_shared` を使う。

---

## エラーハンドリング

詳細: [docs/conventions/error_handling.md](conventions/error_handling.md)

| 状況 | 手段 |
|------|------|
| 回復不可能エラー | `assert()` |
| 回復可能エラー | `bool` 戻り値 |
| DX11 HRESULT | `FBZZ_HR_CHECK(hr)` マクロ |
| 例外 | `throw` / `std::exception` 禁止 |

---

## 禁止パターン

| パターン | 理由 |
|---------|------|
| `dynamic_cast` | Component はデータのみ。型ごとの配列に直接アクセスするため不要 |
| `reinterpret_cast` | シェーダー定数バッファ転送以外で禁止 |
| グローバル変数 | `Application` シングルトン以外禁止 |
| `new` / `delete` 直接使用 | `make_unique` / `make_shared` を使う |
| `throw` / `std::exception` | エンジンコード内では禁止 |
| `#include` 循環依存 | 前方宣言で解決 |
| `DX11Renderer*` ダウンキャスト | 上位レイヤーは `IRenderer&` のみ参照 |
| GLM / Bullet / PhysX / Box2D | 数学・物理は自作 |

---

## サードパーティライブラリ

| ライブラリ | 用途 | 導入 Step |
|-----------|------|----------|
| DirectX 11 SDK | レンダリング | Step 1 |
| DirectXTex | テクスチャ読み込み | Step 2 |
| Assimp | メッシュ読み込み | Step 5 |
| XAudio2 | オーディオ再生 | Step 5 |
| DirectX 12 SDK | レイトレーシング | Step 6 |
| `Microsoft::WRL::ComPtr` | COM リソース管理 | Step 1 |

---

## Step 5 実装順

各サブドキュメントを参照すること。

| 順序 | モジュール | ドキュメント |
|------|-----------|-------------|
| 1 | Scene 基盤 (EntityID, ComponentArray, Transform) | [docs/scene/Design.md](scene/Design.md) |
| 2 | GameObject / Scene / SceneManager | [docs/scene/Design.md](scene/Design.md) |
| 3 | Renderer 追加 (Mesh, Material, LightSystem) | [docs/renderer/Design.md](renderer/Design.md) |
| 4 | MeshRenderer / RigidBodyComponent | [docs/scene/Design.md](scene/Design.md) |
| 5 | TransformSystem / RenderSystem / PhysicsSystem | [docs/scene/Design.md](scene/Design.md) |
| 6 | Asset System (AssetManager, MeshImporter, TextureImporter) | [docs/asset/Design.md](asset/Design.md) |
| 7 | Audio System (AudioSystem) | [docs/audio/Design.md](audio/Design.md) |
| 8 | sandbox: サンプルシーンで動作確認 | — |

---

## Git 運用

詳細: [docs/conventions/git.md](conventions/git.md)

```
main → develop → feature/<name>
```

コミット形式: `[Feature] / [Fix] / [Design] / [Build] / [Refactor] + 概要`
