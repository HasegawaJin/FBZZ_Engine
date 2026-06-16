// FBZZ Engine
// NavigationSystem.cpp | fbzz::scene
// NavMeshAgentComponent の毎フレーム更新: パス計算 (A* + Funnel Algorithm) と移動 (Steering)。
//
// WHY: 複数 NavMeshSurface の合成 (オーバーラップ領域の統合) は実装が複雑なため、
//      シーン内で最初に見つかった有効な NavMeshSurfaceComponent のみを使用する単一運用とする。
//      Agent に親 GO がある場合は PhysicsSystem と同じ式で world pose を親ローカルへ
//      逆変換して書き戻す (WriteWorldPoseToTransform)。
#include "Engine/Scene/Systems/NavigationSystem.hpp"
#include "Engine/Scene/Scene.hpp"
#include "Engine/Scene/GameObject.hpp"
#include "Engine/Scene/Components/NavMeshSurfaceComponent.hpp"
#include "Engine/Scene/Components/NavMeshAgentComponent.hpp"
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

// XZ 平面上の符号付き面積の 2 倍 (Funnel Algorithm の左右判定に使う)。
float TriArea2(const math::Vector3& a, const math::Vector3& b, const math::Vector3& c)
{
    return (b.x - a.x) * (c.z - a.z) - (c.x - a.x) * (b.z - a.z);
}

bool NearlyEqualXZ(const math::Vector3& a, const math::Vector3& b)
{
    constexpr float kEps = 0.0001f;
    return std::abs(a.x - b.x) < kEps && std::abs(a.z - b.z) < kEps;
}

// 指定座標を含むポリゴンを返す。範囲外なら最も近いポリゴンへフォールバックする
// (Agent が NavMesh の境界からわずかに外れているケースを許容する)。
int FindNearestPolygon(const NavMesh& navMesh, const math::Vector3& pos)
{
    int best = -1;
    float bestDistSq = FLT_MAX;
    for (size_t i = 0; i < navMesh.polygons.size(); ++i) {
        const auto& poly = navMesh.polygons[i];
        if (poly.ContainsXZ(pos.x, pos.z))
            return static_cast<int>(i);

        const float distSq = poly.DistanceSqXZ(pos.x, pos.z);
        if (distSq < bestDistSq) { bestDistSq = distSq; best = static_cast<int>(i); }
    }
    return best;
}

// ポリゴン隣接グラフ上の A* 探索。
// 最適化: open リストを二分ヒープ (std::priority_queue) で管理する。
// decrease-key を行わない代わりに、より良い g が見つかるたびに新しいエントリを push し、
// pop 時に closed 済み (=確定済みより悪い古いエントリ) を読み捨てる lazy deletion 方式を使う。
// これにより大きな NavMesh でも O((V+E) log V) で探索できる。
bool FindPolygonPath(const NavMesh& navMesh, int startPoly, int goalPoly, std::vector<int>& outPath)
{
    if (startPoly == goalPoly) { outPath = { startPoly }; return true; }

    const size_t n = navMesh.polygons.size();
    std::vector<float> gScore(n, FLT_MAX);
    std::vector<int> cameFrom(n, -1);
    std::vector<uint8_t> closed(n, 0);
    gScore[static_cast<size_t>(startPoly)] = 0.0f;

    auto heuristic = [&](int a, int b) {
        return (navMesh.polygons[static_cast<size_t>(a)].Center()
              - navMesh.polygons[static_cast<size_t>(b)].Center()).Length();
    };

    using OpenEntry = std::pair<float, int>; // (fScore, polygon index)
    std::priority_queue<OpenEntry, std::vector<OpenEntry>, std::greater<OpenEntry>> open;
    open.push({ heuristic(startPoly, goalPoly), startPoly });

    while (!open.empty()) {
        const int current = open.top().second;
        open.pop();
        if (closed[static_cast<size_t>(current)]) continue; // 古い (悪化済み) エントリは読み捨てる

        if (current == goalPoly) {
            outPath.clear();
            for (int node = current; node != -1; node = cameFrom[static_cast<size_t>(node)])
                outPath.push_back(node);
            std::reverse(outPath.begin(), outPath.end());
            return true;
        }

        closed[static_cast<size_t>(current)] = 1;
        for (const auto& portal : navMesh.polygons[static_cast<size_t>(current)].portals) {
            if (closed[static_cast<size_t>(portal.neighbor)]) continue;
            const float tentativeG = gScore[static_cast<size_t>(current)] + heuristic(current, portal.neighbor);
            if (tentativeG < gScore[static_cast<size_t>(portal.neighbor)]) {
                gScore[static_cast<size_t>(portal.neighbor)] = tentativeG;
                cameFrom[static_cast<size_t>(portal.neighbor)] = current;
                open.push({ tentativeG + heuristic(portal.neighbor, goalPoly), portal.neighbor });
            }
        }
    }
    return false;
}

// ポリゴン経路から Funnel Algorithm (Simple Stupid Funnel Algorithm) で
// 直線最短パスへ平滑化する。先頭要素は startPos そのもの。
std::vector<math::Vector3> BuildFunnelPath(const NavMesh& navMesh, const std::vector<int>& polyPath,
                                            const math::Vector3& startPos, const math::Vector3& goalPos)
{
    std::vector<math::Vector3> result;
    result.push_back(startPos);

    if (polyPath.size() <= 1) {
        result.push_back(goalPos);
        return result;
    }

    // ── チャンネル構築: 各境界の Portal を進行方向基準の左右へ並べ直す ──────
    struct ChannelPortal { math::Vector3 left, right; };
    std::vector<ChannelPortal> channel;
    channel.push_back({ startPos, startPos });

    for (size_t i = 0; i + 1 < polyPath.size(); ++i) {
        const NavMeshPolygon& cur = navMesh.polygons[static_cast<size_t>(polyPath[i])];
        const NavMeshPolygon& nxt = navMesh.polygons[static_cast<size_t>(polyPath[i + 1])];

        const NavMeshPolygon::Portal* found = nullptr;
        for (const auto& portal : cur.portals) {
            if (portal.neighbor == polyPath[i + 1]) { found = &portal; break; }
        }
        if (!found) {
            const math::Vector3 mid = math::Vector3::Lerp(cur.Center(), nxt.Center(), 0.5f);
            channel.push_back({ mid, mid });
            continue;
        }

        math::Vector3 travelDir = nxt.Center() - cur.Center();
        travelDir.y = 0.0f;
        const math::Vector3 leftDir = { -travelDir.z, 0.0f, travelDir.x };
        const math::Vector3 mid = (found->left + found->right) * 0.5f;
        const bool aIsLeft = math::Vector3::Dot(found->left - mid, leftDir) >
                             math::Vector3::Dot(found->right - mid, leftDir);
        channel.push_back(aIsLeft ? ChannelPortal{ found->left, found->right }
                                  : ChannelPortal{ found->right, found->left });
    }
    channel.push_back({ goalPos, goalPos });

    math::Vector3 apex  = channel[0].left;
    math::Vector3 left  = channel[0].left;
    math::Vector3 right = channel[0].right;
    size_t apexIndex = 0, leftIndex = 0, rightIndex = 0;

    for (size_t i = 1; i < channel.size(); ++i) {
        const math::Vector3& cl = channel[i].left;
        const math::Vector3& cr = channel[i].right;

        if (TriArea2(apex, right, cr) <= 0.0f) {
            if (NearlyEqualXZ(apex, right) || TriArea2(apex, left, cr) > 0.0f) {
                right = cr; rightIndex = i;
            } else {
                result.push_back(left);
                apex = left; apexIndex = leftIndex;
                left = apex; right = apex; leftIndex = apexIndex; rightIndex = apexIndex;
                i = apexIndex;
                continue;
            }
        }

        if (TriArea2(apex, left, cl) >= 0.0f) {
            if (NearlyEqualXZ(apex, left) || TriArea2(apex, right, cl) < 0.0f) {
                left = cl; leftIndex = i;
            } else {
                result.push_back(right);
                apex = right; apexIndex = rightIndex;
                left = apex; right = apex; leftIndex = apexIndex; rightIndex = apexIndex;
                i = apexIndex;
                continue;
            }
        }
    }

    result.push_back(goalPos);
    return result;
}

// パスのみ放棄して停止する (Stop() と異なり target は保持する)。
// Target Follow 中の「到達」「パス失敗」両方で使う: target が再び動けば repathTimer の
// タイミングで自動的に追跡を再開できるようにする。
void HaltKeepingTarget(NavMeshAgentComponent& agent)
{
    agent.hasDestination = false;
    agent.path.clear();
    agent.currentWaypoint = 0;
    agent.currentSpeed = 0.0f;
    agent.remainingDistance = 0.0f;
    agent.state = NavMeshAgentState::IDLE;
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

void NavigationSystem(Scene& scene, float dt)
{
    NavMeshSurfaceComponent* activeVolume = nullptr;
    for (EntityID veid : scene.GetEntities<NavMeshSurfaceComponent>()) {
        auto* v = scene.GetComponent<NavMeshSurfaceComponent>(veid);
        if (v && v->enabled && v->navMesh.IsValid()) { activeVolume = v; break; }
    }
    if (!activeVolume) return;
    const NavMesh& navMesh = activeVolume->navMesh;

    const auto agentEntities = scene.GetEntities<NavMeshAgentComponent>();

    // 回避用の空間ハッシュをフレーム先頭で 1 回構築する。
    const float bucketSize = std::max(0.5f, activeVolume->cellSize * 2.0f);
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

        // ── Target Follow: 追跡対象が設定されていれば定期的に destination を更新する ──
        if (agent->target.IsValid()) {
            auto* targetGo = scene.GetGameObject(agent->target);
            if (!targetGo) {
                agent->ClearTarget();
            } else {
                agent->repathTimer -= dt;
                const float movedSq = (targetGo->transform.worldPosition - agent->lastTargetPos).LengthSq();
                const float repathMoveDist = activeVolume->cellSize;
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
            if (startPoly >= 0 && goalPoly >= 0 && FindPolygonPath(navMesh, startPoly, goalPoly, polyPath)) {
                auto waypoints = BuildFunnelPath(navMesh, polyPath, go->transform.worldPosition, agent->destination);
                waypoints.erase(waypoints.begin()); // 出発点は経路から除く (Agent は既にそこにいる)
                agent->path = std::move(waypoints);
                agent->currentWaypoint = 0;
            } else {
                if (agent->target.IsValid()) HaltKeepingTarget(*agent);
                else agent->Stop();
                NotifyScripts(scene, eid, *go, &Script::OnNavMeshPathFailed);
                continue;
            }
        }

        if (agent->path.empty() || agent->currentWaypoint >= agent->path.size())
            continue;

        const math::Vector3 pos = go->transform.worldPosition;

        // 残り距離は isStopped 中も Script/Inspector から参照できるよう毎フレーム更新する。
        {
            float remaining = (agent->path[agent->currentWaypoint] - pos).Length();
            for (size_t i = agent->currentWaypoint; i + 1 < agent->path.size(); ++i)
                remaining += (agent->path[i + 1] - agent->path[i]).Length();
            agent->remainingDistance = remaining;
        }

        // isStopped 中はパスを保持したまま移動・回転だけ止める (Resume() で再開できる)。
        if (agent->isStopped) {
            agent->currentSpeed = 0.0f;
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
        float arriveThreshold = isLastWaypoint ? agent->stoppingDistance : (activeVolume->cellSize * 0.5f);

        if (distance <= arriveThreshold) {
            if (isLastWaypoint) {
                agent->destinationReached = true;
                if (agent->target.IsValid()) HaltKeepingTarget(*agent);
                else agent->Stop();
                NotifyScripts(scene, eid, *go, &Script::OnNavMeshDestinationReached);
                continue;
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
        agent->currentSpeed = std::min(agent->currentSpeed + agent->acceleration * dt, agent->maxSpeed);
        const float moveDist = std::min(agent->currentSpeed * dt, distance);

        math::Vector3 newPos = pos + moveDir * moveDist;
        const float t = (distance > 0.0001f) ? std::min(1.0f, moveDist / distance) : 0.0f;
        newPos.y = pos.y + (target.y - pos.y) * t;

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

        WriteWorldPoseToTransform(*go, newPos, newRot);
    }
}

} // namespace fbzz::scene
