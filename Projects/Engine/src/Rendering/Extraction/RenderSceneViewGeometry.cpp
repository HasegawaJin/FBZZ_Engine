/// @file    RenderSceneViewGeometry.cpp
/// @brief   ビューごとの GPU 変形入力と境界の有効性を更新する。
/// @author  Hasegawa Jin
/// @date    2026-11-21
#include "RenderSceneExtraction.hpp"
#include "../Passes/Geometry/GeometryPasses.hpp"

#include <Core/Profiler/ProfileScope.hpp>
#include <Engine/Core/Timing/Time.hpp>
#include <Graphics/Renderer/Resources/Buffers/IBuffer.hpp>
#include <Engine/Scene/Components/Geometry/SkinnedMeshRenderer.hpp>
#include <Engine/Scene/Scene.hpp>
#include <utility>

namespace fbzz::scene::render_extraction_detail {

namespace {

void ApplyPreparedOutput(renderer::RenderMeshItem& item, const renderer::PreparedSkinningWrite& write)
{
    const bool current = write.recorded && item.preparedSkinning->IsWriteCurrent(item.preparedSkinningWrite);
    item.deformedVertexBuffer = current ? write.output : renderer::ResourceHandle<renderer::BufferTag>{};
    item.deformedContentVersion = current ? write.resultingVersion : 0;
}

}

std::vector<uint32_t> RefreshViewMeshInputs(RenderPassContext& context, renderer::RenderScene& snapshot,
    bool rayTracingInputs, const std::optional<SkinnedObjectRange>& skinnedObjects)
{
    // 後から開いた Ray view が LOD0 を追加変形しても、最初の Raster view の snapshot に閉じ込めない。
    std::vector<uint32_t> preparedItemIndices;
    const auto meshInputsProfile = profiler::Profiler::BeginScope(
        profiler::ProfilerMarker("RenderSceneExtractor::MeshInputs", "Rendering"));
    const auto objects = snapshot.Objects();
    const bool validSkinnedRange = skinnedObjects && skinnedObjects->begin <= skinnedObjects->end
        && skinnedObjects->end <= objects.size();
    const size_t skinnedBegin = validSkinnedRange ? skinnedObjects->begin : 0;
    const size_t skinnedEnd = validSkinnedRange ? skinnedObjects->end : objects.size();
    if (validSkinnedRange && profiler::Profiler::IsEnabled())
        FBZZ_PROFILE_MARKER("RenderSceneExtractor::SkinnedRangeInputs");
    // 静的個体とスキン付き個体は Entity 順で混在するため、範囲内でも種類を確認する。
    for (size_t objectIndex = skinnedBegin; objectIndex < skinnedEnd; ++objectIndex) {
        const auto& object = objects[objectIndex];
        if (!object.skinned) {
            continue;
        }
        auto* owner = context.scene.GetGameObject({object.sourceIndex, object.sourceGeneration});
        const auto* skinnedRenderer = owner ? owner->GetComponent<SkinnedMeshRenderer>() : nullptr;
        for (uint32_t itemIndex = object.firstItem; itemIndex < object.firstItem + object.itemCount; ++itemIndex) {
            auto& item = snapshot.items[itemIndex];
            item.preparedSkinning.reset();
            item.preparedSkinningWrite = UINT32_MAX;
            item.deformedVertexBuffer = skinnedRenderer && skinnedRenderer->gpuSkinningFrame == Time::frameCount
                && skinnedRenderer->skinnedBufferModel == skinnedRenderer->model ? skinnedRenderer->ResolveSlotSkinnedVertexBuffer(item.materialSlot)
                : renderer::ResourceHandle<renderer::BufferTag>{};
            // 当該フレームの同じモデルから生成したスキニング結果だけを参照する。
            const bool currentPreparation = skinnedRenderer && skinnedRenderer->preparedSkinning
                && skinnedRenderer->preparedSkinningFrame == Time::frameCount
                && skinnedRenderer->skinnedBufferModel == skinnedRenderer->model;
            if (!currentPreparation) {
                continue;
            }
            if (item.sourceSubmesh >= skinnedRenderer->preparedSkinningRows.size()
                || skinnedRenderer->SubmeshAt(item.materialSlot) != item.sourceSubmesh) {
                continue;
            }

            // LOD や材質スロットの再解決で頂点入力が変わった場合は、元の処理結果を流用しない。
            const auto* mesh = skinnedRenderer->SubmeshMesh(item.materialSlot);
            if (!mesh || mesh->cpuSkinnedVertices.size() != item.vertexCount
                || skinnedRenderer->ResolveSlotVertexBuffer(item.materialSlot, mesh->vertexBuffer) != item.skinningVertexBuffer) {
                continue;
            }

            const uint32_t ticketRow = skinnedRenderer->preparedSkinningRows[item.sourceSubmesh];
            const auto writes = skinnedRenderer->preparedSkinning->Writes();
            if (ticketRow >= writes.size()) {
                continue;
            }
            item.preparedSkinning = skinnedRenderer->preparedSkinning;
            item.preparedSkinningWrite = ticketRow;
            preparedItemIndices.push_back(itemIndex);
            ApplyPreparedOutput(item, writes[ticketRow]);
        }
    }
    // Raster では宣言済みの stride を使い、Ray 専用メタデータの取得による RHI キューの同期を避ける。
    if (rayTracingInputs) {
        for (auto& item : snapshot.items) {
            const auto* vertexBuffer = context.resources.Get(item.vertexBuffer);
            const auto* indexBuffer = context.resources.Get(item.indexBuffer);
            const auto* deformedBuffer = context.resources.Get(item.deformedVertexBuffer);
            item.deformedContentVersion = deformedBuffer ? deformedBuffer->GetContentVersion() : 0;
            item.vertexContentVersion = vertexBuffer ? vertexBuffer->GetContentVersion() : 0;
            item.indexContentVersion = indexBuffer ? indexBuffer->GetContentVersion() : 0;
            if (vertexBuffer) {
                item.vertexStride = vertexBuffer->GetStride();
            }
        }
    }
    profiler::Profiler::EndScope(meshInputsProfile);
    return preparedItemIndices;
}

void RefreshPreparedMeshOutputs(renderer::RenderScene& snapshot, std::span<const uint32_t> preparedItemIndices)
{
    // 材質/環境の callback が ticket を完成しても、未公開 geometry 自体は再抽出しない。
    for (uint32_t itemIndex : preparedItemIndices) {
        auto& item = snapshot.items[itemIndex];
        const auto writes = item.preparedSkinning->Writes();
        if (item.preparedSkinningWrite >= writes.size()) {
            continue;
        }
        ApplyPreparedOutput(item, writes[item.preparedSkinningWrite]);
    }
}

bool LeafWindBoundsMayHaveExpanded(Scene& scene, const renderer::RenderObject& object)
{
    if (!(object.boundsRadius > 0.0f)) {
        return false;
    }
    auto* owner = scene.GetGameObject({object.sourceIndex, object.sourceGeneration});
    if (!owner) {
        return false;
    }
    const auto* skinnedRenderer = object.skinned ? owner->GetComponent<SkinnedMeshRenderer>() : nullptr;
    const float currentWindPadding = ComputeLeafWindBoundsPadding(*owner, skinnedRenderer ? skinnedRenderer->SubmeshCount() : 1);
    return currentWindPadding < 0.0f || currentWindPadding > object.leafWindBoundsPadding;
}

/// @note コールバックで材質の風変位が増えた場合は、過去のビューが共有する境界を保持したまま現在の境界を無効にする。
void FailOpenChangedLeafWindBounds(Scene& scene, renderer::RenderScene& snapshot)
{
    const auto objects = snapshot.Objects();
    std::vector<renderer::RenderObject> updatedObjects;
    bool boundsInvalidated = false;
    for (size_t objectIndex = 0; objectIndex < objects.size(); ++objectIndex) {
        const auto& object = objects[objectIndex];
        if (!LeafWindBoundsMayHaveExpanded(scene, object)) {
            continue;
        }
        if (!boundsInvalidated) {
            updatedObjects.assign(objects.begin(), objects.end());
        }
        boundsInvalidated = true;
        updatedObjects[objectIndex].boundsRadius = 0.0f;
        for (uint32_t itemIndex = object.firstItem; itemIndex < object.firstItem + object.itemCount; ++itemIndex) {
            snapshot.items[itemIndex].boundsRadius = 0.0f;
            snapshot.items[itemIndex].occluderRadius = 0.0f;
        }
    }
    if (!boundsInvalidated) {
        return;
    }
    snapshot.geometry.ReplaceObjects(std::move(updatedObjects));
}

}
