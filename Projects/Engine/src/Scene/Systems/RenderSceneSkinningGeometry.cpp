/// @file    RenderSceneSkinningGeometry.cpp
/// @brief   スキン姿勢の描画入力とクラスタ変位の上限を求める。
/// @author  Hasegawa Jin
/// @date    2026-11-21
#include "RenderSceneSkinningGeometry.hpp"
#include "RenderPasses/Geometry/GeometryPasses.hpp"

#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Scene/Components/AnimatorComponent.hpp>
#include <Engine/Scene/Components/SkinnedMeshRenderer.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Graphics/Renderer/MeshletGeometry.hpp>
#include <algorithm>
#include <cmath>
#include <span>

namespace fbzz::scene::render_extraction_detail {

namespace {

/// @note 正規化したスキン重みでは、バインド姿勢の AABB 上の最大ボーン変位が上限になる。
float SkinClusterDisplacement(const renderer::RenderObject& object, const SkinnedMeshRenderer& skin,
    std::span<const math::Matrix4> palette)
{
    float maximum = 0.0f;
    for (size_t slot = 0; slot < skin.SubmeshCount(); ++slot) {
        const auto* mesh = skin.SubmeshMesh(slot);
        if (!mesh || !mesh->ClusterLod()) continue;
        const auto measure = [&](const math::Matrix4& bone) {
            for (unsigned corner = 0; corner < 8; ++corner) {
                const math::Vector3 sign{corner & 1u ? 1.0f : -1.0f, corner & 2u ? 1.0f : -1.0f,
                    corner & 4u ? 1.0f : -1.0f};
                const math::Vector3 position{mesh->boundsCenter.x + sign.x * mesh->boundsExtents.x,
                    mesh->boundsCenter.y + sign.y * mesh->boundsExtents.y,
                    mesh->boundsCenter.z + sign.z * mesh->boundsExtents.z};
                const auto skinned = bone * math::Vector4{position, 1.0f};
                const auto delta = object.world * math::Vector4{skinned.x - position.x,
                    skinned.y - position.y, skinned.z - position.z, 0.0f};
                const auto transformed = object.world * math::Vector4{skinned.x, skinned.y, skinned.z, 0.0f};
                const float length = math::Vector3{delta.x, delta.y, delta.z}.Length()
                    + mesh->ClusterLod()->maximumSkinWeightError
                    * math::Vector3{transformed.x, transformed.y, transformed.z}.Length();
                if (!std::isfinite(length)) return false;
                maximum = (std::max)(maximum, length);
            }
            return true;
        };
        if (!measure(math::Matrix4::Identity())) return -1.0f;
        for (const auto& bone : palette)
            if (!measure(bone)) return -1.0f;
    }
    return maximum + 1.0e-4f;
}

/// @note ベイク済みクリップやブレンドの境界は、モデル空間原点を中心とする球。
float BakedSkinClusterDisplacement(const SkinnedMeshRenderer& skin, float poseRadius, const math::Matrix4& world)
{
    if (!(poseRadius > 0.0f) || !std::isfinite(poseRadius)) return -1.0f;
    float bindRadius = 0.0f;
    for (size_t slot = 0; slot < skin.SubmeshCount(); ++slot) {
        const auto* mesh = skin.SubmeshMesh(slot);
        if (mesh && mesh->ClusterLod()) {
            const float radius = mesh->boundsCenter.Length() + mesh->boundsExtents.Length();
            if (!std::isfinite(radius)) return -1.0f;
            bindRadius = (std::max)(bindRadius, radius);
        }
    }
    float scaleSquared = 0.0f;
    for (size_t row = 0; row < 3; ++row)
        scaleSquared += math::Vector3{world.m[row][0], world.m[row][1], world.m[row][2]}.LengthSq();
    const float displacement = (poseRadius + bindRadius) * std::sqrt(scaleSquared) + 1.0e-4f;
    return std::isfinite(displacement) ? displacement : -1.0f;
}

}

void ExtractSkinnedPose(GameObject& owner, const SkinnedMeshRenderer& skin,
    renderer::ResourceHandle<renderer::ConstantBufferTag> identityPalette,
    const renderer::ResourceManager* resources, renderer::RenderObject& object)
{
    WorldBounds bounds{};
    if (ComputeSkinnedWorldBounds(owner, skin, bounds)) {
        object.boundsCenter = bounds.center;
        object.boundsRadius = bounds.radius;
    }
    const auto* animator = FindAnimator(owner);
    object.skinningPalette = ResolveSkinningCB(animator ? animator->skinningBuffer.Get()
        : renderer::ResourceHandle<renderer::ConstantBufferTag>{}, skin.model, identityPalette);
    object.previousSkinningValid = animator && animator->prevBoneMatricesValid &&
        animator->skinningBuffer.IsValid() && animator->prevSkinningBuffer.IsValid();
    object.previousSkinningPalette = object.previousSkinningValid
        ? animator->prevSkinningBuffer.Get() : object.skinningPalette;
    if (animator) {
        const auto& bake = animator->boneBake;
        if (resources && bake.active && bake.gpuFrame == resources->FrameStamp()
            && bake.gpuOwner == resources->GetInstanceId() && bake.gpuEpoch == resources->GetResetVersion()) {
            object.gpuBones = {bake.gpuPalette, bake.gpuPreviousPalette, bake.gpuOffset, bake.gpuPreviousOffset};
        }
        /// @note リソースのコールバックで退役した入力では、保留中の CPU 姿勢を描画に渡さない。
        if (bake.cpuDeferred && !object.gpuBones.current.IsValid()) { object.lodVisible = false; object.rayVisible = false; }
    }
    if (object.gpuBones.current.IsValid()) {
        object.skinClusterDisplacement = BakedSkinClusterDisplacement(skin, animator->boneBake.radius, object.world);
    } else {
        const std::span<const math::Matrix4> palette = animator && animator->skinningBuffer.IsValid()
            ? std::span<const math::Matrix4>(animator->boneMatrices)
            : skin.model->skeleton ? std::span<const math::Matrix4>(skin.model->skeleton->referencePose)
                                   : std::span<const math::Matrix4>{};
        object.skinClusterDisplacement = SkinClusterDisplacement(object, skin, palette.first(
            (std::min)(palette.size(), size_t{128})));
    }
}

}
