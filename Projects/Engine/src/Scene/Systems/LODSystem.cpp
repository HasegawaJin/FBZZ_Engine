// FBZZ Engine
// LODSystem.cpp | fbzz::scene
// LODGroupComponent の参照解決と Renderer 可視性の更新
// enabled と lodVisible を分離し、ユーザーが設定した Renderer 有効状態を上書きしない。
#include <Engine/Scene/Systems/LODSystem.hpp>
#include <Engine/Core/Scheduler/SystemContext.hpp>
#include <Engine/Scene/Systems/TransformSystem.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Components/CameraComponent.hpp>
#include <Engine/Scene/Components/LODGroupComponent.hpp>
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include <Engine/Scene/Components/SkinnedMeshRenderer.hpp>
#include <algorithm>
#include <cmath>

namespace fbzz::scene {
namespace {

void SetRendererVisible(Scene& scene, LODRendererReference& reference, bool visible)
{
    if (!scene.IsValid(reference.entity) && !reference.instanceId.empty()) {
        if (GameObject* resolved = scene.FindByGuid(reference.instanceId)) {
            reference.entity = resolved->GetID();
        }
    }
    if (!scene.IsValid(reference.entity)) return;
    if (auto* mesh = scene.GetComponent<MeshRenderer>(reference.entity)) mesh->lodVisible = visible;
    if (auto* skinned = scene.GetComponent<SkinnedMeshRenderer>(reference.entity)) skinned->lodVisible = visible;
}

} // namespace

ComponentAccess LODSystem::GetAccess() const
{
    return ComponentAccess{}
        .Reads<CameraComponent, Transform>()
        .Writes<LODGroupComponent, MeshRenderer, SkinnedMeshRenderer>();
}

OrderingHints LODSystem::GetOrder() const
{
    return OrderingHints{}.After<TransformLateUpdate>();
}

void LODSystem::Update(SystemContext& ctx)
{
    // WHY: LODGroup が削除・無効化された直後も、前フレームの非表示状態を Renderer に残さない。
    for (EntityID id : ctx.scene.GetEntities<MeshRenderer>())
        if (auto* renderer = ctx.scene.GetComponent<MeshRenderer>(id)) renderer->lodVisible = true;
    for (EntityID id : ctx.scene.GetEntities<SkinnedMeshRenderer>())
        if (auto* renderer = ctx.scene.GetComponent<SkinnedMeshRenderer>(id)) renderer->lodVisible = true;

    const CameraComponent* camera = nullptr;
    const Transform* cameraTransform = nullptr;
    for (EntityID id : ctx.scene.GetEntities<CameraComponent>()) {
        const auto* candidate = ctx.scene.GetComponent<CameraComponent>(id);
        const auto* go = ctx.scene.GetGameObject(id);
        if (candidate && candidate->enabled && candidate->isMain && go) {
            camera = candidate;
            cameraTransform = &go->transform;
            break;
        }
    }

    constexpr float DEG_TO_RAD = 3.14159265358979323846f / 180.0f;
    for (EntityID id : ctx.scene.GetEntities<LODGroupComponent>()) {
        auto* group = ctx.scene.GetComponent<LODGroupComponent>(id);
        const auto* go = ctx.scene.GetGameObject(id);
        if (!group || !go) continue;

        for (auto& level : group->levels)
            for (auto& renderer : level.renderers)
                SetRendererVisible(ctx.scene, renderer, false);

        if (!group->enabled || !camera || !cameraTransform || group->levels.empty()) {
            for (auto& level : group->levels)
                for (auto& renderer : level.renderers)
                    SetRendererVisible(ctx.scene, renderer, true);
            continue;
        }

        const float distance = (go->transform.worldPosition - cameraTransform->worldPosition).Length();
        const float halfFov = (std::max)(camera->fovY, 1.0f) * 0.5f * DEG_TO_RAD;
        const float worldScale = (std::max)(
            (std::max)(std::fabs(go->transform.worldScale.x),
                       std::fabs(go->transform.worldScale.y)),
            std::fabs(go->transform.worldScale.z));
        const float projectedHeight = distance <= 0.001f
            ? 1.0f
            : ((std::max)(group->size, 0.0f) * worldScale) /
              (2.0f * distance * std::tan(halfFov));

        size_t selected = group->levels.size();
        for (size_t i = 0; i < group->levels.size(); ++i) {
            if (projectedHeight >= group->levels[i].screenRelativeHeight) {
                selected = i;
                break;
            }
        }
        if (selected == group->levels.size() && !group->cullBelowLastLevel)
            selected = group->levels.size() - 1;

        if (selected < group->levels.size()) {
            for (auto& renderer : group->levels[selected].renderers)
                SetRendererVisible(ctx.scene, renderer, true);
        }
    }
}

} // namespace fbzz::scene
