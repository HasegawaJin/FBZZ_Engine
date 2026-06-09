# TrailRenderer — 現状の不足・改善点

現在の実装状況と、追加・修正が必要な機能の一覧。
概要設計は [overview.md](overview.md) を参照。

---

## 実装済み機能（確認済み）

| 機能 | 実装箇所 |
|---|---|
| リングバッファ + 時間ベースサンプリング + 最小移動距離チェック | `TrailRenderSystem.cpp` |
| Catmull-Rom サブディビジョン（`smoothSubdivisions`） | `BuildRenderPoints()` |
| マイタージョイント付きリボン頂点生成 | `BuildTrailVertices()` |
| CameraFacing / WorldUp アライメント | `ComputeRibbonNormal()` |
| テクスチャ + UV スクロール + タイリング | `EnsureResources()` + `TrailCB` |
| GPU 側 age lerp（色・幅） | `Trail.hlsl` + `TrailCB` |
| `clearOnDisable=false` 時の自然フェードアウト | `ExecuteTrailPass()` |
| 複数 Trail の深度ソート | `TrailDrawItem` + `NearestTrailDistanceSq()` |
| ボーンソケット追従 | `attachBone` / `attachOffset` + `ResolveTrailSamplePosition()` |
| 幅のイージング | `TrailWidthEasing` + `ApplyWidthEasing()` |
| UV モード（Stretch / Tile） | `TrailUVMode` + `BuildTrailVertices()` |
| Script API（SetEnabled / Clear / SetWidth / SetColor / SetTexture / SetAlignment / SetSmoothSubdivisions / SetAttachBone） | `ScriptTrailProxy.hpp` + `ScriptProxies.cpp` |

---

## 実装完了した不足機能・バグ

### 重要度：高

#### `enabled=false` 時に Proxy と System が乖離するバグ（完了）

`ScriptTrailProxy::SetEnabled(false, clearWhenDisabled=false)` はリングバッファを保持する（Proxy 実装は正しい）。
しかし `ExecuteTrailPass` では `!enabled` なら無条件でバッファをリセットしてしまう：

```cpp
// TrailRenderSystem.cpp — 問題箇所
if (!trail->enabled) {
    trail->ringCount = 0;
    trail->ringHead  = 0;
    trail->ringTail  = 0;
    trail->lastSampleTime = -1.0f;
    continue;
}
```

**結果：** "攻撃終了後にトレイルをフェードアウトさせたい" ユースケースが機能しない。

**修正方針：** `TrailComponent` に `bool clearOnDisable` フィールドを追加するか、
`!enabled` 時はサンプリングのみ停止して既存の点列の自然消滅（`RingExpireOld`）に任せる。

**実装：** `clearOnDisable=false` の場合は点列を保持し、`RingExpireOld()` による自然消滅だけを行う。

---

#### 深度ソート不在（完了）

複数の TrailComponent が存在する場合、カメラ距離でのソートなしに全 DrawCall を
`TRANSPARENT_LAYER` へ Submit しているため、半透明の Z 順が破綻する。

**修正方針：** `ExecuteTrailPass` 内で各トレイルの最近傍点と Camera 距離を算出し、
遠い順に Submit する（または `dc.sortKey` を設定する）。

**実装：** DrawCall を `TrailDrawItem` に一度集め、カメラ最近傍点までの距離で遠い順に `Submit()` する。

---

### 重要度：中

#### ボーンソケット追従（完了）

サンプリングは `go.transform.position`（GameObject ルート座標）固定。
武器の刃先・手先など特定ボーン位置に追従させる手段がない。

**修正方針：** `TrailComponent` に `std::string attachBone` + `Vector3 attachOffset` を追加し、
`AnimatorComponent` 経由でボーンのワールド行列を取得してサンプリング位置を上書きする。

**実装：** `SkinnedMeshRenderer::nodeEntities` の Bone GameObject を優先し、見つからない場合は GameObject 原点へフォールバックする。

---

#### 幅のイージング（完了）

`widthStart → widthEnd` の線形補間のみ。急激な太さの変化が表現できない。

**修正方針：** `Util/Easing.hpp` が既存なので `widthEasing: EasingType` フィールドを追加し、
`BuildTrailVertices` 内の `math::Lerp` を easing 適用版に差し替える。

**実装：** `TrailWidthEasing` を `BuildTrailVertices()` の幅補間に適用する。

---

### 重要度：低

#### UV モード（Stretch / Tile）（完了）

U 座標は常に `float(i) / segmentDenom`（セグメント数正規化）。
テクスチャをトレイル全長に対して一度だけ引き伸ばす "Stretch" と、
固定間隔でタイルする "Tile" を切り替えられない。

**修正方針：** `enum class TrailUVMode { Stretch, Tile }` を追加し、
`BuildTrailVertices` 内の U 計算を分岐する。

**実装：** Stretch は点列正規化、Tile は累積ワールド長で U 座標を生成する。

---

## 修正優先順位まとめ

| 優先度 | 項目 | 工数目安 |
|---|---|---|
| 1 | `enabled=false` / `clearWhenDisabled=false` のバグ修正 | 小（System 1 箇所の条件分岐） |
| 2 | 深度ソート | 中（`ExecuteTrailPass` に距離計算 + ソート追加） |
| 3 | ボーンソケット追従 | 中（Component フィールド追加 + AnimatorComponent 参照） |
| 4 | 幅のイージング | 小（Easing.hpp 適用のみ） |
| 5 | UV モード | 小（BuildTrailVertices 内の U 計算分岐） |

---

## 隣接ドキュメント

- [overview.md](overview.md) — TrailRenderer 全体設計・フェーズ計画
- [rendering.md](rendering.md) — データ構造・マイタージョイント・シェーダー詳細
- [../MeshTrail/overview.md](../MeshTrail/overview.md) — MeshTrailRenderer 設計・不足機能
