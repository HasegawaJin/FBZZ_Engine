# Editor UI リファクタリング計画

調査日: 2026-06-27  
対象: `Projects/Editor/` 全体

---

## 現状のスコア

| カテゴリ | 評価 | 主な問題 |
|---------|------|---------|
| 描画順序 | 4/10 | AddComponent メニューと Inspector 描画順がズレている |
| セクションヘッダー | 4/10 | `SeparatorText` / `TextDisabled` / `SectionHeader` が混在 |
| スコープ管理 | 5/10 | static local / anonymous namespace / メンバー変数が混在 |
| Undo 統一性 | 6/10 | テンプレート・手動・独自実装が3系統並存 |
| 命名一貫性 | 7/10 | `DrawTransformInspector` のみ単数形 |
| ウィジェット再利用 | 7/10 | `DragVec3` / `AssetPathField` は統一済み。`ColorEdit4` が未統一 |
| 関数化度 | 8/10 | `DrawColliderCommon` 等の共通化は進んでいる。MeshCollider が例外 |

---

## 1. 順序の問題

### 1-A. Inspector 描画順 vs AddComponent 順がズレている

`InspectorPanel.cpp` の `OnRenderContent` 呼び出し順:

```
Rendering → Animation → Material → Lighting → Effects
→ Audio → Physics → Environment → UI → TerrainWater → Navigation → Script
```

`InspectorCommon.hpp` の `DrawAddComponentMenu` カテゴリ順:

```
Rendering → Physics → Navigation → Animation → Audio → UI → Scripts → Environment
```

Physics と Navigation の位置が特に乖離が大きい。

**修正案**: 両方を以下の順に揃える

```
Rendering (MeshRenderer / Material)
→ Lighting (Light / Camera)
→ Physics (RigidBody / Collider)
→ Animation (Animator / IK)
→ Audio
→ Effects (Particle / Trail)
→ Environment (Decal / IBL / PostProcess / Sky)
→ Navigation (NavMesh 系)
→ UI
→ Terrain / Water
→ Script  ← 常に最後
```

### 1-B. `DrawTransformInspector` が単数形

他の描画関数はすべて `DrawXxxInspectors` (複数形) なのに Transform だけ `DrawTransformInspector` になっている。

```cpp
// InspectorCore.cpp:74
void DrawTransformInspector(...)   // ← 単数形
void DrawRenderingInspectors(...)  // 複数形
void DrawAnimationInspectors(...)  // 複数形
```

**修正**: `DrawTransformInspectors` に改名。

---

## 2. 重複・分散している API

### 2-A. `ColorEdit4` — 9 箇所で `float[4]` 手動変換が重複

`math::Vector4` → `float[4]` の変換と `ImGui::ColorEdit4` の呼び出しが毎回手書きされている。

| ファイル | 変数 |
|---------|------|
| InspectorEffects.cpp | ParticleEmitter::colorStart/colorEnd |
| InspectorEffects.cpp | TrailComponent::colorStart/colorEnd |
| InspectorEffects.cpp | MeshTrailComponent::colorStart/colorEnd |
| InspectorUI.cpp | UIImage::color |
| InspectorUI.cpp | UIButton::normalColor / hoverColor / pressedColor |
| InspectorUI.cpp | UIText::color |
| InspectorUI.cpp | UIAnimator::colorTween from/to |
| InspectorEnvironment.cpp | DecalComponent::albedoColor / emissiveColor |
| InspectorEnvironment.cpp | AtmosphericScattering::fogColor |

**統合案** — `ImGuiWidgets.hpp` に追加:

```cpp
namespace fbzz::editor::widgets {

// 既存: Vector3 用 (ColorEdit3 は既実装済み)
bool ColorEdit3(const char* label, math::Vector3& color);

// 追加: Vector4 用
inline bool ColorEdit4(const char* label, math::Vector4& color) {
    float v[4] = { color.x, color.y, color.z, color.w };
    if (ImGui::ColorEdit4(label, v)) {
        color = { v[0], v[1], v[2], v[3] };
        return true;
    }
    return false;
}

} // namespace fbzz::editor::widgets
```

### 2-B. セクションヘッダーが 3 種類混在

```cpp
// 方式 1: 新しい ImGui API (Material/Decal/Environment 等)
ImGui::SeparatorText("Emission");

// 方式 2: TextDisabled (Physics 等の古い書き方)
ImGui::TextDisabled("Shadow");

// 方式 3: widgets::SectionHeader (ImGuiWidgets.hpp に定義あるが未使用)
widgets::SectionHeader("Label");
```

**統合案** — `widgets::SectionHeader` を `SeparatorText` で実装し、全箇所に適用:

```cpp
// ImGuiWidgets.hpp
namespace fbzz::editor::widgets {
inline void SectionHeader(const char* label) {
    ImGui::SeparatorText(label);
}
} // namespace fbzz::editor::widgets
```

移行後は `ImGui::TextDisabled` のセクション用途を `widgets::SectionHeader` に置き換え。

### 2-C. Terrain LayerMaterials が `AssetPathField` を使っていない

`InspectorTerrainWater.cpp` の LayerMaterials 描画だけが `ImGui::InputText` + 手動 D&D で実装されており、他の Asset パス入力で統一されている `widgets::AssetPathField` と乖離している。

**修正**: `widgets::AssetPathField("Layer Material", layer.materialPath, ".mat", ctx.projectRoot)` に統一。

### 2-D. `MeshColliderComponentSection` が独自 Undo 実装

`DrawMeshColliderComponentSection` と `DrawConvexHullColliderComponentSection` だけが `DrawComponentSection<T>` テンプレートを使わず、有効/無効トグル・Undo 記録を独自実装している (約 100 行)。

**中期対応**: `DrawComponentSection<T>` に「スナップショットカスタマイズ」コールバックを追加し、MeshCollider の深いコピーを受け付けられるようにする。

---

---

## 3. メニューバー・設定 UI の重複

### 3-A. ポストプロセス設定が 3 箇所に存在

同じ `ProjectSettings.render.postProcess` フィールドを 3 つの UI から編集できる。

| 設定項目 | Debug > Post Process メニュー | ProjectSettings パネル | PostProcessVolume Inspector |
|---------|:---:|:---:|:---:|
| Bloom / Fog / FXAA / Color Grading 等 15 項目 | MenuItem + Slider | CollapsingHeader | DrawPostProcessInspector |

**問題の構造**:

```
EditorApp_MenuBar.cpp   →  render.postProcess.bloomEnabled = !bloomEnabled
ProjectSettingsPanel.cpp →  DrawPostProcessInspector(render.postProcess, &render)
InspectorEnvironment.cpp →  DrawPostProcessInspector(ppv.settings, nullptr)
```

- `DrawPostProcessInspector()` の共有は正しい設計 (ProjectSettings ↔ Volume で共通 UI)
- Debug メニューはその値を **MenuItem でミラー** しているだけ → 冗長
- Debug メニューにしか存在しないスライダー (Exposure / Contrast / Saturation 等) が **詳細設定との分断**を生んでいる

**修正案 (Debug > Post Process を削除し Quick Toggle に統合)**:

```cpp
// EditorApp_MenuBar.cpp の Debug メニュー
if (ImGui::BeginMenu("Post Process")) {
    // 削除: 15 個の MenuItem + Slider

    // 代替: ProjectSettings へのジャンプ
    if (ImGui::MenuItem("Open Post Process Settings...")) {
        m_projectSettingsPanel->visible = true;
        m_projectSettingsPanel->ScrollTo(ProjectSettingsSection::PostProcess);
    }
    ImGui::Separator();
    // 残す: よく切り替える最重要トグル 3 つのみ
    ImGui::MenuItem("Bloom",  nullptr, &render.postProcess.bloom.enabled);
    ImGui::MenuItem("Shadow", nullptr, &render.shadowEnabled);
    ImGui::MenuItem("FXAA",   nullptr, &render.postProcess.fxaaEnabled);
    ImGui::EndMenu();
}
```

**削減**: メニュー項目 14 個 → 3 個 + ジャンプリンク

### 3-B. デバッグオーバーレイ設定が 2 箇所に存在

| フラグ | Debug メニュー | ProjectSettings > Render |
|-------|:---:|:---:|
| showColliders | MenuItem | Checkbox |
| showTerrainCollision | MenuItem | Checkbox |
| showNavMesh | MenuItem | Checkbox |
| showAiSensors | MenuItem | Checkbox |
| showDecalBounds | MenuItem | Checkbox |
| viewMode | Submenu (Combo) | Combo |

ProjectSettings 側には Shadow の詳細パラメーター (PCF Radius, PCSS 等) もあるが Debug メニューには単純トグルのみ → 非対称。

**修正案**: Debug メニューのトグルを残しつつ、ProjectSettings Render セクションに Debug Visualization の CollapsingHeader を追加してグルーピング:

```cpp
// ProjectSettingsPanel.cpp — Render セクションに追加
if (ImGui::CollapsingHeader("Debug Visualization")) {
    ImGui::Checkbox("Colliders",        &render.showColliders);
    ImGui::Checkbox("Terrain Collision",&render.showTerrainCollision);
    ImGui::Checkbox("NavMesh",          &render.showNavMesh);
    ImGui::Checkbox("AI Sensors",       &render.showAiSensors);
    ImGui::Checkbox("Decal Bounds",     &render.showDecalBounds);
    ImGui::Checkbox("Selection Outline",&render.showSelectionOutline);
}
```

### 3-C. EditorSettings にポストプロセスフィールドが 30 件以上ある

`EditorSettings.hpp` に `ppBloomEnabled`, `ppExposure`, `ppBloomIntensity` 等が定義されているが、これらは `ProjectSettings.render.postProcess` と **役割が重複**している。

どちらを編集すると実際に反映されるのかが不明確。

**修正案**: EditorSettings から PP 関連フィールドを削除し、ProjectSettings に一本化。EditorSettings はカメラ速度・グリッドサイズ等の**エディター操作 UX 設定のみ**に絞る。

### 3-D. パネル可視性制御の経路が 5 系統ある

パネルを開く手段が統一されていない:

```cpp
// 経路 1: View > Panels メニュー (直接フラグ)
panel->visible = true;

// 経路 2: ワンショット要求フラグ (EditorContext)
ctx.requestOpenBuildSettings = true;

// 経路 3: ツールフラグ (EditorContext)
ctx.showTerrainTool = true;

// 経路 4: 特定パネル直接参照
m_iblBakePanel->visible = true;

// 経路 5: Tools メニュー Map Mode
switchToMapEditingMode();
```

**修正案 (IPanel 拡張)**:

```cpp
// IPanel.hpp — 追加フィールド
class IPanel {
public:
    virtual const char* GetShortcutKey()       const { return nullptr; }
    virtual bool        GetDefaultVisibility() const { return false;   }
    virtual const char* GetMenuCategory()      const { return "Window";}
};

// EditorApp.cpp — パネル管理を自動化
void EditorApp::OpenPanel(const char* panelName) {
    for (auto& p : m_panels)
        if (strcmp(p->GetWindowName(), panelName) == 0) { p->visible = true; return; }
}
```

これにより `requestOpenBuildSettings` 等のワンショットフラグが不要になる。

---

## 4. UX 改善提案

### 4-A. ProjectSettings に検索バーを追加

ProjectSettings は 9 セクション・100 以上の項目があり、目的の設定を探すのに数クリック必要。

```cpp
// ProjectSettingsPanel::OnRenderContent() 先頭に追加
static char searchBuf[128] = {};
ImGui::SetNextItemWidth(-1.f);
ImGui::InputTextWithHint("##search", "Search settings...", searchBuf, sizeof(searchBuf));
// 各 CollapsingHeader を表示するかどうかを searchBuf でフィルタ
```

### 4-B. Tools メニューのツール項目をサブメニューにまとめる

```cpp
// 変更前: フラット 4 項目
ImGui::MenuItem("Terrain Tool", nullptr, &ctx.showTerrainTool);
ImGui::MenuItem("Water Tool",   nullptr, &ctx.showWaterTool);
ImGui::MenuItem("Detail Tool",  nullptr, &ctx.showDetailTool);
ImGui::MenuItem("Foliage Tool", nullptr, &ctx.showFoliageTool);

// 変更後: サブメニュー化
if (ImGui::BeginMenu("Terrain & Map")) {
    ImGui::MenuItem("Terrain Tool", nullptr, &ctx.showTerrainTool);
    ImGui::MenuItem("Water Tool",   nullptr, &ctx.showWaterTool);
    ImGui::MenuItem("Detail Tool",  nullptr, &ctx.showDetailTool);
    ImGui::MenuItem("Foliage Tool", nullptr, &ctx.showFoliageTool);
    ImGui::Separator();
    ImGui::MenuItem("Map Editing Mode");
    ImGui::EndMenu();
}
```

### 4-C. ホットキーとメニュー文字列を同期

現在メニューに `"Ctrl+O"` 等の文字列がハードコードされており、HotkeyManager の定義と二重管理になっている。

```cpp
// 改善案: IPanel から取得した文字列をメニューに渡す
const char* shortcut = panel->GetShortcutKey(); // "Ctrl+Shift+B" など
if (ImGui::MenuItem(panel->GetWindowName(), shortcut))
    panel->visible = !panel->visible;
```

---

## 5. 実施ロードマップ (全体統合版)

### Phase 1 — 小さくて確実 (優先度高)

| # | 作業 | ファイル | 工数 |
|---|------|---------|------|
| 1 | `ColorEdit4` ウィジェット追加 | `ImGuiWidgets.hpp` | 15 分 |
| 2 | `SectionHeader` を `SeparatorText` で実装し全箇所移行 | 全 Inspector | 30 分 |
| 3 | `DrawTransformInspectors` に改名 | `InspectorCore.cpp` + 呼び出し元 | 5 分 |
| 4 | Inspector 描画順 + AddComponent 順を上記基準に揃える | `InspectorPanel.cpp` + `InspectorCommon.hpp` | 20 分 |
| 5 | Terrain LayerMaterials を `AssetPathField` に統一 | `InspectorTerrainWater.cpp` | 20 分 |
| 6 | Debug > Post Process を Quick Toggle 3 件 + ジャンプリンクに削減 | `EditorApp_MenuBar.cpp` | 20 分 |
| 7 | ProjectSettings Render セクションに Debug Visualization ヘッダーを追加 | `ProjectSettingsPanel.cpp` | 15 分 |
| 8 | Tools メニューの地形ツール 4 件をサブメニューにまとめる | `EditorApp_MenuBar.cpp` | 10 分 |

### Phase 2 — 中規模 (余裕ができたら)

| # | 作業 | 目的 |
|---|------|------|
| 9 | `DrawComponentSection` に Undo カスタマイズ対応を追加 | MeshCollider/ConvexHull の独自実装を統合 |
| 10 | `AssetPathFieldWithLoad` — ロードコールバック付き版 | Material/Texture 変更時の再ロードパターンを集約 |
| 11 | EditorSettings から PP フィールド削除し ProjectSettings に一本化 | 設定の重複除去 |
| 12 | ProjectSettings に検索バーを追加 | UX 改善 |

### Phase 3 — 将来 (設計検討が必要)

| # | 作業 |
|---|------|
| 13 | IPanel 拡張 (GetShortcutKey / GetDefaultVisibility / GetMenuCategory) でパネル管理を自動化 |
| 14 | Undo 機構の完全テンプレート統一 (現在 3 系統) |
| 15 | スコープ管理の整理 (static local / anonymous namespace を整理) |

---

## 6. 変更影響範囲

Phase 1 の変更はすべて `Projects/Editor/` 内に完結し、Engine 側のヘッダーに影響しない。  
`DrawTransformInspectors` 改名は `InspectorPanel.cpp` の呼び出し 1 箇所のみ変更。  
描画順変更は見た目のみで動作に影響しない。  
Debug > Post Process の削減は `EditorApp_MenuBar.cpp` の 60 行程度の削除のみ。
