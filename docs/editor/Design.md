# Editor System 設計書

`fbzz::editor` — ImGui ベースのランタイムエディター。  
シーン階層・インスペクター・ビューポート・ライト編集パネルを提供する。

---

## 目的と方針

| 目的 | 詳細 |
|------|------|
| 実行時編集 | ライト・マテリアル・Transform をゲームループ中に変更できる |
| ポートフォリオ映え | 動くエディター UI は採用担当に設計力を示せる |
| DX12 移行耐性 | ImGui のバックエンド切り替えを `IRenderer` の中に閉じ込め、editor コードは DX 世代に依存しない |

---

## モジュール配置

```
FBZZ_Engine/
├── editor/
│   ├── CMakeLists.txt
│   ├── include/editor/
│   │   ├── EditorApp.hpp
│   │   ├── EditorContext.hpp
│   │   └── Panels/
│   │       ├── IPanel.hpp
│   │       ├── SceneHierarchyPanel.hpp
│   │       ├── InspectorPanel.hpp
│   │       ├── ViewportPanel.hpp
│   │       └── LightPanel.hpp
│   └── src/
│       ├── EditorApp.cpp
│       └── Panels/
│           ├── SceneHierarchyPanel.cpp
│           ├── InspectorPanel.cpp
│           ├── ViewportPanel.cpp
│           └── LightPanel.cpp
└── third_party/
    └── imgui/          (docking ブランチ, v1.91+)
```

依存方向 (逆転禁止):

```
editor → engine → physics → math
```

`engine` は `editor` を知らない。`editor` からのみ `engine` を参照する。

---

## ImGui 統合戦略 — IRenderer 経由

ImGui のバックエンド (`imgui_impl_dx11` / `imgui_impl_dx12`) は  
`IRenderer` の仮想メソッドとして隠蔽する。  
`editor` コードは直接 DX11/DX12 の型を触らない。

```cpp
// IRenderer.hpp に追加する仮想メソッド (3 つ)
class IRenderer {
public:
    // ... 既存メソッド ...

    virtual void ImGuiInit()                  = 0;  // imgui_impl_dxXX_Init 呼び出し
    virtual void ImGuiShutdown()              = 0;  // imgui_impl_dxXX_Shutdown 呼び出し
    virtual void ImGuiRenderDrawData()        = 0;  // imgui_impl_dxXX_RenderDrawData 呼び出し
    virtual void ImGuiNewFrame()              = 0;  // imgui_impl_dxXX_NewFrame 呼び出し

    // ビューポートパネルで IRenderTarget のカラーを ImGui テクスチャとして渡すため
    virtual ImTextureID GetImTextureID(std::shared_ptr<IRenderTarget> rt, int slot = 0) = 0;
};
```

DX11 実装例:
```cpp
void DX11Renderer::ImGuiInit() {
    ImGui_ImplDX11_Init(m_device.Get(), m_context.Get());
}
void DX11Renderer::ImGuiRenderDrawData() {
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
}
```

DX12 移行時は `DX12Renderer` が同じインターフェースを実装するだけ。  
`editor` コードは一切変更不要。

---

## クラス設計

### EditorContext

エディター全体で共有する状態。パネル間の疎結合を保つ仲介役。

```cpp
// editor/include/editor/EditorContext.hpp
namespace fbzz::editor {

struct EditorContext {
    scene::Scene*           activeScene   = nullptr;  // 非所有
    renderer::LightSystem*  lightSystem   = nullptr;  // 非所有
    renderer::Camera*       editorCamera  = nullptr;  // 非所有

    // 選択状態
    scene::EntityID         selectedEntity = scene::INVALID_ENTITY;

    // ビューポートでのカメラ操作が有効かどうか
    bool viewportFocused = false;
};

} // namespace fbzz::editor
```

---

### IPanel

全パネルの基底インターフェース。

```cpp
// editor/include/editor/Panels/IPanel.hpp
namespace fbzz::editor {

class IPanel {
public:
    virtual ~IPanel() = default;
    virtual void OnRender(EditorContext& ctx) = 0;

    bool visible = true;
};

} // namespace fbzz::editor
```

---

### SceneHierarchyPanel

シーン内の全 GameObject をツリー表示し、選択状態を `EditorContext` に書き込む。

```cpp
class SceneHierarchyPanel : public IPanel {
public:
    void OnRender(EditorContext& ctx) override;
    // ImGui::Begin("Scene Hierarchy") → scene->GetEntities() を走査
    // → ImGui::Selectable(go.name) → ctx.selectedEntity に書き込む
};
```

---

### InspectorPanel

`EditorContext::selectedEntity` が示す GameObject のコンポーネントを表示・編集する。

```cpp
class InspectorPanel : public IPanel {
public:
    void OnRender(EditorContext& ctx) override;
    // Transform: DragFloat3 で localPosition / localScale / localRotation
    // MeshRenderer: enabled チェックボックス、マテリアル名表示
    // ParticleEmitter: emitRate / lifetime スライダー
};
```

コンポーネントの描画は `DrawComponent<T>(EditorContext&)` のテンプレート関数で分割する。  
新しいコンポーネントを追加するときは関数を増やすだけでよい。

```cpp
template<typename T>
void DrawComponent(EditorContext& ctx);  // 各コンポーネント型に特殊化
```

---

### ViewportPanel

レンダリング結果を ImGui テクスチャとして表示し、カメラ操作を受け付ける。

```cpp
class ViewportPanel : public IPanel {
public:
    void OnRender(EditorContext& ctx) override;
    // ImGui::Begin("Viewport") → パネルサイズ取得
    // → ctx.editorCamera のアスペクト比を更新
    // → ImGui::Image(renderer.GetImTextureID(hdrRT)) でシーンを表示
    // → IsWindowFocused() → ctx.viewportFocused を更新
    // → フォーカス中は DebugCamera 操作を有効にする

    std::shared_ptr<renderer::IRenderTarget> hdrRT;  // RenderSystem が描いた結果
};
```

---

### LightPanel

ディレクショナル・ポイント・スポットの各ライトを ImGui スライダーで編集する。

```cpp
class LightPanel : public IPanel {
public:
    void OnRender(EditorContext& ctx) override;
    // DirectionalLight: ColorEdit3 (color) + SliderFloat (intensity) + DragFloat3 (direction)
    // PointLight[i]:    ColorEdit3 / SliderFloat(intensity) / SliderFloat(range) / DragFloat3(position)
    // SpotLight[i]:     上記 + SliderAngle(innerAngle) + SliderAngle(outerAngle)
};
```

---

### EditorApp

エディター全体のライフサイクルを管理するクラス。

```cpp
// editor/include/editor/EditorApp.hpp
namespace fbzz::editor {

class EditorApp {
public:
    bool Init(renderer::IRenderer& renderer, void* hwnd);
    void Shutdown();

    // ゲームループから毎フレーム呼ぶ
    void BeginFrame();                  // ImGui::NewFrame() + ドックスペース構築
    void RenderPanels(EditorContext& ctx);  // 全パネルの OnRender を呼ぶ
    void EndFrame(renderer::IRenderer& renderer);  // ImGui::Render() + RenderDrawData

    EditorContext& GetContext() { return m_ctx; }

private:
    EditorContext m_ctx;
    std::vector<std::unique_ptr<IPanel>> m_panels;
};

} // namespace fbzz::editor
```

---

## レンダーループへの組み込み

```cpp
// main.cpp (editor 版)
editor::EditorApp editor;
editor.Init(renderer, hwnd);

// EditorContext にエンジンオブジェクトを登録
editor.GetContext().activeScene  = sm.GetActive();
editor.GetContext().lightSystem  = &lights;
editor.GetContext().editorCamera = &debugCamera.camera;

while (app.IsRunning())
{
    // --- 更新 ---
    core::Time::Tick();
    input::Input::Update();
    app.GetWindow().PollEvents();
    sm.Update(dt, physWorld);

    // --- 描画 ---
    renderer.BeginFrame();
    renderer.Clear({ 0.005f, 0.005f, 0.02f, 1.0f });

    scene::RenderSystem(*sm.GetActive(), renderer, debugCamera.camera, lights);

    // --- エディター UI (シーン描画の後、バックバッファへ) ---
    editor.BeginFrame();
    editor.RenderPanels(editor.GetContext());
    editor.EndFrame(renderer);

    renderer.EndFrame();
}

editor.Shutdown();
```

`RenderSystem` はビューポートパネル内の `IRenderTarget` に描画し、  
エディター UI はバックバッファに描画する。2 つの描画パスは独立している。

---

## ImGui ドックスペース構成

```
┌─────────────────────────────────────────────┐
│  Menu Bar  [File] [View] [Light] ...        │
├──────────┬──────────────────────┬───────────┤
│ Scene    │                      │ Inspector │
│ Hierarchy│    Viewport          │           │
│          │   (IRenderTarget)    │ Transform │
│          │                      │ Material  │
│          │                      │ Emitter   │
├──────────┴──────────────────────┴───────────┤
│  Light Panel (Directional / Point / Spot)   │
└─────────────────────────────────────────────┘
```

`ImGui::DockSpaceOverViewport()` でウィンドウ全体をドックスペースにする。  
各パネルは独立した `ImGui::Begin()` ウィンドウとして実装するため、ユーザーが自由に並び替えられる。

---

## サードパーティ追加

| ライブラリ | バージョン | 追加場所 |
|-----------|-----------|---------|
| Dear ImGui | v1.91+ (docking ブランチ) | `third_party/imgui/` |

`CMakeLists.txt` に `IMGUI_DEFINE_MATH_OPERATORS` と `IMGUI_ENABLE_DOCKING` を定義する。  
`imgui_impl_win32.h` / `imgui_impl_dx11.h` を `engine/src/Renderer/Platform/DX11/` に置く。

---

## 実装順序

| 順序 | タスク | 概要 |
|------|--------|------|
| 1 | ImGui セットアップ | `third_party/imgui` 追加、CMake 設定、DX11Renderer に `ImGuiInit` 等を実装 |
| 2 | EditorApp 骨格 | `EditorApp`, `EditorContext`, `IPanel` を作成、ドックスペースのみ表示 |
| 3 | SceneHierarchyPanel | GameObject ツリー表示 + 選択 |
| 4 | InspectorPanel | Transform 編集 (DragFloat3) |
| 5 | LightPanel | DirectionalLight 編集、Point/Spot ライト追加・削除 |
| 6 | ViewportPanel | `IRenderTarget` → `GetImTextureID` → `ImGui::Image` |
| 7 | InspectorPanel 拡張 | Material / ParticleEmitter 編集 |

---

## 変更ファイル一覧

| 種別 | ファイル |
|------|---------|
| 新規ディレクトリ | `editor/` |
| 新規 | `editor/CMakeLists.txt` |
| 新規 | `editor/include/editor/EditorApp.hpp` |
| 新規 | `editor/include/editor/EditorContext.hpp` |
| 新規 | `editor/include/editor/Panels/IPanel.hpp` |
| 新規 | `editor/include/editor/Panels/SceneHierarchyPanel.hpp` |
| 新規 | `editor/include/editor/Panels/InspectorPanel.hpp` |
| 新規 | `editor/include/editor/Panels/ViewportPanel.hpp` |
| 新規 | `editor/include/editor/Panels/LightPanel.hpp` |
| 新規 | `editor/src/EditorApp.cpp` |
| 新規 | `editor/src/Panels/*.cpp` |
| 修正 | `engine/include/engine/Renderer/IRenderer.hpp` (`ImGui*` 仮想メソッド追加) |
| 修正 | `engine/src/Renderer/Platform/DX11/DX11Renderer.hpp/.cpp` (ImGui メソッド実装) |
| 修正 | `sandbox/src/main.cpp` (EditorApp 統合) |
| 追加 | `third_party/imgui/` |

---

## DX12 移行時の影響

`editor` コードへの影響はほぼゼロ。  
`DX12Renderer::ImGuiInit()` 等を `imgui_impl_dx12.h` で実装するだけ。  
`GetImTextureID()` の戻り値 (`ImTextureID`) は `D3D12_GPU_DESCRIPTOR_HANDLE` を `uint64_t` キャストするのが DX12 側の実装。
