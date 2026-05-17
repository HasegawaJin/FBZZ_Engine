# Git 運用規約

ポートフォリオとして GitHub に公開することを前提とした Git 運用ルール。
Solo 開発だが、採用担当者がブランチ・PR・コミット履歴を見ることを意識する。

---

## ブランチ構成

```
main
└── develop
    ├── feature/window
    ├── feature/triangle
    ├── feature/debug-draw
    ├── feature/physics
    ├── feature/scene
    └── feature/dx12
```

| ブランチ | 役割 |
|---------|------|
| `main` | **マイルストーン版のみ**。Step 完了時にのみ develop からマージ。タグを打つ |
| `develop` | **常にビルドが通る統合ブランチ**。feature をここにマージしていく |
| `feature/<name>` | **機能単位の実装ブランチ**。develop から切り、develop に戻す |

---

## feature ブランチの粒度

1 つの feature ブランチ = **「1 つの動作するサブシステム」**

| ブランチ名 | 実装内容 |
|-----------|---------|
| `feature/window` | Win32 ウィンドウ生成 + DX11 デバイス初期化 + スワップチェーン |
| `feature/triangle` | 頂点バッファ・インデックスバッファ・基本シェーダー・三角形描画 |
| `feature/debug-draw` | DebugDraw クラス・DebugLine.hlsl・コライダー可視化 |
| `feature/physics` | RigidBody・Collider・World・Solver |
| `feature/scene` | Scene・GameObject・Component・Transform・MeshRenderer |
| `feature/dx12` | DX12Renderer・DX12Buffer・DX12Shader |

---

## develop へのマージ条件 (feature → develop)

以下を**すべて満たしたら** develop に PR を出してマージする。

- [ ] ビルドが Release / Debug 両方で通る
- [ ] 意図した動作を自分で確認した (目視 or スクリーンショット)
- [ ] `git diff develop...HEAD` を読んで意図しない変更が含まれていない
- [ ] デバッグ用の `printf` / 一時コードを削除した
- [ ] WIP コミットをまとめた (`git rebase -i develop`)

---

## main へのマージ条件 (develop → main)

**「エンジンの動作として区切りがいい状態」** のときのみ。目安は Step 完了時。

```bash
git switch main
git merge --no-ff develop    # マージコミットを残す
git tag v0.1 -m "Step 1: Win32 ウィンドウ + DX11 初期化"
git push origin main --tags
```

---

## PR テンプレート

```
タイトル: [Feature] Win32 ウィンドウ生成と DX11 初期化

## 実装内容
- Win32 API で 1280x720 ウィンドウを生成
- DX11 デバイス・スワップチェーン・レンダーターゲット初期化

## 動作確認
- [x] ウィンドウが表示される
- [x] × ボタンで終了する
- [x] Debug / Release ビルドともに通る

## スクリーンショット
(画像を貼る)
```

---

## コミットメッセージ規約

```
[タグ] 動詞 + 概要 (日本語 50 字以内)

## 実装内容
- 箇条書きで列挙

## バグ修正  ← あれば
- 修正内容と原因を一行で
```

- **1 行目 (Subject)**: タグ + 動詞 + 概要。50 字以内。
- **本文 (Body)**: **Markdown で記述する。** GitHub の commit ページや PR のコミット一覧でそのままレンダリングされるため、見出し (`##`) と箇条書き (`-`) を使って構造化する。

| タグ | 用途 |
|-----|------|
| `[Feature]` | 機能追加 |
| `[Fix]` | バグ修正 |
| `[Design]` | 設計ドキュメント追加・変更 |
| `[Build]` | CMakeLists、ビルド設定 |
| `[Refactor]` | 動作を変えないリファクタリング |

```
# 良い
[Feature] Win32 ウィンドウ生成と DX11 デバイス初期化を実装

## 実装内容
- Win32 API で 1280x720 ウィンドウを生成
- DX11 デバイス・スワップチェーン・レンダーターゲット初期化

# 悪い
fix / 修正した / wip / update
```
