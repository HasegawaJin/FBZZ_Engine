# Panel Component化後の拡張・統合候補

Panel が IPanel インターフェースに統一されたことで着手可能になった項目の洗い出し。

---

## 現状の IPanel

```cpp
class IPanel {
    virtual ~IPanel() = default;
    virtual void OnRender(EditorContext& ctx) = 0;
    bool visible = true;
};
```

共通化済み: `visible` フラグ、EditorApp による一括 `OnRender()` 呼び出し。  
未共通化: ImGui ウィンドウ管理、null チェック、ライフサイクル。

---

## A. IPanel 基底クラスの拡充

**対象ファイル:** `Projects/Editor/include/Editor/Panels/IPanel.hpp`

全 7 パネルが同じ ImGui::Begin/End/Early-Exit パターンを手書きしている。

```cpp
// 各パネルで重複しているコード
if (!ImGui::Begin("Panel Name")) { ImGui::End(); return; }
// ...
ImGui::End();
```

### 対応案

**テンプレートメソッドパターン化**

```cpp
class IPanel {
public:
    virtual ~IPanel() = default;
    virtual const char* GetWindowName() const = 0;

    void OnRender(EditorContext& ctx) {
        if (!ImGui::Begin(GetWindowName())) { ImGui::End(); return; }
        OnRenderContent(ctx);
        ImGui::End();
    }

    bool visible = true;

protected:
    virtual void OnRenderContent(EditorContext& ctx) = 0;
};
```

**ライフサイクル追加（任意）**

```cpp
virtual void OnInit(EditorContext& ctx) {}
virtual void OnShutdown() {}
```

AssetBrowserPanel の `RefreshDirectory()`、ConsolePanel の sink 設定が OnInit に移せる。

---

## B. InspectorPanel の Component 編集テンプレート化

**対象ファイル:** `Projects/Editor/src/Panels/InspectorPanel.cpp` (L69–269)

10 種類の Component に対して同一パターンが手動反復:

```cpp
if (auto* mr = go->GetComponent<MeshRenderer>()) {
    if (ImGui::CollapsingHeader("Mesh Renderer", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Checkbox("Enabled##mr", &mr->enabled);
        ImGui::Separator();
        // フィールド編集...
    }
}
// × 10 Component
```

### 対応案

**DrawComponentSection<T>() テンプレート関数**

```cpp
template<typename T>
void DrawComponentSection(scene::GameObject* go, EditorContext& ctx,
                          const char* label, auto drawFn) {
    auto* comp = go->GetComponent<T>();
    if (!comp) return;
    if (!ImGui::CollapsingHeader(label, ImGuiTreeNodeFlags_DefaultOpen)) return;
    ImGui::Checkbox("Enabled", &comp->enabled);
    ImGui::Separator();
    drawFn(comp, ctx);
}
```

274 行 → 約 120 行に短縮可能。新規 Component 追加時のボイラープレートが消える。

### さらに進んだ対応: Reflect 統合

Script/Reflect システムが完成済みのため、エンジン Component に `Reflect()` を実装すれば Inspector 自動 UI が実現できる。

```cpp
// エンジン側 Component
struct MeshRenderer : IComponent {
    void Reflect(IReflector& r) override {
        r.Field("enabled", enabled);
        r.Field("castShadow", castShadow);
        // ...
    }
};

// InspectorPanel 側
for (auto* comp : go->GetAllComponents()) {
    if (ImGui::CollapsingHeader(comp->GetTypeName())) {
        ImGuiReflector r;
        comp->Reflect(r);  // ← 自動 UI
    }
}
```

新 Component を追加しても Inspector 側の変更ゼロになる。

---

## C. EditorContext ヘルパーの標準化

**対象ファイル:** `Projects/Editor/include/Editor/EditorContext.hpp`

SceneHierarchyPanel・InspectorPanel・LightPanel の 3 箇所で同一パターンが重複:

```cpp
if (!ctx.activeScene) {
    ImGui::TextDisabled("No active scene");
    ImGui::End();
    return;
}
```

### 対応案

```cpp
struct EditorContext {
    // 既存フィールド...

    bool HasActiveScene() const { return activeScene != nullptr; }

    scene::GameObject* GetSelectedGO() const {
        auto sel = PrimarySelected();
        if (!sel.IsValid() || !activeScene) return nullptr;
        return activeScene->GetGameObject(sel);
    }
};
```

IPanel のテンプレートメソッド化（A）と組み合わせると各パネルから null チェックを一掃できる。

---

## D. Scene::GetComponents<T>() の追加

**対象ファイル:** `Projects/Engine/include/Engine/Scene/Scene.hpp`

LightPanel が毎フレーム `GameObjects()` 全走査で LightComponent を収集している。  
将来 CameraPanel・ParticlePanel 等が増えると同パターンが再発する。

```cpp
// LightPanel.cpp で現在やっていること
for (auto& go : ctx.activeScene->GameObjects()) {
    auto* lc = go.GetComponent<LightComponent>();
    if (!lc) continue;
    // ...
}
```

### 対応案

```cpp
// Scene 側にクエリを追加
template<typename T>
std::vector<T*> Scene::GetComponents() const;

// LightPanel 側
for (auto* lc : ctx.activeScene->GetComponents<LightComponent>()) {
    // ...
}
```

---

## E. Step 7 (DX12 / RenderGraph) への準備

IPanel が統一されたことで以下のパネルを低コストで追加できる:

| 追加候補パネル | 用途 |
|--------------|------|
| RenderGraphPanel | ノードグラフ形式の RenderGraph 可視化 |
| ShaderBrowserPanel | シェーダーファイル一覧・リロード |
| GpuMemoryPanel | GPU バッファ・テクスチャの使用量表示 |
| ProfilerPanel | CPU/GPU タイム計測の可視化 |

いずれも IPanel を継承し EditorApp::m_panels に追加するだけで DockSpace に組み込める。

---

## 優先度まとめ

| 優先 | 項目 | 効果 | 難易度 |
|------|------|------|--------|
| 高 | **B. InspectorPanel DrawComponent<T>() テンプレート化** | 重複 150 行削減・新 Component 追加コスト削減 | 中 |
| 高 | **B+. エンジン Component への Reflect() 実装** | Reflect システムを活かし切る・Inspector 自動 UI | 高 |
| 中 | **A. IPanel テンプレートメソッド化** | 全パネルの Begin/End 重複を一掃 | 低 |
| 中 | **D. Scene::GetComponents<T>() 追加** | Panel 側の走査ボイラープレートを Scene に集約 | 低 |
| 低 | **C. EditorContext ヘルパー拡充** | null チェック重複の解消 | 低 |
| 低 | **E. Step 7 用パネル追加** | DX12 移行後の可視化基盤 | 高 |
