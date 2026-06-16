// FBZZ Engine
// NavMeshSensorSystem.cpp | fbzz::scene
// NavMeshSensorComponent の視野角・距離・遮蔽判定を評価し、検知状態の変化を
// Script コールバック (OnNavMeshTargetSpotted/Lost) で通知する。
// autoChase 中は同 GO の NavMeshAgentComponent と連携し、見失った直後は
// 最後に見えた位置まで一度だけ移動させてから巡回などへ戻れるようにする。
#include "Engine/Scene/Systems/NavMeshSensorSystem.hpp"
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

void NavMeshSensorSystem(Scene& scene, const physics::World* world, float dt)
{
    (void)dt; // 現状フレームレート依存の積分は行わないが、将来の検知遅延導入に備えて引数を残す。

    for (EntityID eid : scene.GetEntities<NavMeshSensorComponent>()) {
        auto* sensor = scene.GetComponent<NavMeshSensorComponent>(eid);
        auto* go     = scene.GetGameObject(eid);
        if (!sensor || !go || !sensor->enabled) continue;

        GameObject* targetGo = sensor->targetTag.empty() ? nullptr : scene.FindWithTag(sensor->targetTag);
        bool visible = false;

        if (targetGo) {
            const math::Vector3 origin = go->transform.worldPosition;
            math::Vector3 toTarget = targetGo->transform.worldPosition - origin;
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
                        visible = true;
                        if (sensor->useLineOfSight && world) {
                            // 目線の高さを軽く持ち上げてから Raycast する (地面との誤交差を避ける)。
                            const math::Vector3 eyePos = origin + math::Vector3::UP * 0.5f;
                            physics::World::RaycastHit hit;
                            if (world->Raycast(eyePos, dir, dist, hit))
                                visible = (hit.distance >= dist - 0.1f);
                        }
                    }
                }
            }
        }

        const bool wasVisible = sensor->targetVisible;
        sensor->targetVisible   = visible;
        sensor->detectedTarget  = visible ? targetGo->GetID() : EntityID::INVALID;
        if (visible) sensor->lastKnownTargetPos = targetGo->transform.worldPosition;

        if (visible && !wasVisible)
            NotifyScripts(scene, eid, *go, &Script::OnNavMeshTargetSpotted);
        else if (!visible && wasVisible)
            NotifyScripts(scene, eid, *go, &Script::OnNavMeshTargetLost);

        if (sensor->autoChase) {
            if (auto* agent = scene.GetComponent<NavMeshAgentComponent>(eid)) {
                if (visible) {
                    agent->SetTarget(targetGo->GetID(), sensor->chaseRepathInterval);
                } else if (wasVisible) {
                    // 見失った瞬間: 追跡をやめ、最後に見えた位置まで一度だけ移動して捜索する。
                    agent->SetDestination(sensor->lastKnownTargetPos);
                }
            }
        }
    }
}

} // namespace fbzz::scene
