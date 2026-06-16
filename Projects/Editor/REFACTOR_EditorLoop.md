# EditorApp ループ統合リファクタリング計画

## 背景・目的

現在 `EditorLauncher/src/main.cpp` の `RunEditorLoop()` がゲームループ・システム呼び出し・レンダリングをすべて担っており、  
`SceneManager` と重複したシステム呼び出し管理が生じている。

`StandaloneApp` はすでに自己完結したループ管理者として `app.Run(standaloneApp)` に収まっているが、  
エディタだけ main.cpp がループを持つ非対称な構造になっている。

**目標:** EditorApp を StandaloneApp と同じ立ち位置に引き上げ、main.cpp を薄いエントリポイントにする。

```
// 変更前
main.cpp → RunEditorLoop(...)   ← main.cpp がループを所有

// 変更後
main.cpp → app.Run(editorApp)   ← EditorApp がループを所有
```

---

## 変更後の main.cpp イメージ（目標 ~50 行）

```cpp
if (args.standalone) {
    // ... 既存の StandaloneApp パス（変更なし）
} else {
    if (!app.Init()) return 1;
    renderer::ResourceManager resources(renderer);
    asset::AssetManager::Init(resources, ...);

    EditorApp editorApp;
    editorApp.Init(renderer, imguiRenderer, resources, app.GetWindow());
    editorApp.OpenProject(...);
    app.Run(editorApp);          // ← EditorApp が IApplication を実装する形
}
```

---

## Step 1 — SceneManager の整備（先に行う）

### 1-1. FoliageBakeSystem の実行順をエディタに合わせる

現在 SceneManager はシミュレート時に `Script → Transform → FoliageBake → Physics` の順で実行しているが、  
エディタ(main.cpp) は `Transform → FoliageBake → Script → Transform → Physics` の順。  
スクリプトが stamp 子 GO にアクセスする初回フレームで不整合が生じるため、SceneManager を修正する。

```cpp
// SceneManager::Update — 変更後のシミュレートパス
FoliageBakeSystem(*scene);   // ← Phase 0: Script より前に移動
ScriptSystem(*scene, dt);    // Phase 1
TransformSystem(*scene);     // Phase 2
// Phase 3: Physics loop ...
```

### 1-2. ApplyPhysicsSettings の扱い

エディタはプレイ中毎フレーム `ApplyPhysicsSettings(physicsWorld, settings)` を呼んでいる。  
選択肢:
- **A)** `SceneManager::SetPhysicsSettings(const PhysicsSettings&)` を追加し内部で適用
- **B)** `SceneManager::Update` の前に呼び出し側（EditorApp）から毎フレーム呼ぶ（今と同じ構造、シンプル）

→ B が変更が少なく安全。EditorApp 内部で Update 前に呼ぶ。

---

## Step 2 — RunEditorLoop を EditorApp に移植

### 移動するもの

| 項目 | 移動先 | 備考 |
|------|--------|------|
| `physics::World physicsWorld` | `EditorApp` メンバ | `playMode->ApplyPendingRestore` でリセット |
| `float physicsAccumulator` | `SceneManager` が既に持つ | `SetSimulating(false)` でリセット済み |
| `renderer::DebugCamera debugCamera` | `EditorApp` メンバ | `GetContext().editorCamera` に既に公開されている |
| `FocusAnim` ロジック | `EditorApp::Tick()` 内 | `requestFocusOnSelected` は既に EditorContext にある |
| `WarmupRenderResources()` | `EditorApp::Init()` 末尾 | Init 完了後に 1 度だけ呼ぶ |
| システム呼び出し群 | `SceneManager::Update/LateUpdate` | Step 1 完了後に置き換え |

### 移動しないもの（main.cpp または app フレームワーク側）

- `app.GetWindow().PollEvents()` / `app.IsRunning()`
- `Time::Tick()` / `input::Input::Update()`
- `profiler::Profiler::BeginFrame/EndFrame`
- `renderer.BeginFrame/EndFrame`

→ これらは `IApplication::Tick()` の共通前後処理としてすでに `app.Run()` 側が担っていれば不要。  
   `StandaloneApp` がどこで呼んでいるか確認して合わせる。

### EditorApp に追加するメソッド

```cpp
// EditorApp.hpp
class EditorApp : public core::IApplication {   // IApplication 実装に昇格
public:
    bool Init(renderer::IRenderer&, renderer::IImGuiRenderer&,
              renderer::ResourceManager&, core::Window&);
    void Tick(float dt) override;   // ← RunEditorLoop のループ本体
    void Shutdown() override;
};
```

### Tick() の骨格

```cpp
void EditorApp::Tick(float dt)
{
    BeginFrame();

    if (m_playMode->ApplyPendingRestore(*m_scene)) {
        m_physicsWorld = physics::World{};
        scene::ApplyPhysicsSettings(m_physicsWorld, GetContext().projectSettings);
    }

    if (!m_playMode->IsPlaying())
        m_debugCamera.Update(dt, GetContext().sceneViewportHovered);

    // フォーカスアニメ
    UpdateFocusAnim(dt);

    // SceneManager へ状態を伝達
    const bool stepFrame = m_playMode->ConsumeStep();
    m_sceneManager.SetSimulating(m_playMode->IsPlaying() || stepFrame);
    m_sceneManager.SetSingleStep(stepFrame);

    if (m_playMode->IsPlaying()) {
        scene::ApplyPhysicsSettings(m_physicsWorld, GetContext().projectSettings);
        scene::Script::SetPhysicsWorld(&m_physicsWorld);
    }

    m_sceneManager.Update(dt, m_physicsWorld);
    m_sceneManager.LateUpdate(dt, m_physicsWorld);

    // レンダリング
    RenderSceneView();
    RenderGameView();

    RenderPanels(GetContext());
    EndFrame(m_imguiRenderer);
}
```

---

## Step 3 — 確認・整理

- [ ] `PhysicsStepCountMarkerName()` が main.cpp と SceneManager で重複定義されているので片方に統一
- [ ] `Script::SetPhysicsWorld` の呼び出しタイミングを確認（Stop 時に nullptr を渡す箇所を EditorApp に移す）
- [ ] `scene->Clear()` / `Shutdown()` の順序が崩れていないか確認

---

## 作業順序

1. `fix/map-system` マージ後、新ブランチ `refactor/editor-loop` を切る
2. Step 1 (SceneManager 整備) → ビルド確認
3. Step 2 (RunEditorLoop → EditorApp 移植) → 動作確認
4. Step 3 (重複コード整理)
