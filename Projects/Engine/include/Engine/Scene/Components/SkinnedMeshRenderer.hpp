// FBZZ Engine
// SkinnedMeshRenderer.hpp | fbzz::scene
// Component that draws a skinned model and owns runtime bone entity links.
#pragma once

#include <Engine/Asset/Model.hpp>
#include <Engine/Scene/Entity.hpp>
#include <Engine/Scene/Script.hpp>
#include <string>
#include <unordered_map>
#include <vector>

namespace fbzz::scene {

struct SkinnedMeshRenderer {
    bool enabled = true;
    // LODSystem 専用のランタイム可視性。Scene には保存しない。
    bool lodVisible = true;
    std::string modelPath;
    int meshIndex = -1; // -1 = 全 submesh, >=0 = 子 GO が担当する特定 submesh

    asset::Model* model = nullptr;

    // WHY: Transform is the common base for scene editing, animation and rigging.
    // SkeletonNode remains the asset-side lookup table; these EntityID values are
    // rebuilt at load/runtime and point at the visible Bone GameObjects.
    EntityID skeletonRootEntity = EntityID::INVALID;
    std::vector<EntityID> nodeEntities;
    // Morph は共有 Mesh を変更せず、この Renderer インスタンス専用の頂点バッファへ反映する。
    std::unordered_map<std::string, float> morphWeights;
    std::unordered_map<std::string, float> appliedMorphWeights;
    std::vector<renderer::ResourceHandle<renderer::BufferTag>> morphVertexBuffers;

    renderer::ResourceHandle<renderer::BufferTag> ResolveVertexBuffer(
        size_t index,
        renderer::ResourceHandle<renderer::BufferTag> fallback) const
    {
        return index < morphVertexBuffers.size() && morphVertexBuffers[index].IsValid()
            ? morphVertexBuffers[index] : fallback;
    }

    const char* GetTypeName() const { return "Skinned Mesh Renderer"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled",   enabled);
        r.Field("modelPath", modelPath);
        r.Field("meshIndex", meshIndex);
    }
};

} // namespace fbzz::scene
