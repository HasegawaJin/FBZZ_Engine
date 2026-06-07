# Animation Graph Editor 設計書

## 目的

`AnimatorComponent` のステートマシンをノードグラフとして視覚的に編集するエディタパネル。  
ランタイムの `AnimatorComponent` 設計は変更せず、**Editor 層の UI のみを追加**する。  
Inspector のリスト形式では把握しにくいステート間の遷移フローを一目で確認・編集できることが目標。

---

## ユースケース

| ユースケース | 担当 |
|---|---|
| ステートをノードとして配置・移動 | `AnimationGraphPanel` |
| ステート間の遷移をドラッグ接続 | `AnimationGraphPanel` |
| パラメーター（Float/Int/Bool/Trigger）の追加・削除 | サイドバー |
| 遷移条件（Condition）の編集 | ノード選択 → 詳細ペイン |
| 再生中の現在ステートをリアルタイムでハイライト | `AnimationGraphPanel` |
| 既存の Inspector 編集との双方向同期 | 共通データ（`AnimatorComponent`）を直接参照 |

---

## 依存ライブラリ

**imnodes** — Dear ImGui 用ノードエディタライブラリ  
GitHub: https://github.com/Nelarius/imnodes  
MIT ライセンス。ノードの描画・ドラッグ移動・リンク接続・ピン管理を提供する。  
`ThirdParty/imnodes/` に配置し、既存の ImGui と同様に `ThirdParty/CMakeLists.txt` に追加する。

---

## アーキテクチャ

```
Editor Layer
  AnimationGraphPanel          ← 本設計の追加対象
    ↓ AnimatorComponent を直接読み書き
    ↓ ノード位置は GraphLayout（Editor 専用）に保持

Scene Layer
  AnimatorComponent            ← 既存。変更なし
    states[]                   ← ノードに対応
    parameters[]               ← サイドバーに表示
    AnimationTransition        ← リンク（エッジ）に対応

System Layer
  AnimatorSystem               ← 既存。変更なし
```

### データの所有関係

`AnimatorComponent` はランタイムデータ（ステート・パラメーター・遷移）を持つ。  
ノードの **キャンバス上の座標（X, Y）は EditorContext 内の `GraphLayout` が所有**する。

```
EditorContext
  graphLayouts : unordered_map<std::string /*instanceId*/, GraphLayout>

GraphLayout
  nodePositions : unordered_map<std::string /*stateName*/, ImVec2>
```

WHY: キャンバス座標はエディタ専用情報であり、ランタイムの `AnimatorComponent` を汚染しない。  
また `instanceId` をキーにすることで GameObject リネーム後も対応関係が維持される。

---

## UI レイアウト

```
┌─────────────────────────────────────────────────────────┐
│ Animation Graph  [GameObject名]  [+ State] [▶ Play]     │
├──────────────┬──────────────────────────────────────────┤
│ Parameters   │                                          │
│              │                                          │
│ [Float] Speed│      ┌──────────┐                        │
│  0.00        │      │  Idle  ● │──────→ ┌──────────┐   │
│ [Bool] Grnd  │      └──────────┘        │  Walk    │   │
│  false       │                          └──────────┘   │
│ [Trig] Jump  │                ┌──────────┐             │
│              │                │  Jump  ● │             │
│ [+ Param]    │                └──────────┘             │
│              │                                          │
├──────────────┴──────────────────────────────────────────┤
│ Transition: Idle → Walk                                  │
│  Duration: 0.25s  ExitTime: ☑ 1.0                       │
│  Conditions:  Speed  Greater  0.1    [x]                 │
│               [+ Condition]                              │
└─────────────────────────────────────────────────────────┘
```

### 各エリアの役割

| エリア | 内容 |
|---|---|
| **キャンバス** | imnodes による State ノード・遷移リンクの描画と操作 |
| **サイドバー（左）** | `parameters[]` の一覧と値編集。型コンボ + 名前 + 値 + 削除ボタン |
| **詳細ペイン（下）** | 選択中のリンク（`AnimationTransition`）の Condition 編集 |
| **ツールバー** | GameObject 名表示、`+ State`、`+ Parameter`、デバッグ再生ボタン |

---

## ノード設計（imnodes）

各 `AnimationState` を 1 ノードとして表示する。

```
┌──────────────────────────────┐
│  Walk                    [●] │  ← 出力ピン（遷移元）
│  Clip: run_cycle             │
│  Speed: 1.0  Loop: ☑         │
│  IK Weight: 1.0              │
│[●]                           │  ← 入力ピン（遷移先）
└──────────────────────────────┘
```

- **入力ピン（左）**: 他ステートからの遷移受け口
- **出力ピン（右）**: 他ステートへの遷移元
- **ノード内フィールド**: clipName / speed / loop / ikWeight をインライン編集
- **デフォルトステート**: ノード枠を金色でハイライト
- **現在再生中ステート**: ノード枠を緑色でハイライト（ランタイム時のみ）

### ノード ID の管理

imnodes の各 ID（NodeId / PinId / LinkId）は `int` 型。  
以下のルールで `AnimatorComponent` のインデックスと双方向マッピングする。

```
NodeId  = stateIndex + 1               // 0 は無効のため +1
PinId入力 = stateIndex * 2 + 1
PinId出力 = stateIndex * 2 + 2
LinkId  = (fromStateIndex << 16) | transitionIndex   // 衝突しない範囲で結合
```

WHY: `AnimatorComponent` 自体に ID フィールドを持たせず、描画時に都度算出することで  
ステートの追加・削除時の ID 再割り当てを簡潔に保つ。

---

## ノード位置のシリアライズ

`GraphLayout` はシーンファイル（`.fbzz`）に含めず、**サイドカーファイル**として保存する。

```
Scenes/
  Main.fbzz
  Main.fbzz.animgraph     ← エディタ専用。.gitignore に含めない（チームで共有する）
```

フォーマットは TOML（toml++ を使用）。シーン保存時に同時に書き出す。

```toml
version = 1

[[nodes]]
instanceId = "abc-123"
stateName  = "Idle"
x = 100.0
y = 200.0

[[nodes]]
instanceId = "abc-123"
stateName  = "Walk"
x = 350.0
y = 200.0
```

ファイルが存在しない場合は `AnimationGraphPanel` が自動レイアウト（等間隔横並び）を行う。

---

## 操作仕様

| 操作 | 動作 |
|---|---|
| ノードをドラッグ | キャンバス上の位置を `GraphLayout` に保存 |
| 出力ピン → 入力ピンをドラッグ | 新しい `AnimationTransition` を追加 |
| リンクを選択 | 詳細ペインに Condition 編集フォームを表示 |
| リンクを Delete キーで削除 | 対応する `AnimationTransition` を削除 |
| ノードを右クリック | コンテキストメニュー（Rename / Set as Default / Delete） |
| キャンバス右クリック | `+ New State` |
| キャンバス中ボタンドラッグ / スクロール | パン・ズーム（imnodes 標準） |

---

## 実装フェーズ計画

| Phase | 内容 | 完了条件 |
|---|---|---|
| **1** | imnodes を ThirdParty に追加、CMake 組み込み | ビルドが通る |
| **2** | `AnimationGraphPanel` の骨格実装（パネル開閉、キャンバス描画） | 空のキャンバスが表示される |
| **3** | ステートノードの読み取り表示（編集なし） | 既存 AnimatorComponent のステートがノードとして表示される |
| **4** | ノード内フィールド編集（clipName / speed / loop / ikWeight） | Inspector と同じ操作がグラフ上でできる |
| **5** | 遷移リンクの表示と新規接続、Condition 詳細ペイン | ドラッグで遷移を作れる |
| **6** | `GraphLayout` シリアライズ（`.animgraph` サイドカー保存・読み込み） | エディタ再起動後もノード位置が維持される |
| **7** | ランタイムハイライト（現在ステート / 遷移中リンクを色付き表示） | 再生中にグラフが更新される |
| **8** | パラメーターサイドバー完成（型コンボ・値編集・ランタイム値表示） | Script からの SetFloat 結果がリアルタイムで見える |

---

## ファイル配置

```
ThirdParty/
  imnodes/                          ← Phase 1 で追加
    imnodes.h
    imnodes.cpp
    imnodes_internal.h

Projects/Editor/
  include/Editor/Panels/
    AnimationGraphPanel.hpp         ← パネルクラス宣言
  src/Panels/
    AnimationGraphPanel.cpp         ← パネル実装
  include/Editor/
    GraphLayout.hpp                 ← キャンバス座標データ構造
    GraphLayoutSerializer.hpp       ← .animgraph 読み書き

Docs/Editor/
  animation_graph_editor.md        ← 本ドキュメント
```

---

## 隣接ドキュメント・コード

- `Projects/Engine/include/Engine/Scene/Components/AnimatorComponent.hpp` — ランタイムデータ定義
- `Projects/Engine/src/Scene/Systems/AnimatorSystem.cpp` — ステートマシン更新ロジック
- `Projects/Editor/src/Panels/Inspector/InspectorAnimation.cpp` — 既存 Inspector UI（リスト形式）
