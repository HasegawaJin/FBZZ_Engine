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
#include <Math/Matrix4.hpp>
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>
#include <array>
#include <Graphics/Renderer/ShaderCapabilities.hpp>
#include <cstdint>
#include <vector>

namespace fbzz::renderer {

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
    bool reliableOccluder = false;
    math::Vector3 boundsCenter{};
    float boundsRadius = 0.0f;
};

struct RenderObject {
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
    ResourceHandle<ConstantBufferTag> skinningPalette;
    ResourceHandle<ConstantBufferTag> previousSkinningPalette;
    bool previousSkinningValid = false;
    /// @note LODGroup membership needs a camera-independent canonical ray shape; lodVisible alone cannot prove coverage.
    bool rayLodSelectionRequired = false;
    /// @note レイは LOD0 を固定使用し、Raster の選択・crossfade・cull とは独立する。
    bool rayVisible = true;
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

/// @note GameObject / Component / Model / Mesh / Material のポインターを保持しない。
/// @note GPU ハンドルは非所有。記録中の資源解放・差し替えを禁止し、GPU 退役は ResourceManager に委ねる。
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
    std::vector<RenderMeshTrailInput> meshTrails;
    std::vector<RenderParticleInput> particles;
    uint64_t snapshotSerial = 0;
    uint64_t sceneGeneration = 0;
    uint64_t frameStamp = 0;
    std::vector<RenderObject> objects;
    std::vector<RenderMeshItem> items;
    std::vector<RenderRayLodDiagnostic> rayLodDiagnostics;
    std::vector<RenderMaskGroup> maskGroups;
    uint32_t liveMaskRequests = 0;
    std::vector<RenderRayEffectSource> rayUnsupportedEffects;
};

} /// @note namespace fbzz::renderer
