/// @file    LODSystem.cpp
/// @brief   LODGroupComponent の参照解決と Renderer 可視性の更新。
/// @author  Hasegawa Jin
/// @date    2026-07-15
///
/// enabled と lodVisible を分離し、ユーザーが設定した Renderer 有効状態を上書きしない。
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

/// dither: Rendering/LodDither.hlsli のしきい値。0 = 遷移なし、>0 = 出現中、<0 = 退場中。
void SetRendererVisible(Scene& scene, LODRendererReference& reference, bool visible,
                        float dither = 0.0f)
{
    if (!scene.IsValid(reference.entity) && !reference.instanceId.empty()) {
        if (GameObject* resolved = scene.FindByGuid(reference.instanceId)) {
            reference.entity = resolved->GetID();
        }
    }
    if (!scene.IsValid(reference.entity)) return;
    if (auto* mesh = scene.GetComponent<MeshRenderer>(reference.entity)) {
        mesh->lodVisible = visible;
        mesh->lodDither  = dither;
    }
    if (auto* skinned = scene.GetComponent<SkinnedMeshRenderer>(reference.entity)) {
        skinned->lodVisible = visible;
        skinned->lodDither  = dither;
    }
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
    /// @note LODGroup が削除・無効化された直後も、前フレームの非表示状態を Renderer に残さない。
    for (EntityID id : ctx.scene.GetEntities<MeshRenderer>())
        if (auto* renderer = ctx.scene.GetComponent<MeshRenderer>(id)) {
            renderer->lodVisible = true;
            renderer->lodDither  = 0.0f;
        }
    for (EntityID id : ctx.scene.GetEntities<SkinnedMeshRenderer>())
        if (auto* renderer = ctx.scene.GetComponent<SkinnedMeshRenderer>(id)) {
            renderer->lodVisible = true;
            renderer->lodDither  = 0.0f;
        }

    const CameraComponent* camera = nullptr;
    const Transform* cameraTransform = nullptr;
    for (EntityID id : ctx.scene.GetEntities<CameraComponent>()) {
        const auto* candidate = ctx.scene.GetComponent<CameraComponent>(id);
        const auto* go = ctx.scene.GetGameObject(id);
        if (candidate && candidate->enabled && candidate->isMain && go && go->activeInHierarchy()) {
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

        /// @note 親ごと無効化された LOD も «無効» と同じく全段を戻す。描画側は activeInHierarchy で子ごと落とすので、戻しておけば再有効化で元の見た目から始まる。
        if (!group->enabled || !go->activeInHierarchy() || !camera || !cameraTransform || group->levels.empty()) {
            for (auto& level : group->levels)
                for (auto& renderer : level.renderers)
                    SetRendererVisible(ctx.scene, renderer, true);
            group->activeLevel = -1;
            group->fadingLevel = -1;
            group->fadeElapsed = 0.0f;
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

        /// @note selected == levels.size() は「最後のレベルより遠い = カリング」を意味する。
        ///       遷移状態も同じ番号で持ち、-1 は「まだ一度も決まっていない」だけに使う。
        const int selectedLevel = static_cast<int>(selected);

        if (group->activeLevel != selectedLevel) {
            /// @note 遷移中にさらに切り替わったら、退場中だったレベルは即座に消す。3 レベルを
            ///       同時にディザすると市松が噛み合わず穴が開く。速く動くカメラでは 1 段飛ばしが普通に起きる。
            group->fadingLevel = (group->activeLevel >= 0) ? group->activeLevel : -1;
            group->activeLevel = selectedLevel;
            group->fadeElapsed = 0.0f;
        }

        const float fadeDuration = (std::max)(group->fadeDuration, 0.0f);
        float fadeT = 1.0f;
        if (group->fadingLevel >= 0 && fadeDuration > 0.0f) {
            group->fadeElapsed += ctx.dt;
            fadeT = (std::min)(group->fadeElapsed / fadeDuration, 1.0f);
        }
        if (fadeT >= 1.0f)
            group->fadingLevel = -1;

        const bool fading = group->fadingLevel >= 0;

        if (selected < group->levels.size()) {
            /// @note 出現中は正のしきい値。遷移していなければ 0 (全画素)。
            const float dither = fading ? fadeT : 0.0f;
            for (auto& renderer : group->levels[selected].renderers)
                SetRendererVisible(ctx.scene, renderer, true, dither);
        }
        if (fading && static_cast<size_t>(group->fadingLevel) < group->levels.size()) {
            /// @note 退場中は負のしきい値。出現側と判定の向きが逆になり、2 つで画面が埋まる。
            for (auto& renderer : group->levels[group->fadingLevel].renderers)
                SetRendererVisible(ctx.scene, renderer, true, -fadeT);
        }
    }
}

} // namespace fbzz::scene
