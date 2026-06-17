# SystemScheduler 設計書（ISystem 完全移行版）

## 概要

SceneManager にハードコードされたシステム実行順序を、**宣言的な ISystem クラス群 + 自動並列化**に置き換える。  
フレーム内並列化のバックエンドは TaskSystem を使用する。

> **依存:** TaskSystem（`task_system.md`）が先に実装されている前提。

---

## 完全移行方針（重要）

**ラッパーは作らない。旧 API は削除する。**

| 移行前 | 移行後 |
|---|---|
| `void FoliageBakeSystem(Scene&)` — 自由関数 | `class FoliageBakeSystem : public ISystem` — クラス |
| 実装が `.cpp` の自由関数本体にある | 実装が `Update(SystemContext&)` の中にある |
| SceneManager が直接呼び出す | SystemScheduler 経由でのみ実行される |
| ヘッダに自由関数宣言 | ヘッダに class 宣言（同一ファイル名、内容を置き換え） |

自由関数の宣言と実装は**削除**する。`detail::` 名前空間への降格や、旧関数を呼ぶラッパーは作らない。

---

## なぜ ISystem クラスか

| 観点 | ラムダ登録方式 | ISystem クラス（本設計） |
|---|---|---|
| メタデータの置き場所 | 登録サイト（SceneManager 側） | System クラス自身 |
| System を増やすとき | 登録コードを探して書き足す | クラスを追加するだけ |
| 実行順の型安全性 | `After("FoliageBakeSystem")` — 文字列 | `After<FoliageBakeSystem>()` — 型 |
| 単体テスト | スケジューラを通さないと動かない | `sys.Update(ctx)` で直接テスト可 |
| ライフサイクル | 別途 API が必要 | `OnInit()` / `OnShutdown()` をオーバーライド |

---

## コア型

### SystemContext — 全 System への統一パラメータ

```cpp
// Engine/Core/Scheduler/SystemContext.hpp
namespace fbzz {

struct SystemContext {
    scene::Scene&                  scene;
    physics::World&                world;
    renderer::ResourceManager*     resources;   // nullable（LateUpdate 以外は nullptr 可）
    float                          dt;
    float                          fixedDt;     // Physics 固定ステップ時のみ有効
    bool                           simulating;
};

} // namespace fbzz
```

---

### Phase / RunMode / PhaseConfig

```cpp
// Engine/Core/Scheduler/Phase.hpp
namespace fbzz {

enum class Phase : uint8_t {
    PreScript   = 0,   // [EditorOnly] TransformEditorPreview → FoliageBake || NavMeshBake → FoliageCull
    Script,            // ScriptSystem
    PrePhysics,        // TransformSystem（Physics 前同期）
    Physics,           // PhysicsSystem（固定ステップ）
    PostPhysics,       // TransformSystem（Physics 書き戻し）
    Navigation,        // NavMeshSensor → NavMeshPatrol → Navigation
    LateScript,        // LateScriptSystem
    Cleanup,           // LifetimeSystem → FlushDestroyQueueSystem
    LateUpdate,        // TransformSystem, AnimatorSystem, IKSystem
    Count
};

enum class RunMode : uint8_t {
    Always,      // エディタ停止中・シミュレーション中どちらでも実行
    SimOnly,     // シミュレーション中のみ
    EditorOnly,  // エディタ停止中のみ
};

struct PhaseConfig {
    bool  fixedStep  = false;
    int   hz         = 60;
    float maxCatchUp = 8.0f;   // fixedDt × maxCatchUp が accumulator 上限
};

// 単一フレームだけ物理を 1 ステップ進める（エディタの「1フレーム送り」用）
// SystemScheduler::SetSingleStep(true) で有効化、次 Update() 後に自動解除

} // namespace fbzz
```

---

### ComponentAccess

```cpp
// Engine/Core/Scheduler/ComponentAccess.hpp
namespace fbzz {

struct ComponentAccess {
    std::vector<std::type_index> reads;
    std::vector<std::type_index> writes;
    bool                         unrestricted = false;

    template<typename... Ts>
    ComponentAccess& Reads()        { (reads.push_back(typeid(Ts)), ...);  return *this; }

    template<typename... Ts>
    ComponentAccess& Writes()       { (writes.push_back(typeid(Ts)), ...); return *this; }

    ComponentAccess& Unrestricted() { unrestricted = true;                 return *this; }
};

} // namespace fbzz
```

---

### OrderingHints — 同 Phase 内の順序制約（型安全）

```cpp
// Engine/Core/Scheduler/OrderingHints.hpp
namespace fbzz {

struct OrderingHints {
    std::vector<std::type_index> after;
    std::vector<std::type_index> before;

    template<typename T> OrderingHints& After()  { after.push_back(typeid(T));  return *this; }
    template<typename T> OrderingHints& Before() { before.push_back(typeid(T)); return *this; }
};

} // namespace fbzz
```

---

## ISystem インターフェース

```cpp
// Engine/Core/Scheduler/ISystem.hpp
namespace fbzz {

class ISystem {
public:
    virtual ~ISystem() = default;

    // 毎フレーム呼ばれる（Phase ループ内から）
    virtual void Update(SystemContext& ctx) = 0;

    // スケジューラが Build() 時に一度だけ読む
    virtual std::string_view Name()       const = 0;
    virtual Phase            GetPhase()   const = 0;
    virtual RunMode          GetRunMode() const { return RunMode::Always; }
    virtual ComponentAccess  GetAccess()  const { return {}; }
    virtual OrderingHints    GetOrder()   const { return {}; }

    // ライフサイクル（任意オーバーライド）
    virtual void OnInit()     {}   // Build() 完了後に呼ばれる
    virtual void OnShutdown() {}   // エンジン終了時に呼ばれる

    // 将来拡張用フック（デフォルトは常に実行・毎フレーム）
    virtual bool ShouldRun(const SystemContext&) const { return true; }  // RunIf
    virtual int  RunInterval()                   const { return 1; }      // RunEvery(N)
};

} // namespace fbzz
```

---

## SystemScheduler

```cpp
// Engine/Core/Scheduler/SystemScheduler.hpp
namespace fbzz {

class SystemScheduler {
public:
    // Physics Phase など固定ステップを使う Phase の設定
    void ConfigurePhase(Phase phase, PhaseConfig cfg);

    // 型引数で登録（デフォルトコンストラクタ可能な T のみ）
    template<typename T>
    requires std::derived_from<T, ISystem> && std::default_initializable<T>
    void AddSystem() { AddSystemPtr(std::make_unique<T>()); }

    // コンストラクタ引数付きで登録（TransformSystem など）
    void AddSystemPtr(std::unique_ptr<ISystem> sys);

    // 全登録後に一度呼ぶ。DAG 構築 + 循環依存チェック
    void Build();

    void Update    (SystemContext ctx);   // PreScript 〜 Cleanup
    void LateUpdate(SystemContext ctx);   // LateUpdate のみ

    // Physics 単一ステップモード（エディタの「1フレーム送り」）
    // true にすると次の Update() で固定ステップを 1 回だけ実行し、自動で false に戻る
    void SetSingleStep(bool enabled);

    // シミュレーション停止時に accumulator をリセット（SceneManager::SetSimulating と連動）
    void ResetAccumulator();

    void DumpGraph() const;   // デバッグ用: 実行グラフ出力
};

} // namespace fbzz
```

---

## 具体的な System クラス（ヘッダ + 実装）

### FoliageBakeSystem

```cpp
// Engine/Scene/Systems/FoliageBakeSystem.hpp
// 旧: void FoliageBakeSystem(Scene&) 宣言を削除し、以下の class 宣言に置き換える
namespace fbzz::scene {

class FoliageBakeSystem final : public ISystem {
public:
    std::string_view Name()       const override { return "FoliageBakeSystem"; }
    Phase            GetPhase()   const override { return Phase::PreScript; }
    RunMode          GetRunMode() const override { return RunMode::Always; }
    ComponentAccess  GetAccess()  const override {
        return ComponentAccess{}
            .Reads <TransformComponent, TerrainComponent, FoliageComponent>()
            .Writes<FoliageComponent>();
    }
    void Update(SystemContext& ctx) override;
};

} // namespace fbzz::scene
```

```cpp
// Engine/Scene/Systems/FoliageBakeSystem.cpp
// 旧: void FoliageBakeSystem(Scene& scene) { ... } を削除し、Update() に移す
void fbzz::scene::FoliageBakeSystem::Update(SystemContext& ctx) {
    // 旧自由関数の実装本体がそのままここに入る
    Scene& scene = ctx.scene;
    // ...
}
```

---

### NavMeshBakeSystem

```cpp
// Engine/Scene/Systems/NavMeshBakeSystem.hpp
class NavMeshBakeSystem final : public ISystem {
public:
    std::string_view Name()       const override { return "NavMeshBakeSystem"; }
    Phase            GetPhase()   const override { return Phase::PreScript; }
    RunMode          GetRunMode() const override { return RunMode::Always; }
    ComponentAccess  GetAccess()  const override {
        return ComponentAccess{}
            .Reads <TransformComponent, TerrainComponent,
                    NavMeshSurfaceComponent, NavMeshModifierComponent>()
            .Writes<NavMeshSurfaceComponent>();
    }
    void Update(SystemContext& ctx) override;

    // 非同期ベイクの進捗取得（エディタの進捗バー用）
    float BakeProgress(uint32_t surfaceId) const;

private:
    // 旧: std::future<NavMesh> + std::atomic<float>* progress をメンバへ
    struct BakeJob { std::future<NavMesh> future; std::shared_ptr<std::atomic<float>> progress; };
    std::unordered_map<uint32_t, BakeJob> m_jobs;
};
```

> 非同期ベイク状態（`std::future<NavMesh>` / `progress`）は自由関数時代はローカル static だったが、  
> クラスメンバに昇格することで複数 Surface の並列ベイクが自然に管理できる。

---

### FoliageCullSystem — `After<FoliageBakeSystem>` で型安全な順序指定

```cpp
// Engine/Scene/Systems/FoliageCullSystem.hpp
class FoliageCullSystem final : public ISystem {
public:
    std::string_view Name()       const override { return "FoliageCullSystem"; }
    Phase            GetPhase()   const override { return Phase::PreScript; }
    RunMode          GetRunMode() const override { return RunMode::Always; }
    ComponentAccess  GetAccess()  const override {
        return ComponentAccess{}
            .Reads <FoliageComponent, CameraComponent>()
            .Writes<FoliageComponent>();
    }
    OrderingHints GetOrder() const override {
        return OrderingHints{}.After<FoliageBakeSystem>();
    }
    void Update(SystemContext& ctx) override;
};
```

---

### TransformSystem — 複数 Phase 問題をタグサブクラスで解決

`TransformSystem` は 1 フレームに **4 回**、別の Phase と RunMode で実行される。

| タグクラス | Phase | RunMode | 役割 |
|---|---|---|---|
| `TransformEditorPreview` | PreScript | EditorOnly | Editor 停止中に Foliage/NavMesh より先に行列更新 |
| `TransformPrePhysics` | PrePhysics | SimOnly | Script が動かした Transform を物理前に同期 |
| `TransformPostPhysics` | PostPhysics | SimOnly | 物理エンジンの書き戻し後に行列再計算 |
| `TransformLateUpdate` | LateUpdate | Always | AnimatorSystem 前の最終同期 |

実装は共通だが `After<T>()` が型で識別するため、**タグサブクラス**で型 ID を分ける。

```cpp
// Engine/Scene/Systems/TransformSystem.hpp

// 共通実装を持つ基底クラス（直接登録しない）
class TransformSystem : public ISystem {
public:
    ComponentAccess GetAccess() const override {
        return ComponentAccess{}.Writes<TransformComponent>();
    }
    void Update(SystemContext& ctx) override;  // 実装は共通
};

// Phase ごとのタグサブクラス（メタデータだけ異なる、実装は TransformSystem から継承）
class TransformPrePhysics final : public TransformSystem {
public:
    std::string_view Name()       const override { return "TransformSystem.PrePhysics"; }
    Phase            GetPhase()   const override { return Phase::PrePhysics; }
    RunMode          GetRunMode() const override { return RunMode::SimOnly; }
};

class TransformPostPhysics final : public TransformSystem {
public:
    std::string_view Name()       const override { return "TransformSystem.PostPhysics"; }
    Phase            GetPhase()   const override { return Phase::PostPhysics; }
    RunMode          GetRunMode() const override { return RunMode::SimOnly; }
};

class TransformLateUpdate final : public TransformSystem {
public:
    std::string_view Name()       const override { return "TransformSystem.LateUpdate"; }
    Phase            GetPhase()   const override { return Phase::LateUpdate; }
    RunMode          GetRunMode() const override { return RunMode::Always; }
};

// Editor 停止中のみ動作。FoliageBake/NavMeshBake が Transform を読む前に行列を更新する。
// WHY: Sim モードでは Physics 書き戻し後の Transform を翌フレーム先頭の Foliage/NavMesh が
//      使うため Transform は PreScript より後に走る。Editor モードは Physics がないため
//      先頭で明示的に更新が必要。
// ComponentAccess の W[Transform] ∩ R[Transform](FoliageBake/NavMeshBake) が
// 競合として検出され、DAG がこのシステムを先に配置する。
class TransformEditorPreview final : public TransformSystem {
public:
    std::string_view Name()       const override { return "TransformSystem.EditorPreview"; }
    Phase            GetPhase()   const override { return Phase::PreScript; }
    RunMode          GetRunMode() const override { return RunMode::EditorOnly; }
};
```

登録は `AddSystem<T>()` を 4 回：

```cpp
scheduler.AddSystem<TransformEditorPreview>();
scheduler.AddSystem<TransformPrePhysics>();
scheduler.AddSystem<TransformPostPhysics>();
scheduler.AddSystem<TransformLateUpdate>();
```

他 System からの参照も型安全：

```cpp
OrderingHints GetOrder() const override {
    return OrderingHints{}.After<TransformLateUpdate>();  // 文字列ではなく型
}
```

---

### ScriptSystem / LateScriptSystem — Unrestricted

```cpp
class ScriptSystem final : public ISystem {
public:
    std::string_view Name()       const override { return "ScriptSystem"; }
    Phase            GetPhase()   const override { return Phase::Script; }
    RunMode          GetRunMode() const override { return RunMode::SimOnly; }
    ComponentAccess  GetAccess()  const override { return ComponentAccess{}.Unrestricted(); }
    void Update(SystemContext& ctx) override;
};

class LateScriptSystem final : public ISystem {
public:
    std::string_view Name()       const override { return "LateScriptSystem"; }
    Phase            GetPhase()   const override { return Phase::LateScript; }
    RunMode          GetRunMode() const override { return RunMode::SimOnly; }
    ComponentAccess  GetAccess()  const override { return ComponentAccess{}.Unrestricted(); }
    void Update(SystemContext& ctx) override;
};
```

---

### PhysicsSystem — 固定ステップは PhaseConfig で制御

```cpp
class PhysicsSystem final : public ISystem {
public:
    std::string_view Name()       const override { return "PhysicsSystem"; }
    Phase            GetPhase()   const override { return Phase::Physics; }
    RunMode          GetRunMode() const override { return RunMode::SimOnly; }
    ComponentAccess  GetAccess()  const override {
        return ComponentAccess{}
            .Reads <ColliderComponent, RigidBodyComponent, CharacterControllerComponent>()
            .Writes<TransformComponent, RigidBodyComponent, VolumeComponent>();
    }
    void Update(SystemContext& ctx) override;
    // ctx.fixedDt を使う。固定ステップループは SystemScheduler::RunPhase が担う
};
```

---

### Navigation 群 — 型安全な順序チェーン

```cpp
class NavMeshSensorSystem final : public ISystem {
public:
    std::string_view Name()      const override { return "NavMeshSensorSystem"; }
    Phase            GetPhase()  const override { return Phase::Navigation; }
    RunMode          GetRunMode()const override { return RunMode::SimOnly; }
    ComponentAccess  GetAccess() const override {
        return ComponentAccess{}
            .Reads <NavMeshSensorComponent, TransformComponent>()
            .Writes<NavMeshSensorComponent, NavMeshAgentComponent>();
    }
    void Update(SystemContext& ctx) override;
};

class NavMeshPatrolSystem final : public ISystem {
public:
    std::string_view Name()      const override { return "NavMeshPatrolSystem"; }
    Phase            GetPhase()  const override { return Phase::Navigation; }
    RunMode          GetRunMode()const override { return RunMode::SimOnly; }
    ComponentAccess  GetAccess() const override {
        return ComponentAccess{}
            .Reads <NavMeshPatrolComponent, NavMeshSensorComponent>()
            .Writes<NavMeshAgentComponent>();
    }
    OrderingHints GetOrder() const override {
        return OrderingHints{}.After<NavMeshSensorSystem>();
    }
    void Update(SystemContext& ctx) override;
};

class NavigationSystem final : public ISystem {
public:
    std::string_view Name()      const override { return "NavigationSystem"; }
    Phase            GetPhase()  const override { return Phase::Navigation; }
    RunMode          GetRunMode()const override { return RunMode::SimOnly; }
    ComponentAccess  GetAccess() const override {
        return ComponentAccess{}
            .Reads <NavMeshAgentComponent>()
            .Writes<TransformComponent>();
    }
    OrderingHints GetOrder() const override {
        return OrderingHints{}.After<NavMeshPatrolSystem>();
    }
    void Update(SystemContext& ctx) override;
};
```

---

### LifetimeSystem / FlushDestroyQueueSystem — Cleanup チェーン

`scene->FlushDestroyQueue(dt)` は LifetimeSystem の直後に呼ばれていた。  
移行後は `FlushDestroyQueueSystem` として Cleanup Phase に組み込む。

```cpp
class LifetimeSystem final : public ISystem {
public:
    std::string_view Name()      const override { return "LifetimeSystem"; }
    Phase            GetPhase()  const override { return Phase::Cleanup; }
    RunMode          GetRunMode()const override { return RunMode::SimOnly; }
    ComponentAccess  GetAccess() const override {
        return ComponentAccess{}.Reads<LifetimeComponent>().Writes<LifetimeComponent>();
    }
    void Update(SystemContext& ctx) override;
};

// Scene::FlushDestroyQueue() を ISystem として包む。
// ComponentAccess は Unrestricted — 任意の Component を持つ GO が破棄されうるため。
class FlushDestroyQueueSystem final : public ISystem {
public:
    std::string_view Name()      const override { return "FlushDestroyQueueSystem"; }
    Phase            GetPhase()  const override { return Phase::Cleanup; }
    RunMode          GetRunMode()const override { return RunMode::SimOnly; }
    ComponentAccess  GetAccess() const override { return ComponentAccess{}.Unrestricted(); }
    OrderingHints    GetOrder()  const override { return OrderingHints{}.After<LifetimeSystem>(); }
    void Update(SystemContext& ctx) override {
        ctx.scene.FlushDestroyQueue(ctx.dt);
    }
    // WHY: 実装が 1 行で完結するため .cpp には分けない
};
```

---

### AnimatorSystem / IKSystem — LateUpdate チェーン

```cpp
class AnimatorSystem final : public ISystem {
public:
    std::string_view Name()      const override { return "AnimatorSystem"; }
    Phase            GetPhase()  const override { return Phase::LateUpdate; }
    ComponentAccess  GetAccess() const override {
        return ComponentAccess{}
            .Reads <AnimatorComponent>()
            .Writes<AnimatorComponent, BoneComponent>();
    }
    OrderingHints GetOrder() const override {
        return OrderingHints{}.After<TransformLateUpdate>();
    }
    void Update(SystemContext& ctx) override;
    // ctx.resources が nullptr の場合はスキップ（Update() 内で guard）
};

class IKSystem final : public ISystem {
public:
    std::string_view Name()      const override { return "IKSystem"; }
    Phase            GetPhase()  const override { return Phase::LateUpdate; }
    ComponentAccess  GetAccess() const override {
        return ComponentAccess{}
            .Reads <IKSolverComponent, BoneComponent>()
            .Writes<BoneComponent>();
    }
    OrderingHints GetOrder() const override {
        return OrderingHints{}.After<AnimatorSystem>();   // FK ポーズ後に IK 補正
    }
    void Update(SystemContext& ctx) override;
};
```

---

## 並列化ロジック（Build() 時に確定）

```
conflict(A, B):
    A.unrestricted || B.unrestricted
    OR  A.writes ∩ B.writes ≠ ∅      (write-write 競合)
    OR  A.writes ∩ B.reads  ≠ ∅      (A が書くものを B が読む)
    OR  B.writes ∩ A.reads  ≠ ∅      (B が書くものを A が読む)

conflict しない System ペアを同バッチに配置。OrderingHints は追加の直列制約。
```

### PreScript Phase の実行グラフ

```
TransformEditorPreview  W[Transform]           EditorOnly
FoliageBakeSystem       R[Transform] W[Foliage]
NavMeshBakeSystem       R[Transform] W[NavMeshSurface]
FoliageCullSystem       R[Foliage]   W[Foliage]   After<FoliageBakeSystem>

競合チェック:
  TransformEditorPreview vs FoliageBake  → W[Transform]∩R[Transform] → 競合（EditorPreview が先）
  TransformEditorPreview vs NavMeshBake  → W[Transform]∩R[Transform] → 競合（EditorPreview が先）
  FoliageBake vs NavMeshBake             → W∩W=∅, W∩R=∅              → 並列 OK ✓
  FoliageCull vs FoliageBake             → W[Foliage] 競合 + After    → 後段でシリアル

Editor モード実行:
  step 1:  TransformEditorPreview
  step 2:  FoliageBakeSystem || NavMeshBakeSystem   ← TaskSystem::Submit × 2
  step 3:  FoliageCullSystem

Sim モード実行（TransformEditorPreview は EditorOnly なのでスキップ）:
  step 1:  FoliageBakeSystem || NavMeshBakeSystem
  step 2:  FoliageCullSystem
```

---

## SystemScheduler 内部実装スケッチ

```cpp
void SystemScheduler::Build() {
    // Phase ごとにグループ化
    // 各 Phase 内: ComponentAccess 競合 + OrderingHints → DAG 構築
    // トポロジカルソートで「実行バッチ列」を確定
    //   例 PreScript: [{FoliageBake, NavMeshBake}, {FoliageCull}]
    // Unrestricted System はそのバッチ単独占有
    // 循環依存があれば Build() 時にアサート
}

void SystemScheduler::RunPhase(Phase p, SystemContext& ctx) {
    auto& cfg = m_phaseConfigs[p];

    auto runBatches = [&](SystemContext& c) {
        for (auto& batch : m_batches[p]) {
            if (batch.size() == 1) {
                batch[0]->Update(c);
            } else {
                std::vector<std::future<void>> futs;
                for (auto* sys : batch)
                    futs.push_back(TaskSystem::Submit([sys, &c]{ sys->Update(c); }));
                for (auto& f : futs) f.wait();
            }
        }
    };

    if (!cfg.fixedStep) {
        runBatches(ctx);
    } else {
        // 固定ステップループ（PhysicsSystem 用）
        // m_accumulator と m_singleStep は SystemScheduler のメンバとして保持
        const float fixedDt  = 1.0f / cfg.hz;
        const float maxAccum = fixedDt * cfg.maxCatchUp;
        SystemContext fixedCtx = ctx;
        fixedCtx.fixedDt = fixedDt;
        if (m_singleStep) {
            runBatches(fixedCtx);
            m_singleStep = false;   // 次フレームは通常モードに戻す
        } else {
            m_accumulator = std::min(m_accumulator + ctx.dt, maxAccum);
            while (m_accumulator >= fixedDt) {
                runBatches(fixedCtx);
                m_accumulator -= fixedDt;
            }
        }
    }
}
```

---

## スケジューラ外のシステム（移行対象外）

以下は SystemScheduler に組み込まず、ゲームループから直接呼ぶ。

| システム | 理由 |
|---|---|
| `RenderSystem` | BeginFrame / EndFrame の都合でゲームループ側が制御する |
| `UISystem` | RenderSystem と同様、描画フレームに紐づく |
| `DebugDrawSystem` 群 | 描画パスに依存、ゲームループ側から条件付きで呼ぶ |

---

## 自動プロファイリング

移行後、SceneManager にあった手動の `FBZZ_PROFILE_SCOPE("SceneManager::XxxSystem")` は不要になる。  
SystemScheduler の `RunPhase` が各 System の `Update()` を `ISystem::Name()` でラップする。

```cpp
// RunPhase 内（スケッチ）
FBZZ_PROFILE_SCOPE(sys->Name());
sys->Update(ctx);
```

---

## 移行後の SceneManager

```cpp
void SceneManager::Update(float dt, physics::World& world) {
    if (!m_pendingLoad.empty()) { /* シーン切り替え（変わらず SceneManager の責務）*/ }
    Scene* scene = CurrentScene();
    if (!scene) return;
    m_scheduler.Update({ *scene, world, nullptr, dt, 0.0f, m_simulating });
}

void SceneManager::LateUpdate(float dt, physics::World& world) {
    Scene* scene = CurrentScene();
    if (!scene) return;
    m_scheduler.LateUpdate({ *scene, world, renderer::ResourceManager::Active(), dt, 0.0f, m_simulating });
}

// SetSingleStep / SetSimulating はそのまま SceneManager のインターフェースに残し、
// 内部で m_scheduler に委譲する
void SceneManager::SetSingleStep(bool v)   { m_scheduler.SetSingleStep(v); }
void SceneManager::SetSimulating(bool sim) {
    m_simulating = sim;
    if (!sim) m_scheduler.ResetAccumulator();
}
```

SceneManager が保持していた `m_physicsAccumulator` / `m_singleStep` / `m_physicsHz` は  
SystemScheduler の `PhaseConfig` と内部状態に移り、SceneManager から削除する。

---

## ファイル配置

### 新規追加

```
Projects/Engine/include/Engine/Core/Scheduler/
    ISystem.hpp
    SystemScheduler.hpp
    SystemContext.hpp
    Phase.hpp               ← Phase, RunMode, PhaseConfig
    ComponentAccess.hpp
    OrderingHints.hpp

Projects/Engine/src/Core/Scheduler/
    SystemScheduler.cpp
```

### 置き換え（同名ファイルを上書き）

既存 System ヘッダの自由関数宣言を class 宣言に置き換え、  
対応 `.cpp` の自由関数実装を `Update()` メソッドに移動して**旧関数を削除する**。

```
Engine/Scene/Systems/FoliageBakeSystem.hpp   / .cpp
Engine/Scene/Systems/FoliageCullSystem.hpp   / .cpp
Engine/Scene/Systems/NavMeshBakeSystem.hpp   / .cpp
Engine/Scene/Systems/NavMeshSensorSystem.hpp / .cpp
Engine/Scene/Systems/NavMeshPatrolSystem.hpp / .cpp
Engine/Scene/Systems/NavigationSystem.hpp    / .cpp
Engine/Scene/Systems/ScriptSystem.hpp        / .cpp   ← ScriptSystem + LateScriptSystem
Engine/Scene/Systems/TransformSystem.hpp     / .cpp   ← TransformSystem 基底 + 4 タグサブクラス
Engine/Scene/Systems/PhysicsSystem.hpp       / .cpp
Engine/Scene/Systems/AnimatorSystem.hpp      / .cpp
Engine/Scene/Systems/IKSystem.hpp            / .cpp
Engine/Scene/Systems/LifetimeSystem.hpp      / .cpp   ← LifetimeSystem + FlushDestroyQueueSystem
```

---

## 実装ステップ

1. `Core/Scheduler/` 以下のヘッダ群を定義（ISystem, Phase, ComponentAccess, OrderingHints, SystemContext）
2. `SystemScheduler` を実装——まず**シリアル実行のみ**（Build + RunPhase の直列版）
3. System クラスを 1 つずつ実装：
   - `.hpp` の自由関数宣言を class 宣言に**置き換え**
   - `.cpp` の自由関数本体を `Update()` に**移動**し、旧関数定義を**削除**
   - `TransformSystem` は基底 + 4 タグサブクラス（EditorPreview / PrePhysics / PostPhysics / LateUpdate）
   - `FlushDestroyQueueSystem` は `LifetimeSystem.hpp` に同居、実装はヘッダ内 1 行
   - SceneManager の直接呼び出しをコンパイルエラーにして移行漏れを検出する
4. `RegisterAllSystems()` で全 System を `AddSystem<T>()` 登録し、`Build()` を呼ぶ
5. `SceneManager::Update()` / `LateUpdate()` を薄いラッパーに置き換え
   - `m_physicsAccumulator` / `m_singleStep` / `m_physicsHz` を SceneManager から削除
6. `DumpGraph()` で実行グラフが期待通りか確認
7. `PhysicsSystem` 固定ステップをシミュレーションで動作確認
8. TaskSystem によるバッチ並列実行を組み込み（Step 2 のシリアル版を置き換え）

> Step 3 で「SceneManager の直接呼び出しをコンパイルエラーにする」のがポイント。  
> ラッパーを作ると移行完了したように見えて旧 API が残るため、**コンパイルエラーで強制**する。

---

## 将来拡張

### 1. System の有効/無効切り替え（SetEnabled）

ランタイムで特定 System を止める。デバッグ中に AI だけ無効化するなど。

```cpp
scheduler.SetEnabled<NavMeshBakeSystem>(false);   // 次フレームから実行されない
scheduler.SetEnabled<NavMeshBakeSystem>(true);    // 再有効化
```

実装: `ISystem` に `bool m_enabled = true` を持たせ、`RunPhase` でスキップ。  
ComponentAccess の DAG は変えないため `Build()` の再実行は不要。

---

### 2. System グループ（Group）

関連する System をグループ化し、まとめて一時停止・再開する。

```cpp
// 登録時にグループを宣言
sched.Register("NavMeshSensorSystem").Group("AI")...;
sched.Register("NavMeshPatrolSystem").Group("AI")...;
sched.Register("NavigationSystem")   .Group("AI")...;

// ランタイムで AI 系を一括停止
scheduler.PauseGroup("AI");
scheduler.ResumeGroup("AI");
```

カットシーン中に物理・AI を止め、アニメーションだけ動かす用途などに使える。

---

### 3. 条件付き実行（RunIf）

`ISystem::ShouldRun()` をオーバーライドして毎フレームの実行可否を制御する。  
「NavMeshSurface を持つ GO がなければ NavMeshBakeSystem をスキップ」など。

```cpp
class NavMeshBakeSystem final : public ISystem {
    // ...
    bool ShouldRun(const SystemContext& ctx) const override {
        return ctx.scene.HasAny<NavMeshSurfaceComponent>();
    }
};
```

`RunPhase` は `GetRunMode()` チェックの後に `ShouldRun()` を評価する。  
デフォルト実装は `true` を返すので既存 System は影響を受けない。

---

### 4. 実行頻度の間引き（Throttle）

`ISystem::RunInterval()` をオーバーライドして N フレームに 1 回だけ実行する。  
NavMeshBakeSystem のような変化頻度が低い処理に有効。

```cpp
class NavMeshBakeSystem final : public ISystem {
    // ...
    int RunInterval() const override { return 3; }  // 3 フレームに 1 回
};
```

`RunPhase` が `m_frameCounters[sys]++ % sys->RunInterval() == 0` で実行判定。  
デフォルト実装は `1` を返すので毎フレーム実行（既存 System は影響なし）。

---

### 5. フレームバジェット（FrameBudget）

System ごとに ms 上限を設定し、超えたら翌フレームに defer する。  
重い NavMeshBakeSystem がフレームを 16ms 超えて占有するのを防ぐ。

```cpp
sched.Register("NavMeshBakeSystem")
    .BudgetMs(2.0f)   // 2ms を超えたら今フレームは中断し翌フレームに再開
    ...;
```

実装は `std::chrono::steady_clock` で Update() 前後を計測し、超過時は残りチャンクを内部キューに積む。

---

### 6. 統計・デバッグオーバーレイ

既存の `Profiler` と連携し、各 System の平均/最大フレーム時間を収集。  
エディタの ImGui パネルで並列化状況・ボトルネックを可視化する。

```cpp
// SystemScheduler が内部で収集
struct SystemStats {
    float avgMs;
    float maxMs;
    float lastMs;
    int   frameCount;
    bool  wasParallel;   // 最後のフレームで並列実行されたか
};

const SystemStats& GetStats(std::string_view name) const;
```

```
// エディタ ImGui パネルのイメージ
┌─ SystemScheduler ──────────────────────────────────────┐
│ Phase: PreScript                              3.1 ms   │
│   ├─ [||] FoliageBakeSystem    avg 1.2ms  max 2.1ms   │
│   ├─ [||] NavMeshBakeSystem    avg 1.8ms  max 3.4ms   │
│   └─ [─]  FoliageCullSystem    avg 0.3ms  max 0.5ms   │
│ Phase: Script                                 2.4 ms   │
│   └─ [─]  ScriptSystem         avg 2.4ms  max 5.1ms   │
│ ...                                                    │
└────────────────────────────────────────────────────────┘
  [||] = 並列実行  [─] = シリアル実行
```

`DumpGraph()` と同じ情報にタイミングデータを加えた形で、`Editor/Panels/` に `SystemSchedulerPanel` として実装する。

---

## 非目標

- Component アクセス宣言のコンパイル時安全性（実行時 `type_index` 比較のみ）
- ECS archetype / sparse set レイアウト変更
- System のホットリロード・動的追加削除
- System 間イベントキュー（既存の仕組みを維持）
