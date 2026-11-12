/// @file    RenderWaterInput.hpp
/// @brief   水面の形状・波・材質の解決済み入力。
/// @author  Hasegawa Jin
/// @date    2026-09-21
#pragma once
#include <Graphics/Pipeline/RenderConstants.hpp>
#include <Graphics/Renderer/SurfacePatchLod.hpp>
#include <array>
namespace fbzz::renderer {
struct WaterStripIndices {
    ResourceHandle<BufferTag> buffer;
    uint32_t count = 0;
};

struct RenderWaterPatch {
    renderer::ResourceHandle<renderer::BufferTag> vertexBuffer;
    renderer::ResourceHandle<renderer::BufferTag> indexBuffer;
    uint32_t indexCount = 0;
    math::Vector3 aabbMin;
    math::Vector3 aabbMax;
    WaterStripIndices strip{};
};
struct WaterWaveMargin {
    float horizontal = 0.0f;
    float vertical   = 0.0f;
};

/// @note local[m] = dot(row.xyz,world[m])+row.w。半径はlocal空間、親と符号付きscaleは行に解決済み。
struct WaterMediumExclusionInput {
    std::array<math::Vector4, 3> worldToLocalRows{};
    math::Vector3 halfExtent{};
};

/// @note Sphere position/radius [m] とRGB散乱係数[m^-1]、強度。寿命はScene側が更新する。
struct WaterMediumPlumeInput {
    math::Vector4 worldPositionRadius{};
    math::Vector4 scatteringDensity{};
};

/// @note 一つの実効ownerだけを出力し、Sceneと物理への参照を持たない。
struct WaterMediumInput {
    uint64_t stableOwnerKey = 0;
    int32_t priority = 0;
    math::Vector4 worldBoundsXZ{};
    std::array<math::Vector4, 3> worldToLocalRows{};
    math::Vector4 absorptionPhaseG{};
    math::Vector4 scatteringLight{};
    float surfaceY = 0.0f;
    float bottomY = 0.0f;
    float depthLimit = 0.0f;
    float maxDistance = 120.0f;
    float transitionDepth = 0.15f;
    float causticsIntensity = 1.0f;
    float distortion = 0.001f;
    int lightSteps = 48;
    bool opticalEnabled = false;
    bool causticsEnabled = false;
    bool analyticRectangle = false;
    /// @note The sampled physical depth equals depthLimit everywhere, even when an auxiliary terrain texture is bound.
    bool depthIsUniform = false;
    math::Vector2 localHalfExtent{};
    std::vector<WaterMediumExclusionInput> exclusions;
    std::vector<WaterMediumPlumeInput> plumes;
};

struct RenderWaterInput {
    uint32_t layer = 0;
    bool selected = false;
    WaterCB constants{};
    WaterEffectParams effects{};
    std::vector<WaterMaterialBlendTarget> zoneMaterials;
    uint64_t horizonReceiverKey = 0;
    WaterMediumInput medium;
    WaterWaveMargin margin;
    math::Vector3 aabbMin, aabbMax;
    ResourceHandle<TextureTag> foam, ripple, detailNoise, velocityField;
    ResourceHandle<ShaderTag> shader;
    /// @note Extracted material owns this reflected b2 buffer; texture indices are bindless and do not replace water service slots.
    ResourceHandle<ConstantBufferTag> appearanceParameters;
    std::array<ResourceHandle<TextureTag>, 8> appearanceTextures{};
    std::vector<RenderWaterPatch> patches;
    /// @note GPU 実体は抽出側が所有し、選択結果は描画ビューごとに作る。
    bool adaptivePatchLod = false;
    /// @note 保存済み opt-in。selection mask を含む全ビューで色 pass と同じ morph を使う。
    bool patchMorphEnabled = false;
    SurfacePatchLodSettings patchLod;
    ResourceHandle<BufferTag> patchVertexBuffer;
    std::array<ResourceHandle<BufferTag>, 16> patchIndexBuffers{};
    std::array<uint32_t, 16> patchIndexCounts{};
    /// @note 水面と水中アトラスが同じ view 範囲へ延長する水平水域。near も共有パッチで描く。
    bool farOceanEnabled = false;
    float farOceanCoverageScale = 1.05f;
    float farOceanRippleFadeDistance = 4.0f;
    std::array<WaterStripIndices, 16> patchStrips{};
};
}
