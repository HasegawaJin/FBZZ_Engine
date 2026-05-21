# Scene System 設計書

`fbzz::scene` — GameObject / Component / System による シーン管理。  
Unity ライクな OOP API と、データ指向の高速 System イテレーションを両立する。

---

## 依存関係

```
fbzz::scene
├── fbzz::math     (Vector3, Quaternion, Matrix4)
├── fbzz::physics  (World, RigidBody)
├── fbzz::renderer (IRenderer, Camera, Mesh, Material, RenderSettings)
├── fbzz::audio    (AudioSystem — AudioSystem free function 経由)
└── fbzz::core     (Application, Time)
```

---

## EntityID

内部識別子。`ComponentArray` と `Scene` が直接扱う。ゲームコードは `GameObject` を使う。

```cpp
struct EntityID {
    uint32_t index      = 0;
    uint32_t generation = 0;

    bool IsValid() const;
    bool operator==(const EntityID&) const = default;

    static const EntityID INVALID;
};
```

`generation` は Entity 破棄時にインクリメントする。再利用された index に対して古い EntityID を使っていないか検出できる。

---

## ComponentArray\<T\>

EntityID → Component のスパースセット。dense 配列で連続メモリを確保し、キャッシュ効率を保つ。

```cpp
template<typename T>
class ComponentArray {
public:
    void     Add(EntityID id, T component);
    void     Remove(EntityID id);
    bool     Has(EntityID id) const;
    T&       Get(EntityID id);
    const T& Get(EntityID id) const;

    std::span<T>        Data();      // System がイテレートする
    std::span<EntityID> Entities();  // dense index → EntityID

    uint32_t Count() const;

private:
    static constexpr uint32_t MAX = 4096;
    T        m_dense[MAX];
    EntityID m_denseToEntity[MAX];
    uint32_t m_sparseToIndex[MAX];  // EntityID.index → dense index
    uint32_t m_count = 0;
};
```

### スパースセットの構造

```
sparse[EntityID.index] → dense index
dense[dense index]     → Component T
denseToEntity[dense index] → EntityID  (Remove 時に使う)
```

Remove は dense 末尾の要素を削除位置に swap して O(1) で完了する。

---

## Transform

全 `GameObject` が必ず持つ特別な Component。`ComponentArray` には入れず `GameObject` 内に直接格納する。

```cpp
struct Transform {
    // ローカル空間 (直接書き換え可)
    math::Vector3    localPosition = math::Vector3::ZERO;
    math::Quaternion localRotation = math::Quaternion::Identity();
    math::Vector3    localScale    = math::Vector3::ONE;

    // ワールド空間 (TransformSystem が毎フレーム更新。直接書き換え不可)
    math::Vector3    position = math::Vector3::ZERO;
    math::Quaternion rotation = math::Quaternion::Identity();

    // 方向ベクトル (Unity: transform.forward / up / right)
    math::Vector3 Forward() const;
    math::Vector3 Up()      const;
    math::Vector3 Right()   const;

    // Unity: transform.Translate / Rotate / LookAt
    void Translate(const math::Vector3& delta, bool worldSpace = false);
    void Rotate(const math::Vector3& eulerDegrees, bool worldSpace = false);
    void LookAt(const math::Vector3& worldTarget);

    math::Matrix4 GetWorldMatrix() const;  // position + rotation + scale から生成
};
```

`Forward()` / `Up()` / `Right()` は `rotation.RotateVector(Vector3::FORWARD)` 等で計算する。

---

## GameObject

`name` / `tag` / `transform` を直接メンバーとして持つ。`Scene` が `unique_ptr` で所有する。

```cpp
class GameObject {
public:
    std::string name      = "GameObject";
    std::string tag       = "Untagged";
    Transform   transform;

    // Unity: SetActive / activeSelf
    void SetActive(bool active);
    bool activeSelf() const;

    // Unity: CompareTag
    bool CompareTag(const std::string& tag) const;

    // Unity: AddComponent / GetComponent
    template<typename T> T&  AddComponent(T component = {});
    template<typename T> T*  GetComponent();   // なければ nullptr

    // Unity: SetParent / GetParent / GetChild / GetChildCount
    void        SetParent(GameObject& parent);
    GameObject* GetParent()         const;
    int         GetChildCount()     const;
    GameObject* GetChild(int index) const;

    // Unity: GameObject.Find / FindWithTag / FindObjectsOfType (static)
    // Application::Get().GetSceneManager().GetActive() 経由で Scene を取得して検索する
    static GameObject*              Find(const std::string& name);
    static GameObject*              FindWithTag(const std::string& tag);
    template<typename T>
    static std::vector<GameObject*> FindObjectsOfType();

    // Unity: Object.Destroy(go) / Object.Destroy(go, delay)
    // delay = 0 → 次フレーム末尾で削除
    // delay > 0 → Scene の DestroyEntry queue に積み、毎フレーム減算して削除
    static void Destroy(GameObject& go, float delay = 0.0f);

    bool     IsValid() const;
    EntityID GetID()   const;

private:
    EntityID              m_id       = EntityID::INVALID;
    bool                  m_isActive = true;
    EntityID              m_parent   = EntityID::INVALID;
    std::vector<EntityID> m_children;
    Scene*                m_scene    = nullptr;  // 非所有参照
};
```

---

## Scene

`GameObject` を `unique_ptr` で所有する。他 Component は `ComponentArray<T>` で管理する。

```cpp
class Scene {
public:
    // Unity: new GameObject("name")
    GameObject& CreateGameObject(const std::string& name = "GameObject");

    // Unity: GameObject.Find 系の実体
    GameObject*              Find(const std::string& name)       const;
    GameObject*              FindWithTag(const std::string& tag) const;
    template<typename T>
    std::vector<GameObject*> FindObjectsOfType()                 const;

    // Unity: scene.GetRootGameObjects()
    std::vector<GameObject*> GetRootGameObjects() const;

    // Unity ライク: gameObject. で操作する
    GameObjectRange GameObjects();

    // System 用: 複数 Component を高速イテレート (EnTT の view 相当)
    template<typename... Ts>
    SceneView<Ts...> View();

    bool        IsValid(EntityID id) const;

    // delay 付き Destroy を処理する。毎フレーム末尾で呼ぶ
    void FlushDestroyQueue(float dt);

private:
    std::vector<std::unique_ptr<GameObject>> m_gameObjects;

    ComponentArray<MeshRenderer>       m_meshRenderers;
    ComponentArray<RigidBodyComponent> m_rigidBodies;

    struct DestroyEntry { EntityID id; float delay; };
    std::vector<DestroyEntry> m_destroyQueue;
};
```

---

## GameObjectRange

`scene.GameObjects()` が返す range。Unity ライクな foreach イテレーション用。

```cpp
class GameObjectRange {
public:
    struct Iterator {
        GameObject& operator*();
        Iterator&   operator++();
        bool        operator!=(const Iterator&) const;
    };
    Iterator begin();
    Iterator end();
};
```

```cpp
// 使い方
for (auto& gameObject : scene.GameObjects())
{
    if (!gameObject.activeSelf()) continue;
    auto* mr = gameObject.GetComponent<MeshRenderer>();
    if (mr) mr->enabled = false;
}
```

---

## SceneView\<T...\>

System 用の高速マルチ Component イテレーター。EnTT の `view<A,B>` 相当。  
最小の ComponentArray を基準に走査し、他の Component の有無を確認する。

```cpp
template<typename... Ts>
class SceneView {
public:
    struct Iterator {
        std::tuple<Ts&...> operator*();
        Iterator& operator++();
        bool operator!=(const Iterator&) const;
    };
    Iterator begin();
    Iterator end();
};
```

```cpp
// 使い方
for (auto& [tf, mr] : scene.View<Transform, MeshRenderer>())
{
    if (mr.enabled)
        SubmitDrawCall(tf, mr);
}
```

---

## SceneManager

Title → Game → Result などのシーン遷移を管理する。  
`LoadScene()` はフレーム末尾で適用する (Update 中の破棄を防ぐため)。

```cpp
class SceneManager {
public:
    using SceneFactory = std::function<std::unique_ptr<Scene>()>;

    void Register(const std::string& name, SceneFactory factory);
    void LoadScene(const std::string& name);   // 次フレームで切り替え

    void   Update(float dt, physics::World& world);
    Scene* GetActive();

private:
    std::unordered_map<std::string, SceneFactory> m_factories;
    std::unique_ptr<Scene>                        m_active;
    std::string                                   m_pendingLoad;
};
```

---

## System 関数

System はロジックを持つ free function。ゲームループで明示的に呼ぶ。

```cpp
// TransformSystem: 親子階層のワールド行列・position・rotation を再計算
void TransformSystem(Scene& scene);

// RenderSystem: MeshRenderer + LightComponent を走査しマルチパス描画を発行
//   ライト情報は内部で View<Transform, LightComponent>() から収集する
void RenderSystem(Scene& scene,
                  renderer::IRenderer& renderer,
                  const renderer::Camera& camera,
                  const std::shared_ptr<renderer::IRenderTarget>& outputRT = nullptr,
                  const renderer::RenderSettings* settings = nullptr);

// AudioSystem: AudioSourceComponent を走査して再生・停止を処理する
void AudioSystem(Scene& scene, audio::AudioSystem& audioSystem, float dt);

// PhysicsSystem: RigidBodyComponent ↔ physics::World を同期
void PhysicsSystem(Scene& scene, physics::World& world, float dt);
```

### SceneManager::Update の内部実装イメージ

```cpp
void SceneManager::Update(float dt, physics::World& world)
{
    if (!m_pendingLoad.empty())
    {
        m_active = m_factories[m_pendingLoad]();
        m_pendingLoad.clear();
    }

    if (!m_active) return;

    PhysicsSystem(*m_active, world, dt);  // 物理を先に解決
    TransformSystem(*m_active);           // 次にワールド行列を更新
    m_active->FlushDestroyQueue(dt);      // フレーム末尾で削除
    // RenderSystem はゲームループ側から BeginFrame/EndFrame の間に呼ぶ
}
```

---

## ゲームループとの接続

```cpp
sceneManager.Register("Title",  []{ return std::make_unique<TitleScene>();  });
sceneManager.Register("Game",   []{ return std::make_unique<GameScene>();   });
sceneManager.Register("Result", []{ return std::make_unique<ResultScene>(); });

sceneManager.LoadScene("Title");

while (app.IsRunning())
{
    Time::Tick();
    Input::Update();

    sm.Update(dt, physWorld);

    renderer.BeginFrame();
    renderer.Clear({ 0.05f, 0.08f, 0.15f, 1.0f });
    scene::RenderSystem(*sm.GetActive(), renderer, camera);
    renderer.EndFrame();
}
```
