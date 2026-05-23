# FBZZ Engine — UI システム設計書

最終更新: 2026-05-23  
対象 Step: 6.7（Step 6 完了後・Step 7 DX12 移行前）

---

## 目的と方針

| 観点 | 方針 |
|------|------|
| 用途 | ゲームランタイム用 2D UI（HUD・メニュー・スコア表示）。ImGui はエディター専用のまま維持 |
| 描画方針 | 既存の `DrawCall` / `RenderLayer::OVERLAY` を利用。`IRenderer` に新規仮想関数を追加しない |
| 座標系 | UICanvas の設計解像度空間（左上原点 px）。正射影行列でクリップ空間へ変換し、GPU が実 RT サイズへ自動スケーリング |
| 頂点バッファ | Phase 1: UIImage ごとに毎フレーム新規 VB を生成（簡潔さ優先）。Phase 2: Map/Unmap 動的 VB に差し替え |
| フォント | Phase 1: ビットマップフォントアトラス（PNG + UV テーブル）。Phase 2: SDF / FreeType（Step 7 以降） |
| DX12 互換 | `DrawCall` ベースのため UISystem 本体は DX12 移行時に変更不要 |

---

## モジュール配置

```
Engine/include/Engine/Scene/Components/
├── UICanvas.hpp        UI 座標空間のルートマーカー
├── UIImage.hpp         テクスチャ・単色矩形描画
├── UIButton.hpp        クリック判定・視覚状態管理
└── UIText.hpp          テキスト描画（Phase 2）

Engine/include/Engine/Scene/Systems/
└── UISystem.hpp        UISystem 関数宣言

Engine/src/Scene/Systems/
└── UISystem.cpp        クアッド生成・DrawCall 構築・Submit

Assets/shaders/UI/
├── UISprite.hlsl       テクスチャ / 単色矩形シェーダー
└── UIText.hlsl         SDF テキストシェーダー（Phase 2）
```

---

## 依存関係

```
UISystem
├── scene::Scene        (SceneView / GetRootGameObjects)
├── renderer::IRenderer (Submit / DrawCall)
├── renderer::ResourceManager
└── input::Input        (UIButton: マウス座標をキャンバス空間に変換した値を受け取る)
```

依存方向（逆転禁止）:

```
editor → engine(scene, ui) → renderer, input
```

---

## 座標系と正射影行列

```
(0, 0) ──────────────────► X (px)
  │
  │    UICanvas 設計解像度空間
  │    例: 1920 × 1080
  ▼
  Y (px)
```

**正射影行列**（行優先・左手系 DX11）:

```
OrthoLH(left=0, right=canvas.canvasWidth, bottom=canvas.canvasHeight, top=0, zNear=0, zFar=1)
```

- ortho は UISystem 引数（実ピクセルサイズ）ではなく **`UICanvas.canvasWidth / canvasHeight`（設計解像度）** から生成する
- これにより UIImage の座標は常に設計解像度空間で記述でき、実 RT サイズが変わっても自動スケーリングされる
- UISystem に渡す `viewportWidth / viewportHeight` はヒットテストの座標変換にのみ使用する

---

## コンポーネント設計

### UICanvas

`GetParent() == nullptr` の GameObject にのみ付けることを想定する。  
UISystem は **ルート GO（親なし）かつ UICanvas を持つ** ものだけを処理する。  
親 GO を持つ UICanvas は無視される。

```cpp
// FBZZ Engine
// UICanvas.hpp | fbzz::scene

#pragma once
#include "Engine/Scene/Script.hpp" // IReflector

namespace fbzz::scene {

struct UICanvas {
    float canvasWidth  = 1920.f; // 設計解像度 (px)
    float canvasHeight = 1080.f;
    int   sortOrder    = 0;      // 大きいほど後に描画（手前）
    bool  enabled      = true;

    const char* GetTypeName() const { return "UICanvas"; }
    void Reflect(IReflector& r) {
        r.Field("enabled",      enabled);
        r.Field("canvasWidth",  canvasWidth);
        r.Field("canvasHeight", canvasHeight);
        r.Field("sortOrder",    sortOrder);
    }
};

} // namespace fbzz::scene
```

### UIImage

UICanvas の子孫 GO に付ける。**UICanvas 配下にない UIImage は UISystem に無視される。**  
`texture` が空の場合は UISystem が内部で生成した白 1×1 テクスチャを使い、`color` のみで塗りつぶす。

```cpp
// FBZZ Engine
// UIImage.hpp | fbzz::scene

#pragma once
#include "Engine/Renderer/ResourceHandle.hpp"
#include "Engine/Math/Vector2.hpp"
#include "Engine/Math/Vector4.hpp"
#include "Engine/Scene/Script.hpp"

namespace fbzz::scene {

struct UIImage {
    // 配置（設計解像度 px、canvas 左上からの絶対座標）
    math::Vector2 position = { 0.f, 0.f };
    math::Vector2 size     = { 100.f, 100.f };

    // 見た目
    ResourceHandle<TextureTag> texture = {}; // 空 = 単色（白 1×1 テクスチャを使用）
    math::Vector4              color   = { 1, 1, 1, 1 };
    math::Vector2              uvMin   = { 0, 0 };
    math::Vector2              uvMax   = { 1, 1 };
    bool                       enabled = true;

    const char* GetTypeName() const { return "UIImage"; }
    void Reflect(IReflector& r) {
        // Phase 1 制限: ResourceHandle<T> は IReflector 非対応 (D-9-4) のため
        // texture フィールドは Inspector 編集・TOML シリアライズ対象外。
        r.Field("enabled",  enabled);
        r.Field("position", position);
        r.Field("size",     size);
        r.Field("color",    color);
    }
};

} // namespace fbzz::scene
```

> **拡張メモ**: Anchor / Pivot による相対レイアウト（Unity 相当）は Phase 2 で追加。  
> Phase 1 は `position` / `size` のキャンバス絶対値で実装する。

### UIButton

`UIImage` と同じ GO に付けることで、その矩形をクリック可能にする。  
ヒットテストは UISystem が受け取る `mouseInCanvasSpace` で行う。  
`onClick` は「クリック完了の 1 フレームだけ true」のパルス値。

```cpp
// FBZZ Engine
// UIButton.hpp | fbzz::scene

#pragma once
#include "Engine/Math/Vector4.hpp"
#include "Engine/Scene/Script.hpp"

namespace fbzz::scene {

enum class UIButtonState { NORMAL, HOVERED, PRESSED };

struct UIButton {
    math::Vector4 normalColor  = { 1.0f, 1.0f, 1.0f, 1.0f };
    math::Vector4 hoverColor   = { 0.85f, 0.85f, 0.85f, 1.0f };
    math::Vector4 pressedColor = { 0.7f,  0.7f,  0.7f,  1.0f };
    bool          isInteractable = true;
    bool          enabled        = true;

    // runtime state — シリアライズ対象外
    UIButtonState state   = UIButtonState::NORMAL;
    bool          onClick  = false; // 押し上がりフレームのみ true
    bool          onEnter  = false; // ホバー開始フレームのみ true
    bool          onExit   = false; // ホバー終了フレームのみ true

    const char* GetTypeName() const { return "UIButton"; }
    void Reflect(IReflector& r) {
        r.Field("enabled",        enabled);
        r.Field("isInteractable", isInteractable);
        r.Field("normalColor",    normalColor);
        r.Field("hoverColor",     hoverColor);
        r.Field("pressedColor",   pressedColor);
    }
};

} // namespace fbzz::scene
```

---

## UISystem

### 宣言

```cpp
// FBZZ Engine
// UISystem.hpp | fbzz::scene

#pragma once
#include "Engine/Math/Vector2.hpp"

namespace fbzz {
    namespace scene    { class Scene; }
    namespace renderer { class IRenderer; class ResourceManager; }
}

namespace fbzz::scene {

// RenderSystem の直後に呼ぶ。
//
// mouseInCanvasSpace: Input::GetMousePos() をキャンバス座標系に変換した値。
//   変換は呼び出し側（EditorApp / main.cpp）が担う。
//   エディター: viewport 内マウス位置 → (pos / viewportSize) * canvasSize
//   スタンドアロン: ウィンドウ座標 → (pos / windowSize) * canvasSize
//
// viewportWidth / viewportHeight: 実際の Game RT ピクセルサイズ（ヒットテスト変換用）。
void UISystem(
    Scene&                     scene,
    renderer::IRenderer&       renderer,
    renderer::ResourceManager& resources,
    float                      viewportWidth,
    float                      viewportHeight,
    math::Vector2              mouseInCanvasSpace,
    bool                       mousePressed
);

} // namespace fbzz::scene
```

### 処理フロー

```
UISystem() {
    1. 初回のみ: 白 1×1 テクスチャを ResourceManager で生成して内部キャッシュ
    2. Scene::GetRootGameObjects() でルート GO 列挙
       → UICanvas を持つ & enabled なルートを sortOrder 昇順にソート
    3. 各 UICanvas ルートに対して深さ優先で子 GO をたどる
       ├─ UIImage が enabled
       │    → UIButton が同 GO に存在 → mouseInCanvasSpace でヒットテスト → state / イベント更新
       │    → 実効カラー = UIImage.color * (UIButton があれば state に応じた color)
       │    → クアッド 6 頂点をキャンバス座標で生成
       │    → 頂点バッファを ResourceManager で生成（Phase 1: 毎フレーム新規）
       │    → UIConstants 定数バッファを更新
       │         ortho   = OrthoLH(0, canvas.canvasWidth, canvas.canvasHeight, 0)
       │         color   = 実効カラー
       │         uvRect  = { uvMin, uvMax }
       │    → DrawCall 構築 { layer=OVERLAY, blend=ALPHA_BLEND, depth=DEPTH_OFF }
       │    → renderer.Submit(drawCall, resources)
       └─ UIImage が存在しない GO はスキップ（Transform だけの中間ノード等）
}
```

### 白 1×1 テクスチャの生成

UISystem の内部（.cpp のファイルスコープ）に `ResourceHandle<TextureTag> s_whiteTexture` をキャッシュする。  
初回呼び出し時に `!s_whiteTexture.IsValid()` を確認し、1×1 RGBA(255,255,255,255) のテクスチャを生成する。  
`texture == {}` の UIImage はこのテクスチャを差し込む。

### UIButton ヒットテスト

```cpp
// mouseInCanvasSpace が UIImage の矩形内かどうか
bool hit = (mouse.x >= img.position.x && mouse.x <= img.position.x + img.size.x &&
            mouse.y >= img.position.y && mouse.y <= img.position.y + img.size.y);
```

呼び出し側でのマウス座標変換例（エディター Play モード）:

```cpp
// EditorApp.cpp (概略)
math::Vector2 mouseScreen = Input::GetMousePos(); // ウィンドウ座標
math::Vector2 vpOrigin    = ctx.gameViewportOrigin; // ImGui viewport の左上 (px)
math::Vector2 vpSize      = { ctx.gameViewportWidth, ctx.gameViewportHeight };
math::Vector2 mouseInVP   = mouseScreen - vpOrigin;
math::Vector2 mouseInCanvas = {
    mouseInVP.x / vpSize.x * canvas.canvasWidth,
    mouseInVP.y / vpSize.y * canvas.canvasHeight
};
UISystem(scene, renderer, resources, vpSize.x, vpSize.y, mouseInCanvas, mousePressed);
```

### 定数バッファ レイアウト (register b0)

```cpp
struct UIConstants {
    math::Matrix4x4 ortho;   // キャンバス設計解像度 → クリップ空間
    math::Vector4   color;   // tint 乗算カラー
    math::Vector4   uvRect;  // xy = uvMin, zw = uvMax
};
```

### 頂点レイアウト

```cpp
struct UIVertex {
    math::Vector2 pos; // POSITION  (キャンバス設計解像度 px)
    math::Vector2 uv;  // TEXCOORD0 (0..1)
};
```

1 クアッドあたり 6 頂点（インデックスなし）:

```
頂点:  0=(left,top)    uv=(0,0)
       1=(left,bottom) uv=(0,1)
       2=(right,top)   uv=(1,0)
       3=(right,bottom)uv=(1,1)
三角形: {0,1,2}, {2,1,3}
```

---

## HLSL シェーダー — UISprite.hlsl

```hlsl
// FBZZ Engine — UISprite.hlsl

cbuffer UIConstants : register(b0) {
    float4x4 g_Ortho;
    float4   g_Color;
    float4   g_UVRect; // xy=uvMin  zw=uvMax
};

Texture2D    g_Texture : register(t0);
SamplerState g_Sampler : register(s5); // CLAMP_LINEAR

struct VSIn { float2 pos : POSITION; float2 uv : TEXCOORD0; };
struct PSIn { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

PSIn VS(VSIn v) {
    PSIn o;
    o.pos = mul(float4(v.pos, 0.f, 1.f), g_Ortho);
    o.uv  = g_UVRect.xy + v.uv * (g_UVRect.zw - g_UVRect.xy);
    return o;
}

float4 PS(PSIn p) : SV_TARGET {
    return g_Texture.Sample(g_Sampler, p.uv) * g_Color;
}
```

---

## PipelineState

UISprite 専用の `PipelineStateDesc`:

| フィールド | 値 | 理由 |
|-----------|-----|------|
| `blend` | `BlendMode::ALPHA_BLEND` | 半透明 UI |
| `depth` | `DepthMode::DEPTH_OFF` | UI は深度競合しない |
| `rasterizer` | `RasterizerMode::SOLID` | 標準 |
| `layer` (DrawCall) | `RenderLayer::OVERLAY` | 3D 描画の後に確実に重ねる |

---

## ゲームループへの組み込み

```cpp
// EditorApp / sandbox main loop（概略）

// --- マウス座標をキャンバス空間へ変換（呼び出し側の責務）---
math::Vector2 mouseInCanvas = ComputeMouseInCanvasSpace(ctx, canvas);
bool mousePressed = Input::IsMouseButtonDown(MouseButton::LEFT);

renderer.BeginFrame();
renderer.SetRenderTarget(gameRT, resources);
renderer.Clear(clearColor);
renderer.ClearDepth(1.0f);

scene::RenderSystem(*scene, renderer, resources, camera, gameRT); // 3D
scene::UISystem(*scene, renderer, resources,                       // 2D
                gameViewportW, gameViewportH, mouseInCanvas, mousePressed);

renderer.SetRenderTarget({}, resources); // スワップチェーンへ戻す
editor.Render(ctx);
renderer.EndFrame();
```

`UISystem` は `RenderSystem` の **直後** に呼ぶ。`OVERLAY` レイヤーは 3D より後に描画される。

---

## エディター統合

| 箇所 | 対応内容 |
|------|---------|
| SceneHierarchyPanel | UICanvas / UIImage / UIButton を通常 GameObject と同列表示 |
| InspectorPanel | 各コンポーネントの `Reflect()` フィールドを既存 `DrawComponentSection` テンプレートで描画 |
| Add Component メニュー | UICanvas / UIImage / UIButton を追加 |
| SceneSerializer | UICanvas・UIImage の TOML シリアライズ対応（`texture` は Phase 1 対象外） |

---

## 実装順序

| # | タスク | 新規 / 修正 | 依存 |
|---|--------|------------|------|
| 1 | `UICanvas` / `UIImage` / `UIButton` ヘッダー + `ComponentRegistry.hpp` 追記 | 新規 / 修正 | — |
| 2 | `UISprite.hlsl` + PipelineState 登録 | 新規 | ResourceManager |
| 3 | `UISystem` — 白テクスチャ生成・クアッド生成・DrawCall Submit | 新規 | 1, 2 |
| 4 | ゲームループへの `UISystem` 呼び出し追加（マウス座標変換含む） | 修正 | 3 |
| 5 | `UIButton` ヒットテスト（mouseInCanvasSpace 連携） | 修正 UISystem | 3, 4 |
| 6 | `Reflect()` / InspectorPanel 統合 | 修正 InspectorPanel | 1 |
| 7 | SceneSerializer 対応（texture 除く） | 修正 SceneSerializer | 1 |
| 8 | `UIText` — ビットマップフォントアトラス | 新規 | 3 |

---

## 変更ファイル一覧

### 新規

| ファイル | 内容 |
|---------|------|
| `Engine/include/Engine/Scene/Components/UICanvas.hpp` | UI ルートコンポーネント |
| `Engine/include/Engine/Scene/Components/UIImage.hpp` | テクスチャ / 単色矩形 |
| `Engine/include/Engine/Scene/Components/UIButton.hpp` | クリック判定 |
| `Engine/include/Engine/Scene/Components/UIText.hpp` | テキスト（Phase 2） |
| `Engine/include/Engine/Scene/Systems/UISystem.hpp` | UISystem 宣言 |
| `Engine/src/Scene/Systems/UISystem.cpp` | UISystem 実装 |
| `Assets/shaders/UI/UISprite.hlsl` | スプライト / 矩形シェーダー |
| `Assets/shaders/UI/UIText.hlsl` | SDF テキストシェーダー（Phase 2） |

### 修正

| ファイル | 変更内容 |
|---------|---------|
| `Engine/include/Engine/Scene/ComponentRegistry.hpp` | `UICanvas` / `UIImage` / `UIButton` を `ComponentList` に追加 |
| `Editor/src/EditorApp.cpp` | `UISystem` 呼び出し・マウス座標変換を RenderSystem の直後に追加 |
| `Sandbox/src/main.cpp` | 同上（スタンドアロン実行時） |
| `Editor/src/Panels/InspectorPanel.cpp` | UICanvas / UIImage / UIButton セクション追加 |
| `Engine/src/Scene/SceneSerializer.cpp` | UICanvas / UIImage の TOML シリアライズ追加（texture 除く） |

---

## Phase 1 既知の制限

| 制限 | 理由 | 解消予定 |
|------|------|---------|
| `UIImage.texture` は Inspector 編集・TOML 保存不可 | IReflector が `ResourceHandle<T>` 非対応（D-9-4） | Phase 2 で D-9-4 解消後に対応 |
| UIImage ごとに毎フレーム VB を新規生成 | Map/Unmap 動的 VB API が未実装 | Phase 2 で動的 VB に差し替え |
| Anchor / Pivot レイアウト未対応 | スコープ外 | Phase 2 |

---

## Phase 2 以降（Step 7 後）

| 機能 | 概要 |
|------|------|
| 動的頂点バッファ | `IRenderer::MapVertexBuffer` / `UnmapVertexBuffer` を追加。毎フレーム VB 生成を廃止 |
| Anchor / Pivot レイアウト | Unity 相当の相対配置。`anchorMin` / `anchorMax` / `pivot` による伸縮 |
| UIText / SDF フォント | FreeType でアトラス生成 → `UIText.hlsl` で SDF 描画 |
| `UIImage.texture` Inspector 対応 | D-9-4（IReflector enum / ResourceHandle 対応）が前提 |
| UILayoutGroup | HorizontalLayoutGroup / VerticalLayoutGroup |
| UIAnimator | カラー・位置の Tween アニメーション |
| World Space Canvas | 3D 空間に配置する UI（HP バー等） |
