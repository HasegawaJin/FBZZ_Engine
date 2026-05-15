# FBZZ Engine 設計書

C++20 自作 3D ゲームエンジン。数学・物理エンジンを自作し、DX11 レンダラーを抽象インターフェースで隠蔽する。

---

## モジュール構成

```
FBZZ_Engine
├── math       独立ライブラリ。自作数学ライブラリ
├── physics    独立ライブラリ。math をリンク
├── engine     コアエンジン。physics / math をリンク
└── sandbox    動作確認・サンプルアプリ
```

依存方向:

```
sandbox → engine → physics → math
                ↘           ↗
                  (math を共有)
```

---

## 規約ドキュメント

| ドキュメント | 内容 |
|-------------|------|
| [conventions/error_handling.md](conventions/error_handling.md) | HRESULT チェック、assert、bool 戻り値の使い分け |
| [conventions/ownership.md](conventions/ownership.md) | unique_ptr / shared_ptr / raw pointer の使い分け規約 |
| [conventions/threading.md](conventions/threading.md) | シングルスレッドモデル、Step 6 以降の分離方針 |
| [conventions/git.md](conventions/git.md) | ブランチ戦略、コミットメッセージ規約、ポートフォリオ管理 |

---

## 詳細設計ドキュメント

### Math モジュール

| ドキュメント | 内容 |
|-------------|------|
| [math/vector.md](math/vector.md) | Vector2 / Vector3 / Vector4 の API・演算子一覧 |
| [math/matrix.md](math/matrix.md) | Matrix3 / Matrix4、HLSL との Row/Column-Major 対応 |
| [math/quaternion.md](math/quaternion.md) | Quaternion、Slerp、ジンバルロック回避 |
| [math/utils.md](math/utils.md) | 定数 (PI, DEG2RAD)、Clamp / Lerp 等スカラー関数 |

### Physics モジュール

| ドキュメント | 内容 |
|-------------|------|
| [physics/world.md](physics/world.md) | World、シミュレーションループ全体 |
| [physics/rigidbody.md](physics/rigidbody.md) | RigidBody、半陰的オイラー積分 |
| [physics/collider.md](physics/collider.md) | Sphere / AABB / Capsule コライダー |
| [physics/solver.md](physics/solver.md) | Broad/Narrow フェーズ、インパルスベース衝突解決 |

### Renderer モジュール

| ドキュメント | 内容 |
|-------------|------|
| [renderer/interface.md](renderer/interface.md) | IRenderer / IBuffer / IShader / ITexture |
| [renderer/camera.md](renderer/camera.md) | Camera、ビュー・プロジェクション行列 |
| [renderer/shader.md](renderer/shader.md) | ShaderManager、HLSL、実行時コンパイル |
| [renderer/material.md](renderer/material.md) | Material、DrawCall |
| [renderer/dx11.md](renderer/dx11.md) | DX11Renderer / DX11Buffer / DX11Shader の実装 |
| [renderer/debug_draw.md](renderer/debug_draw.md) | DebugDraw ヘルパー、コライダー可視化 |
| [renderer/render_state.md](renderer/render_state.md) | RasterizerMode / BlendMode / DepthMode プリセット |
| [renderer/render_target.md](renderer/render_target.md) | IRenderTarget、オフスクリーン描画、Resize |

### Engine モジュール

| ドキュメント | 内容 |
|-------------|------|
| [engine/application.md](engine/application.md) | Application (シングルトン)、ゲームループ |
| [engine/window.md](engine/window.md) | Window (Win32)、メッセージポンプ |
| [engine/time.md](engine/time.md) | Time、DeltaTime / TotalTime / TimeScale |
| [engine/logger.md](engine/logger.md) | Logger、ログレベル、FBZZ_LOG_* マクロ |
| [engine/scene.md](engine/scene.md) | Scene、GameObject の管理 |
| [engine/gameobject.md](engine/gameobject.md) | GameObject / Component / Transform |
| [engine/camera_component.md](engine/camera_component.md) | CameraComponent、主カメラ登録 |
| [engine/input.md](engine/input.md) | Input、KeyCode、マウス |
| [engine/mesh.md](engine/mesh.md) | MeshLoader (Assimp)、Vertex フォーマット |
| [engine/resource_manager.md](engine/resource_manager.md) | ResourceManager、Mesh / Texture キャッシュ |

---

## ロードマップ

| Step | 内容 | 状態 |
|------|------|------|
| 0 | ビルド環境・Application ループ | 完了 |
| 1 | Win32 ウィンドウ表示 + DX11 初期化 | 未着手 |
| 2 | 三角形描画 (頂点バッファ・シェーダー) | 未着手 |
| 3 | デバッグ描画 (DebugDraw) | 未着手 |
| 4 | 物理エンジン (重力・衝突) | 未着手 |
| 5 | シーン管理 (GameObject / Component) | 未着手 |
| 6 | DX12 / レイトレーシング移行 | 未着手 |

---

## サードパーティライブラリ

| ライブラリ | 用途 | 状態 |
|-----------|------|------|
| DirectX 11 SDK | レンダリング | Step 1 で導入 |
| Assimp | メッシュ読み込み | Step 5 で導入 |
| DirectX 12 SDK | レイトレーシング | Step 6 で導入 |

**使用しないもの:** GLM (数学は自作)、GLFW (ウィンドウは Win32)、Bullet / PhysX / Box2D (物理は自作)

---

## 命名規則

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

## ディレクトリ構成

```
engine/
├── include/engine/
│   ├── Core/           Application.hpp, Window.hpp
│   ├── Renderer/       IRenderer.hpp, IBuffer.hpp, Camera.hpp ...
│   ├── Scene/          Scene.hpp, GameObject.hpp, Component.hpp ...
│   ├── Input/          Input.hpp, KeyCode.hpp
│   └── Mesh/           Mesh.hpp, MeshLoader.hpp
└── src/
    ├── Core/
    ├── Renderer/
    │   └── Platform/
    │       └── DX11/   DX11Renderer.hpp/.cpp ...
    ├── Scene/
    ├── Input/
    └── Mesh/

math/
├── include/math/       Vector2.hpp, Vector3.hpp, Matrix4.hpp, Quaternion.hpp ...
└── src/

physics/
├── include/physics/    World.hpp, RigidBody.hpp, Collider.hpp ...
└── src/
```
