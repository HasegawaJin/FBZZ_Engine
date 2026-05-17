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

`step` プレフィックスは使わない。機能名で切ることで何を実装したかが一目でわかる。

---

## feature ブランチの切り方・単位

### 粒度の目安

1 つの feature ブランチ = **「1 つの動作するサブシステム」**

細かすぎると PR が増えてノイズ。大きすぎると差分が読みにくい。

| ブランチ名 | 実装内容 |
|-----------|---------|
| `feature/window` | Win32 ウィンドウ生成 + DX11 デバイス初期化 + スワップチェーン |
| `feature/triangle` | 頂点バッファ・インデックスバッファ・基本シェーダー・三角形描画 |
| `feature/debug-draw` | DebugDraw クラス・DebugLine.hlsl・コライダー可視化 |
| `feature/physics` | RigidBody・Collider・World・Solver |
| `feature/scene` | Scene・GameObject・Component・Transform・MeshRenderer |
| `feature/dx12` | DX12Renderer・DX12Buffer・DX12Shader |

### ブランチを途中で分割したいとき

実装中に「これは別機能だった」と気づいた場合は分割してよい。
例: `feature/physics` を実装中に `feature/math-utils` が必要になったら先に切って develop にマージし、その後 `feature/physics` で rebase する。

---

## いつブランチを切るか

```
develop に前の機能がマージされた直後 → 次の feature を切る
```

並行して複数 feature ブランチを持つのは避ける。
前の機能が develop に入っていない状態で次を始めると、コンフリクトリスクが増える。

```bash
# 例: feature/window が develop にマージされた後
git switch develop
git pull origin develop
git switch -c feature/triangle
```

---

## いつ develop にマージするか (feature → develop)

以下を**すべて満たしたら** develop に PR を出してマージする。

- [ ] ビルドが Release / Debug 両方で通る
- [ ] 意図した動作を自分で確認した (目視 or スクリーンショット)
- [ ] `git diff develop...HEAD` を読んで意図しない変更が含まれていない
- [ ] デバッグ用の `printf` / 一時コードを削除した
- [ ] コミット履歴が読める状態 (WIP コミットをまとめた)

### WIP コミットをまとめる (Interactive Rebase)

実装中の細かい「wip」「typo」コミットは PR 前にまとめる。

```bash
# develop との分岐点以降のコミットをまとめる
git rebase -i develop
```

まとめた後のコミット履歴の理想:

```
[Feature] Win32 ウィンドウ生成と DX11 デバイス初期化を実装
[Feature] DX11 スワップチェーンとレンダーターゲット初期化を追加
```

---

## いつ main にマージするか (develop → main)

**「エンジンの動作として区切りがいい状態」** のときのみ。
目安は Step の完了時。

```
Step 0 完了 → main にマージ → タグ: v0.0
Step 1 完了 → main にマージ → タグ: v0.1
Step 2 完了 → main にマージ → タグ: v0.2
...
```

```bash
git switch main
git merge --no-ff develop    # マージコミットを残す (履歴を明確にする)
git tag v0.1 -m "Step 1: Win32 ウィンドウ + DX11 初期化"
git push origin main --tags
```

`--no-ff` (no fast-forward) を使う理由: fast-forward だとマージのタイミングが履歴から消えてしまうため。

---

## PR の使い方

Solo 開発でも PR を使う。理由: **GitHub 上に diff と説明が残り、ポートフォリオとして読める**。

### PR タイトルと本文

```
タイトル: [Feature] Win32 ウィンドウ生成と DX11 初期化

## 実装内容
- Win32 API で 1280x720 ウィンドウを生成
- DX11 デバイス・スワップチェーン・レンダーターゲット初期化
- Application::Run() のゲームループに統合

## 動作確認
- [x] ウィンドウが表示される
- [x] × ボタンで終了する
- [x] Debug / Release ビルドともに通る

## スクリーンショット
(画像を貼る)
```

---

## コミットメッセージ規約

### 形式

```
[タグ] 動詞 + 概要 (日本語 50 字以内)
```

### タグ一覧

| タグ | 用途 |
|-----|------|
| `[Feature]` | 機能追加 |
| `[Fix]` | バグ修正 |
| `[Design]` | 設計ドキュメント追加・変更 |
| `[Build]` | CMakeLists、ビルド設定 |
| `[Refactor]` | 動作を変えないリファクタリング |

### 良い例 / 悪い例

```
# 良い
[Feature] Win32 ウィンドウ生成と DX11 デバイス初期化を実装
[Feature] 頂点バッファと基本シェーダーで三角形を描画
[Fix]     Transform 親子破棄時の dangling pointer を修正
[Design]  IRenderer に CreateVertexBuffer の stride パラメータを追加

# 悪い
fix
修正した
wip
update
とりあえず
```

---

## README の更新タイミング

各 feature が develop にマージされたタイミングで README の進捗テーブルを更新する。
動作スクリーンショット / GIF は PR 本文に貼り、README にも転記する。

---

## .gitignore の方針

```gitignore
# ビルド成果物
build/
out/
*.obj
*.pdb
*.ilk
*.exp
*.exe
*.dll

# IDE
.vs/
*.user
*.suo

# OS
Thumbs.db
.DS_Store
```

シェーダー (`.hlsl`) とアセット (`assets/`) はリポジトリに含める。
サードパーティ (Assimp 等) はシステムインストールを前提とし、リポジトリに含めない。
