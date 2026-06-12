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

~~**`ScreenSpaceCamera` が Overlay と完全同一実装**~~

✅ **修正済み** — `BuildCanvasRuntimeState` に `ScreenSpaceCamera` ブランチを追加。カメラ前方 `planeDistance` ワールド単位に Canvas を配置し、カメラ姿勢に追従する `VP * TRS * pixelToLocal` 変換を実装。深度テストあり PSO (`worldPso`) を使用するため 3D オブジェクトに遮蔽される。マウス座標変換は Overlay と同じスクリーン座標系を維持。

---

~~**WorldSpace Canvas のマウスヒット判定が未実装**~~

✅ **修正済み** — `UIEventSystem` に WorldSpace ブランチを追加。`Ray::FromNDC` でワールドレイを構築し、`Plane::FromNormalAndPoint` + `Ray::IntersectPlane` でキャンバス平面との交差を取り、`Matrix4::Inverse` によるローカル座標変換でキャンバスピクセル座標を求めて `ProcessUIEventsRecursive` に渡す。

---

#### 重要度：中

~~**Canvas が RootGameObject 直下限定**~~

✅ **修正済み** — `CollectCanvasesRecursive` を追加し、任意の深さの子 GameObject に置かれた Canvas も収集する。

---

~~**グローバル静的リソースの共有（スレッドセーフでない）**~~

✅ **修正済み** — 9 個の `s_*` static 変数を全て削除し `UISystemContext` 構造体に収めた。`RenderSystemUIOptions::context` で呼び出し元 Viewport が所有し、Game / Scene / CanvasEditor Viewport がそれぞれ独立したインスタンスを持つ。`UISystemFlushCache(ctx)` と `UISystemSetDefaultFontPath(ctx, path)` の引数も `ctx` を受け取るよう変更済み。

---

### UIImage

#### 重要度：高

~~**毎フレーム `resources.LoadTexture(texturePath)` を呼び出している**~~

✅ **修正済み** — `loadedTexturePath` フィールドを追加し、`texturePath` が変わったときだけ再ロードするよう変更。

---

#### 重要度：中

~~**スプライトシートのピクセル単位 UV 変換ヘルパーが未実装**~~

✅ **修正済み** — `UIImage::PixelRectToUV(x, y, w, h, texW, texH)` static ヘルパーを追加。`ScriptUIProxy::SetImageSpriteRect` でスクリプトから呼び出せる。

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

~~**フォントアトラスキャッシュがシーン破棄時にリリースされない**~~

✅ **修正済み** — `UISystemFlushCache()` を追加（`UISystem.hpp` 公開 API）。シーン破棄時・アセットリロード時に呼んでキャッシュをクリアする。シーン破棄イベントへの自動連動は呼び出し側で設定する。

---

#### 重要度：低

~~**固定 VB サイズ（682 グリフ上限）が実行時に変更できない**~~

✅ **修正済み** — `SubmitTextWithAtlas` を `kTextVBVertices`（4096 頂点）単位のチャンクに分割して複数ドローコールを発行するよう変更。ResourceManager / GPU バッファサイズの変更なしに任意の長さのテキストを描画できる。

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

**ヒット矩形が AABB のみ（回転非対応）**【仕様】

`UpdateButton` は回転を考慮せず AABB で判定する。回転した UIButton では不正確になるが、UI 回転は稀ユースケースのため **仕様として明記**。OBB 対応が必要になった時点で `UpdateButton` に回転角パラメータを追加する。

---

### UILayoutGroup

#### 重要度：高

~~**`UIImage` を持つ子だけがレイアウト対象になる**~~

✅ **修正済み** — `activeSelf()` を条件にするよう変更。`UIText` / `UIButton` のみを持つ子も正しくレイアウト対象になる。

---

#### 重要度：中

~~**`paddingRight` / `paddingBottom` が実際のレイアウト計算で未使用**~~

✅ **修正済み** — `ApplyLayout` がコンテナサイズ（`transform.scale.xy`）と `paddingRight/Bottom` を比較し、子がはみ出した場合に初回のみ `FBZZ_LOG_WARN` を出力。警告にはキャンバスピクセルとビューポートピクセルの両方のはみ出し量を含む。描画クリッピングはレンダラー側の別機能。

---

~~**Canvas Scaler のスケールを考慮しない**~~

✅ **修正済み** — `UILayoutSystem` が Canvas ごとに `ResolveCanvasScale` を呼び、`canvasScale` を `ApplyUILayoutRecursive` → `ApplyLayout` へ渡す。はみ出し警告のビューポートピクセル換算に使われ、`ScaleWithScreenSize` 環境での診断精度が向上する。

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
| 9 | UICanvas | Canvas が RootGameObject 直下限定 | ✅ 完了 |
| 10 | UIText | フォントアトラスキャッシュのフラッシュ API | ✅ 完了 |
| 11 | UIImage | スプライトシート UV 変換ヘルパー | ✅ 完了 |
| 12 | UILayoutGroup | paddingRight/Bottom 境界チェック + canvasScale 連携 | ✅ 完了 |
| 13 | UIButton | UIImage なしでもヒット判定する | ✅ 完了 |
| 14 | UIButton | ヒット矩形が AABB のみ（回転非対応） | 仕様として明記 |
| 15 | UIText | 固定 VB サイズ（682 グリフ上限） | ✅ 完了（マルチドロー分割） |
| 16 | UICanvas | `ScreenSpaceCamera` モードの実装 | ✅ 完了 |
| 17 | UICanvas | WorldSpace ヒット判定（レイキャスト） | ✅ 完了 |
| 18 | UICanvas | グローバル静的リソースのスレッドセーフ化 | ✅ 完了（UISystemContext） |

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
