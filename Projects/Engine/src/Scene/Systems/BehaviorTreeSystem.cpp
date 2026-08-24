// FBZZ Engine
// BehaviorTreeSystem.cpp | fbzz::scene
// 木のロード・共有・評価と、Scene に触るアクションの実装
#include "Engine/Scene/Systems/BehaviorTreeSystem.hpp"

#include "Engine/Core/Scheduler/SystemContext.hpp"
#include "Engine/Core/Logger.hpp"
#include "Engine/Scene/Scene.hpp"
#include "Engine/Scene/GameObject.hpp"
#include "Engine/Scene/ScriptComponent.hpp"
#include "Engine/Scene/Components/AnimatorComponent.hpp"
#include "Engine/Scene/Components/AudioSourceComponent.hpp"
#include "Engine/Scene/Components/BehaviorTreeComponent.hpp"
#include "Engine/Scene/Components/NavMeshAgentComponent.hpp"
#include "Engine/Scene/Components/NavMeshPatrolComponent.hpp"
#include "Engine/Scene/Components/NavMeshSensorComponent.hpp"
#include "Engine/Scene/Systems/NavMeshSensorSystem.hpp"
#include "Engine/Scene/Systems/NavMeshPatrolSystem.hpp"

#include <Engine/AI/BehaviorTreeAsset.hpp>
#include <Physics/World.hpp>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <unordered_map>

namespace fbzz::scene {

namespace {

constexpr float kPi = 3.14159265358979323846f;

// ── 木の共有キャッシュ ───────────────────────────────────────────────────────
// パス → コンパイル済みランタイム。weak_ptr で持ち、誰も参照しなくなれば解放する。
//
// WHY 共有するか: 敵 100 体が同じ木を使うとき、構造を 100 個複製するのは
//     メモリの浪費であり、キャッシュ効率も悪い。実行状態だけを個体が持つ。
std::unordered_map<std::string, std::weak_ptr<const ai::BehaviorTreeRuntime>>& TreeCache()
{
    static std::unordered_map<std::string, std::weak_ptr<const ai::BehaviorTreeRuntime>> cache;
    return cache;
}

std::shared_ptr<const ai::BehaviorTreeRuntime> AcquireTree(const std::string& path)
{
    if (path.empty()) return nullptr;

    auto& cache = TreeCache();
    if (const auto it = cache.find(path); it != cache.end()) {
        if (auto existing = it->second.lock()) return existing;
    }

    ai::BehaviorTreeAsset asset;
    std::string error;
    if (!ai::LoadBehaviorTreeAsset(path, asset, &error)) {
        FBZZ_LOG_WARN("BehaviorTree: %s を読み込めません (%s)", path.c_str(), error.c_str());
        return nullptr;
    }

    auto runtime = std::make_shared<ai::BehaviorTreeRuntime>();
    if (!ai::CompileBehaviorTree(asset, *runtime, &error)) {
        FBZZ_LOG_ERROR("BehaviorTree: %s のコンパイルに失敗 (%s)", path.c_str(), error.c_str());
        return nullptr;
    }
    runtime->sourcePath = path;

    for (const std::string& warning : runtime->compileWarnings)
        FBZZ_LOG_WARN("BehaviorTree [%s]: %s", path.c_str(), warning.c_str());

    cache[path] = runtime;
    return runtime;
}

// ── Scene に触るアクションの実装 ─────────────────────────────────────────────
class SceneActionHandler final : public ai::IBTActionHandler {
public:
    SceneActionHandler(Scene& scene, physics::World& world) : m_scene(scene), m_world(world) {}

    ai::BTStatus Execute(const ai::BehaviorTreeRuntime& tree, std::uint16_t node,
                         ai::BTInstanceState& state, ai::Blackboard& blackboard,
                         ai::BTTickContext& ctx) override
    {
        GameObject* self = m_scene.GetGameObject(ctx.self);
        if (!self) return ai::BTStatus::Failure;

        const ai::BTNodeParams& params = tree.params[tree.nodes[node].paramIndex];

        switch (tree.nodes[node].type) {
        case ai::BTNodeType::MoveTo:      return ExecuteMoveTo(params, *self, ctx, blackboard, state, node);
        case ai::BTNodeType::Patrol:      return ExecutePatrol(*self, ctx);
        case ai::BTNodeType::LookAt:      return ExecuteLookAt(params, *self, ctx, blackboard);
        case ai::BTNodeType::PlayAnimation: return ExecutePlayAnimation(params, *self, ctx);
        case ai::BTNodeType::PlayAudio:   return ExecutePlaySound(params, *self, ctx);

        // 段階 7 で FBZZ_BT_ACTION + ScriptCodeGen により実装する。
        // WHY Failure を返すか: Success にすると未実装の枝が黙って通過し、
        //     木が「動いているように見えて何もしていない」状態になる。
        case ai::BTNodeType::RunScript:   return ai::BTStatus::Failure;

        default: return ai::BTStatus::Failure;
        }
    }

    void Abort(const ai::BehaviorTreeRuntime& tree, std::uint16_t node,
               ai::BTInstanceState& state, ai::BTTickContext& ctx) override
    {
        (void)state;
        GameObject* self = m_scene.GetGameObject(ctx.self);
        if (!self) return;

        const ai::BTNodeParams& params = tree.params[tree.nodes[node].paramIndex];

        // Running 中に中断されたリーフの後始末。
        // WHY 必要か: MoveTo を中断したまま放置すると、agent が古い目的地へ
        //     走り続け、割り込んだ行動と競合して「動きがおかしい敵」になる。
        switch (tree.nodes[node].type) {
        case ai::BTNodeType::MoveTo: {
            if (auto* agent = m_scene.GetComponent<NavMeshAgentComponent>(ctx.self)) {
                if (params.chaseEntity) agent->ClearTarget();
                agent->Stop();
            }
            break;
        }
        // Patrol は currentIndex を保持したままにする (復帰時に続きから巡回する)。
        case ai::BTNodeType::Patrol:
        default:
            break;
        }
    }

    bool EvaluateCondition(const ai::BehaviorTreeRuntime& tree, std::uint16_t node,
                           const ai::Blackboard& blackboard,
                           const ai::BTTickContext& ctx) const override
    {
        const ai::BTNodeParams& params = tree.params[tree.nodes[node].paramIndex];

        switch (tree.nodes[node].type) {
        case ai::BTNodeType::HasTarget: {
            bool hasTarget = false;
            return blackboard.GetBool(ai::bb::HasTarget, hasTarget) && hasTarget;
        }

        case ai::BTNodeType::IsTargetInRange: {
            GameObject* self = m_scene.GetGameObject(ctx.self);
            if (!self) return false;
            math::Vector3 targetPos = math::Vector3::ZERO;
            if (!blackboard.GetVector3(ai::bb::TargetPosition, targetPos)) return false;
            bool hasTarget = false;
            if (!blackboard.GetBool(ai::bb::HasTarget, hasTarget) || !hasTarget) return false;

            const float distance = (targetPos - self->transform.worldPosition).Length();
            return distance <= params.range;
        }

        case ai::BTNodeType::IsHealthBelow: {
            float health = 1.0f;
            if (!blackboard.GetFloat(ai::bb::Health01, health)) return false;
            return health < params.threshold01;
        }

        case ai::BTNodeType::HasLineOfSight: {
            GameObject* self = m_scene.GetGameObject(ctx.self);
            if (!self) return false;
            math::Vector3 targetPos = math::Vector3::ZERO;
            if (!blackboard.GetVector3(ai::bb::TargetPosition, targetPos)) return false;

            const math::Vector3 origin = self->transform.worldPosition + math::Vector3::UP * 0.5f;
            math::Vector3 toTarget = targetPos - origin;
            const float distance = toTarget.Length();
            if (distance < 0.0001f) return true;

            const math::Vector3 direction = toTarget * (1.0f / distance);
            physics::World::RaycastHit hit;
            if (!m_world.Raycast(origin, direction, distance, hit)) return true;
            // 遮蔽物までの距離がターゲットとほぼ同じなら、遮っているのはターゲット自身。
            return hit.distance >= distance - 0.1f;
        }

        default:
            return false;
        }
    }

private:
    ai::BTStatus ExecuteMoveTo(const ai::BTNodeParams& params, GameObject& self,
                               ai::BTTickContext& ctx, ai::Blackboard& blackboard,
                               ai::BTInstanceState& state, std::uint16_t node)
    {
        auto* agent = m_scene.GetComponent<NavMeshAgentComponent>(ctx.self);
        if (!agent) return ai::BTStatus::Failure;

        // 追跡モード: Entity キーの対象を SetTarget で追い続ける。
        if (params.chaseEntity) {
            EntityID target = EntityID::INVALID;
            if (params.moveKey == ai::kInvalidBlackboardKey
                || !blackboard.GetEntity(params.moveKey, target) || !target.IsValid()) {
                return ai::BTStatus::Failure;
            }

            // 既に同じ相手を追っているなら SetTarget を呼び直さない。
            // WHY: SetTarget は再パスのタイマーをリセットする。毎 tick 呼ぶと
            //      経路が確定せず、その場で足踏みする。
            if (!(agent->target == target))
                agent->SetTarget(target, params.repathInterval);

            GameObject* targetGo = m_scene.GetGameObject(target);
            if (!targetGo) return ai::BTStatus::Failure;

            const float distance =
                (targetGo->transform.worldPosition - self.transform.worldPosition).Length();
            if (distance <= params.acceptanceRadius) return ai::BTStatus::Success;

            // 経路が見つからない状態が続いたら諦める。
            if (agent->isStuck) return ai::BTStatus::Failure;
            return ai::BTStatus::Running;
        }

        // 座標モード: Vector3 キー (無ければ定数) の位置へ 1 度だけ向かう。
        math::Vector3 destination = params.valueVector3;
        if (params.moveKey != ai::kInvalidBlackboardKey) {
            if (!blackboard.GetVector3(params.moveKey, destination))
                return ai::BTStatus::Failure;
        }

        // 初回入場時にだけ SetDestination する。
        if (!(state.flags[node] & ai::BTNodeFlag::Entered)) {
            state.flags[node] |= ai::BTNodeFlag::Entered;
            agent->SetDestination(destination);
            return ai::BTStatus::Running;
        }

        const float distance = (destination - self.transform.worldPosition).Length();
        if (distance <= params.acceptanceRadius
            || (agent->destinationReached && !agent->hasDestination)) {
            state.flags[node] &= static_cast<std::uint8_t>(~ai::BTNodeFlag::Entered);
            return ai::BTStatus::Success;
        }
        if (agent->isStuck) {
            state.flags[node] &= static_cast<std::uint8_t>(~ai::BTNodeFlag::Entered);
            return ai::BTStatus::Failure;
        }
        return ai::BTStatus::Running;
    }

    ai::BTStatus ExecutePatrol(GameObject& self, ai::BTTickContext& ctx)
    {
        (void)self;
        auto* patrol = m_scene.GetComponent<NavMeshPatrolComponent>(ctx.self);
        auto* agent  = m_scene.GetComponent<NavMeshAgentComponent>(ctx.self);
        if (!patrol || !agent || patrol->waypoints.empty()) return ai::BTStatus::Failure;

        // 実際の巡回進行は NavMeshPatrolSystem が行う。
        // WHY 二重実装しないか: ウェイポイントの前後・待機・速度上書きのロジックは
        //     既に NavMeshPatrolSystem にあり、そこが唯一の真実であるべき。
        //     BT の Patrol ノードは「巡回させ続ける意思表示」として Running を返し、
        //     追跡へ切り替わるときに中断される側に回る。
        //
        // NavMeshPatrolSystem は agent->target が有効だと巡回を止めるため、
        // 巡回に入る時点で追跡状態を解除しておく。
        if (agent->target.IsValid()) agent->ClearTarget();

        // 巡回は明示的に終わらない。中断されるまで Running。
        return ai::BTStatus::Running;
    }

    ai::BTStatus ExecuteLookAt(const ai::BTNodeParams& params, GameObject& self,
                               ai::BTTickContext& ctx, ai::Blackboard& blackboard)
    {
        math::Vector3 targetPos = params.valueVector3;
        if (params.moveKey != ai::kInvalidBlackboardKey) {
            if (!blackboard.GetVector3(params.moveKey, targetPos)) return ai::BTStatus::Failure;
        } else if (params.key != ai::kInvalidBlackboardKey) {
            if (!blackboard.GetVector3(params.key, targetPos)) return ai::BTStatus::Failure;
        }

        math::Vector3 toTarget = targetPos - self.transform.worldPosition;
        toTarget.y = 0.0f;
        const float distance = toTarget.Length();
        if (distance < 0.0001f) return ai::BTStatus::Success;

        const math::Vector3 desired = toTarget * (1.0f / distance);
        math::Vector3 forward = self.transform.forward;
        forward.y = 0.0f;
        if (forward.LengthSq() < 0.0001f) return ai::BTStatus::Success;
        forward = forward.Normalized();

        const float dot = std::clamp(math::Vector3::Dot(forward, desired), -1.0f, 1.0f);
        const float angleDeg = std::acos(dot) * (180.0f / kPi);

        // 3 度以内なら向き終わったとみなす。
        // WHY 閾値を置くか: 完全一致を待つと浮動小数の誤差で永久に Running になる。
        if (angleDeg <= 3.0f) return ai::BTStatus::Success;

        const float step = std::min(params.turnSpeedDeg * ctx.dt, angleDeg);
        const float t = angleDeg > 0.0001f ? step / angleDeg : 1.0f;

        const math::Quaternion target = math::Quaternion::LookRotation(desired, math::Vector3::UP);
        self.transform.rotation = math::Quaternion::Slerp(self.transform.rotation, target, t);
        return ai::BTStatus::Running;
    }

    ai::BTStatus ExecutePlayAnimation(const ai::BTNodeParams& params, GameObject& self,
                                      ai::BTTickContext& ctx)
    {
        (void)self;
        auto* animator = m_scene.GetComponent<AnimatorComponent>(ctx.self);
        if (!animator || params.text.empty()) return ai::BTStatus::Failure;

        animator->SetTrigger(params.text);
        // waitForAnimation は段階 3 では未対応 (Animator の再生完了通知が要る)。
        // 現状はトリガーを立てた時点で Success。
        return ai::BTStatus::Success;
    }

    ai::BTStatus ExecutePlaySound(const ai::BTNodeParams& params, GameObject& self,
                                  ai::BTTickContext& ctx)
    {
        (void)self;
        auto* source = m_scene.GetComponent<AudioSourceComponent>(ctx.self);
        if (!source || params.text.empty()) return ai::BTStatus::Failure;

        if (source->m_pendingOneShots.size() >= AudioSourceComponent::MAX_PENDING_ONE_SHOTS)
            return ai::BTStatus::Failure;

        AudioSourceComponent::OneShotRequest request;
        request.path        = params.text;
        // ノードの volume は今まで参照されていなかった。one-shot が倍率を持てるようになったので繋ぐ。
        request.volumeScale = params.volume < 0.0f ? 0.0f : params.volume;
        source->m_pendingOneShots.push_back(std::move(request));
        return ai::BTStatus::Success;
    }

    Scene&          m_scene;
    physics::World& m_world;
};

// 知覚結果を予約キーへ書き込む。
//
// 段階 3 では NavMeshSensorComponent を情報源にする。
// 段階 4 で PerceptionComponent を追加したら、そちらを優先する分岐をここへ足す。
void PopulateReservedKeys(Scene& scene, EntityID eid, GameObject& self,
                          BehaviorTreeComponent& bt)
{
    ai::Blackboard& blackboard = bt.blackboard;

    blackboard.SetEntity(ai::bb::Self, eid);

    // HomePosition は最初の 1 回だけ記録する (帰還先)。
    if (!blackboard.IsSet(ai::bb::HomePosition))
        blackboard.SetVector3(ai::bb::HomePosition, self.transform.worldPosition);

    const auto* sensor = scene.GetComponent<NavMeshSensorComponent>(eid);
    if (!sensor) return;

    blackboard.SetBool(ai::bb::HasTarget, sensor->targetVisible);

    if (sensor->targetVisible && sensor->detectedTarget.IsValid()) {
        blackboard.SetEntity(ai::bb::TargetEntity, sensor->detectedTarget);
        if (GameObject* target = scene.GetGameObject(sensor->detectedTarget))
            blackboard.SetVector3(ai::bb::TargetPosition, target->transform.worldPosition);
    }

    // 最後に見た位置は見失った後も残す (捜索行動が使う)。
    blackboard.SetVector3(ai::bb::LastKnownPosition, sensor->lastKnownTargetPos);
}

} // namespace

ComponentAccess BehaviorTreeSystem::GetAccess() const
{
    return ComponentAccess{}
        .Reads<NavMeshSensorComponent>()
        .Writes<BehaviorTreeComponent, NavMeshAgentComponent, NavMeshPatrolComponent,
                AnimatorComponent, AudioSourceComponent>();
}

OrderingHints BehaviorTreeSystem::GetOrder() const
{
    // Sensor の結果を読み、Patrol より先に agent を掴む。
    return OrderingHints{}
        .After<NavMeshSensorSystem>()
        .Before<NavMeshPatrolSystem>();
}

void BehaviorTreeSystem::Update(SystemContext& ctx)
{
    Scene& scene = ctx.scene;
    SceneActionHandler actions(scene, ctx.world);

    for (EntityID eid : scene.GetEntities<BehaviorTreeComponent>()) {
        auto* bt = scene.GetComponent<BehaviorTreeComponent>(eid);
        auto* go = scene.GetGameObject(eid);
        if (!bt || !go || !go->activeInHierarchy() || !bt->enabled) continue;

        // ── ロード / 再ロード ────────────────────────────────────────────────
        if (!bt->initialized || bt->reloadRequested || bt->loadedTreePath != bt->treePath) {
            bt->runtime         = AcquireTree(bt->treePath);
            bt->loadedTreePath  = bt->treePath;
            bt->initialized     = true;
            bt->reloadRequested = false;
            bt->tickCount       = 0;
            bt->elapsedTime     = 0.0f;

            if (bt->runtime) {
                bt->state.Resize(bt->runtime->NodeCount());
                bt->blackboard.Reset(bt->runtime->blackboard);
                bt->lastNodeStatus.assign(bt->runtime->NodeCount(), 0);

                // 個体ごとに RNG の種を変える (同じ木でも選択がばらける)。
                bt->state.rngState = 0x9E3779B9u ^ (eid.index * 2654435761u);

                // ── 評価位相の分散 ──────────────────────────────────────────
                // WHY 黄金比を使うか: index % N は連番スポーンで全個体が同じ剰余に
                //     落ち、結局同じフレームに集中する。無理数倍の小数部を使うと
                //     連番でも均等にばらける (低食い違い列)。
                const float phase = std::fmod(static_cast<float>(eid.index) * 0.6180339887f, 1.0f);
                bt->tickTimer = bt->tickRate * phase;
            } else {
                bt->state.Clear();
                bt->lastNodeStatus.clear();
            }
        }

        if (!bt->runtime || bt->startPaused) continue;

        if (bt->restartRequested) {
            bt->state.Clear();
            bt->restartRequested = false;
        }

        // ── tickRate による間引き ────────────────────────────────────────────
        float tickDt = ctx.dt;
        if (bt->tickRate > 0.0f) {
            bt->tickTimer -= ctx.dt;
            if (bt->tickTimer > 0.0f) continue;

            // 実際に経過した時間を木へ渡す。
            // WHY dt をそのまま渡さないか: Wait や Cooldown が tickRate 間隔で
            //     呼ばれるのに 1 フレーム分の dt を受け取ると、待ち時間が
            //     tickRate/dt 倍に伸びる。
            tickDt = bt->tickRate - bt->tickTimer;
            bt->tickTimer += bt->tickRate;
        }

        bt->elapsedTime += tickDt;
        ++bt->tickCount;

        // ── 知覚結果を Blackboard へ ────────────────────────────────────────
        bt->blackboard.SetTick(bt->tickCount);
        bt->blackboard.SetTime(bt->elapsedTime);
        PopulateReservedKeys(scene, eid, *go, *bt);

        // ── 評価 ────────────────────────────────────────────────────────────
        ai::BTTickContext tickCtx;
        tickCtx.scene         = &scene;
        tickCtx.world         = &ctx.world;
        tickCtx.self          = eid;
        tickCtx.dt            = tickDt;
        tickCtx.tick          = bt->tickCount;
        tickCtx.actions       = &actions;
        tickCtx.outNodeStatus = &bt->lastNodeStatus;

        bt->lastRootStatus =
            ai::TickBehaviorTree(*bt->runtime, bt->state, bt->blackboard, tickCtx);
    }
}

} // namespace fbzz::scene
