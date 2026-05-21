# FBZZ Engine — engine モジュール設計書

`engine/` モジュール全体を横断的に記述する一覧リファレンス。  
各サブシステムの詳細は末尾のリンク先ドキュメントを参照すること。

---

## モジュール構成

```
engine/
├── Core/      Application, Window, Time, Logger, ILogSink, HResult
├── Input/     Input, KeyCode
├── Renderer/  IRenderer (+ DX11 実装), Camera, DebugCamera, DebugDraw,
│              Mesh, PrimitiveMesh, Material, LightSystem, RenderSettings,
│              ShaderManager, DrawCall, ComputeCall,
│              IBuffer, IConstantBuffer, IShader, ITexture,
│              IPipelineState, IRenderTarget, RenderLayer, RenderState, SamplerMode
├── Scene/     EntityID, ComponentArray, Transform, GameObject, Scene, SceneManager
│              Components/  MeshRenderer, RigidBodyComponent, ParticleEmitter, SkyRenderer
│                           LightComponent, CameraComponent, AudioSourceComponent
│              Systems/     TransformSystem, PhysicsSystem, RenderSystem, AudioSystem
├── Asset/     AssetManager, Model, ModelImporter
├── Audio/     AudioSystem, IAudioDevice, XAudio2Device
└── Util/      FileSystem, StringUtils
```

---

## 依存関係

```
engine → physics → math
       → math
```

engine 内の namespace 間の依存方向 (逆転禁止):

```
scene    → renderer, physics, asset, audio  (AudioSystem free function 経由)
asset    → renderer
audio    → (独立)
renderer → math
core     → renderer, scene
util     → (独立)
input    → math (Vector2)
```

---

## Core

### Application

エンジンのシングルトン。`main.cpp` はループを自前で回し `IsRunning()` で継続を判定する。  
`Application::Run()` は存在するが sandbox では使わない方針。

```cpp
class Application {
public:
    static Application& Get();

    bool Init();
    void Shutdown();
    void Run();
    void Quit();

    bool                    IsRunning()       const;
    Window&                 GetWindow()       const;
    renderer::IRenderer&    GetRenderer()     const;
    scene::SceneManager&    GetSceneManager() const;
private:
    unique_ptr<Window>              m_window;
    unique_ptr<renderer::IRenderer> m_renderer;
    unique_ptr<scene::SceneManager> m_sceneManager;
};
```

### Window

Win32 ウィンドウの生成・管理。コールバック登録で DX11Renderer / ImGui と疎結合につなぐ。

```cpp
class Window {
public:
    struct Config { wstring title; uint32_t width; uint32_t height; };

    bool Initialize(const Config&);
    void Shutdown();
    void PollEvents();

    bool     ShouldClose() const;
    HWND     GetHandle()   const;
    uint32_t GetWidth()    const;
    uint32_t GetHeight()   const;

    // DX11Renderer が Resize 時に登録する
    void SetResizeCallback(function<void(uint32_t, uint32_t)>);
    // ImGui 等が WndProc を横断するために登録する。true を返すと後続処理をスキップ
    void SetWndProcHook(function<bool(HWND, UINT, WPARAM, LPARAM)>);
};
```

### Time

QPC を使ったフレームタイム管理。シングルスレッド専用。

```cpp
class Time {
public:
    static void     Tick();               // ゲームループ先頭で呼ぶ
    static float    DeltaTime();          // TimeScale 適用済み
    static float    UnscaledDeltaTime();  // UI・エフェクト用
    static float    TotalTime();
    static uint64_t FrameCount();
    static float    TimeScale();
    static void     SetTimeScale(float scale);  // 0=停止, 0.5=スロー, 1=通常
};
```

### Logger / ILogSink

```cpp
enum class LogLevel { INFO, WARNING, LOG_ERROR };

class Logger {
public:
    static void Info / Warn / Error(const char* fmt, ...);
    static void SetMinLevel(LogLevel);
    static void AddSink(ILogSink*);     // 非所有。呼び出し元がライフタイム管理
    static void RemoveSink(ILogSink*);
};

// マクロ (Release では ((void)0) に展開)
FBZZ_LOG_INFO(fmt, ...)
FBZZ_LOG_WARN(fmt, ...)
FBZZ_LOG_ERROR(fmt, ...)
```

```cpp
// ILogSink を継承して AddSink() に渡すとログを購読できる
class ILogSink {
public:
    virtual void OnLog(const core::LogEntry& entry) = 0;
};
```

### HResult

```cpp
#define FBZZ_HR_CHECK(hr)  // HRESULT 失敗時に assert() で停止
```

---

## Input

```cpp
namespace fbzz::input {

class Input {
public:
    static void Init();    // Window 生成後に一度呼ぶ
    static void Update();  // ゲームループ先頭で呼ぶ (前フレーム状態を保存)

    // キーボード
    static bool KeyDown(KeyCode key);   // 押した瞬間のみ true
    static bool KeyHeld(KeyCode key);
    static bool KeyUp  (KeyCode key);   // 離した瞬間のみ true

    // マウスボタン (0=左, 1=右, 2=中)
    static bool         MouseButton / MouseButtonDown / MouseButtonUp(int);
    static math::Vector2 MousePosition();   // クライアント座標
    static math::Vector2 MouseDelta();
    static float        MouseScrollDelta(); // 1 ノッチ = ±1.0
};

} // namespace fbzz::input
```

`KeyCode` は Win32 仮想キーコードのラッパー (`engine/Input/KeyCode.hpp`)。

---

## Renderer

詳細: [docs/renderer/Design.md](../renderer/Design.md)

### IRenderer — 全インターフェース

```cpp
// フレーム制御
BeginFrame() / EndFrame() / Clear(Vector4 color)

// リソース生成
CreateVertexBuffer(data, sizeBytes, stride)
CreateIndexBuffer(data, count)
CreateConstantBuffer(sizeBytes)
CreateShader(path)
CreateTexture(path)
CreatePipelineState(PipelineStateDesc)
CreateRenderTarget(w, h, colorCount=1)   // colorCount>1 で MRT
CreateComputeTexture(w, h)               // CS UAV 出力先 (SRV+UAV 両用)

// 描画
Submit(DrawCall)
Dispatch(ComputeCall)

// ウィンドウ
Resize(w, h) / GetWidth() / GetHeight()

// RT 制御
SetRenderTarget(rt)     // nullptr = バックバッファ
ClearDepth(depth=1.0f)  // 深度のみクリア (シャドウパス前に呼ぶ)

// サンプラー
SetSampler(slot, SamplerMode)

// ImGui 統合 (DX 世代差異を IRenderer に閉じ込める)
ImGuiInit(hwnd) / ImGuiShutdown()
ImGuiNewFrame() / ImGuiRenderDrawData()
GetImTextureID(rt, slot=0) → void*
```

具体実装: `engine/src/Renderer/Platform/DX11/DX11Renderer.cpp`

### Camera / DebugCamera

```cpp
class Camera {
    Vector3 m_position; Quaternion m_rotation;
    float m_fovY=60, m_aspect=16/9, m_near=0.1, m_far=1000;

    Matrix4 GetViewMatrix() / GetProjectionMatrix() / GetViewProjection() const;
    Vector3 GetForward() / GetRight() / GetUp() const;
    void LookAt(const Vector3& target);
};

// Unity Scene View 風カメラ制御ラッパー
class DebugCamera {
    Camera camera;
    float moveSpeed, fastMultiplier, mouseSens, panSensitivity, scrollSpeed;

    void LookAt(const Vector3& target);
    void Update(float dt);
    // RMB+WASD/QE : フライ移動 (Shift で高速)
    // 中ドラッグ   : パン
    // スクロール   : ドリー
    // Alt+左ドラッグ: オービット
};
```

### Mesh / Vertex

```cpp
struct Vertex {
    Vector3 position;
    Vector3 normal;
    Vector3 tangent;   // 法線マップ対応
    Vector2 uv;
};

struct Mesh {
    shared_ptr<IBuffer> vertexBuffer;
    shared_ptr<IBuffer> indexBuffer;
    uint32_t vertexCount, indexCount;
};

class PrimitiveMesh {
    static shared_ptr<Mesh> Cube  (IRenderer&);
    static shared_ptr<Mesh> Sphere(IRenderer&, int segments=16);
    static shared_ptr<Mesh> Plane (IRenderer&);
};
```

### Material

```cpp
struct MaterialParams {
    Vector4  albedo        = {1,1,1,1};
    float    metallic      = 0.0f;
    float    roughness     = 0.8f;
    float    emissiveScale = 0.0f;
    uint32_t textureMask   = 0;  // bit0=albedo bit1=normal bit2=metalRough bit3=emissive
};

class Material {
    shared_ptr<IShader>         shader;
    shared_ptr<ITexture>        albedoTexture;  // nullptr = 単色
    shared_ptr<ITexture>        normalTexture;  // nullptr = 法線マップなし
    shared_ptr<IConstantBuffer> paramsBuffer;
    MaterialParams              params;

    void Init(IRenderer&);  // paramsBuffer を生成する
    void Upload();           // params → GPU へ転送
};
```

### LightSystem (内部実装)

`LightSystem` クラスは **RenderSystem 内部専用**。ゲームコードから直接インスタンス化しない。  
ライト情報は `LightComponent` を持つ GameObject として Scene に置く（後述）。

RenderSystem が毎フレーム `View<Transform, LightComponent>()` で収集し、  
`LightConstantsCB` (560 bytes) を組み立てて GPU へ転送する。

### RenderSettings

```cpp
struct RenderSettings {
    bool  wireframeMode = false;
    bool  shadowEnabled = true;
    bool  bloomEnabled  = true;
    bool  fxaaEnabled   = true;
    bool  fogEnabled    = true;
    float exposure      = 1.0f;
    float fogDensity    = 0.06f;
    float fogFar        = 10.0f;
    float fogColor[3]   = { 0.01f, 0.01f, 0.04f };
};
```

---

## Scene

詳細: [docs/scene/Design.md](../scene/Design.md)

### EntityID / ComponentArray

```cpp
struct EntityID {
    uint32_t index = 0, generation = 0;
    bool IsValid() const;
    static const EntityID INVALID;
};

template<typename T>
class ComponentArray {  // スパースセット実装
    void Add(EntityID, T) / Remove(EntityID) / bool Has(EntityID) const;
    T& Get(EntityID);
    span<T>        Data();      // System がイテレートする
    span<EntityID> Entities();
    static constexpr uint32_t MAX = 4096;
};
```

### Transform

```cpp
struct Transform {
    // ローカル空間 (直接書き換え可)
    Vector3    localPosition; Quaternion localRotation; Vector3 localScale;
    // ワールド空間 (TransformSystem が毎フレーム更新)
    Vector3    position;      Quaternion rotation;

    Vector3 Forward() / Up() / Right() const;
    void Translate / Rotate / LookAt(...);
    Matrix4 GetWorldMatrix() const;
};
```

### GameObject

```cpp
class GameObject {
    string    name, tag;
    Transform transform;

    void SetActive(bool) / bool activeSelf() const;
    bool CompareTag(const string&) const;

    template<T> T&  AddComponent(T = {});
    template<T> T*  GetComponent();          // 非保持なら nullptr

    void SetParent(GameObject&) / GetParent / GetChild / GetChildCount;
    static void Destroy(GameObject&, float delay = 0.0f);
    bool IsValid() const / EntityID GetID() const;
};
```

### Scene

```cpp
class Scene {
    GameObject& CreateGameObject(const string& name = "GameObject");
    GameObject* Find(const string&) / FindWithTag(const string&);
    GameObjectRange GameObjects();                 // foreach 用 range
    template<Ts...> SceneView<Ts...> View();      // System 用マルチ Component イテレーター
    void FlushDestroyQueue(float dt);

    // ※ ComponentArray の追加は Scene.hpp に直接記述する
    ComponentArray<MeshRenderer>       m_meshRenderers;
    ComponentArray<RigidBodyComponent> m_rigidBodies;
    ComponentArray<ParticleEmitter>    m_particleEmitters;
    ComponentArray<SkyRenderer>        m_skyRenderers;
    ComponentArray<LightComponent>     m_lights;
    ComponentArray<CameraComponent>    m_cameras;
    ComponentArray<AudioSourceComponent> m_audioSources;
};
```

`GetEntities<T>()` で `ComponentArray<T>` の全 Entity を取得可能。

### SceneManager

```cpp
class SceneManager {
    void Register(const string& name, SceneFactory factory);
    void LoadScene(const string& name);   // 次フレーム先頭で切り替え
    void Update(float dt, physics::World& world);  // Physics + Transform + FlushDestroyQueue
    Scene* GetActive();
};
```

### Components

| Component | namespace | 役割 |
|-----------|-----------|------|
| `MeshRenderer` | `scene` | Mesh + Material 参照。`enabled` で描画 on/off |
| `RigidBodyComponent` | `scene` | `shared_ptr<physics::RigidBody>` を保持 |
| `ParticleEmitter` | `scene` | CPU パーティクル。位置・速度・色・サイズ・寿命パラメーター |
| `SkyRenderer` | `scene` | Rayleigh/Mie 散乱パラメーター (Skydome.hlsl へ渡す) |
| `LightComponent` | `scene` | Directional / Point / Spot を Type enum で統一。位置・方向は Transform から取得 |
| `CameraComponent` | `scene` | fovY / near / far。位置・向きは Transform から取得。isMain=true が RenderSystem の既定カメラ |
| `AudioSourceComponent` | `scene` | 再生クリップ・loop・volume を保持。AudioSystem free function が処理 |

```cpp
// LightComponent
struct LightComponent {
    enum class Type { Directional, Point, Spot };
    Type          type      = Type::Directional;
    math::Vector3 color     = { 1.0f, 1.0f, 1.0f };
    float         intensity = 1.0f;
    float         range     = 10.0f;   // Point / Spot のみ
    float         innerCone = 15.0f;   // Spot のみ (degrees)
    float         outerCone = 30.0f;   // Spot のみ (degrees)
    bool          enabled   = true;
    // Directional / Spot の方向 → Transform::Forward()
    // Point / Spot の位置      → Transform::position
};

// CameraComponent
struct CameraComponent {
    float fovY    = 60.0f;
    float nearZ   = 0.1f;
    float farZ    = 1000.0f;
    bool  isMain  = true;   // 複数カメラ時: 最初に見つかった isMain=true を使用
    bool  enabled = true;
    // 位置・向き → Transform から取得
};

// AudioSourceComponent
struct AudioSourceComponent {
    std::string clipPath;
    bool        playOnAwake = false;
    bool        loop        = false;
    float       volume      = 1.0f;
    bool        enabled     = true;
};
```

### Systems (free functions)

```cpp
// TransformSystem: 親子階層のワールド行列・position・rotation を再計算
void TransformSystem(Scene& scene);

// PhysicsSystem: RigidBodyComponent ↔ physics::World を同期
void PhysicsSystem(Scene& scene, physics::World& world, float dt);

// RenderSystem: MeshRenderer を走査しマルチパス描画を発行
//   ライト情報は内部で View<Transform, LightComponent>() から収集する
//   camera: エディターカメラ (DebugCamera) または CameraComponent から生成した Camera
void RenderSystem(Scene& scene,
                  IRenderer& renderer,
                  const Camera& camera,
                  const shared_ptr<IRenderTarget>& outputRT = nullptr,
                  const RenderSettings* settings = nullptr);

// AudioSystem: AudioSourceComponent を走査して再生・停止を処理する (scene → audio 橋渡し)
void AudioSystem(Scene& scene, audio::AudioSystem& audioSystem, float dt);
```

**RenderSystem 内部パス:**

```
Pass 0  LightComponent 収集 → LightConstantsCB を組み立て
Pass 1  Shadow Map     → shadowMapRT (depth only)
Pass 2  HDR Forward    → hdrRT       (opaque meshes + SkyRenderer)
Pass 3  Bloom CS       → bloomHalf → bloomFull
Pass 4  Composite      → ldrRT       (ToneMap + Bloom + Fog)
Pass 5  FXAA           → outputRT
```

`settings` が nullptr の場合はデフォルト `RenderSettings` (全パス有効) で動作する。

---

## Asset

```cpp
namespace fbzz::asset {

class AssetManager {
    static void Init(IRenderer&, const string& basePath = "assets/");
    static void UnloadAll();

    // 対応型: Model, renderer::ITexture
    template<T> static shared_ptr<T> Load(const string& relativePath);
    template<T> static void          Unload(const string& relativePath);
};

struct Model {
    vector<shared_ptr<renderer::Mesh>>     meshes;
    vector<shared_ptr<renderer::Material>> materials;
};

class ModelImporter {
    // Assimp を使用して .fbx / .obj をロード
    static bool Import(IRenderer&, const string& path, Model& out);
};

} // namespace fbzz::asset
```

---

## Audio

```cpp
namespace fbzz::audio {

class IAudioDevice { /* XAudio2 抽象 */ };

class AudioSystem {
public:
    explicit AudioSystem(IAudioDevice& device);
    bool Init();
    void Shutdown();

    // 高レベル BGM / SE API (ゲームループから直接呼ぶ場合)
    void PlayBGM(const string& path, bool loop = true);
    void StopBGM();
    void PlaySE(const string& path);
    void SetBGMVolume(float v) / SetSEVolume(float v);
};

} // namespace fbzz::audio
```

`AudioSourceComponent` の処理は `scene::AudioSystem(Scene&, audio::AudioSystem&, float dt)` が担う。  
直接 BGM/SE を鳴らしたい場合は `audio::AudioSystem` を直接呼んでよい。

---

## Util

```cpp
namespace fbzz::util {

class FileSystem {
    static bool   Exists / IsDirectory(const string&);
    static string GetExtension / GetFilename / GetDirectory(const string&);
    static vector<string> ListFiles(const string& dir, const string& ext = "");
    static bool   EnsureDirectory(const string& path);
    static bool   ReadText(const string& path, string& out);
    static bool   WriteText(const string& path, const string& text);
};

class StringUtils {
    static bool   Contains(const string&, const string&);
    static bool   ContainsCI(const string&, const string&);  // 大文字小文字無視
    static string ToLower / ToUpper(const string&);
    static bool   StartsWith / EndsWith(const string&, const string&);
    static vector<string> Split(const string&, char delim);
    static string Trim(const string&);
    static wstring ToWide(const string&);    // Win32 API 用
    static string  ToNarrow(const wstring&);
};

} // namespace fbzz::util
```

---

## 標準ゲームループ接続パターン

```cpp
// ---- 初期化 ----
auto& app      = core::Application::Get();
auto& window   = app.GetWindow();
auto& renderer = app.GetRenderer();
auto& sm       = app.GetSceneManager();

input::Input::Init();
asset::AssetManager::Init(renderer);

editor::EditorApp editorApp;
editorApp.Init(renderer, window);
auto& ctx = editorApp.GetContext();

renderer::DebugCamera debugCam;
physics::World        physWorld;

// シーン内に LightComponent / CameraComponent を持つ GameObject を作成する
sm.Register("Main", []{ return make_unique<MainScene>(); });
sm.LoadScene("Main");

ctx.activeScene  = sm.GetActive();
ctx.editorCamera = &debugCam.camera;

// ---- メインループ ----
while (app.IsRunning() && !window.ShouldClose())
{
    core::Time::Tick();
    input::Input::Update();
    window.PollEvents();

    float dt = core::Time::DeltaTime();
    debugCam.Update(dt);
    sm.Update(dt, physWorld);

    renderer.BeginFrame();
    renderer.Clear({ 0.005f, 0.005f, 0.02f, 1.0f });

    auto vpRT = editorApp.GetViewportRT();
    scene::RenderSystem(*sm.GetActive(), renderer,
                        debugCam.camera, vpRT,
                        &ctx.renderSettings);
    // scene::AudioSystem(*sm.GetActive(), audioSystem, dt);  // Audio 使用時

    editorApp.BeginFrame();
    editorApp.RenderPanels(ctx);
    editorApp.EndFrame(renderer);

    renderer.EndFrame();
}

// ---- シャットダウン ----
editorApp.Shutdown();
asset::AssetManager::UnloadAll();
app.Shutdown();
```

---

## 実装状態

| モジュール | 状態 |
|-----------|------|
| Core (Application / Window / Time / Logger / HResult) | **完了** |
| Input | **完了** |
| Renderer インターフェース + DX11 実装 | **完了** |
| Renderer (Mesh / Material / RenderSettings) | **完了** |
| Renderer (RenderSystem マルチパス) | **完了** |
| Renderer (DebugCamera) | **完了** |
| Scene 基盤 (EntityID / ComponentArray / Transform / Scene / SceneManager) | **完了** |
| Scene Components (MeshRenderer / RigidBodyComponent / ParticleEmitter / SkyRenderer) | **完了** |
| Scene Components (LightComponent / CameraComponent / AudioSourceComponent) | **未実装** |
| Scene Systems (TransformSystem / PhysicsSystem / RenderSystem) | **完了** |
| Scene Systems (AudioSystem free function) | **未実装** |
| RenderSystem: LightSystem 引数削除 → LightComponent 収集に移行 | **未実装** |
| Asset (AssetManager / ModelImporter) | **完了** |
| Audio (AudioSystem / XAudio2Device) | **完了** |
| Util (FileSystem / StringUtils) | **完了** |
| Renderer DX12 移行 | 未着手 (Step 7) |

---

## サブドキュメント

| ドキュメント | 対象 |
|-------------|------|
| [docs/renderer/Design.md](../renderer/Design.md) | IRenderer / Mesh / Material / RenderSystem 詳細 |
| [docs/scene/Design.md](../scene/Design.md) | EntityID / ComponentArray / Scene / SceneManager 詳細 |
| [docs/editor/Design.md](../editor/Design.md) | EditorApp / EditorContext / Panels 詳細 |
| [docs/shaders/Design.md](../shaders/Design.md) | HLSL ライブラリ構成・cbuffer レイアウト |
| [docs/physics/Design.md](../physics/Design.md) | physics モジュール詳細 |
| [docs/asset/Design.md](../asset/Design.md) | Asset モジュール詳細 |
| [docs/audio/Design.md](../audio/Design.md) | Audio モジュール詳細 |
