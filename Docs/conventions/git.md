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
    ├── feature/dx12
    └── optimize/terrain
```

| ブランチ | 役割 |
|---------|------|
| `main` | **マイルストーン版のみ**。Step 完了時にのみ develop からマージ。タグを打つ |
| `develop` | **常にビルドが通る統合ブランチ**。feature をここにマージしていく |
| `feature/<name>` | **機能単位の実装ブランチ**。develop から切り、develop に戻す |
| `optimize/<name>` | **パフォーマンス改善ブランチ**。機能追加を伴わない最適化専用。develop から切り、develop に戻す |

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

**「エンジンの動作として区切りがいい状態」** かつ **develop の CI が通っている** ときのみ。

1. GitHub で `develop → main` の PR を出す (タイトルは `[Release] vX.Y.Z ...`)。PR の CI が通ったら **Create a merge commit** でマージする (マージコミットを残す)
2. main のマージコミットに注釈付きタグを打ち、GitHub Release を作る

```bash
git switch main && git pull
git tag -a v0.9.1 -m "v0.9.1: <概要>"
git push origin v0.9.1
gh release create v0.9.1 --verify-tag --title "v0.9.1" --notes-file <リリースノート>
```

### タグの書式

- **`vMAJOR.MINOR.PATCH` の 3 桁で書く** (`v0.9.1`)。`v0.9` のような 2 桁は使わない (旧タグ `v0.5`〜`v0.9` は `v0.5.0`〜`v0.9.0` へ改名済み)
- MINOR は機能の区切り (旧 Step 完了相当)、PATCH は同じ区切りの中の修正・整備
- **タグと SDK の版は同じ番号にする**。リリース前に develop で `CMakeLists.txt` の `project(FBZZEngine VERSION X.Y.Z)` を上げ、その版でタグを打つ
  - 版は SDK の置き場所 `SDK/<版>/`・`find_package(FBZZ <版> EXACT)`・Script DLL の ABI 署名に効く。上げたら GreenWare の `CMakeLists.txt` と `.fbzz_proj` (`engine_version` / `sdk_root`)、`.vscode/launch.json` の SDK 起動先も揃え、SDK を公開し直す
  - 版を上げるとスクリプト DLL は再ビルドが要る (旧版の DLL は ABI 署名の不一致で読み込みを拒否される)

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
| `[Optimize]` | パフォーマンス改善（LOD・カリング・描画バッチ等） |
| `[Design]` | 設計ドキュメント追加・変更 |
| `[Build]` | CMakeLists、ビルド設定 |
| `[Refactor]` | 動作を変えないリファクタリング |
| `[Chore]` | .gitignore / TASKS.md / コメント修正など、コードの動作に影響しない雑務 |
| `[Release]` | develop → main マイルストーンマージ |

```
# 良い
[Feature] Win32 ウィンドウ生成と DX11 デバイス初期化を実装

## 実装内容
- Win32 API で 1280x720 ウィンドウを生成
- DX11 デバイス・スワップチェーン・レンダーターゲット初期化

# 悪い
fix / 修正した / wip / update
```
