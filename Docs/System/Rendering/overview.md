# Rendering コンポーネント 設計 — 全体概要

## 対象コンポーネント
`CameraComponent` / `LightComponent` / `SkyRenderer` / `DecalComponent` / `MaterialComponent` / `MeshRenderer` / `SkinnedMeshRenderer`
システム: `RenderSystem`

---

## 実装済み機能

### CameraComponent

| 機能 | 状態 |
|---|---|
| fovY / nearZ / farZ + Reflect | ✅ |
| `isMain` フラグによるメインカメラ識別 | ✅ |
| `cullingMask`（LayerMask）レイヤー単位カリング | ✅ |
| ScriptProxy: SetAsMain / SetFOV / SetNearFar | ✅ |

### LightComponent

| 機能 | 状態 |
|---|---|
| Type 3 種（Directional / Point / Spot） | ✅ |
| color / intensity / range / innerCone / outerCone + Reflect | ✅ |
| Directional 1 灯・Point 最大 8 灯・Spot 最大 4 灯の GPU 送信 | ✅ |
| Directional シャドウマップ | ✅ |
| ScriptProxy: SetColor / SetIntensity / SetRange / SetEnabled | ✅ |

### SkyRenderer

| 機能 | 状態 |
|---|---|
| Rayleigh / Mie 散乱係数・太陽強度・Henyey-Greenstein パラメーター + Reflect | ✅ |
| 物理ベース大気散乱シェーダー（AtmosphereCB 経由） | ✅ |
| 球体スカイドームメッシュ描画（SOLID_NOCULL + DEPTH_SKY PSO） | ✅ |

### DecalComponent

| 機能 | 状態 |
|---|---|
| Albedo / Normal / Emissive 3 チャンネルテクスチャ + Reflect | ✅ |
| ライフタイム管理・フェードアルファ算出 | ✅ |
| `receiverLayerMask` によるレイヤー単位投影除外 | ✅ |
| Deferred パス 深度バッファ復元型 OBB 投影 | ✅ |
| DecalDebugPass（OBB ワイヤーフレーム） | ✅ |

### MaterialComponent / MeshRenderer / SkinnedMeshRenderer

| 機能 | 状態 |
|---|---|
| `.fzmat` アセット遅延ロード（EnsureMaterialAsset） | ✅ |
| BlendMode / DoubleSided / RenderQueue / ShaderPath / MeshType 読み出し | ✅ |
| Forward / Deferred 両パイプライン対応 | ✅ |
| 不透明・半透明の分離 + RenderQueue 奥行きソート | ✅ |
| DoubleSided / BlendMode ごとの PSO キャッシュ | ✅ |
| 視錐台カリング（MeshRenderer: バウンディング球） | ✅ |
| ソフトウェアオクルージョンカリング（静的不透明のみ） | ✅ |
| ScriptMaterialProxy: SetFloat/Int/Vector/Texture/BlendMode 等フル実装 | ✅ |
| Deferred GBuffer 互換変換（96 byte レイアウト） | ✅ |
| SkinnedMeshRenderer Surface シェーダー誤適用警告 + フォールバック | ✅ |

---

## 不足機能・バグ

---

### CameraComponent

#### 重要度：高

**`WorldToScreenPoint` / `ScreenToWorldPoint` が未実装スタブ**

引数をそのまま返すだけで、スクリプトからの UI 座標計算・クリック判定がすべて誤動作する。

**修正方針：** Camera の VP 行列と RenderSystem が管理している RT サイズを使って座標変換を実装する。

---

#### 重要度：中

**アスペクト比フィールドがない**

`CameraComponent` にアスペクト比フィールドがなく、RenderSystem が Renderer 側の Camera から取得するため、CameraComponent と実際の Camera の間で値が乖離するリスクがある。

**修正方針：** `aspectRatio` フィールドを追加して Reflect 対応し、RenderSystem がこの値で `renderer::Camera` を構築する。

---

**ScriptProxy に `SetCullingMask()` がない**

スクリプトからカリングマスクを動的変更できない。

**修正方針：** `ScriptCameraProxy` に `SetCullingMask(LayerMask)` を追加する。

---

#### 重要度：低

**複数 `isMain=true` のバリデーションがない**

エディターで直接 Reflect 編集すると複数 main が共存しうる。

**修正方針：** RenderSystem が Camera を選択する際に最初の enabled な `isMain=true` をピックする防御コードを追加する。

---

### LightComponent

#### 重要度：高

**Point / Spot ライトにシャドウが生成されない**

ShadowPass は DirectionalLight の単一正射影のみ実装されており、Point/Spot のシャドウマップが未実装。

**修正方針：** Spot ライトは透視投影シャドウマップ、Point ライトはキューブマップ Shadow で追加対応する。設計方針を先に決定すること。

---

**シャドウのライト視錐台が定数ハードコード**

```cpp
sceneCenter = {0, 1, 4};  // 固定
// 射影範囲 ±20、far=60 も固定
```

シーン規模や DirectionalLight の向きに依存せず破綻する。

**修正方針：** カメラ視錐台または シーン AABB から動的に lightVP を計算する（CSM / LiSPSM の基礎）。

---

#### 重要度：中

**ScriptProxy に `SetType()` / `SetConeAngle()` がない**

スクリプトからライト種別やスポット角度を動的変更できない。

**修正方針：** `ScriptLightProxy` に `SetType(LightComponent::Type)` / `SetInnerCone(float)` / `SetOuterCone(float)` を追加する。

---

#### 重要度：低

**アンビエントカラーが RenderSystem 内に `0.08` ハードコード**

`LightComponent` から設定できない。

**修正方針：** シーンレベルの `AmbientLight` コンポーネントまたは `RenderSettings` に `ambientColor` フィールドを追加する。

---

### SkyRenderer

#### 重要度：高

**`bool enabled` が構造体に宣言されていない**

`Reflect()` 内で `r.Field("enabled", enabled)` を呼んでいるが、構造体本体に `bool enabled` の宣言がない。コンパイルエラーの原因になる。

**修正方針：** `SkyRenderer` 構造体に `bool enabled = true;` を追加する。

---

#### 重要度：中

**太陽方向（`sunDirection`）フィールドがない**

SkyPass は LightCB（Directional ライトの向き）から取得するが、Directional ライトがないシーンで太陽位置が不定になる。

**修正方針：** `math::Vector3 sunDirection` フィールドを追加するか、LightCB 参照に依存することをコメントで明示する。

---

**大気半径・地球半径がハードコード**

`planetRadius=6371` / `atmosphereRadius=6471` が SkyPass.cpp 内に固定されており、非現実スケールのシーンで調整できない。

**修正方針：** `SkyRenderer` に両フィールドを追加して Reflect 対応する。

---

### DecalComponent

#### 重要度：中

**ライフタイム満了後に GameObject が残り続ける**

`enabled=false` になっても GO が削除されないため、毎フレームのチェック対象になり続ける。

**修正方針：** `DecalComponent` に `pendingDestroy` フラグを設けるか、RenderSystem 側で `enabled=false かつ lifetime >= 0` の GO を自動削除する。

---

**`receiverLayerMask` が `int` 経由でシリアライズされる**

`~0u`（Everything）が `-1` として保存され、シリアライザーが符号拡張を正しく処理しない場合にラウンドトリップが壊れる可能性がある。

**修正方針：** IReflector に uint32_t サポートを追加するか、TOML/JSON レイヤーで符号なし整数として明示保存する。

---

### MaterialComponent / MeshRenderer / SkinnedMeshRenderer

#### 重要度：高

**SkinnedMeshRenderer に視錐台カリングが未実装**

コメントにも「aggregateBounds フィールドを追加する拡張が考えられる」と明記されており、Transform 位置のみで判定しているに過ぎず、全スキンドメッシュが毎フレーム描画される。

**修正方針：** `SkinnedMeshRenderer` に `aggregateBounds`（バインドポーズ全メッシュのワールド球）を追加し、アセットロード時に計算して視錐台テストに使う。

---

**静的リソースハンドルのデバイスリセット非対応（FIXME あり）**

フルスクリーン切り替えや GPU ドライバ更新で DX11 デバイスリセットが発生すると、static な RT / PSO / CB 等のハンドルが無効化されたままクラッシュする。

**修正方針：** `ResourceManager::Reset()` API を新設し、Application ループからデバイスロスト検出時に呼び出してハンドルを再生成する。

---

#### 重要度：中

**`ScriptMaterialProxy::SetInt` が `float` キャストのみ**

`static_cast<float>(v)` で変換するため、整数型パラメーター（テクスチャインデックス・mode フラグ等）で精度が失われる可能性がある。

**修正方針：** `MaterialAsset::params` に `variant<float, int>` を導入するか、int 専用の保存パスを追加する。

---

**`materialPath` の型が `std::str` と `std::string` で混在**

`MeshRenderer` は `std::str`、`SkinnedMeshRenderer` は `std::string` を使用しており不整合がある。

**修正方針：** エンジン全体で統一した型エイリアスを使用するか、`std::string` に統一する。

---

#### 重要度：低

| 項目 | 説明 | 修正方針 |
|---|---|---|
| **プリミティブ meshPath の再解決なし** | Runtime でパスを変更した場合に再解決が起きない | `MeshRenderer` に `EnsureMesh()` ヘルパーを追加 |
| **透明・スキンドメッシュの統計カウント漏れ** | `statsDrawCalls` が静的不透明のみカウント | 各ループにもインクリメントを追加 |

---

## 修正優先順位まとめ

| 優先度 | コンポーネント | 項目 | 工数目安 |
|---|---|---|---|
| 1 | SkyRenderer | `bool enabled` 宣言欠落の修正 | 極小 |
| 2 | Camera | WorldToScreenPoint / ScreenToWorldPoint 実装 | 中 |
| 3 | SkinnedMeshRenderer | 視錐台カリング追加 | 中 |
| 4 | Light | シャドウ視錐台の動的計算 | 大 |
| 5 | Light | Point / Spot シャドウ | 大 |
| 6 | DecalComponent | ライフタイム満了後の自動削除 | 小 |
| 7 | Camera / Light | ScriptProxy API 不足補完 | 小 |
| 8 | RenderSystem | デバイスリセット対応 | 大 |

---

## ファイル配置

```
Projects/Engine/
  include/Engine/Scene/
    Components/
      CameraComponent.hpp / LightComponent.hpp / SkyRenderer.hpp
      DecalComponent.hpp / MaterialComponent.hpp
      MeshRenderer.hpp / SkinnedMeshRenderer.hpp
    ScriptProxy/
      ScriptCameraProxy.hpp / ScriptLightProxy.hpp / ScriptMaterialProxy.hpp
    Systems/
      RenderSystem.hpp / .cpp

Docs/System/Rendering/
  overview.md    ← このファイル
```

---

## 隣接ドキュメント

- [../Physics/overview.md](../Physics/overview.md) — カリングで使う LayerMask の定義
- [../Particle/overview.md](../Particle/overview.md) — 同じ TRANSPARENT_LAYER を使うパーティクル
- [../Trail/overview.md](../Trail/overview.md) — 同じ TRANSPARENT_LAYER を使うトレイル
