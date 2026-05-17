# docs_helper — 実装補助ドキュメント

モジュール別の詳細実装ガイド。実際にコードを書くときに参照する。

AI が常に参照すべき設計・規約は `docs/` にある。

---

## ディレクトリ構成

```
docs_helper/
├── engine/       Application, Window, Scene, GameObject 等の詳細仕様
├── math/         Vector / Matrix / Quaternion の実装詳細
├── physics/      World / RigidBody / Collider / Solver の実装詳細
└── renderer/     IRenderer / DX11 / Camera / Light 等の実装詳細
```

## docs/ との役割分担

| ディレクトリ | 役割 | 内容 |
|------------|------|------|
| `docs/` | AI 参照・設計基準 | Design.md、conventions/ (規約) |
| `docs_helper/` | 実装補助 | モジュール別詳細仕様・コードスニペット |
