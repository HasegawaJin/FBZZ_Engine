# FBZZ Engine

C++20 で自作する 3D ゲームエンジン。数学・物理エンジンをゼロから実装し、DirectX 11 レンダラーを抽象インターフェースで隠蔽する構成。

> ゲーム業界就職ポートフォリオ向けに設計から実装まで一貫して自作することにこだわったプロジェクト。

---

## 特徴

- **数学ライブラリ自作** — Vector2 / Vector3 / Vector4 / Matrix4 / Quaternion を GLM に頼らず実装
- **物理エンジン自作** — 半陰的オイラー積分、AABB ブロードフェーズ、インパルスベース衝突解決
- **抽象レンダラー** — `IRenderer` インターフェースで DX11 を隠蔽。将来 DX12 / レイトレに差し替え可
- **Win32 ネイティブ** — GLFW に依存せず Win32 API でウィンドウ・入力を管理
- **C++20** — `std::span`, Concepts, designated initializers を活用

---

## 進捗

| Step | 内容 | 状態 |
|------|------|------|
| 0 | ビルド環境・Application ループ | ✅ 完了 |
| 1 | Win32 ウィンドウ表示 + DX11 初期化 | 🔲 未着手 |
| 2 | 三角形描画 (頂点バッファ・シェーダー) | 🔲 未着手 |
| 3 | デバッグ描画 (DebugDraw) | 🔲 未着手 |
| 4 | 物理エンジン (重力・衝突) | 🔲 未着手 |
| 5 | シーン管理 (GameObject / Component) | 🔲 未着手 |
| 6 | DX12 / レイトレーシング移行 | 🔲 未着手 |

---

## モジュール構成

```
FBZZ_Engine
├── math       独立ライブラリ。自作数学ライブラリ (GLM 不使用)
├── physics    独立ライブラリ。math をリンク (Bullet / PhysX 不使用)
├── engine     コアエンジン。physics / math をリンク
└── sandbox    動作確認・サンプルアプリ
```

依存方向: `sandbox → engine → physics → math`

---

## ビルド方法

### 要件

- Windows 10/11
- Visual Studio 2022 (C++20 対応)
- CMake 3.20 以上
- DirectX 11 SDK (Windows SDK に同梱)

### ビルド手順

```powershell
git clone https://github.com/Ungoi18/FBZZ_Engine.git
cd FBZZ_Engine
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

---

## ディレクトリ構成

```
FBZZ_Engine/
├── engine/
│   ├── include/engine/
│   │   ├── Core/        Application.hpp, Window.hpp
│   │   ├── Renderer/    IRenderer.hpp, IBuffer.hpp, Camera.hpp
│   │   ├── Scene/       Scene.hpp, GameObject.hpp, Component.hpp
│   │   ├── Input/       Input.hpp, KeyCode.hpp
│   │   └── Mesh/        Mesh.hpp, MeshLoader.hpp
│   └── src/
│       └── Renderer/Platform/DX11/  DX11 具体実装
├── math/
│   └── include/math/    Vector2.hpp, Vector3.hpp, Matrix4.hpp, Quaternion.hpp
├── physics/
│   └── include/physics/ World.hpp, RigidBody.hpp, Collider.hpp
├── sandbox/             サンプルアプリ
└── docs/                設計ドキュメント
    ├── design.md        全体設計・ロードマップ
    └── conventions/     コーディング規約・Git 運用
```

---

## 設計ドキュメント

詳細設計は [`docs/design.md`](docs/design.md) を参照。

| カテゴリ | ドキュメント |
|---------|------------|
| 全体設計 | [docs/design.md](docs/design.md) |
| 数学 | [Vec/Mat/Quaternion](docs/math/) |
| 物理 | [World/RigidBody/Collider/Solver](docs/physics/) |
| レンダラー | [IRenderer/Camera/Shader/DX11](docs/renderer/) |
| エンジン | [Application/Scene/GameObject](docs/engine/) |
| 規約 | [エラー処理/所有権/スレッド/Git](docs/conventions/) |

---

## 使用ライブラリ

| ライブラリ | 用途 | 導入 Step |
|-----------|------|----------|
| DirectX 11 SDK | レンダリング | Step 1 |
| Microsoft::WRL (ComPtr) | COM リソース RAII | Step 1 |
| Assimp | メッシュ読み込み | Step 5 |
| DirectX 12 SDK | DX12 移行 | Step 6 |

**使用しないライブラリ:** GLM (数学は自作) / GLFW (ウィンドウは Win32) / Bullet・PhysX (物理は自作)

---

## ライセンス

MIT License — 詳細は [LICENSE](LICENSE) を参照。
