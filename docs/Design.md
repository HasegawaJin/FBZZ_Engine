# FBZZ Engine 設計書

C++20 自作 3D ゲームエンジン。数学・物理エンジンを自作し、DirectX 11 レンダラーを抽象インターフェースで隠蔽する。  
就活ポートフォリオ目的。設計を重視し、Step 単位で段階的に実装する。

---

## ロードマップ

| Step | 内容 | 状態 |
|------|------|------|
| 0 | ビルド環境・Application ループ | **完了** |
| 1 | Win32 ウィンドウ表示 + DX11 初期化 | 未着手 |
| 2 | 三角形描画 (頂点バッファ・シェーダー) | 未着手 |
| 3 | デバッグ描画 (DebugDraw) | 未着手 |
| 4 | 物理エンジン (重力・衝突) | 未着手 |
| 5 | シーン管理 (GameObject / Component) | 未着手 |
| 6 | DX12 / レイトレーシング移行 | 未着手 |

---

## モジュール構成

```
FBZZ_Engine/
├── math/          独立ライブラリ。依存なし
├── physics/       math をリンク
├── engine/        physics / math をリンク
├── sandbox/       動作確認・サンプル。engine をリンク
├── third_party/   Assimp / DirectXTex
├── cmake/         コンパイラオプション等
└── docs/          設計ドキュメント (このファイル)
```

依存方向 (逆転禁止):

```
sandbox → engine → physics → math
```

---

## ディレクトリ構成

### 実装済み (Step 0 完了)

```
engine/
├── include/engine/
│   ├── Core/
│   │   ├── Application.hpp     シングルトン、ゲームループ
│   │   ├── Window.hpp          Win32 ウィンドウ、メッセージポンプ
│   │   ├── Logger.hpp          ログレベル、FBZZ_LOG_* マクロ
│   │   ├── Time.hpp            DeltaTime / TotalTime / TimeScale
│   │   └── HResult.hpp         FBZZ_HR_CHECK マクロ
│   ├── Input/
│   │   ├── Input.hpp           キーボード・マウス状態管理
│   │   └── KeyCode.hpp         KEY_* 定数
│   └── Renderer/
│       ├── IRenderer.hpp       レンダラー抽象インターフェース
│       ├── IBuffer.hpp         頂点・インデックスバッファ
│       ├── IShader.hpp         頂点・ピクセルシェーダーペア
│       ├── IConstantBuffer.hpp 定数バッファ
│       ├── ITexture.hpp        2D テクスチャ
│       ├── IPipelineState.hpp  ラスタライザ・ブレンド・深度ステート
│       ├── IRenderTarget.hpp   オフスクリーン RT
│       ├── DrawCall.hpp        描画命令 (自己完結)
│       ├── RenderLayer.hpp     OPAQUE / TRANSPARENT / DEBUG
│       ├── RenderState.hpp     PipelineStateDesc
│       └── SamplerMode.hpp     WRAP_LINEAR / WRAP_POINT / CLAMP_LINEAR
└── src/
    ├── Core/
    │   ├── Application.cpp
    │   ├── Window.cpp
    │   ├── Logger.cpp
    │   └── Time.cpp
    ├── Input/
    │   └── Input.cpp
    └── Renderer/
        └── Platform/
            └── DX11/           (内部実装。上位レイヤーは参照しない)
                ├── DX11Renderer.hpp/.cpp
                ├── DX11Buffer.hpp/.cpp
                ├── DX11Shader.hpp/.cpp
                ├── DX11ConstantBuffer.hpp/.cpp
                ├── DX11PipelineState.hpp/.cpp
                ├── DX11RenderTarget.hpp/.cpp
                └── DX11Texture.hpp/.cpp

math/
├── include/math/
│   ├── Math.hpp        全ヘッダのまとめ include
│   ├── MathUtils.hpp   PI, DEG2RAD, Clamp, Lerp 等スカラー関数
│   ├── Vector2.hpp
│   ├── Vector3.hpp
│   ├── Vector4.hpp
│   ├── Matrix3.hpp
│   ├── Matrix4.hpp
│   └── Quaternion.hpp
└── src/
    ├── Vector2.cpp ... Vector4.cpp
    ├── Matrix3.cpp, Matrix4.cpp
    └── Quaternion.cpp

physics/
└── src/
    └── physics.cpp     (Step 4 まではスタブ)

sandbox/
└── src/
    └── main.cpp
```

### Step 1 で追加予定

```
engine/
└── src/
    └── Renderer/
        └── Platform/
            └── DX11/
                └── DX11Renderer.cpp  (Init を実装: デバイス・スワップチェーン生成)
```

### Step 2 以降で追加予定

```
engine/
├── include/engine/
│   ├── Renderer/
│   │   ├── Camera.hpp          ビュー・プロジェクション行列
│   │   ├── Light.hpp           DirectionalLight / AmbientLight / LightSystem
│   │   ├── Material.hpp        テクスチャ・シェーダーのセット
│   │   └── RenderQueue.hpp     RenderLayer 別ソート・Flush
│   ├── Scene/
│   │   ├── Scene.hpp           GameObject コンテナ
│   │   ├── GameObject.hpp      名前・Transform・Component リスト
│   │   ├── Component.hpp       Component 基底クラス
│   │   └── Transform.hpp       position / rotation / scale
│   └── Mesh/
│       ├── Mesh.hpp            Vertex フォーマット、サブメッシュ
│       └── MeshLoader.hpp      Assimp ラッパー
└── src/
    ├── Renderer/
    │   ├── Camera.cpp
    │   ├── Light.cpp
    │   ├── RenderQueue.cpp
    │   └── Platform/DX11/
    │       └── (既存ファイル拡張)
    ├── Scene/
    │   ├── Scene.cpp
    │   ├── GameObject.cpp
    │   └── Transform.cpp
    └── Mesh/
        ├── Mesh.cpp
        └── MeshLoader.cpp

physics/
├── include/physics/
│   ├── World.hpp
│   ├── RigidBody.hpp
│   ├── Collider.hpp
│   └── Solver.hpp
└── src/
    ├── World.cpp
    ├── RigidBody.cpp
    ├── Collider.cpp
    └── Solver.cpp
```

---

## アーキテクチャ原則

| 原則 | 内容 |
|------|------|
| レンダラー抽象化 | 上位レイヤーは `IRenderer&` のみ参照。`DX11Renderer*` へのダウンキャスト禁止 |
| ファクトリー集約 | `Application::Run()` 内でのみ `DX11Renderer` を `make_unique` し `IRenderer` に格納 |
| シーン管理 | Unity 同様の GameObject / Component パターン (OOP) |
| 数学・物理 | 自作のみ。GLM / Bullet / PhysX 禁止 |
| スレッド | Step 1〜5 はシングルスレッド。`std::thread` / `std::mutex` を engine/physics/math に持ち込まない |

---

## 命名規則

| 対象 | 規則 | 例 |
|------|------|-----|
| クラス名 | `PascalCase` | `RigidBody`, `ShaderManager` |
| 関数名 | `PascalCase` | `ApplyForce()`, `GetComponent()` |
| 変数名 | `lowerCamelCase` | `deltaTime`, `vertexCount` |
| メンバ変数 | `m_` + `lowerCamelCase` | `m_position`, `m_isStatic` |
| 定数 / enum | `UPPER_SNAKE_CASE` | `MAX_LIGHTS`, `KEY_ESCAPE` |
| インターフェース | `I` + `PascalCase` | `IRenderer`, `IBuffer` |
| 名前空間 | `snake_case` | `fbzz::physics`, `fbzz::renderer` |
| ファイル | `PascalCase` | `RigidBody.cpp`, `IRenderer.hpp` |
| ディレクトリ | `PascalCase` | `Renderer/`, `Core/`, `DX11/` |

### ファイルヘッダーコメント

すべての `.hpp` / `.cpp` の先頭、`#pragma once` の直前に記述する。

```cpp
// FBZZ Engine
// Vector3.hpp | fbzz::math
// 3次元ベクトルの演算と定数
```

---

## 所有権モデル

詳細: [docs/conventions/ownership.md](conventions/ownership.md)

| スマートポインタ | 使う場面 |
|----------------|---------|
| `std::unique_ptr` | デフォルト。1 箇所が所有するリソース (Component, GameObject 等) |
| `std::shared_ptr` | 複数システム共有リソース (IBuffer, IShader, RigidBody) |
| raw pointer (`T*`) | 非所有の参照のみ (m_owner, m_parent 等) |

`new` / `delete` 直接使用禁止。`make_unique` / `make_shared` を使う。

---

## エラーハンドリング

詳細: [docs/conventions/error_handling.md](conventions/error_handling.md)

| 状況 | 手段 |
|------|------|
| 回復不可能エラー | `assert()` |
| 回復可能エラー | `bool` 戻り値 |
| DX11 HRESULT | `FBZZ_HR_CHECK(hr)` マクロ |
| 例外 | `throw` / `std::exception` 禁止 |

---

## 禁止パターン

| パターン | 理由 |
|---------|------|
| `dynamic_cast` | RTTI コスト。`GetComponent<T>()` で代替 |
| `reinterpret_cast` | シェーダー定数バッファ転送以外で禁止 |
| グローバル変数 | `Application` シングルトン以外禁止 |
| `new` / `delete` 直接使用 | `make_unique` / `make_shared` を使う |
| `throw` / `std::exception` | エンジンコード内では禁止 |
| `#include` 循環依存 | 前方宣言 (`class Foo;`) で解決 |
| `DX11Renderer*` ダウンキャスト | 上位レイヤーは `IRenderer&` のみ参照 |
| GLM / Bullet / PhysX / Box2D | 数学・物理は自作 |

---

## サードパーティライブラリ

| ライブラリ | 用途 | 導入 Step |
|-----------|------|----------|
| DirectX 11 SDK | レンダリング | Step 1 |
| DirectXTex | テクスチャ読み込み (PNG/JPG/DDS) | Step 2 |
| Assimp | メッシュ読み込み | Step 5 |
| DirectX 12 SDK | レイトレーシング | Step 6 |
| `Microsoft::WRL::ComPtr` | COM リソース管理 | Step 1 |

---

## Math モジュール (`fbzz::math`)

### Vector2

```cpp
struct Vector2 {
    float x, y;

    Vector2  operator+(const Vector2&) const;
    Vector2  operator-(const Vector2&) const;
    Vector2  operator*(float) const;
    Vector2  operator/(float) const;
    float    Length() const;
    float    LengthSq() const;
    Vector2  Normalized() const;

    static float  Dot(const Vector2& a, const Vector2& b);
    static Vector2 Lerp(const Vector2& a, const Vector2& b, float t);

    static const Vector2 ZERO, ONE;
};
```

### Vector3

```cpp
struct Vector3 {
    float x, y, z;

    Vector3  operator+(const Vector3&) const;
    Vector3  operator-(const Vector3&) const;
    Vector3  operator*(float) const;
    Vector3  operator/(float) const;
    Vector3  operator-() const;
    float    Length() const;
    float    LengthSq() const;
    Vector3  Normalized() const;

    static float   Dot(const Vector3& a, const Vector3& b);
    static Vector3 Cross(const Vector3& a, const Vector3& b);
    static Vector3 Lerp(const Vector3& a, const Vector3& b, float t);

    static const Vector3 ZERO, ONE, UP, RIGHT, FORWARD;
};
```

### Vector4

```cpp
struct Vector4 {
    float x, y, z, w;
    // Vector3 同等の演算子 + w 成分
    static float Dot(const Vector4& a, const Vector4& b);
    static const Vector4 ZERO, ONE;
};
```

### Matrix4

Row-major 格納。HLSL へ転送時に transpose する。

```cpp
struct Matrix4 {
    float m[4][4];

    Matrix4 operator*(const Matrix4&) const;
    Vector3 TransformPoint(const Vector3&)     const; // w=1
    Vector3 TransformDirection(const Vector3&) const; // w=0
    Matrix4 Transposed() const;

    static Matrix4 Identity();
    static Matrix4 Translation(const Vector3&);
    static Matrix4 Scale(const Vector3&);
    static Matrix4 RotationX(float rad);
    static Matrix4 RotationY(float rad);
    static Matrix4 RotationZ(float rad);
    static Matrix4 FromQuaternion(const Quaternion&);
    static Matrix4 LookAt(const Vector3& eye, const Vector3& target, const Vector3& up);
    static Matrix4 PerspectiveFovLH(float fovY, float aspect, float zNear, float zFar);
    static Matrix4 OrthographicLH(float w, float h, float zNear, float zFar);
};
```

### Quaternion

```cpp
struct Quaternion {
    float x, y, z, w;

    Quaternion  operator*(const Quaternion&) const;
    Quaternion  Conjugate()   const;
    Quaternion  Normalized()  const;
    Vector3     RotateVector(const Vector3&) const;

    static Quaternion Identity();
    static Quaternion FromAxisAngle(const Vector3& axis, float rad);
    static Quaternion FromEuler(float pitchRad, float yawRad, float rollRad);
    static Quaternion Slerp(const Quaternion& a, const Quaternion& b, float t);
};
```

### MathUtils

```cpp
namespace fbzz::math {
    constexpr float PI       = 3.14159265358979f;
    constexpr float DEG2RAD  = PI / 180.0f;
    constexpr float RAD2DEG  = 180.0f / PI;

    float Clamp(float v, float lo, float hi);
    float Lerp(float a, float b, float t);
    float Abs(float v);
    float Sqrt(float v);
}
```

---

## Physics モジュール (`fbzz::physics`) — Step 4 で実装

### World

```cpp
class World {
public:
    void AddRigidBody(std::shared_ptr<RigidBody> body);
    void RemoveRigidBody(std::shared_ptr<RigidBody> body);
    void Step(float dt);  // 重力適用 → 積分 → 衝突検出 → 衝突解決

    void SetGravity(const math::Vector3& g);
};
```

### RigidBody

半陰的オイラー積分 (速度を先に更新してから位置を更新)。

```cpp
class RigidBody {
public:
    void ApplyForce(const math::Vector3& force);
    void ApplyImpulse(const math::Vector3& impulse);

    math::Vector3 position;
    math::Vector3 velocity;
    math::Vector3 force;
    float         mass       = 1.0f;
    float         restitution = 0.5f;  // 反発係数
    bool          isStatic   = false;
};
```

### Collider (Shape Union)

```cpp
enum class ColliderType { SPHERE, AABB, CAPSULE };

struct SphereCollider  { float radius; };
struct AABBCollider    { math::Vector3 halfExtents; };
struct CapsuleCollider { float radius; float halfHeight; };

struct Collider {
    ColliderType type;
    union { SphereCollider sphere; AABBCollider aabb; CapsuleCollider capsule; };
    math::Vector3 offset;  // RigidBody 位置からのローカルオフセット
};
```

### Solver

```cpp
struct ContactPoint {
    math::Vector3 normal;
    float         penetration;
    RigidBody*    bodyA;
    RigidBody*    bodyB;
};

class Solver {
public:
    // Broad フェーズ (AABB テスト) → Narrow フェーズ → ContactPoint 生成
    void DetectCollisions(std::vector<std::shared_ptr<RigidBody>>& bodies,
                          std::vector<ContactPoint>& contacts);

    // インパルスベース衝突解決
    void Resolve(std::vector<ContactPoint>& contacts);
};
```

---

## Engine モジュール (`fbzz::core` / `fbzz::renderer`)

### Application (Step 0 完了)

シングルトン。`Run()` がゲームループを駆動する。

```cpp
class Application {
public:
    static Application& Get();

    void Run();
    void Quit();

    bool              IsRunning()   const;
    renderer::IRenderer& GetRenderer() const;

private:
    bool                              m_isRunning = true;
    std::unique_ptr<Window>           m_window;
    std::unique_ptr<renderer::IRenderer> m_renderer;
};
```

ゲームループ:

```
while (m_isRunning) {
    window.PollEvents()
    if (window.ShouldClose()) Quit()
    Time::Update()
    Input::Update()
    // [Step 2〜] scene.Update(dt)
    renderer.BeginFrame()
    renderer.Clear(clearColor)
    // [Step 2〜] scene.Render(renderer)
    renderer.EndFrame()
}
```

### Window (Step 0 完了)

```cpp
class Window {
public:
    struct Config { std::wstring title; uint32_t width; uint32_t height; };

    bool Initialize(const Config& config);
    void Shutdown();
    void PollEvents();

    bool     ShouldClose() const;
    HWND     GetHandle()   const;
    uint32_t GetWidth()    const;
    uint32_t GetHeight()   const;

    using ResizeCallback = std::function<void(uint32_t, uint32_t)>;
    void SetResizeCallback(ResizeCallback cb);
};
```

### Logger (Step 0 完了)

```cpp
enum class LogLevel { DEBUG, INFO, WARN, ERROR };

class Logger {
public:
    static void Log(LogLevel level, const char* file, int line, const char* fmt, ...);
};

#define FBZZ_LOG_DEBUG(fmt, ...) Logger::Log(LogLevel::DEBUG, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define FBZZ_LOG_INFO(fmt, ...)  Logger::Log(LogLevel::INFO,  __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define FBZZ_LOG_WARN(fmt, ...)  Logger::Log(LogLevel::WARN,  __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define FBZZ_LOG_ERROR(fmt, ...) Logger::Log(LogLevel::ERROR, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
```

### Time (Step 0 完了)

```cpp
class Time {
public:
    static void   Update();
    static float  DeltaTime();
    static float  TotalTime();
    static float  TimeScale();
    static void   SetTimeScale(float scale);
};
```

### Input (Step 0 完了)

```cpp
class Input {
public:
    static void Update();

    static bool IsKeyDown(KeyCode key);    // 押しっぱなし
    static bool IsKeyPressed(KeyCode key); // 押した瞬間
    static bool IsKeyReleased(KeyCode key);// 離した瞬間

    static bool  IsMouseButtonDown(int button); // 0=左 1=右 2=中
    static float GetMouseX();
    static float GetMouseY();
    static float GetMouseDeltaX();
    static float GetMouseDeltaY();
};
```

### Scene / GameObject / Component — Step 5 で実装

```cpp
class Component {
public:
    virtual ~Component() = default;
    virtual void Update(float dt) {}
    virtual void Render(renderer::IRenderer& renderer) {}
    GameObject* GetOwner() const { return m_owner; }
private:
    GameObject* m_owner = nullptr;  // 非所有参照
};

class GameObject {
public:
    std::string name;
    Transform   transform;

    template<typename T, typename... Args>
    T* AddComponent(Args&&... args);

    template<typename T>
    T* GetComponent();  // dynamic_cast 不使用、type_index で検索

    void Update(float dt);
    void Render(renderer::IRenderer& renderer);

private:
    std::vector<std::unique_ptr<Component>> m_components;
};

class Scene {
public:
    GameObject* CreateGameObject(const std::string& name);
    void        DestroyGameObject(GameObject* obj);
    void        Update(float dt);
    void        Render(renderer::IRenderer& renderer);
private:
    std::vector<std::unique_ptr<GameObject>> m_objects;
};
```

---

## Renderer モジュール (`fbzz::renderer`)

### IRenderer (Step 0 完了 / Step 1 で初期化)

```cpp
class IRenderer {
public:
    virtual ~IRenderer() = default;

    virtual void BeginFrame() = 0;
    virtual void EndFrame()   = 0;
    virtual void Clear(const math::Vector4& color) = 0;

    virtual std::shared_ptr<IBuffer>         CreateVertexBuffer(const void*, size_t, uint32_t stride) = 0;
    virtual std::shared_ptr<IBuffer>         CreateIndexBuffer(const void*, uint32_t count) = 0;
    virtual std::shared_ptr<IConstantBuffer> CreateConstantBuffer(size_t sizeBytes) = 0;
    virtual std::shared_ptr<IShader>         CreateShader(const std::string& path) = 0;
    virtual std::shared_ptr<ITexture>        CreateTexture(const std::string& path) = 0;
    virtual std::shared_ptr<IPipelineState>  CreatePipelineState(const PipelineStateDesc&) = 0;
    virtual std::shared_ptr<IRenderTarget>   CreateRenderTarget(uint32_t w, uint32_t h) = 0;

    virtual void Submit(const DrawCall& call) = 0;
    virtual void Resize(uint32_t width, uint32_t height) = 0;
    virtual void SetRenderTarget(std::shared_ptr<IRenderTarget> rt) = 0; // nullptr = バックバッファ
    virtual void SetSampler(uint32_t slot, SamplerMode mode) = 0;
};
```

### DrawCall (Step 0 完了)

`Submit()` に渡す自己完結な描画命令。

```cpp
struct DrawCall {
    std::shared_ptr<IBuffer>        vertexBuffer;
    std::shared_ptr<IBuffer>        indexBuffer;   // nullptr = 非インデックス描画
    std::shared_ptr<IShader>        shader;
    std::shared_ptr<IPipelineState> pipelineState;

    std::array<std::shared_ptr<IConstantBuffer>, 4> constantBuffers = {};
    std::array<std::shared_ptr<ITexture>, 8>        textures        = {};

    uint32_t    indexCount  = 0;
    uint32_t    vertexCount = 0;
    uint32_t    startIndex  = 0;
    uint32_t    baseVertex  = 0;
    RenderLayer layer       = RenderLayer::OPAQUE;
};
```

### RenderLayer

```cpp
enum class RenderLayer : uint8_t {
    OPAQUE      = 0,
    TRANSPARENT = 1,
    DEBUG       = 2,
};
```

### SamplerMode

```cpp
enum class SamplerMode : uint8_t {
    WRAP_LINEAR  = 0,  // 通常テクスチャ
    WRAP_POINT   = 1,  // ピクセルアート
    CLAMP_LINEAR = 2,  // UI / スプライト
};
```

### PipelineStateDesc

```cpp
enum class RasterizerMode { SOLID, WIREFRAME, SOLID_NO_CULL };
enum class BlendMode      { OPAQUE, ALPHA, ADDITIVE };
enum class DepthMode      { READ_WRITE, READ_ONLY, DISABLED };

struct PipelineStateDesc {
    RasterizerMode rasterizer = RasterizerMode::SOLID;
    BlendMode      blend      = BlendMode::OPAQUE;
    DepthMode      depth      = DepthMode::READ_WRITE;
};
```

### DX11Renderer 実装 (Step 1 で初期化完成)

`Application.cpp` のみが `make_unique<DX11Renderer>()` し、`IRenderer` として保持する。  
他のクラスは `DX11Renderer` を知らない。

```cpp
class DX11Renderer : public IRenderer {
public:
    bool Init(HWND hwnd, uint32_t width, uint32_t height);
    void Shutdown();
    // IRenderer の全 virtual を override
private:
    ComPtr<ID3D11Device>           m_device;
    ComPtr<ID3D11DeviceContext>    m_context;
    ComPtr<IDXGISwapChain>         m_swapChain;
    ComPtr<ID3D11RenderTargetView> m_renderTargetView;
    ComPtr<ID3D11DepthStencilView> m_depthStencilView;
    ComPtr<ID3D11Texture2D>        m_depthStencilBuffer;
    ComPtr<ID3D11SamplerState>     m_samplers[3]; // SamplerMode に対応
    uint32_t m_width, m_height;
};
```

### Camera — Step 2 で実装

```cpp
class Camera {
public:
    math::Matrix4 GetViewMatrix()       const;
    math::Matrix4 GetProjectionMatrix() const;

    math::Vector3 position = math::Vector3::ZERO;
    math::Quaternion rotation = math::Quaternion::Identity();
    float fovY   = 60.0f;
    float aspect = 16.0f / 9.0f;
    float zNear  = 0.1f;
    float zFar   = 1000.0f;
};
```

### RenderQueue — Step 2 で実装

```cpp
class RenderQueue {
public:
    void Push(const DrawCall& call);
    void Flush(IRenderer& renderer);  // RenderLayer 順にソートして Submit
    void Clear();
};
```

### Light — Step 2 で実装

```cpp
struct DirectionalLight {
    math::Vector3 direction;
    math::Vector3 color;
    float         intensity;
};

struct AmbientLight {
    math::Vector3 color;
    float         intensity;
};

class LightSystem {
public:
    void SetDirectional(const DirectionalLight& light);
    void SetAmbient(const AmbientLight& light);
    void UploadToConstantBuffer(IConstantBuffer& cb) const;
};
```

### DebugDraw — Step 3 で実装

```cpp
class DebugDraw {
public:
    static void Line(IRenderer& r, const math::Vector3& from, const math::Vector3& to,
                     const math::Vector4& color = {1,1,1,1});
    static void Box(IRenderer& r, const math::Vector3& center, const math::Vector3& halfExtents,
                    const math::Vector4& color = {0,1,0,1});
    static void Sphere(IRenderer& r, const math::Vector3& center, float radius,
                       const math::Vector4& color = {0,1,0,1});
    static void Capsule(IRenderer& r, const math::Vector3& center, float radius, float halfHeight,
                        const math::Vector4& color = {0,1,0,1});
};
```

---

## スレッドモデル

詳細: [docs/conventions/threading.md](conventions/threading.md)

Step 1〜5 はシングルスレッド。`std::thread` / `std::mutex` / `std::atomic` を engine / physics / math に持ち込まない。

---

## Git 運用

詳細: [docs/conventions/git.md](conventions/git.md)

```
main → develop → feature/<name>
```

コミット形式: `[Feature] / [Fix] / [Design] / [Build] / [Refactor] + 動詞 + 概要`

例:
- `[Feature] Win32 ウィンドウ初期化を実装`
- `[Fix] DX11 デバイス生成時の HRESULT チェック漏れを修正`
- `[Design] Renderer インターフェース設計を更新`
