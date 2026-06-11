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

~~**毎フレーム `resources.LoadTexture(texturePath)` を呼び出している**~~

✅ **修正済み** — `loadedTexturePath` フィールドを追加し、`texturePath` が変わったときだけ再ロードするよう変更。

---

#### 重要度：中

**スプライトシートのピクセル単位 UV 変換ヘルパーが未実装**

`uvMin/uvMax` はノーマライズ済み UV で保持するが、テクスチャサイズを取得する手段がないためスクリプトから扱いにくい。

**修正方針：** `ScriptUIProxy` またはユーティリティ関数として「ピクセル矩形 → 正規化 UV 変換」を提供する。

---

### UIText

#### 重要度：高

~~**テキストの幅・高さが計算されない**~~

✅ **修正済み** — `UITextSizeSystem` プリパスで `ComputeTextLogicalSize()` を呼び、`transform.scale.xy` へ書き込む。UILayout・ヒット判定の前に実行される。

---

~~**`UIText` 構造体に `bool enabled` の宣言がない可能性**~~

✅ **確認済み** — `bool enabled = true;` は宣言されていた。

---

#### 重要度：中

~~**テキスト整列（左/中央/右寄せ）が未実装**~~

✅ **修正済み** — `enum class TextAlign { Left, Center, Right }` を追加。`SubmitTextWithAtlas` で行幅を事前計算し開始位置を調整。`align` フィールドは Reflect 対応済み。

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

~~**`onClick` 検出ロジックにバグあり**~~

✅ **修正済み** — `wasPressedOnThis` + `lastMouseState` をボタン内部に保持し、押下がこのボタン上で開始された場合のみ `onClick` を発火するよう変更。ドラッグアウト後の誤発火と外部からの流入押下を両方防止。

---

**WorldSpace Canvas 上のボタンがヒット判定されない**（UICanvas 節と共通）

---

#### 重要度：中

~~**`disabled` 状態の視覚フィードバックが未実装**~~

✅ **修正済み** — `disabledColor` フィールドを追加。`ButtonTint` で `isInteractable == false` のとき `disabledColor` を返す。Reflect にも追加済み。

---

#### 重要度：低

**ヒット矩形が AABB のみ（回転非対応）**

`UpdateButton` は回転を考慮せず `rect.pos + rect.size` の AABB で判定するため、回転した UIButton では不正確なヒット領域になる。

**修正方針：** 回転角を受け取って OBB 判定に切り替えるか、回転 UI には対応しない旨を仕様として明記する。

---

### UILayoutGroup

#### 重要度：高

~~**`UIImage` を持つ子だけがレイアウト対象になる**~~

✅ **修正済み** — `activeSelf()` を条件にするよう変更。`UIText` / `UIButton` のみを持つ子も正しくレイアウト対象になる。

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

~~**pingPong のスワップ検出が `elapsed == 0.0f` の厳密比較に依存**~~

✅ **修正済み** — `AdvanceTween` に `bool& didWrap` 出力パラメーターを追加し、`fmod` ラップ検出をフラグで行うよう変更。

---

~~**`PositionTween` が `UIImage` の有無に依存して無効化される**~~

✅ **修正済み** — ColorTween は `UIImage` / `UIText` の存在確認を行い、PositionTween / ScaleTween は `image` の有無に依存しない独立した条件で動作する。

---

#### 重要度：中

~~**`colorTween.easing` / `positionTween.easing` が `Reflect` に含まれていない**~~

✅ **修正済み** — `colorEasing` / `posEasing` / `scaleEasing` を int キャストで `Reflect` に追加。

---

~~**`elapsed` が `Reflect` に含まれており、ランタイム状態がシリアライズされる**~~

✅ **確認済み** — `elapsed` は元々 `Reflect` に含まれていなかった。

---

~~**`UIScaleTween`（スケールアニメーション）が未実装**~~

✅ **修正済み** — `UIScaleTween` 構造体を追加。`UIAnimatorSystem` で `transform.scale.xy` へ書き込む。Reflect 対応済み。

---

#### 重要度：低

| 項目 | 説明 | 状態 |
|---|---|---|
| ~~**`UIText` の色アニメーション非対応**~~ | `colorTween` は `UIImage::color` のみ書き込む | ✅ **修正済み** — `UIText::color` への書き込みも追加 |
| ~~**`UIImage` なし時のサイレントスキップ**~~ | エラー・警告が出ないため気づきにくい | ✅ **修正済み** — `FBZZ_LOG_WARN`（初回のみ）を追加 |

---

## 修正優先順位まとめ

| 優先度 | コンポーネント | 項目 | 状態 |
|---|---|---|---|
| 1 | UIText | `enabled` 宣言欠落の確認・修正 | ✅ 確認済み（元から存在） |
| 2 | UIText | テキストサイズ計算と `localScale.xy` への反映 | ✅ 完了 |
| 3 | UIButton | `onClick` 検出ロジックのバグ修正 | ✅ 完了 |
| 4 | UIImage | 毎フレーム `LoadTexture` の変更検知キャッシュ | ✅ 完了 |
| 5 | UILayoutGroup | `UIImage` 以外の子もレイアウト対象にする | ✅ 完了 |
| 6 | UIAnimator | pingPong のスワップ検出をフラグ方式に変更 | ✅ 完了 |
| 7 | UIAnimator | PositionTween と ColorTween の条件分離 | ✅ 完了 |
| 8 | UIAnimator | `elapsed` を `Reflect` から除外 | ✅ 確認済み（元から除外） |
| 9 | UICanvas | `ScreenSpaceCamera` モードの実装 | 未着手（大） |
| 10 | UICanvas | WorldSpace ヒット判定（レイキャスト） | 未着手（大） |

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
