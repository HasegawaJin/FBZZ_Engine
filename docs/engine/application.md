# Engine / Application

`fbzz::core::Application`。エンジン全体のエントリポイント。シングルトン。
全サブシステムのライフタイムを管理し、ゲームループを回す。

---

## クラス定義

```cpp
namespace fbzz::core {

class Application {
public:
    static Application& Get();

    void Run();
    void Quit();

    bool IsRunning() const { return m_isRunning; }

    // サブシステムアクセス
    Window&              GetWindow()          { return *m_window; }
    renderer::IRenderer& GetRenderer()        { return *m_renderer; }
    scene::Scene&        GetScene()           { return *m_scene; }
    physics::World&      GetPhysicsWorld()    { return *m_physicsWorld; }
    ResourceManager&     GetResourceManager() { return *m_resourceManager; }

private:
    Application();
    ~Application();

    void Init();
    void Shutdown();
    float CalcDeltaTime();

    std::unique_ptr<Window>              m_window;
    std::unique_ptr<renderer::IRenderer> m_renderer;
    std::unique_ptr<ResourceManager>     m_resourceManager;
    std::unique_ptr<scene::Scene>        m_scene;
    std::unique_ptr<physics::World>      m_physicsWorld;

    bool  m_isRunning   = true;
    float m_lastTime    = 0.0f;
    float m_accumulator = 0.0f;

    static constexpr float FIXED_DT = 1.0f / 60.0f;  // 物理固定タイムステップ
};

} // namespace fbzz::core
```

---

## 初期化順序

```
Application::Init()
│
├─ Window::Init(title, width, height)     Win32 ウィンドウ生成
├─ DX11Renderer::Init(hwnd, w, h)         DX11 デバイス・スワップチェーン生成
├─ ResourceManager::Init(renderer)        メッシュ・テクスチャキャッシュ初期化
├─ ShaderManager::Init()                  シェーダーキャッシュ初期化
├─ DebugDraw::Init(renderer)              デバッグ描画登録
├─ Scene::Init()                          シーン初期化
└─ Input::Init(hwnd)                      入力システム初期化
```

---

## ゲームループ

```cpp
void Application::Run() {
    Init();

    while (m_isRunning) {
        float dt = CalcDeltaTime();
        Time::Update(dt);                 // DeltaTime / TotalTime を更新

        m_window->PollEvents();           // Win32 メッセージポンプ
        if (m_window->ShouldClose()) {
            Quit();
            break;
        }

        Input::Update();                  // 前フレームの状態を保存

        // 固定タイムステップ物理 (フレームレートに依存しない安定した積分)
        m_accumulator += dt;
        while (m_accumulator >= FIXED_DT) {
            m_physicsWorld->Step(FIXED_DT);
            m_accumulator -= FIXED_DT;
        }
        m_scene->DispatchCollisionEvents(*m_physicsWorld);  // Enter/Stay/Exit を通知

        m_scene->Update(dt);              // 全 Component::OnUpdate() を呼ぶ
        m_scene->LateUpdate(dt);          // カメラ追従など、Update 後に行う処理

        m_renderer->BeginFrame();
        m_renderer->Clear(math::Vector4::BLACK);
        m_scene->Render(*m_renderer);     // 全 MeshRenderer::Draw() を呼ぶ
        DebugDraw::Flush();               // デバッグ描画をまとめて描画
        m_renderer->EndFrame();           // Present
    }

    Shutdown();
}
```

---

## デルタタイム計算

```cpp
float Application::CalcDeltaTime() {
    float currentTime = GetCurrentTimeSeconds();   // プラットフォーム依存
    float dt = currentTime - m_lastTime;
    m_lastTime = currentTime;
    return std::min(dt, 0.05f);  // 最大 50ms でキャップ (デバッグ中のスパイク対策)
}
```

---

## ファイル構成

```
engine/
├── include/engine/
│   └── Core/
│       └── Application.hpp
└── src/
    └── Core/
        └── Application.cpp
```

---

## 参考ドキュメント

- [QueryPerformanceCounter](https://learn.microsoft.com/ja-jp/windows/win32/api/profileapi/nf-profileapi-queryperformancecounter) — 高精度タイマー (CalcDeltaTime の実装に使う)
- [QueryPerformanceFrequency](https://learn.microsoft.com/ja-jp/windows/win32/api/profileapi/nf-profileapi-queryperformancefrequency) — タイマー周波数取得 (QPC と合わせて使う)
