/// @file    RenderFiberExtraction.hpp
/// @brief   繊維の材質・面・流れを引き渡す描画抽出用の状態。
/// @author  Hasegawa Jin
/// @date    2026-11-21
#pragma once

#include <Engine/Rendering/Passes/Geometry/FiberRenderPass.hpp>
#include <Engine/Asset/Material/MaterialAsset.hpp>
#include <Engine/Asset/Material/FiberMaterialSettings.hpp>
#include <Graphics/Renderer/Resources/Buffers/DynamicBufferPool.hpp>
#include <Engine/Scene/Fields/FlowField.hpp>
#include <Engine/Scene/Fields/FlowFieldFrame.hpp>
#include "../../Passes/Geometry/FiberSurfaceSources.hpp"
#include "../../Passes/Geometry/FiberContacts.hpp"
#include "../../Passes/Geometry/FiberPlacementResources.hpp"
#include "../../Passes/Geometry/FiberAppearanceResources.hpp"
#include "../../Passes/Geometry/GeometryPasses.hpp"
#include <array>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace fbzz::scene { class Scene; class GameObject; struct FiberComponent; struct RenderPassContext; }

namespace fbzz::scene::fiber_extraction_detail {

/// @note Fin は頂点/インデックスバッファ、Blade は根元の StructuredBuffer だけを持つ。
struct FiberFinBuffers {
    renderer::ResourceHandle<renderer::BufferTag> m_vertices;
    renderer::ResourceHandle<renderer::BufferTag> m_indices;
    uint32_t m_indexCount = 0;
    renderer::ResourceHandle<renderer::StructuredBufferTag> m_blades;
    uint32_t m_bladeCount = 0;
    uint64_t m_seen = 0;
    bool m_terrain = false;
};

/// @note 1 フレーム内の全パス・全カスケードで共有する解決結果。フレームが進むと読み直す。
struct FiberMaterialEntry {
    asset::FiberMaterialSettings m_settings;
    FiberMaskSlot m_mask;
    FiberAppearanceResources m_appearance;
    asset::MaterialAsset::AuthoringSnapshotPtr m_snapshot;
    uint64_t m_shaderVersion = UINT64_MAX;
    uint64_t m_frame = (std::numeric_limits<uint64_t>::max)();
    bool m_valid = false;
};

/// @note シーンに置いた FlowField を 1 フレーム 1 回だけ詰め、前フレームの一覧を velocity 用に後ろへ連結する。
/// @note 環境流は FiberFrameCB の風が持つため含めない (FlowFieldFrame::sceneFieldCount で切る)。
struct FiberFlowCache {
    uint64_t m_frame = (std::numeric_limits<uint64_t>::max)();
    /// @note 物体ごとの交差判定用。前フレームの vectorField は解放済みかもしれないので null にして持つ。
    std::vector<ActiveFlowField> m_current;
    std::vector<ActiveFlowField> m_previous;
    std::vector<GpuFlowField> m_currentPacked;
    std::vector<GpuFlowField> m_previousPacked;
    std::vector<GpuFlowField> m_upload;
    renderer::ResourceHandle<renderer::StructuredBufferTag> m_buffer;
};

/// @note ResourceManager が GPU 実体を単独所有する。保持するのは失効検出付き参照だけ。
struct FiberResources {
    renderer::ResourceManager* m_owner = nullptr;
    uint64_t m_resetVersion = 0;
    uint64_t m_ownerIdentity = 0;
    uint64_t m_sweptFrame = (std::numeric_limits<uint64_t>::max)();
    std::map<std::array<uint32_t, 8>, FiberFinBuffers> m_fins;
    std::map<std::pair<uint64_t,std::string>,FiberTerrainCache> m_terrains;
    std::map<std::pair<uint64_t,std::string>,FiberPlacementResources> m_placements;
    std::map<uint64_t,FiberContactCache> m_contactStates;
    std::unordered_map<std::string, FiberMaterialEntry> m_materials;
    /// @note FiberComponent::m_maskPath による個体ごとの差し替え。パスごとに 1 枠。
    std::unordered_map<std::string, FiberMaskSlot> m_maskOverrides;
    std::map<uint64_t, FiberFlowCache> m_flows;
    /// @note 流れの一覧は毎フレーム書き換える。GPU が読んでいる前フレームのバッファを上書きしないよう借り直す。
    /// @note プールの既定コンストラクターは explicit。集成体の {} 初期化から呼べるよう明示的に構築する。
    renderer::DynamicStructuredBufferPool m_flowBuffers = renderer::DynamicStructuredBufferPool();
};

struct FiberSurface {
    renderer::Mesh* mesh=nullptr;
    renderer::ResourceHandle<renderer::BufferTag> vertices;
    renderer::ResourceHandle<renderer::ConstantBufferTag> skin,previousSkin;
    bool cast=true;
    bool validPreviousSkin=true;
    float dither=0;
    bool densityFromAlpha=false;  ///< @brief 地形の層指定時だけ true。葉の密度を頂点色 A で間引く
    /// @note コンピュートスキニング済みの頂点 (静的メッシュと同じ並び)。有効なら Shell は VS でスキニングし直さない。
    renderer::ResourceHandle<renderer::BufferTag> skinnedVertices;
    std::shared_ptr<renderer::PreparedSkinningTicket> preparedSkinning;
    uint32_t preparedSkinningWrite = UINT32_MAX;
    /// @note スキンの今の姿勢のワールド球 (毛丈込み)。得られないときは false で、カリングせずに描く。
    bool hasSkinBounds=false;
    WorldBounds skinBounds{};
    bool terrain = false;
};

FiberResources& GetFiberResources(renderer::ResourceManager& resources);
FiberFlowCache& UpdateFiberFlows(FiberResources& cache, Scene& scene, renderer::ResourceManager& resources);
uint32_t CountFiberFlows(const std::vector<ActiveFlowField>& fields, uint32_t channels, const math::Vector3& center, float radius);
const FiberMaterialEntry& GetFiberMaterial(FiberResources& cache, renderer::ResourceManager& resources, const std::string& path);
asset::FiberMaterialSettings ApplyFiberOverrides(const asset::FiberMaterialSettings& base, const FiberComponent& fiber,
    FiberResources& cache, renderer::ResourceManager& resources);
const FiberFinBuffers& GetFiberFinBuffers(FiberResources& cache, renderer::ResourceManager& resources,
    const renderer::Mesh& mesh, const FiberComponent& fiber, float bladeDensity, bool blades, bool densityFromAlpha, bool terrain);
bool HasFiberSurface(GameObject& owner, fbzz::LayerMask mask);
void CollectFiberSurfaces(GameObject& owner, const FiberComponent& fiber, float fiberPadding,
    FiberResources& cache, RenderPassContext& context, std::vector<FiberSurface>& surfaces);

} /// @note namespace fbzz::scene::fiber_extraction_detail
