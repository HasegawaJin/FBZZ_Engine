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
| aspectRatio + Reflect / 実 Viewport との同期 | ✅ |
| `isMain` フラグによるメインカメラ識別 | ✅ |
| `cullingMask`（LayerMask）レイヤー単位カリング | ✅ |
| ScriptProxy: SetAsMain / SetFOV / SetAspectRatio / SetNearFar / SetCullingMask | ✅ |
| WorldToScreenPoint / ScreenToWorldPoint | ✅ |

### LightComponent

| 機能 | 状態 |
|---|---|
| Type 3 種（Directional / Point / Spot） | ✅ |
| color / intensity / range / innerCone / outerCone + Reflect | ✅ |
| Directional 1 灯・Point 最大 8 灯・Spot 最大 4 灯の GPU 送信 | ✅ |
| Directional シャドウマップ | ✅ |
| シーン bounds 由来の Directional shadow lightVP 動的計算 | ✅ |
| ScriptProxy: SetColor / SetType / SetIntensity / SetRange / SetInnerCone / SetOuterCone / SetEnabled | ✅ |

### SkyRenderer

| 機能 | 状態 |
|---|---|
| Rayleigh / Mie 散乱係数・太陽強度・Henyey-Greenstein パラメーター + Reflect | ✅ |
| 物理ベース大気散乱シェーダー（AtmosphereCB 経由） | ✅ |
| 球体スカイドームメッシュ描画（SOLID_NOCULL + DEPTH_SKY PSO） | ✅ |
| enabled / planetRadius / atmosphereRadius + Reflect / Serialize | ✅ |

### DecalComponent

| 機能 | 状態 |
|---|---|
| Albedo / Normal / Emissive 3 チャンネルテクスチャ + Reflect | ✅ |
| ライフタイム管理・フェードアルファ算出 | ✅ |
| ライフタイム満了後の GameObject 自動削除 | ✅ |
| `receiverLayerMask` によるレイヤー単位投影除外 | ✅ |
| Deferred パス 深度バッファ復元型 OBB 投影 | ✅ |
| DecalDebugPass（OBB ワイヤーフレーム） | ✅ |

### MaterialComponent / MeshRenderer / SkinnedMeshRenderer

| 機能 | 状態 |
|---|---|
| `.mat` アセット遅延ロード（EnsureMaterialAsset） | ✅ |
| BlendMode / DoubleSided / RenderQueue / ShaderPath / MeshType 読み出し | ✅ |
| Forward / Deferred 両パイプライン対応 | ✅ |
| 不透明・半透明の分離 + RenderQueue 奥行きソート | ✅ |
| DoubleSided / BlendMode ごとの PSO キャッシュ | ✅ |
| 視錐台カリング（MeshRenderer: バウンディング球） | ✅ |
| 視錐台カリング（SkinnedMeshRenderer: aggregate バインドポーズ球） | ✅ |
| ソフトウェアオクルージョンカリング（静的不透明のみ） | ✅ |
| ScriptMaterialProxy: SetFloat/Int/Vector/Texture/BlendMode 等フル実装 | ✅ |
| Deferred GBuffer 互換変換（96 byte レイアウト） | ✅ |
| SkinnedMeshRenderer Surface シェーダー誤適用警告 + フォールバック | ✅ |

### RenderSystem / ResourceManager

| 機能 | 状態 |
|---|---|
| `ResourceManager::Reset()` + RenderSystem static resource 世代検知 | ✅ |

---

## 不足機能・バグ

---

### CameraComponent

#### 重要度：低

**複数 `isMain=true` のバリデーションがない**

エディターで直接 Reflect 編集すると複数 main が共存しうる。

**修正方針：** RenderSystem が Camera を選択する際に最初の enabled な `isMain=true` をピックする防御コードを追加する。

---

### LightComponent

#### 重要度：高

**Point / Spot ライトにシャドウが生成されない**

ShadowPass は 1 枚の `lightViewProjection` と `Texture2D<float>` を使う設計で、Point のキューブマップ Shadow と複数 Spot の shadow atlas が未実装。

**修正方針：** Spot ライトは透視投影シャドウマップ、Point ライトはキューブマップ Shadow で追加対応する。設計方針を先に決定すること。

---

#### 重要度：低

**アンビエントカラーが RenderSystem 内に `0.08` ハードコード**

`LightComponent` から設定できない。

**修正方針：** シーンレベルの `AmbientLight` コンポーネントまたは `RenderSettings` に `ambientColor` フィールドを追加する。

---

### SkyRenderer

#### 重要度：中

**太陽方向（`sunDirection`）フィールドがない**

SkyPass は LightCB（Directional ライトの向き）から取得するが、Directional ライトがないシーンで太陽位置が不定になる。

**修正方針：** `math::Vector3 sunDirection` フィールドを追加するか、LightCB 参照に依存することをコメントで明示する。

---

### DecalComponent

#### 重要度：中

**`receiverLayerMask` が `int` 経由でシリアライズされる**

`~0u`（Everything）が `-1` として保存され、シリアライザーが符号拡張を正しく処理しない場合にラウンドトリップが壊れる可能性がある。

**修正方針：** IReflector に uint32_t サポートを追加するか、TOML/JSON レイヤーで符号なし整数として明示保存する。

---

### MaterialComponent / MeshRenderer / SkinnedMeshRenderer

#### 重要度：高

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
| 1 | Light | Point / Spot シャドウ | 大 |

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
