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
    Window&            GetWindow()       { return *m_window; }
    renderer::IRenderer& GetRenderer()  { return *m_renderer; }
    scene::Scene&      GetScene()        { return *m_scene; }
    physics::World&    GetPhysicsWorld() { return *m_physicsWorld; }

private:
    Application();
    ~Application();

    void Init();
    void Shutdown();
    float CalcDeltaTime();

    std::unique_ptr<Window>              m_window;
    std::unique_ptr<renderer::IRenderer> m_renderer;
    std::unique_ptr<scene::Scene>        m_scene;
    std::unique_ptr<physics::World>      m_physicsWorld;

    bool  m_isRunning = true;
    float m_lastTime  = 0.0f;
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

        m_window->PollEvents();           // Win32 メッセージポンプ
        if (m_window->ShouldClose()) {
            Quit();
            break;
        }

        Input::Update();                  // 前フレームの状態を保存

        m_scene->Update(dt);              // 全 Component::OnUpdate() を呼ぶ
        m_physicsWorld->Step(dt);         // 物理シミュレーション

        m_renderer->BeginFrame();
        m_renderer->Clear(math::Vec4::BLACK);
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
