/// @file    NavigationSystem.cpp
/// @brief   NavMeshAgentComponent の毎フレーム更新: パス計算 (A* + Funnel Algorithm) と移動 (Steering)。
/// @author  Hasegawa Jin
/// @date    2026-06-17
/// @note 複数 NavMeshSurface: agentTypeId が一致する Surface を各 Agent が個別に選択する。
/// @note オフメッシュリンク: NavMesh::offMeshLinks を A* のエッジとして扱い、TRAVERSING_LINK 状態で補間移動。
/// @note NavMesh スナップ: snapToNavMesh=true のとき移動後に NavMesh 面の Y へ補正する。
/// @note Agent に親 GO がある場合は PhysicsSystem と同じ式で world pose を親ローカルへ逆変換して書き戻す。
#include "Engine/Scene/Systems/NavigationSystem.hpp"
#include "WorldPoseHelpers.hpp"
#include "Engine/Core/Scheduler/SystemContext.hpp"
#include "Engine/Scene/Systems/NavMeshQuery.hpp"
#include "Engine/Scene/Systems/NavMeshPatrolSystem.hpp"
#include "Engine/Core/Logger.hpp"
#include "Engine/Scene/Scene.hpp"
#include "Engine/Scene/GameObject.hpp"
#include "Engine/Scene/Components/NavMeshSurfaceComponent.hpp"
#include "Engine/Scene/Components/NavMeshAgentComponent.hpp"
#include "Engine/Scene/Components/ColliderComponent.hpp"
#include "Engine/Scene/ScriptComponent.hpp"
#include "Engine/Scene/Systems/TransformSystem.hpp"
#include <Math/Vector3.hpp>
#include <Math/Quaternion.hpp>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <queue>
#include <utility>
#include <vector>

namespace fbzz::scene {

using world_pose_detail::WriteWorldPoseToTransform;

namespace {

/// @note 最近傍ポリゴン / A* / Funnel 平滑化 / 高さサンプルは NavMeshQuery.hpp へ移した。AI
/// @note (Command Bus の navmesh.path) と Editor から、Agent が実際に使うのと同一の経路計算へ
/// @note 問い合わせられるようにするため (同じ fbzz::scene 名前空間なので呼び出し側は変えない)。

/// @note オフメッシュリンクを含む経路を Funnel Algorithm + リンク補間で構築する。
/// @note BuildFunnelPath のセグメントを link で分割し、各セグメントの結果を連結する。
struct PathWithLinks {
    std::vector<math::Vector3> waypoints;
    std::vector<bool>          linkFlags; ///< @note waypoints[i] が true → オフメッシュリンク起点
    std::vector<float>         linkTimes; ///< @note リンク起点のみ有効な通過時間
};

PathWithLinks BuildPathWithLinks(
    const NavMesh& navMesh,
    const std::vector<int>& polyPath,
    const math::Vector3& startPos,
    const math::Vector3& goalPos)
{
    PathWithLinks result;
    if (polyPath.empty()) {
        result.waypoints  = { startPos, goalPos };
        result.linkFlags  = { false, false };
        result.linkTimes  = { 0.0f,   0.0f   };
        return result;
    }

    auto appendFunnel = [&](const std::vector<int>& seg,
                             const math::Vector3& from,
                             const math::Vector3& to) {
        const auto wps = BuildFunnelPath(navMesh, seg, from, to);
        const bool isFirst = result.waypoints.empty();
        for (size_t j = isFirst ? 0u : 1u; j < wps.size(); ++j) {
            result.waypoints.push_back(wps[j]);
            result.linkFlags.push_back(false);
            result.linkTimes.push_back(0.0f);
        }
    };

    int segStart = 0;
    math::Vector3 segStartPos = startPos;

    for (int i = 0; i + 1 < static_cast<int>(polyPath.size()); ++i) {
        const int fromPoly = polyPath[static_cast<size_t>(i)];
        const int toPoly   = polyPath[static_cast<size_t>(i + 1)];

        bool hasPortal = false;
        for (const auto& p : navMesh.polygons[static_cast<size_t>(fromPoly)].portals)
            if (p.neighbor == toPoly) { hasPortal = true; break; }
        if (hasPortal) continue;

        for (const auto& link : navMesh.offMeshLinks) {
            const bool fwd = (link.polyA == fromPoly && link.polyB == toPoly);
            const bool bwd = link.bidirectional && (link.polyB == fromPoly && link.polyA == toPoly);
            if (!fwd && !bwd) continue;

            const math::Vector3 posA = fwd ? link.posA : link.posB;
            const math::Vector3 posB = fwd ? link.posB : link.posA;

            appendFunnel(std::vector<int>(polyPath.begin() + segStart,
                                          polyPath.begin() + i + 1),
                         segStartPos, posA);
            if (!result.linkFlags.empty()) {
                result.linkFlags.back() = true;
                result.linkTimes.back() = link.traversalTime;
            }
            result.waypoints.push_back(posB);
            result.linkFlags.push_back(false);
            result.linkTimes.push_back(0.0f);

            segStart    = i + 1;
            segStartPos = posB;
            break;
        }
    }

    appendFunnel(std::vector<int>(polyPath.begin() + segStart, polyPath.end()),
                 segStartPos, goalPos);
    return result;
}

/// @note パスのみ放棄して停止する (Stop() と異なり target は保持する)。
/// @note Target Follow 中の「到達」「パス失敗」両方で使う: target が再び動けば repathTimer の
/// @note タイミングで自動的に追跡を再開できるようにする。
void HaltKeepingTarget(NavMeshAgentComponent& agent)
{
    agent.hasDestination = false;
    agent.path.clear();
    agent.pathLinkFlags.clear();
    agent.pathLinkTimes.clear();
    agent.linkTraversal.active = false;
    agent.currentWaypoint = 0;
    agent.currentSpeed = 0.0f;
    agent.remainingDistance = 0.0f;
    agent.state = NavMeshAgentState::IDLE;
}

/// @brief GO に付いているコライダーの底面から GO 原点までの Y オフセット (ワールド)。
/// @note snapToNavMesh で足元を NavMesh 面に合わせるために使う。Inspector の寸法はスケールを
/// @note 掛ける前の値で、実際の当たり判定は ColliderSync がスケールを掛けたものなので、
/// @note worldScale を掛けずに使うとスケール 2 のキャラクターが床へ半分めり込む。
static float ColliderFloorOffset(GameObject& go)
{
    const math::Vector3& s = go.transform.worldScale;
    const float sx = std::abs(s.x), sy = std::abs(s.y), sz = std::abs(s.z);

    if (const auto* cap = go.GetComponent<scene::CapsuleColliderComponent>())
        return cap->halfHeight * sy + cap->radius * std::max(sx, sz) - cap->center.y * sy;
    if (const auto* cyl = go.GetComponent<scene::CylinderColliderComponent>())
        return (cyl->halfHeight - cyl->center.y) * sy;
    if (const auto* box = go.GetComponent<scene::BoxColliderComponent>())
        return (box->size.y * 0.5f - box->center.y) * sy;
    if (const auto* sph = go.GetComponent<scene::SphereColliderComponent>())
        return sph->radius * std::max({ sx, sy, sz }) - sph->center.y * sy;
    return 0.0f;
}




/// @note callback: Script のメンバ関数ポインタ (OnNavMeshDestinationReached / OnNavMeshPathFailed)。
void NotifyScripts(Scene& scene, EntityID eid, GameObject& go, void (Script::*callback)(), ScriptCallbackKind kind)
{
    auto* scriptComp = scene.GetComponent<ScriptComponent>(eid);
    if (!scriptComp) return;
    for (auto& entry : scriptComp->scripts) {
        if (!entry.script || !entry.script->enabled) continue;
        entry.script->SetContext(&scene, &go);
        entry.script->ExecuteProfiledCallback(callback, kind, "navigation callback");
    }
}

/// @name 近接 Agent 検索用の簡易空間ハッシュ
/// @note ローカル回避の毎フレーム全 Agent 総当たり (O(n^2)) はコストが無視できないため、
/// @note セル幅 bucketSize の格子へ 1 回だけ登録し近傍 3x3 セルだけを調べる (平均 O(n))。
/// @note フレーム開始時点の位置で作るため、同フレーム内の他 Agent の移動には対応しない
/// @note (1 フレーム遅れるが回避用途では許容できる近似)。
struct BucketKey {
    int x = 0, z = 0;
    bool operator==(const BucketKey&) const = default;
};

BucketKey BucketKeyFor(const math::Vector3& pos, float bucketSize)
{
    return { static_cast<int>(std::floor(pos.x / bucketSize)), static_cast<int>(std::floor(pos.z / bucketSize)) };
}

std::uint64_t TargetDiagnosticSignature(const EntityRef& target, std::uint64_t reason)
{
    std::uint64_t hash = 14695981039346656037ull;
    const auto mix = [&hash](std::uint64_t value) {
        for (std::size_t byte = 0; byte < sizeof(value); ++byte) {
            hash ^= static_cast<std::uint8_t>(value >> (byte * 8));
            hash *= 1099511628211ull;
        }
    };
    for (const unsigned char character : target.guid) {
        hash ^= character;
        hash *= 1099511628211ull;
    }
    mix(target.id.index);
    mix(target.id.generation);
    mix(target.sceneGeneration);
    mix(reason);
    return hash == 0 ? 1 : hash;
}

void ReportInvalidTarget(NavMeshAgentComponent& agent, EntityID agentId,
    std::uint64_t reason, const char* message)
{
    const std::uint64_t signature = TargetDiagnosticSignature(agent.target, reason);
    if (signature == agent.lastTargetDiagnosticSignature) return;
    agent.lastTargetDiagnosticSignature = signature;
    FBZZ_LOG_WARN("NavigationSystem: agent %u:%u target is %s.",
        agentId.index, agentId.generation, message);
}

} /// @note namespace

ComponentAccess NavigationSystem::GetAccess() const
{
    return ComponentAccess{}
        .Reads<NavMeshSurfaceComponent, ScriptComponent, CapsuleColliderComponent,
            CylinderColliderComponent, BoxColliderComponent, SphereColliderComponent>()
        .Writes<NavMeshAgentComponent, Transform>();
}

OrderingHints NavigationSystem::GetOrder() const
{
    return OrderingHints{}.After<NavMeshPatrolSystem>();
}

NavigationSystem::AvoidanceBucket& NavigationSystem::FindAvoidanceBucket(int x, int z)
{
    const size_t hash = (static_cast<size_t>(static_cast<uint32_t>(x)) * 73856093u)
        ^ (static_cast<size_t>(static_cast<uint32_t>(z)) * 19349663u);
    size_t index = hash & (m_avoidanceBuckets.size() - 1);
    while (m_avoidanceBuckets[index].first != SIZE_MAX
        && (m_avoidanceBuckets[index].x != x || m_avoidanceBuckets[index].z != z))
        index = (index + 1) & (m_avoidanceBuckets.size() - 1);
    return m_avoidanceBuckets[index];
}

NavigationSystem::SurfaceEntry& NavigationSystem::FindSurfaceSlot(int agentTypeId)
{
    size_t index = static_cast<size_t>(static_cast<uint32_t>(agentTypeId)) & (m_surfaces.size() - 1);
    while (m_surfaces[index].surface && m_surfaces[index].agentTypeId != agentTypeId)
        index = (index + 1) & (m_surfaces.size() - 1);
    return m_surfaces[index];
}

void NavigationSystem::Update(SystemContext& ctx)
{
    Scene& scene = ctx.scene;
    const float dt = ctx.dt;
    const auto agentSpan = scene.GetEntities<NavMeshAgentComponent>();
    if (agentSpan.empty()) return;
    /// @note 通知中の Component 追加で元 span が再確保されても、フレーム先頭の処理対象と順序を保つ。
    m_agentEntities.assign(agentSpan.begin(), agentSpan.end());
    const auto& agentEntities = m_agentEntities;
    FlushWorldTransforms(scene);
    /// @note agentTypeId → NavMeshSurface のマップを構築する。
    /// @note 同じ typeId が複数ある場合は最初に見つかった有効な Surface を使う。
    m_surfaces.clear();
    const auto surfaceEntities = scene.GetEntities<NavMeshSurfaceComponent>();
    if (!surfaceEntities.empty()) {
        size_t surfaceCount = 8;
        while (surfaceCount / 2 < surfaceEntities.size()) surfaceCount *= 2;
        m_surfaces.assign(surfaceCount, {});
    }
    float minCellSize = 1.0f;
    for (EntityID veid : surfaceEntities) {
        auto* v = scene.GetComponent<NavMeshSurfaceComponent>(veid);
        auto* surfaceGo = scene.GetGameObject(veid);
        if (!v || !surfaceGo || !surfaceGo->activeInHierarchy()
            || !v->enabled || !v->navMesh.IsValid()) continue;
        auto& entry = FindSurfaceSlot(v->agentTypeId);
        if (!entry.surface) {
            entry = {v->agentTypeId, v};
            minCellSize = std::min(minCellSize, v->cellSize);
        }
    }
    /// @note 占有率を半分以下に保つ flat hash と入力順の連結添字で、セルごとの確保と再ソートを避ける。
    const float bucketSize = std::max(0.5f, minCellSize * 2.0f);
    m_avoidanceEntries.clear();
    size_t bucketCount = 8;
    while (bucketCount / 2 < agentEntities.size()) bucketCount *= 2;
    m_avoidanceBuckets.assign(bucketCount, {});
    for (size_t order = 0; order < agentEntities.size(); ++order) {
        const EntityID id = agentEntities[order];
        auto* a = scene.GetComponent<NavMeshAgentComponent>(id);
        auto* g = scene.GetGameObject(id);
        if (!a || !g || !g->activeInHierarchy() || !a->enabled) continue;
        const auto key = BucketKeyFor(g->transform.worldPosition, bucketSize);
        auto& bucket = FindAvoidanceBucket(key.x, key.z);
        const size_t entryIndex = m_avoidanceEntries.size();
        m_avoidanceEntries.push_back({id, SIZE_MAX});
        if (bucket.first == SIZE_MAX) {
            bucket.x = key.x;
            bucket.z = key.z;
            bucket.first = entryIndex;
        } else {
            m_avoidanceEntries[bucket.last].next = entryIndex;
        }
        bucket.last = entryIndex;
    }

    for (EntityID eid : agentEntities) {
        auto* agent = scene.GetComponent<NavMeshAgentComponent>(eid);
        auto* go    = scene.GetGameObject(eid);
        if (!agent || !go || !go->activeInHierarchy() || !agent->enabled) continue;

        const bool hasAssignedTarget = agent->target.IsAssigned();
        GameObject* targetGo = nullptr;
        if (hasAssignedTarget) {
            targetGo = agent->target.Resolve(scene);
            if (!targetGo) {
                ReportInvalidTarget(*agent, eid, 1, "unresolved");
                HaltKeepingTarget(*agent);
                continue;
            }
            if (targetGo == go || targetGo->IsDescendantOf(*go)) {
                ReportInvalidTarget(*agent, eid, 2, "inside its own hierarchy");
                HaltKeepingTarget(*agent);
                continue;
            }
            if (!targetGo->activeInHierarchy()) {
                ReportInvalidTarget(*agent, eid, 3, "inactive");
                HaltKeepingTarget(*agent);
                continue;
            }
            agent->lastTargetDiagnosticSignature = 0;
        } else {
            agent->lastTargetDiagnosticSignature = 0;
        }

        /// @note この Agent が使う NavMeshSurface を agentTypeId で引く。
        /// @note 対応する Surface がなければスキップ (agentTypeId の Surface をまだ置いていない場合等)。
        if (m_surfaces.empty()) continue;
        const auto* surface = FindSurfaceSlot(agent->agentTypeId).surface;
        if (!surface) continue;
        const NavMeshSurfaceComponent& surf = *surface;
        const NavMesh& navMesh = surf.navMesh;
        const float agentCellSize = surf.cellSize;

        /// @name Target Follow: 追跡対象が設定されていれば定期的に destination を更新する
        if (hasAssignedTarget) {
            agent->repathTimer -= dt;
            const float movedSq = (targetGo->transform.worldPosition - agent->lastTargetPos).LengthSq();
            const float repathMoveDist = agentCellSize;
            if (agent->repathTimer <= 0.0f || movedSq > repathMoveDist * repathMoveDist) {
                const EntityRef savedTarget = agent->target;
                agent->SetDestination(targetGo->transform.worldPosition);
                agent->target = savedTarget;
                agent->lastTargetPos = targetGo->transform.worldPosition;
                agent->repathTimer = agent->repathInterval;
            }
        }

        /// @name パス未計算ならここで A* + Funnel を実行する
        if (agent->hasDestination && agent->path.empty() && agent->state == NavMeshAgentState::MOVING) {
            const int startPoly = FindNearestPolygon(navMesh, go->transform.worldPosition);
            const int goalPoly  = FindNearestPolygon(navMesh, agent->destination);

            std::vector<int> polyPath;
            if (startPoly >= 0 && goalPoly >= 0 && FindPolygonPath(navMesh, startPoly, goalPoly, polyPath,
                                                                    agent->areaMask, surf.areaCosts, agent->agentTypeId)) {
                PathWithLinks pwl = BuildPathWithLinks(navMesh, polyPath,
                                                       go->transform.worldPosition, agent->destination);
                pwl.waypoints.erase(pwl.waypoints.begin());
                pwl.linkFlags.erase(pwl.linkFlags.begin());
                pwl.linkTimes.erase(pwl.linkTimes.begin());
                agent->path           = std::move(pwl.waypoints);
                agent->pathLinkFlags  = std::move(pwl.linkFlags);
                agent->pathLinkTimes  = std::move(pwl.linkTimes);
                agent->currentWaypoint = 0;
            } else {
                if (hasAssignedTarget) HaltKeepingTarget(*agent);
                else agent->Stop();
                NotifyScripts(scene, eid, *go, &Script::OnNavMeshPathFailed, ScriptCallbackKind::NAV_PATH_FAILED);
                continue;
            }
        }

        if (agent->path.empty() || agent->currentWaypoint >= agent->path.size()) {
            /// @note パスがない(Idle)状態でもスナップは毎フレーム適用する。
            /// @note 例: 初期配置でエージェントが NavMesh より高い位置にいる場合に地面へ落とす。
            if (agent->snapToNavMesh && agent->updatePosition
                && agent->state != NavMeshAgentState::TRAVERSING_LINK) {
                const float snappedY = SampleNavMeshHeight(navMesh, go->transform.worldPosition);
                if (snappedY > -1e6f) {
                    math::Vector3 p = go->transform.worldPosition;
                    p.y = snappedY + ColliderFloorOffset(*go);
                    WriteWorldPoseToTransform(*go, p, go->transform.worldRotation);
                }
            }
            continue;
        }

        const math::Vector3 pos = go->transform.worldPosition;

        /// @note 残り距離は isStopped 中も Script/Inspector から参照できるよう毎フレーム更新する。
        {
            float remaining = (agent->path[agent->currentWaypoint] - pos).Length();
            for (size_t i = agent->currentWaypoint; i + 1 < agent->path.size(); ++i)
                remaining += (agent->path[i + 1] - agent->path[i]).Length();
            agent->remainingDistance = remaining;
        }

        /// @name オフメッシュリンク通過: lerp 補間で startPos → endPos へ移動する
        if (agent->state == NavMeshAgentState::TRAVERSING_LINK) {
            NavMeshLinkTraversal& lt = agent->linkTraversal;
            lt.elapsed += dt;
            const float t = std::clamp(lt.elapsed / std::max(0.001f, lt.totalTime), 0.0f, 1.0f);
            math::Vector3 linkPos;
            linkPos.x = lt.startPos.x + (lt.endPos.x - lt.startPos.x) * t;
            linkPos.y = lt.startPos.y + (lt.endPos.y - lt.startPos.y) * t;
            linkPos.z = lt.startPos.z + (lt.endPos.z - lt.startPos.z) * t;

            const math::Vector3 dir = lt.endPos - lt.startPos;
            math::Quaternion newRot = go->transform.worldRotation;
            if (dir.x * dir.x + dir.z * dir.z > 0.0001f) {
                const math::Vector3 flatDir(dir.x, 0.0f, dir.z);
                const math::Quaternion targetRot = math::Quaternion::LookRotation(flatDir.Normalized());
                newRot = math::Quaternion::Slerp(newRot, targetRot,
                    std::clamp(agent->angularSpeedDeg * dt / 180.0f, 0.0f, 1.0f));
            }
            agent->velocity = (dt > 0.0f) ? ((linkPos - go->transform.worldPosition) * (1.0f / dt)) : math::Vector3::ZERO;
            if (agent->updatePosition || agent->updateRotation)
                WriteWorldPoseToTransform(*go,
                    agent->updatePosition ? linkPos : go->transform.worldPosition,
                    agent->updateRotation ? newRot  : go->transform.worldRotation);

            if (t >= 1.0f) {
                lt.active = false;
                agent->state = NavMeshAgentState::MOVING;
                /// @note link endPos ウェイポイントへ進める
                ++agent->currentWaypoint;
            }
            continue;
        }

        /// @note isStopped 中はパスを保持したまま移動・回転だけ止める (Resume() で再開できる)。
        if (agent->isStopped) {
            agent->currentSpeed = 0.0f;
            agent->velocity     = math::Vector3::ZERO;
            if (agent->snapToNavMesh && agent->updatePosition) {
                const float snappedY = SampleNavMeshHeight(navMesh, go->transform.worldPosition);
                if (snappedY > -1e6f) {
                    math::Vector3 p = go->transform.worldPosition;
                    p.y = snappedY + ColliderFloorOffset(*go);
                    WriteWorldPoseToTransform(*go, p, go->transform.worldRotation);
                }
            }
            continue;
        }

        /// @name 停滞検出: 一定時間以上前進できていなければ経路を再計算する
        {
            const float movedSq = (pos - agent->lastStuckCheckPos).LengthSq();
            /// @note 5cm
            constexpr float kStuckMoveThresholdSq = 0.05f * 0.05f;
            /// @note [s]
            constexpr float kStuckTimeout = 2.0f;
            if (movedSq > kStuckMoveThresholdSq) {
                agent->lastStuckCheckPos = pos;
                agent->stuckTimer = 0.0f;
                agent->isStuck = false;
            } else {
                agent->stuckTimer += dt;
                if (agent->stuckTimer > kStuckTimeout) {
                    agent->isStuck = true;
                    agent->stuckTimer = 0.0f;
                    agent->path.clear();
                    agent->currentWaypoint = 0;
                    /// @note 次フレームで現在地から再計算する
                    continue;
                }
            }
        }

        /// @name 到達判定
        math::Vector3 target = agent->path[agent->currentWaypoint];
        math::Vector3 toTarget = target - pos;
        toTarget.y = 0.0f;
        float distance = toTarget.Length();

        bool isLastWaypoint = (agent->currentWaypoint + 1 == agent->path.size());
        float arriveThreshold = isLastWaypoint ? agent->stoppingDistance : (agentCellSize * 0.5f);

        if (distance <= arriveThreshold) {
            if (isLastWaypoint) {
                agent->destinationReached = true;
                if (hasAssignedTarget) HaltKeepingTarget(*agent);
                else agent->Stop();
                NotifyScripts(scene, eid, *go, &Script::OnNavMeshDestinationReached, ScriptCallbackKind::NAV_DESTINATION_REACHED);
                continue;
            }

            /// @note オフメッシュリンク起点に到達したらリンク通過を開始する。
            const size_t wp = agent->currentWaypoint;
            if (wp < agent->pathLinkFlags.size() && agent->pathLinkFlags[wp]) {
                const size_t nextWp = wp + 1;
                if (nextWp < agent->path.size()) {
                    NavMeshLinkTraversal& lt = agent->linkTraversal;
                    lt.startPos  = pos;
                    lt.endPos    = agent->path[nextWp];
                    lt.totalTime = (wp < agent->pathLinkTimes.size()) ? agent->pathLinkTimes[wp] : 0.3f;
                    lt.elapsed   = 0.0f;
                    lt.active    = true;
                    agent->state = NavMeshAgentState::TRAVERSING_LINK;
                    /// @note currentWaypoint は TRAVERSING_LINK 完了時に ++する
                    continue;
                }
            }

            ++agent->currentWaypoint;
            target = agent->path[agent->currentWaypoint];
            toTarget = target - pos; toTarget.y = 0.0f;
            distance = toTarget.Length();
        }

        math::Vector3 moveDir = (distance > 0.0001f) ? toTarget.Normalized() : math::Vector3::ZERO;

        /// @name 簡易ローカル回避: 近傍 3x3 バケットの Agent から離れる方向を加算する
        math::Vector3 avoidance = math::Vector3::ZERO;
        const BucketKey selfBucket = BucketKeyFor(pos, bucketSize);
        for (int bz = -1; bz <= 1; ++bz) {
        for (int bx = -1; bx <= 1; ++bx) {
            const BucketKey key{selfBucket.x + bx, selfBucket.z + bz};
            const auto& bucket = FindAvoidanceBucket(key.x, key.z);
            for (size_t index = bucket.first; index != SIZE_MAX; index = m_avoidanceEntries[index].next) {
                const EntityID otherEid = m_avoidanceEntries[index].entity;
                if (otherEid == eid) continue;
                auto* other   = scene.GetComponent<NavMeshAgentComponent>(otherEid);
                auto* otherGo = scene.GetGameObject(otherEid);
                if (!other || !otherGo || !other->enabled) continue;

                math::Vector3 diff = pos - otherGo->transform.worldPosition;
                diff.y = 0.0f;
                const float minDist = agent->radius + other->radius;
                const float distSq  = diff.x * diff.x + diff.z * diff.z;
                if (distSq < minDist * minDist && distSq > 0.0001f) {
                    const float d = std::sqrt(distSq);
                    /// @note 優先度が高い Agent ほど押し出されにくい (例: プレイヤー追跡中の敵が
                    /// @note 雑魚敵に押されて止まらないようにする)。
                    float weight = 1.0f;
                    if (other->avoidancePriority > agent->avoidancePriority)      weight = 1.5f;
                    else if (other->avoidancePriority < agent->avoidancePriority) weight = 0.5f;
                    avoidance += diff * (((minDist - d) / d) * weight);
                }
            }
        }
        }
        const math::Vector3 combined = moveDir + avoidance;
        if (combined.LengthSq() > 0.0001f)
            moveDir = combined.Normalized();

        /// @name 速度更新 (加速) と移動
        /// @note autoBraking: 最終ウェイポイントへの残り距離に応じて maxSpeed を制限する。
        /// @note ブレーキ開始距離 = v² / (2a) として、その範囲内に入ったら速度を絞る。
        float effectiveMaxSpeed = agent->maxSpeed;
        if (agent->autoBraking && isLastWaypoint && agent->acceleration > 0.0f) {
            const float brakeRadius = (agent->maxSpeed * agent->maxSpeed) / (2.0f * agent->acceleration);
            if (distance < brakeRadius && brakeRadius > 0.0f)
                effectiveMaxSpeed = agent->maxSpeed * (distance / brakeRadius);
            /// @note 最低速度を確保
            effectiveMaxSpeed = std::max(effectiveMaxSpeed, agent->maxSpeed * 0.1f);
        }
        agent->currentSpeed = std::min(agent->currentSpeed + agent->acceleration * dt, effectiveMaxSpeed);
        const float moveDist = std::min(agent->currentSpeed * dt, distance);

        math::Vector3 newPos = pos + moveDir * moveDist;
        const float t = (distance > 0.0001f) ? std::min(1.0f, moveDist / distance) : 0.0f;
        newPos.y = pos.y + (target.y - pos.y) * t;

        /// @note NavMesh スナップ: 移動後の Y 座標を NavMesh ポリゴン面に合わせる。
        /// @note 物理・重力と組み合わせる場合は snapToNavMesh=false にして物理側に Y を任せる。
        if (agent->snapToNavMesh) {
            const float snappedY = SampleNavMeshHeight(navMesh, newPos);
            if (snappedY > -1e6f) newPos.y = snappedY + ColliderFloorOffset(*go);
        }

        /// @name 回転 (進行方向への定角速度補間)
        /// @note 2*acos(|dot(a,b)|) で実際の回転角を求め、angularSpeedDeg [deg/s] に正確に
        /// @note 一致する補間係数 t を計算する (角度や dt に依らず一定の角速度で回転する)。
        math::Quaternion newRot = go->transform.worldRotation;
        if (moveDir.LengthSq() > 0.0001f) {
            const math::Quaternion targetRot = math::Quaternion::LookRotation(moveDir);
            const float cosHalfAngle = std::clamp(
                std::abs(math::Quaternion::Dot(newRot, targetRot)), 0.0f, 1.0f);
            const float angleDeg = 2.0f * std::acos(cosHalfAngle) * (180.0f / 3.14159265f);
            const float rotT = (angleDeg > 0.0001f)
                ? std::clamp((agent->angularSpeedDeg * dt) / angleDeg, 0.0f, 1.0f)
                : 1.0f;
            newRot = math::Quaternion::Slerp(newRot, targetRot, rotT);
        }

        /// @note 速度ベクトル更新 (アニメーター連携などに使う)
        agent->velocity = (dt > 0.0f) ? ((newPos - pos) * (1.0f / dt)) : math::Vector3::ZERO;

        if (agent->updatePosition && agent->updateRotation)
            WriteWorldPoseToTransform(*go, newPos, newRot);
        else if (agent->updatePosition)
            WriteWorldPoseToTransform(*go, newPos, go->transform.worldRotation);
        else if (agent->updateRotation)
            WriteWorldPoseToTransform(*go, go->transform.worldPosition, newRot);
    }
    m_agentEntities.clear();
    m_surfaces.clear();
    m_avoidanceEntries.clear();
    m_avoidanceBuckets.clear();
}

} /// @note namespace fbzz::scene
