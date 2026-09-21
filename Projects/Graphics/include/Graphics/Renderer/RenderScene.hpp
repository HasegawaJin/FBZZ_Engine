/// @file    RenderScene.hpp
/// @brief   メッシュ描画候補と解決済み資源の不変スナップショット。
/// @author  Hasegawa Jin
/// @date    2026-09-20
#pragma once
#include <Graphics/Renderer/RenderLightingInput.hpp>
#include <Graphics/Effects/RenderCustomPostInput.hpp>
#include <Graphics/Renderer/GeometryRoute.hpp>
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
#include <cstdint>
#include <vector>

namespace fbzz::renderer {

struct RenderMaterial {
    GeometryMaterialInput capabilities;
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
    /// @note Entity の識別値を不透明な整数として保持する。snapshotSerial の異なる入力間で参照を持ち越さない。
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
};

struct RenderMaskGroup {
    math::Vector4 payload;
    bool visibleOnly = true;
    std::vector<uint32_t> objectIndices;
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
    uint64_t frameStamp = 0;
    std::vector<RenderObject> objects;
    std::vector<RenderMeshItem> items;
    std::vector<RenderMaskGroup> maskGroups;
    uint32_t liveMaskRequests = 0;
};

} /// @note namespace fbzz::renderer
