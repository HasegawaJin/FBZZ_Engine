// FBZZ Engine
// NavMeshSensorSystem.cpp | fbzz::scene
// NavMeshSensorComponent の視野角・距離・遮蔽判定を評価し、検知状態の変化を
// Script コールバック (OnNavMeshTargetSpotted/Lost) で通知する。
// autoChase 中は同 GO の NavMeshAgentComponent と連携し、見失った直後は
// 最後に見えた位置まで一度だけ移動させてから巡回などへ戻れるようにする。
#include "Engine/Scene/Systems/NavMeshSensorSystem.hpp"
#include "Engine/Core/Scheduler/SystemContext.hpp"
#include "Engine/Scene/Scene.hpp"
#include "Engine/Scene/GameObject.hpp"
#include "Engine/Scene/Components/NavMeshSensorComponent.hpp"
#include "Engine/Scene/Components/NavMeshAgentComponent.hpp"
#include "Engine/Scene/ScriptComponent.hpp"
#include <Physics/World.hpp>
#include <Math/Vector3.hpp>
#include <cmath>

namespace fbzz::scene {

namespace {

constexpr float kPi = 3.14159265358979323846f;

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

} // namespace

ComponentAccess NavMeshSensorSystem::GetAccess() const
{
    return ComponentAccess{}
        .Reads<NavMeshSensorComponent>()
        .Writes<NavMeshSensorComponent, NavMeshAgentComponent>();
}

void NavMeshSensorSystem::Update(SystemContext& ctx)
{
    Scene& scene = ctx.scene;
    const physics::World* world = &ctx.world;
    const float dt = ctx.dt;
    for (EntityID eid : scene.GetEntities<NavMeshSensorComponent>()) {
        auto* sensor = scene.GetComponent<NavMeshSensorComponent>(eid);
        auto* go     = scene.GetGameObject(eid);
        if (!sensor || !go || !sensor->enabled) continue;

        // ── scanInterval: 指定秒数ごとにのみ検知チェックを実行する ──────────────
        if (sensor->scanInterval > 0.0f) {
            sensor->scanTimer -= dt;
            if (sensor->scanTimer > 0.0f) {
                // スキャンスキップ中も memoryTimer は減算し続ける
                if (sensor->targetVisible && sensor->memoryTime > 0.0f) {
                    sensor->memoryTimer -= dt;
                    if (sensor->memoryTimer <= 0.0f) {
                        sensor->targetVisible  = false;
                        sensor->detectedTarget = EntityID::INVALID;
                        sensor->memoryTimer    = 0.0f;
                        NotifyScripts(scene, eid, *go, &Script::OnNavMeshTargetLost);
                        if (sensor->autoChase) {
                            if (auto* agent = scene.GetComponent<NavMeshAgentComponent>(eid))
                                agent->SetDestination(sensor->lastKnownTargetPos);
                        }
                    }
                }
                continue;
            }
            sensor->scanTimer = sensor->scanInterval;
        }

        // ── 検知判定 ────────────────────────────────────────────────────────────
        GameObject* targetGo = sensor->targetTag.empty() ? nullptr : scene.FindWithTag(sensor->targetTag);
        bool detected = false;

        if (targetGo) {
            const math::Vector3 origin    = go->transform.worldPosition;
            const math::Vector3 targetPos = targetGo->transform.worldPosition;

            // heightThreshold: Y 差が閾値を超えたら検知しない (0 = 無制限)
            const bool heightOk = (sensor->heightThreshold <= 0.0f) ||
                                  (std::abs(targetPos.y - origin.y) <= sensor->heightThreshold);

            if (heightOk) {
                math::Vector3 toTarget = targetPos - origin;
                toTarget.y = 0.0f;
                const float dist = toTarget.Length();

                if (dist > 0.0001f && dist <= sensor->viewDistance) {
                    const math::Vector3 dir = toTarget * (1.0f / dist);
                    math::Vector3 forward = go->transform.forward;
                    forward.y = 0.0f;
                    if (forward.LengthSq() > 0.0001f) {
                        forward = forward.Normalized();
                        const float cosHalfAngle = std::cos(sensor->viewAngleDeg * 0.5f * (kPi / 180.0f));
                        if (math::Vector3::Dot(forward, dir) >= cosHalfAngle) {
                            detected = true;
                            if (sensor->useLineOfSight && world) {
                                // 目線の高さを軽く持ち上げてから Raycast する (地面との誤交差を避ける)
                                const math::Vector3 eyePos = origin + math::Vector3::UP * 0.5f;
                                physics::World::RaycastHit hit;
                                if (world->Raycast(eyePos, dir, dist, hit))
                                    detected = (hit.distance >= dist - 0.1f);
                            }
                        }
                    }
                }
            }
        }

        // ── 状態遷移 ────────────────────────────────────────────────────────────
        const bool wasVisible = sensor->targetVisible;

        if (detected) {
            // 視界内: memoryTimer をリセットして検知状態を維持
            sensor->targetVisible      = true;
            sensor->detectedTarget     = targetGo->GetID();
            sensor->lastKnownTargetPos = targetGo->transform.worldPosition;
            sensor->memoryTimer        = sensor->memoryTime;

            if (!wasVisible)
                NotifyScripts(scene, eid, *go, &Script::OnNavMeshTargetSpotted);

            if (sensor->autoChase) {
                if (auto* agent = scene.GetComponent<NavMeshAgentComponent>(eid))
                    agent->SetTarget(targetGo->GetID(), sensor->chaseRepathInterval);
            }
        } else {
            // 視界外
            if (wasVisible) {
                if (sensor->memoryTime > 0.0f) {
                    // memoryTime 中は検知状態を保持し、タイマーを減算する
                    sensor->memoryTimer -= dt;
                    if (sensor->memoryTimer > 0.0f) continue; // まだ記憶中
                }
                // 記憶時間切れ (または memoryTime==0): 検知状態をクリア
                sensor->targetVisible  = false;
                sensor->detectedTarget = EntityID::INVALID;
                sensor->memoryTimer    = 0.0f;
                NotifyScripts(scene, eid, *go, &Script::OnNavMeshTargetLost);

                if (sensor->autoChase) {
                    if (auto* agent = scene.GetComponent<NavMeshAgentComponent>(eid))
                        agent->SetDestination(sensor->lastKnownTargetPos);
                }
            }
        }
    }
}

} // namespace fbzz::scene
