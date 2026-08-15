// FBZZ Engine
// NavigationSystem.cpp | fbzz::scene
// NavMeshAgentComponent の毎フレーム更新: パス計算 (A* + Funnel Algorithm) と移動 (Steering)。
//
// 複数 NavMeshSurface: agentTypeId が一致する Surface を各 Agent が個別に選択する。
// オフメッシュリンク: NavMesh::offMeshLinks を A* のエッジとして扱い、TRAVERSING_LINK 状態で補間移動。
// NavMesh スナップ: snapToNavMesh=true のとき移動後に NavMesh 面の Y へ補正する。
// Agent に親 GO がある場合は PhysicsSystem と同じ式で world pose を親ローカルへ逆変換して書き戻す。
#include "Engine/Scene/Systems/NavigationSystem.hpp"
#include "Engine/Core/Scheduler/SystemContext.hpp"
#include "Engine/Scene/Systems/NavMeshQuery.hpp"
#include "Engine/Scene/Systems/NavMeshPatrolSystem.hpp"
#include "Engine/Scene/Scene.hpp"
#include "Engine/Scene/GameObject.hpp"
#include "Engine/Scene/Components/NavMeshSurfaceComponent.hpp"
#include "Engine/Scene/Components/NavMeshAgentComponent.hpp"
#include "Engine/Scene/Components/ColliderComponent.hpp"
#include "Engine/Scene/ScriptComponent.hpp"
#include <Math/Vector3.hpp>
#include <Math/Quaternion.hpp>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <queue>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fbzz::scene {

namespace {

// 最近傍ポリゴン / A* / Funnel 平滑化 / 高さサンプルは NavMeshQuery.hpp へ移した。
// WHY: AI (Command Bus の navmesh.path) と Editor から、Agent が実際に使うのと
//      同一の経路計算へ問い合わせられるようにするため。ここから呼ぶ側の記述は変えない
//      (同じ fbzz::scene 名前空間なので非修飾呼び出しのまま解決する)。

// オフメッシュリンクを含む経路を Funnel Algorithm + リンク補間で構築する。
// BuildFunnelPath のセグメントを link で分割し、各セグメントの結果を連結する。
struct PathWithLinks {
    std::vector<math::Vector3> waypoints;
    std::vector<bool>          linkFlags; // waypoints[i] が true → オフメッシュリンク起点
    std::vector<float>         linkTimes; // リンク起点のみ有効な通過時間
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

// パスのみ放棄して停止する (Stop() と異なり target は保持する)。
// Target Follow 中の「到達」「パス失敗」両方で使う: target が再び動けば repathTimer の
// タイミングで自動的に追跡を再開できるようにする。
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

// GO に付いているコライダーの底面から GO 原点までの Y オフセットを返す。
// snapToNavMesh でエージェントの足元を NavMesh 面に合わせるために使う。
static float ColliderFloorOffset(GameObject& go)
{
    if (const auto* cap = go.GetComponent<scene::CapsuleColliderComponent>())
        return cap->halfHeight + cap->radius - cap->center.y;
    if (const auto* box = go.GetComponent<scene::BoxColliderComponent>())
        return box->size.y * 0.5f - box->center.y;
    if (const auto* sph = go.GetComponent<scene::SphereColliderComponent>())
        return sph->radius - sph->center.y;
    return 0.0f;
}

// World pose を Transform へ書き込む。親 GO があれば PhysicsSystem::WriteWorldPoseToTransform と
// 同じ式で親ローカル空間へ逆変換する (TransformSystem は次フレームまで動かないため、
// 同フレームの Collider 位置・Script 参照との整合性を保つために world 側も直接更新する)。
void WriteWorldPoseToTransform(GameObject& go, const math::Vector3& worldPosition, const math::Quaternion& worldRotation)
{
    auto& tf = go.transform;
    if (auto* parent = go.GetParent()) {
        const auto& parentTf = parent->transform;
        const math::Quaternion invParentRot = parentTf.worldRotation.Inverse();
        const math::Vector3 parentSpace = invParentRot * (worldPosition - parentTf.worldPosition);

        tf.position = {
            parentTf.worldScale.x == 0.0f ? 0.0f : parentSpace.x / parentTf.worldScale.x,
            parentTf.worldScale.y == 0.0f ? 0.0f : parentSpace.y / parentTf.worldScale.y,
            parentTf.worldScale.z == 0.0f ? 0.0f : parentSpace.z / parentTf.worldScale.z
        };
        tf.rotation = (invParentRot * worldRotation).Normalized();
    } else {
        tf.position = worldPosition;
        tf.rotation = worldRotation;
    }

    tf.worldPosition = worldPosition;
    tf.worldRotation = worldRotation;
}

// callback: Script のメンバ関数ポインタ (OnNavMeshDestinationReached / OnNavMeshPathFailed)。
void NotifyScripts(Scene& scene, EntityID eid, GameObject& go, void (Script::*callback)())
{
    auto* scriptComp = scene.GetComponent<ScriptComponent>(eid);
    if (!scriptComp) return;
    for (auto& entry : scriptComp->scripts) {
        if (!entry.script || !entry.script->enabled) continue;
        entry.script->SetContext(&scene, &go);
        (entry.script.get()->*callback)();
    }
}

// ── 近接 Agent 検索用の簡易空間ハッシュ ────────────────────────────────────
// WHY: ローカル回避の毎フレーム全 Agent 総当たり (O(n^2)) は Agent 数が増えると無視できない
//      コストになる。セル幅 bucketSize の格子へ Agent を 1 回だけ登録し、近傍 3x3 セルだけを
//      調べることで平均 O(n) 程度に抑える。フレーム開始時点の位置でバケットを作るため、
//      同フレーム内の他 Agent の移動結果には対応しない (1 フレーム遅れる) が、
//      回避用途では許容できる近似である。
struct BucketKey {
    int x = 0, z = 0;
    bool operator==(const BucketKey&) const = default;
};
struct BucketKeyHash {
    size_t operator()(const BucketKey& k) const
    {
        return (static_cast<size_t>(static_cast<uint32_t>(k.x)) * 73856093u)
             ^ (static_cast<size_t>(static_cast<uint32_t>(k.z)) * 19349663u);
    }
};

BucketKey BucketKeyFor(const math::Vector3& pos, float bucketSize)
{
    return { static_cast<int>(std::floor(pos.x / bucketSize)), static_cast<int>(std::floor(pos.z / bucketSize)) };
}

} // namespace

ComponentAccess NavigationSystem::GetAccess() const
{
    return ComponentAccess{}
        .Reads<NavMeshAgentComponent, NavMeshSurfaceComponent>()
        .Writes<NavMeshAgentComponent>();
}

OrderingHints NavigationSystem::GetOrder() const
{
    return OrderingHints{}.After<NavMeshPatrolSystem>();
}

void NavigationSystem::Update(SystemContext& ctx)
{
    Scene& scene = ctx.scene;
    const float dt = ctx.dt;
    // agentTypeId → NavMeshSurface のマップを構築する。
    // 同じ typeId が複数ある場合は最初に見つかった有効な Surface を使う。
    std::unordered_map<int, NavMeshSurfaceComponent*> surfaceMap;
    float minCellSize = 1.0f;
    for (EntityID veid : scene.GetEntities<NavMeshSurfaceComponent>()) {
        auto* v = scene.GetComponent<NavMeshSurfaceComponent>(veid);
        if (!v || !v->enabled || !v->navMesh.IsValid()) continue;
        if (!surfaceMap.count(v->agentTypeId)) {
            surfaceMap[v->agentTypeId] = v;
            minCellSize = std::min(minCellSize, v->cellSize);
        }
    }
    if (surfaceMap.empty()) return;

    const auto agentEntities = scene.GetEntities<NavMeshAgentComponent>();

    // 回避用の空間ハッシュをフレーム先頭で 1 回構築する。
    const float bucketSize = std::max(0.5f, minCellSize * 2.0f);
    std::unordered_map<BucketKey, std::vector<EntityID>, BucketKeyHash> avoidanceBuckets;
    for (EntityID id : agentEntities) {
        auto* a = scene.GetComponent<NavMeshAgentComponent>(id);
        auto* g = scene.GetGameObject(id);
        if (!a || !g || !a->enabled) continue;
        avoidanceBuckets[BucketKeyFor(g->transform.worldPosition, bucketSize)].push_back(id);
    }

    for (EntityID eid : agentEntities) {
        auto* agent = scene.GetComponent<NavMeshAgentComponent>(eid);
        auto* go    = scene.GetGameObject(eid);
        if (!agent || !go || !agent->enabled) continue;

        // この Agent が使う NavMeshSurface を agentTypeId で引く。
        // 対応する Surface がなければスキップ (agentTypeId の Surface をまだ置いていない場合等)。
        auto surfIt = surfaceMap.find(agent->agentTypeId);
        if (surfIt == surfaceMap.end()) continue;
        const NavMeshSurfaceComponent& surf = *surfIt->second;
        const NavMesh& navMesh = surf.navMesh;
        const float agentCellSize = surf.cellSize;

        // ── Target Follow: 追跡対象が設定されていれば定期的に destination を更新する ──
        if (agent->target.IsValid()) {
            auto* targetGo = scene.GetGameObject(agent->target);
            if (!targetGo) {
                agent->ClearTarget();
            } else {
                agent->repathTimer -= dt;
                const float movedSq = (targetGo->transform.worldPosition - agent->lastTargetPos).LengthSq();
                const float repathMoveDist = agentCellSize;
                if (agent->repathTimer <= 0.0f || movedSq > repathMoveDist * repathMoveDist) {
                    const EntityID savedTarget = agent->target; // SetDestination がクリアするので退避する
                    agent->SetDestination(targetGo->transform.worldPosition);
                    agent->target          = savedTarget;
                    agent->lastTargetPos   = targetGo->transform.worldPosition;
                    agent->repathTimer     = agent->repathInterval;
                }
            }
        }

        // ── パス未計算ならここで A* + Funnel を実行する ─────────────────────
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
                if (agent->target.IsValid()) HaltKeepingTarget(*agent);
                else agent->Stop();
                NotifyScripts(scene, eid, *go, &Script::OnNavMeshPathFailed);
                continue;
            }
        }

        if (agent->path.empty() || agent->currentWaypoint >= agent->path.size()) {
            // パスがない(Idle)状態でもスナップは毎フレーム適用する。
            // 例: 初期配置でエージェントが NavMesh より高い位置にいる場合に地面へ落とす。
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

        // 残り距離は isStopped 中も Script/Inspector から参照できるよう毎フレーム更新する。
        {
            float remaining = (agent->path[agent->currentWaypoint] - pos).Length();
            for (size_t i = agent->currentWaypoint; i + 1 < agent->path.size(); ++i)
                remaining += (agent->path[i + 1] - agent->path[i]).Length();
            agent->remainingDistance = remaining;
        }

        // ── オフメッシュリンク通過: lerp 補間で startPos → endPos へ移動する ──────────
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
                ++agent->currentWaypoint; // link endPos ウェイポイントへ進める
            }
            continue;
        }

        // isStopped 中はパスを保持したまま移動・回転だけ止める (Resume() で再開できる)。
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

        // ── 停滞検出: 一定時間以上前進できていなければ経路を再計算する ──────
        {
            const float movedSq = (pos - agent->lastStuckCheckPos).LengthSq();
            constexpr float kStuckMoveThresholdSq = 0.05f * 0.05f; // 5cm
            constexpr float kStuckTimeout = 2.0f; // [s]
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
                    continue; // 次フレームで現在地から再計算する
                }
            }
        }

        // ── 到達判定 ──────────────────────────────────────────────────────
        math::Vector3 target = agent->path[agent->currentWaypoint];
        math::Vector3 toTarget = target - pos;
        toTarget.y = 0.0f;
        float distance = toTarget.Length();

        bool isLastWaypoint = (agent->currentWaypoint + 1 == agent->path.size());
        float arriveThreshold = isLastWaypoint ? agent->stoppingDistance : (agentCellSize * 0.5f);

        if (distance <= arriveThreshold) {
            if (isLastWaypoint) {
                agent->destinationReached = true;
                if (agent->target.IsValid()) HaltKeepingTarget(*agent);
                else agent->Stop();
                NotifyScripts(scene, eid, *go, &Script::OnNavMeshDestinationReached);
                continue;
            }

            // オフメッシュリンク起点に到達したらリンク通過を開始する。
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
                    // currentWaypoint は TRAVERSING_LINK 完了時に ++する
                    continue;
                }
            }

            ++agent->currentWaypoint;
            target = agent->path[agent->currentWaypoint];
            toTarget = target - pos; toTarget.y = 0.0f;
            distance = toTarget.Length();
        }

        math::Vector3 moveDir = (distance > 0.0001f) ? toTarget.Normalized() : math::Vector3::ZERO;

        // ── 簡易ローカル回避: 近傍 3x3 バケットの Agent から離れる方向を加算する ──
        math::Vector3 avoidance = math::Vector3::ZERO;
        const BucketKey selfBucket = BucketKeyFor(pos, bucketSize);
        for (int bz = -1; bz <= 1; ++bz) {
        for (int bx = -1; bx <= 1; ++bx) {
            const auto it = avoidanceBuckets.find({ selfBucket.x + bx, selfBucket.z + bz });
            if (it == avoidanceBuckets.end()) continue;

            for (EntityID otherEid : it->second) {
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
                    // 優先度が高い Agent ほど押し出されにくい (例: プレイヤー追跡中の敵が
                    // 雑魚敵に押されて止まらないようにする)。
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

        // ── 速度更新 (加速) と移動 ───────────────────────────────────────
        // autoBraking: 最終ウェイポイントへの残り距離に応じて maxSpeed を制限する。
        // ブレーキ開始距離 = v² / (2a) として、その範囲内に入ったら速度を絞る。
        float effectiveMaxSpeed = agent->maxSpeed;
        if (agent->autoBraking && isLastWaypoint && agent->acceleration > 0.0f) {
            const float brakeRadius = (agent->maxSpeed * agent->maxSpeed) / (2.0f * agent->acceleration);
            if (distance < brakeRadius && brakeRadius > 0.0f)
                effectiveMaxSpeed = agent->maxSpeed * (distance / brakeRadius);
            effectiveMaxSpeed = std::max(effectiveMaxSpeed, agent->maxSpeed * 0.1f); // 最低速度を確保
        }
        agent->currentSpeed = std::min(agent->currentSpeed + agent->acceleration * dt, effectiveMaxSpeed);
        const float moveDist = std::min(agent->currentSpeed * dt, distance);

        math::Vector3 newPos = pos + moveDir * moveDist;
        const float t = (distance > 0.0001f) ? std::min(1.0f, moveDist / distance) : 0.0f;
        newPos.y = pos.y + (target.y - pos.y) * t;

        // NavMesh スナップ: 移動後の Y 座標を NavMesh ポリゴン面に合わせる。
        // 物理・重力と組み合わせる場合は snapToNavMesh=false にして物理側に Y を任せる。
        if (agent->snapToNavMesh) {
            const float snappedY = SampleNavMeshHeight(navMesh, newPos);
            if (snappedY > -1e6f) newPos.y = snappedY + ColliderFloorOffset(*go);
        }

        // ── 回転 (進行方向への定角速度補間) ────────────────────────────────
        // 2*acos(|dot(a,b)|) で実際の回転角を求め、angularSpeedDeg [deg/s] に正確に
        // 一致する補間係数 t を計算する (角度や dt に依らず一定の角速度で回転する)。
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

        // 速度ベクトル更新 (アニメーター連携などに使う)
        agent->velocity = (dt > 0.0f) ? ((newPos - pos) * (1.0f / dt)) : math::Vector3::ZERO;

        if (agent->updatePosition && agent->updateRotation)
            WriteWorldPoseToTransform(*go, newPos, newRot);
        else if (agent->updatePosition)
            WriteWorldPoseToTransform(*go, newPos, go->transform.worldRotation);
        else if (agent->updateRotation)
            WriteWorldPoseToTransform(*go, go->transform.worldPosition, newRot);
    }
}

} // namespace fbzz::scene
