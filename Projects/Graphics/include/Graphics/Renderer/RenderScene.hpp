/// @file    RenderScene.hpp
/// @brief   メッシュ描画候補と解決済み資源の不変スナップショット。
/// @author  Hasegawa Jin
/// @date    2026-09-20
#pragma once
#include <Graphics/Renderer/RenderLightingInput.hpp>
#include <Graphics/Effects/RenderCustomPostInput.hpp>
#include <Graphics/Renderer/GeometryRoute.hpp>
#include <Graphics/RayTracing/RayMaterialCapabilities.hpp>
#include <Graphics/RayTracing/SurfaceMaterialData.hpp>
#include <Graphics/Renderer/ResourceHandle.hpp>
#include <Graphics/Renderer/RenderEnvironment.hpp>
#include <Graphics/Effects/RenderTrailInput.hpp>
#include <Graphics/Effects/RenderDecalInput.hpp>
#include <Graphics/Effects/RenderTerrainInput.hpp>
#include <Graphics/Effects/RenderWaterInput.hpp>
#include <Graphics/Effects/RenderFiberInput.hpp>
#include <Graphics/Effects/RenderMeshTrailInput.hpp>
#include <Graphics/Effects/RenderParticleInput.hpp>
#include <Graphics/Effects/RenderInstanceInput.hpp>
#include <Math/Matrix4.hpp>
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>
#include <array>
#include <Graphics/Renderer/ShaderCapabilities.hpp>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace fbzz::renderer {

class PreparedSkinningTicket;

struct RenderMaterial {
    ShaderCapabilities shaderCapabilities;
    GeometryMaterialInput capabilities;
    RayMaterialCapabilities rayCapabilities;
    SurfaceMaterialData surface;
    ResourceHandle<ShaderTag> shader;
    ResourceHandle<ConstantBufferTag> paramsBuffer;
    std::array<ResourceHandle<TextureTag>, 8> textures{};
    std::array<uint8_t, 96> gbufferParams{};
    bool directGBufferParams = false;
    bool valid = false;
    int32_t renderQueue = 2000;
    bool doubleSided = false;
    int32_t depthBias = 0;
    float depthBiasSlope = 0.0f;
};

struct RenderMeshItem {
    uint32_t objectIndex = 0;
    uint32_t materialSlot = 0;
    uint32_t sourceSubmesh = 0;
    ResourceHandle<BufferTag> vertexBuffer;
    ResourceHandle<BufferTag> indexBuffer;
    /// @note VS スキニング入力。モーフがある場合はインスタンス専用の頂点列。
    ResourceHandle<BufferTag> skinningVertexBuffer;
    /// @note 当該フレームに計算済みの静的レイアウト。VS スキニングへ渡してはならない。
    ResourceHandle<BufferTag> deformedVertexBuffer;
    /// @note 元姿勢の出力 lease は Scene/handle の退役から独立し、古い snapshot の consumer も保持する。
    std::shared_ptr<PreparedSkinningTicket> preparedSkinning;
    uint32_t preparedSkinningWrite = UINT32_MAX;
    uint32_t indexCount = 0;
    uint32_t vertexCount = 0;
    /// @note 内容版 0 は未解決または GPU 変形専用。バッファの世代と独立して Update を識別する。
    uint64_t vertexContentVersion = 0;
    uint64_t indexContentVersion = 0;
    uint64_t deformedContentVersion = 0;
    uint32_t vertexStride = 0;
    uint32_t vertexPositionOffset = 0;
    RenderMaterial material;
    /// @note Surface 材質をスキンドに誤指定したときの Forward 用代替。
    RenderMaterial forwardMaterial;
    bool visible = true;
    bool slotVisible = true;
    /// @note 形状密度の粗い指標。遮蔽被覆の証明には occluderRadius と材質契約を使う。
    bool reliableOccluder = false;
    math::Vector3 boundsCenter{};
    float boundsRadius = 0.0f;
    /// @note 実形状内部のワールド球。最小軸スケールで縮小し、カリング余白は加えない。
    float occluderRadius = 0.0f;
};

/// @note Palette handles are valid for this extracted frame only; ordinary snapshot GPU handle lifetime rules apply.
struct RenderGpuBonePalette {
    ResourceHandle<StructuredBufferTag> current, previous;
    uint32_t currentOffset = 0, previousOffset = 0;
};
struct RenderObject {
    RenderGpuBonePalette gpuBones;
    /// @note sceneGeneration と組にした Entity ID。vector 添字や入力への参照は snapshotSerial を跨いで持ち越さない。
    uint32_t sourceIndex = 0;
    uint32_t sourceGeneration = 0;
    uint32_t firstItem = 0;
    uint32_t itemCount = 0;
    uint32_t layer = 0;
    bool skinned = false;
    bool castShadows = true;
    bool lodVisible = true;
    bool colorEligible = false;
    bool selected = false;
    float lodDither = 0.0f;
    math::Matrix4 world = math::Matrix4::Identity();
    math::Matrix4 worldInvTranspose = math::Matrix4::Identity();
    math::Matrix4 previousWorld = math::Matrix4::Identity();
    /// @note 既存 Forward の並べ替え基準を保つローカル位置。描画行列は world を使う。
    math::Vector3 sortPosition{};
    math::Vector3 boundsCenter{};
    float boundsRadius = 0.0f;
    /// @note Immutable geometry witness in world metres; -1 means the leaf material was unresolved at extraction.
    float leafWindBoundsPadding = 0.0f;
    ResourceHandle<ConstantBufferTag> skinningPalette;
    ResourceHandle<ConstantBufferTag> previousSkinningPalette;
    bool previousSkinningValid = false;
    /// @note LODGroup membership needs a camera-independent canonical ray shape; lodVisible alone cannot prove coverage.
    bool rayLodSelectionRequired = false;
    /// @note レイは LOD0 を固定使用し、Raster の選択・crossfade・cull とは独立する。
    bool rayVisible = true;
    /// @note ホストが静的な版と変換を検証できる場合だけベイク候補にする。
    bool shadowBakeStatic = false;
};

struct RenderInstancePart {
    RenderMeshItem mesh;
    uint32_t lod=0;
};
/// @note 各配置元の 128m ワールド XZ セル内で、高精細描画に使う安定順の個体数。0 は上限なし。
struct InstanceVegetationBudget {
    bool enabled = false;
    uint32_t drawDetailPerCell = 0;
    uint32_t nearDetailPerCell = 0;
    uint32_t shadowDetailPerCell = 0;
};
struct RenderInstanceInput {
    std::array<uint64_t,2> identity{};
    uint32_t layer=0;
    bool selected=false, castShadows=true;
    bool selectionOnly=false;
    float maxDistance=0;
    math::Matrix4 world=math::Matrix4::Identity();
    math::Vector3 localBoundsCenter{};
    /// @note Must enclose every LOD's geometry and its maximum leaf wind displacement in set-local units.
    float localBoundsRadius=0;
    std::array<float,4> lodHeights{};
    uint32_t lodCount=0;
    std::shared_ptr<const InstanceSetData> data;
    std::vector<RenderInstancePart> parts;
    InstanceVegetationBudget vegetationBudget;
};

struct RenderMaskGroup {
    math::Vector4 payload;
    bool visibleOnly = true;
    std::vector<uint32_t> objectIndices;
};

struct RenderRayLodDiagnostic {
    uint32_t sourceIndex = 0;
    uint32_t sourceGeneration = 0;
    uint32_t layerMask = UINT32_MAX;
};

enum class RenderRayUnsupportedEffect : uint8_t { PARTICLE, FIBER };

/// @note These sources are collected independently of Raster draw-list culling and retain their complete Scene owner.
struct RenderRayEffectSource {
    uint32_t sourceIndex = 0;
    uint32_t sourceGeneration = 0;
    uint32_t layerMask = 0;
    RenderRayUnsupportedEffect kind = RenderRayUnsupportedEffect::PARTICLE;
};

/// @brief Immutable bounds journal produced while current geometry inputs are validated.
/// @note Nonzero versions identify geometry publications, independently of per-view snapshotSerial.
/// @note A delta is complete only for the exact baseVersion, Scene generation and objectCount; gaps require full discovery.
/// @note Ordinals address this publication's Objects(); membership, order or renderer lifetime changes require fullReplacement.
struct RenderSpatialUpdate {
    uint64_t version = 0;
    uint64_t baseVersion = 0;
    size_t objectCount = 0;
    bool fullReplacement = true;
    std::vector<uint32_t> changedObjectOrdinals;
};

/// @note GameObject / Component / Model / Mesh / Material のポインターを保持しない。
/// @note 通常 GPU ハンドルは非所有。preparedSkinning の出力だけは ticket が元入力と native lease を保持する。
/// @note 候補はカメラで絞らない。各ビューがレイヤー・LOD・視錐台・距離を評価する。
struct RenderScene {
    std::vector<RenderCustomPostInput> customPost;
    RenderLightingInput lighting;
    RenderEnvironmentInput environment;
    std::vector<RenderTrailInput> trails;
    std::vector<RenderDecalInput> decals;
    std::vector<RenderTerrainInput> terrains;
    std::vector<RenderWaterInput> water;
    std::vector<RenderFiberInput> fibers;
    std::vector<RenderInstanceInput> instances;
    std::vector<RenderMeshTrailInput> meshTrails;
    std::vector<RenderParticleInput> particles;
    uint64_t snapshotSerial = 0;
    uint64_t sceneGeneration = 0;
    uint64_t frameStamp = 0;
    std::vector<RenderObject> objects;
    /// @note 同一フレームの姿勢は不変。各ビューの選択・材質・変形入力はこの配列へ書き込まない。
    std::shared_ptr<const std::vector<RenderObject>> sharedObjects;
    /// @note Absent for untrusted/manual sources; shared geometry and material-only publications retain the same immutable journal.
    std::shared_ptr<const RenderSpatialUpdate> spatialUpdate;
    std::vector<uint8_t> objectSelection;
    std::vector<RenderMeshItem> items;
    std::vector<RenderRayLodDiagnostic> rayLodDiagnostics;
    std::vector<RenderMaskGroup> maskGroups;
    uint32_t liveMaskRequests = 0;
    std::vector<RenderRayEffectSource> rayUnsupportedEffects;
    /// @note Engine の正規抽出では静的影候補の ordinal だけを保持する。
    std::vector<uint32_t> shadowBakeMeshObjects;

    [[nodiscard]] std::span<const RenderObject> Objects() const
    {
        return sharedObjects ? std::span<const RenderObject>(*sharedObjects) : std::span<const RenderObject>(objects);
    }

    [[nodiscard]] bool IsObjectSelected(size_t index) const
    {
        return objectSelection.empty() ? Objects()[index].selected : objectSelection[index] != 0;
    }
};

} /// @note namespace fbzz::renderer
