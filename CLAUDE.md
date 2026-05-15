# FBZZ Engine - Claude への指示

詳細設計は `docs/design.md` を参照。以下の方針から外れないようにすること。

## 絶対に守ること

- **数学ライブラリは自作** (GLM 等の外部ライブラリを使わない)
- **物理エンジンは自作** (Bullet / PhysX 等を使わない)
- **ヘッダファイルの拡張子は `.hpp`** で統一
- **C++20** を使用
- **名前空間は `fbzz::`** で統一 (例: `fbzz::physics`, `fbzz::renderer`)

## アーキテクチャ方針

- レンダラーは **抽象インターフェース (`IRenderer`)** を介してコードを書く。DX11 の具体実装に直接依存しない
- シーン管理は **Unity 同様の GameObject / Component パターン (OOP)**
- モジュールの依存方向: `sandbox → engine → physics → math`
- 依存の逆転 (上位モジュールが下位モジュールに依存) を作らない

## サードパーティライブラリ

追加してよいもの:
- Assimp (メッシュ読み込み)
- DirectX 11 SDK (レンダリング)
- DirectX 12 SDK (レンダリング)

追加してはいけないもの:
- GLM (数学は自作)
- GLFW (ウィンドウ・入力)
- Bullet / PhysX / Box2D (物理は自作)

## コーディング規約

- クラス名: `PascalCase` (例: `RigidBody`, `ShaderManager`)
- 関数名: `PascalCase` (例: `ApplyForce()`, `GetComponent()`)
- 変数名: `lowerCamelCase` (例: `deltaTime`, `vertexCount`)
- メンバ変数: `m_` + `lowerCamelCase` (例: `m_position`, `m_isStatic`)
- 定数 / enum: `UPPER_SNAKE_CASE` (例: `MAX_LIGHTS`)
- インターフェース: `I` + `PascalCase` (例: `IRenderer`, `IBuffer`)
- 名前空間: `snake_case` (例: `fbzz::physics`)
- ソース・ヘッダファイル名: `PascalCase` (例: `Main.cpp`, `IRenderer.hpp`)
- ディレクトリ名: `PascalCase` (例: `Renderer/`, `Core/`, `DX11/`)
- コメントは WHY が自明でないときのみ書く。WHAT は書かない

## ロードマップ (現在地を把握すること)

```
Step 1  ウィンドウ表示 (GLFW + DX11)
Step 2  三角形描画 (頂点バッファ, シェーダー)
Step 3  デバッグ描画 (線, 矩形, 円)
Step 4  物理エンジン (重力, 衝突)
Step 5  シーン管理 (GameObject / Component)
Step 6  DX12 / レイトレーシング移行
```
