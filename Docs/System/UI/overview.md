# UI システム 設計 — 全体概要

## 対象コンポーネント
`UICanvas` / `UIImage` / `UIText` / `UIButton` / `UILayoutGroup` / `UIAnimator`
システム: `UISystem` / `UIAnimatorSystem`

---

## 実装済み機能

### UICanvas

| 機能 | 状態 |
|---|---|
| UIRenderMode 3 種（ScreenSpaceOverlay / WorldSpace / ScreenSpaceCamera） | ✅ |
| UICanvasScaleMode 2 種（ConstantPixelSize / ScaleWithScreenSize） | ✅ |
| `referenceWidth/Height` + `matchWidthOrHeight` による Canvas Scaler（対数補間） | ✅ |
| `sortOrder` による Canvas 描画順ソート | ✅ |
| WorldSpace Canvas 用の深度テストあり PSO | ✅ |
| ScreenSpace 正射影行列生成 | ✅ |
| `Reflect` による全フィールドのシリアライズ | ✅ |

### UIImage / UIText / UIButton / UILayoutGroup

| 機能 | 状態 |
|---|---|
| `texturePath` によるテクスチャ遅延ロード + `uvMin/uvMax` UV 矩形（スプライトシート基盤） | ✅ |
| RGBA `color` による色付け | ✅ |
| `fontPath` + デフォルトフォントフォールバック | ✅ |
| フォントアトラスキャッシュ（初回のみロード） | ✅ |
| 改行文字 `\n` 対応 | ✅ |
| `UIButtonState`（NORMAL / HOVERED / PRESSED）状態機械 | ✅ |
| `normalColor` / `hoverColor` / `pressedColor` 色遷移 | ✅ |
| `isInteractable` フラグ | ✅ |
| `onClick` / `onEnter` / `onExit` 1フレーム限定イベントフラグ | ✅ |
| `UILayoutAxis`（H/V）+ spacing + padding + reverseOrder | ✅ |
| 再帰的なレイアウト適用（ネスト対応） | ✅ |

### UIAnimator / UIAnimatorSystem

| 機能 | 状態 |
|---|---|
| `UIColorTween` / `UIPositionTween`（from / to / duration / easing / loop / pingPong） | ✅ |
| `UIEasingType` 4 種（Linear / EaseIn / EaseOut / EaseInOut） | ✅ |
| pingPong 往復再生 | ✅ |
| loop 時の `fmod` ラップアラウンド | ✅ |
| 終端到達時の `active = false` 自動停止 | ✅ |

---

## 不足機能・バグ

---

### UICanvas

#### 重要度：高

**`ScreenSpaceCamera` が Overlay と完全同一実装**

ヘッダーコメントにも「現状は Overlay と同じ座標系で描く」と記述されており、カメラ基準の座標変換が未実装。カメラのビューポートに追従する UI（ミニマップ・スコープ UI 等）が作れない。

**修正方針：** `ScreenSpaceCamera` を検出してカメラの View/Projection 行列から UI の正射影空間を構築するパスを分岐として追加する。

---

**WorldSpace Canvas のマウスヒット判定が未実装**

`UIEventSystem` は `IsScreenSpaceRenderMode` が偽のとき処理全体をスキップするため、WorldSpace に置いた `UIButton` は一切反応しない。

**修正方針：** WorldSpace Canvas に対してスクリーン座標→レイキャストによるヒット判定パスを追加し、`ProcessUIEventsRecursive` を呼ぶように拡張する。

---

#### 重要度：中

**Canvas が RootGameObject 直下限定**

`CollectCanvases` は `scene.GetRootGameObjects()` の直下しか走査しないため、Canvas が子 GameObject に置かれると無視される。

**修正方針：** 再帰検索または `Scene` 側にコンポーネント全索引を持たせ、階層位置に依存しない収集に変更する。

---

**グローバル静的リソースの共有（スレッドセーフでない）**

`s_shader` / `s_constants` / `s_imageVB` / `s_textVB` / `s_fontAtlasCache` が static 変数で、複数シーン同時実行時に競合が起きる。

**修正方針：** リソースを `UISystemContext` 構造体に収めて呼び出し側が所有するか、排他ロックを追加する。

---

### UIImage

#### 重要度：高

**毎フレーム `resources.LoadTexture(texturePath)` を呼び出している**

変更検知なしに毎フレームパスを渡すため、ホットリロード検知と干渉しうる。

**修正方針：** 前回パスをコンポーネント内にキャッシュして変更時のみ再ロードする（TrailComponent の `loadedTexturePath` と同じパターン）。

---

#### 重要度：中

**スプライトシートのピクセル単位 UV 変換ヘルパーが未実装**

`uvMin/uvMax` はノーマライズ済み UV で保持するが、テクスチャサイズを取得する手段がないためスクリプトから扱いにくい。

**修正方針：** `ScriptUIProxy` またはユーティリティ関数として「ピクセル矩形 → 正規化 UV 変換」を提供する。

---

### UIText

#### 重要度：高

**テキストの幅・高さが計算されない**

UIButton のヒット判定やレイアウト配置は `localScale.xy` をサイズとして使うが、`UIText` はそれを更新しない。テキストが変わっても当たり判定・レイアウトが追従しない。

**修正方針：** `SubmitText` または UISystem のプリパスでテキストの論理サイズを計算し、`localScale.xy` または専用フィールドへ書き込む。

---

**`UIText` 構造体に `bool enabled` の宣言がない可能性**

`Reflect()` で `r.Field("enabled", enabled)` を参照しているが、構造体本体に `bool enabled = true;` の宣言が見当たらない（`UIButton` / `UIImage` は明示的に持つ）。

**修正方針：** `UIText` 構造体に `bool enabled = true;` を明示的に追加する。

---

#### 重要度：中

**テキスト整列（左/中央/右寄せ）が未実装**

ペン位置が常に左上起点で固定されており、`TextAlignment` 相当のフィールドが存在しない。

**修正方針：** `enum class TextAlign { Left, Center, Right }` を追加し、`SubmitTextWithAtlas` で行幅を事前計算して開始位置を調整する。

---

**フォントアトラスキャッシュがシーン破棄時にリリースされない**

`s_fontAtlasCache` はプログラム終了まで保持され続け、マルチシーン環境でテクスチャが残留する。

**修正方針：** `UISystemFlushCache()` 呼び出しでキャッシュをクリアするか、シーン破棄イベントに連動させる。

---

#### 重要度：低

**固定 VB サイズ（682 グリフ上限）が実行時に変更できない**

超過時はエラーログだけで描画が打ち切られる。

**修正方針：** VB を動的サイズにするか、超過分を複数ドローコールに分割する。

---

### UIButton

#### 重要度：高

**`onClick` 検出ロジックにバグあり**

現在の実装ではマウスが素早く移動してリリース時に `hit` が偽になるケース（ドラッグアウト）で `onClick` が発火しない。逆に別要素にホバーしながらリリースすると意図しない Click が発火しうる。

**修正方針：** `onClick` はリリース時（前フレーム `mousePressed == true` かつ今フレーム `false`）に `hit` を確認する方式に変更し、押下開始 GameObject（`startPressedGO`）と一致確認を行う。

---

**WorldSpace Canvas 上のボタンがヒット判定されない**（UICanvas 節と共通）

---

#### 重要度：中

**`disabled` 状態の視覚フィードバックが未実装**

`disabledColor` フィールドが存在せず、非インタラクタブル時は `normalColor` のまま。

**修正方針：** `disabledColor` フィールドを追加し、`ButtonTint` で `isInteractable == false` の場合にその色を返す。

---

#### 重要度：低

**ヒット矩形が AABB のみ（回転非対応）**

`UpdateButton` は回転を考慮せず `rect.pos + rect.size` の AABB で判定するため、回転した UIButton では不正確なヒット領域になる。

**修正方針：** 回転角を受け取って OBB 判定に切り替えるか、回転 UI には対応しない旨を仕様として明記する。

---

### UILayoutGroup

#### 重要度：高

**`UIImage` を持つ子だけがレイアウト対象になる**

`UIText` のみ・`UIButton` のみを持つ子はレイアウトから除外される。テキストラベルや非画像ボタンを並べるユースケースで無視される。

**修正方針：** `UIImage` だけでなく `UIText` / `UIButton` を持つ子も対象とするか、`activeSelf`（enabled）のみを条件にする。

---

#### 重要度：中

**`paddingRight` / `paddingBottom` が実際のレイアウト計算で未使用**

`cursor` 開始値には `paddingLeft/Top` が使われるが、後端余白は機能していない。子が親をはみ出してもクリップされない。

**修正方針：** 親 `localScale.xy` からコンテナサイズを取得し、`paddingRight/Bottom` を越えた場合の折り返しまたは警告を追加する。

---

**Canvas Scaler のスケールを考慮しない**

`ApplyLayout` は Canvas のスケール係数を受け取らず、`ScaleWithScreenSize` Canvas 上で表示サイズと配置サイズが乖離する場合がある。

**修正方針：** `ApplyLayout` に Canvas のスケール係数を渡してカーソル計算に適用する。

---

### UIAnimator

#### 重要度：高

**pingPong のスワップ検出が `elapsed == 0.0f` の厳密比較に依存**

`fmod` の結果が浮動小数点誤差で厳密に `0.0f` にならないケースがあり、往復が発動しないフレームが発生しうる。

**修正方針：** `elapsed == 0.0f` の比較を `bool didWrap` フラグで置き換え、`AdvanceTween` がラップを検出したときに呼び出し元へ通知する。

---

**`PositionTween` が `UIImage` の有無に依存して無効化される**

`ProcessGO` の条件が `anim && image && image->enabled` のため、`UIImage` を持たない要素では `PositionTween` も動かない。

**修正方針：** ColorTween の適用は UIImage の存在を条件とし、PositionTween の適用は `image` の有無に依存しないように条件分岐を分離する。

---

#### 重要度：中

**`colorTween.easing` / `positionTween.easing` が `Reflect` に含まれていない**

Inspector やシーンファイルから Easing を設定・保存できない。

**修正方針：** `Reflect` に `colorEasing` / `posEasing` の int キャストフィールドを追加する。

---

**`elapsed` が `Reflect` に含まれており、ランタイム状態がシリアライズされる**

シーンロード直後から途中再生状態になる。

**修正方針：** `elapsed` を `Reflect` から除外する。

---

**`UIScaleTween`（スケールアニメーション）が未実装**

色と位置のみが対象で、ポップアップアニメーション等に必要なスケール変化に対応できない。

**修正方針：** `UIScaleTween` 構造体を追加し `UIAnimatorSystem` で `localScale.xy` へ書き込む。

---

#### 重要度：低

| 項目 | 説明 | 修正方針 |
|---|---|---|
| **`UIText` の色アニメーション非対応** | `colorTween` は `UIImage::color` のみ書き込む | `ProcessGO` に `UIText::color` への書き込みを追加 |
| **`UIImage` なし時のサイレントスキップ** | エラー・警告が出ないため気づきにくい | `ProcessGO` で `UIImage` が存在しないとき `FBZZ_LOG_WARN`（初回のみ） |

---

## 修正優先順位まとめ

| 優先度 | コンポーネント | 項目 | 工数目安 |
|---|---|---|---|
| 1 | UIText | `enabled` 宣言欠落の確認・修正 | 極小 |
| 2 | UIText | テキストサイズ計算と `localScale.xy` への反映 | 中 |
| 3 | UIButton | `onClick` 検出ロジックのバグ修正 | 小 |
| 4 | UIImage | 毎フレーム `LoadTexture` の変更検知キャッシュ | 小 |
| 5 | UILayoutGroup | `UIImage` 以外の子もレイアウト対象にする | 小 |
| 6 | UIAnimator | pingPong のスワップ検出をフラグ方式に変更 | 小 |
| 7 | UIAnimator | PositionTween と ColorTween の条件分離 | 小 |
| 8 | UIAnimator | `elapsed` を `Reflect` から除外 | 極小 |
| 9 | UICanvas | `ScreenSpaceCamera` モードの実装 | 大 |
| 10 | UICanvas | WorldSpace ヒット判定（レイキャスト） | 大 |

---

## ファイル配置

```
Projects/Engine/
  include/Engine/Scene/
    Components/
      UICanvas.hpp / UIImage.hpp / UIText.hpp
      UIButton.hpp / UILayoutGroup.hpp / UIAnimator.hpp
    ScriptProxy/
      ScriptUIProxy.hpp
    Systems/
      UISystem.hpp / UIAnimatorSystem.hpp
  src/Scene/Systems/
    UISystem.cpp / UIAnimatorSystem.cpp

Docs/System/UI/
  overview.md    ← このファイル
```

---

## 隣接ドキュメント

- [../Animation/overview.md](../Animation/overview.md) — UIAnimator が参照する Transform 構造と同系統
- [../Physics/overview.md](../Physics/overview.md) — WorldSpace UI のヒット判定で必要なレイキャスト
