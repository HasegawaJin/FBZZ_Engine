/// @file    RenderFiberInput.hpp
/// @brief   繊維表面と解決済み材質・接触・流れの描画入力。
/// @author  Hasegawa Jin
/// @date    2026-09-21
#pragma once
#include <Graphics/Pipeline/RenderConstants.hpp>
#include <Graphics/Effects/FiberMaterialSettings.hpp>
#include <Graphics/Renderer/FiberMotionHistory.hpp>
#include <memory>
#include <Graphics/Renderer/ShaderCapabilities.hpp>
#include <Graphics/Renderer/ShaderDescriptor.hpp>
#include <vector>
namespace fbzz::renderer {
class PreparedSkinningTicket;
enum class FiberRenderMode { SHELL, FIN, HYBRID, BLADE };
/// @brief 繊維の b5。風・時刻・層数・LOD と局所 FlowField の範囲。
/// @note LAYOUT: Assets/Shaders/Fiber/FiberCommon.hlsli の FiberFrameConstants と一致させる。
/// @note 風はワールド [m/s]、時刻 [s]。lod.x は Blade の提出率、lod.y は根元ディザの残存率 (1 で全部残す)。
/// @note lod.z は Blade の高さ分割数 [1,6]。0 は直接描画する既存呼出元との互換値で 6 として読む。
struct FiberFrameCB {
    math::Vector3 m_wind;
    float m_time = 0.0f;
    float m_shellCount = 1.0f;
    float m_hybrid = 0.0f;
    float m_turbulence = 0.0f;
    float m_pulseFrequency = 0.0f;
    math::Vector4 m_lod{1,1,0,0};
    /// @note 今フレームは [0, count)、前フレームは [previousFirst, +previousCount)。
    uint32_t m_flowCount = 0;
    uint32_t m_flowPreviousFirst = 0;
    uint32_t m_flowPreviousCount = 0;
    uint32_t m_flowChannels = 0;
};

/// @brief 繊維の b10。足元の接触 32 件 × (現在, 前フレーム)。
/// @note LAYOUT: FiberCommon.hlsli の FiberContactConstants と一致させる。radius (centers.w) が 0 の枠は無効。
struct FiberContactCB {
    std::array<math::Vector4,64> m_centers{};
    std::array<math::Vector4,64> m_times{};
    /// @note シェーダーが走査する枠数 [0,32]。今フレームは発生済みで回復しきっていない最後の枠 + 1。
    uint32_t m_count = 0;
    /// @note 前フレームは評価時刻が履歴ごとに違うため時刻で絞らず、半径を持つ最後の枠 + 1。
    uint32_t m_previousCount = 0;
    uint32_t m_padding[2]{};
};

/// @brief シェーダーが走査する接触の枠数を詰め直す。
/// @param time 今フレームの評価時刻 [s]。FiberFrameCB::m_time と同じ値を渡す。
/// @note 今フレームは «発生済み (age >= 0) かつ回復しきっていない (age < recovery)» 最後の枠 + 1。途中の無効枠はシェーダー側で飛ばす。
/// @note 前フレームは速度履歴ごとに評価時刻が 1〜2 フレーム前へずれるため、時刻では絞らず半径を持つ最後の枠 + 1。
inline void UpdateFiberContactCounts(FiberContactCB& data, float time)
{
    data.m_count = 0;
    data.m_previousCount = 0;
    for (uint32_t i = 0; i < 32; ++i) {
        const float age = time - data.m_times[i].x;
        const float recovery = data.m_times[i].z < 0.05f ? 0.05f : data.m_times[i].z;
        if (data.m_centers[i].w > 0.0f && age >= 0.0f && age < recovery) data.m_count = i + 1;
        if (data.m_centers[i + 32].w > 0.0f) data.m_previousCount = i + 1;
    }
}


struct FiberDrawSettings {
    FiberRenderMode m_mode = FiberRenderMode::SHELL;
    int m_shellCount = 16, m_minShellCount = 8, m_shadowShellCount = 6;
    int m_bladeSegments = 6, m_shadowBladeSegments = 0;
    bool m_distanceLod = true;
    float m_lodNear = 8, m_lodFar = 40;
};
struct FiberResolvedGeometry {
    ResourceHandle<BufferTag> m_vertices, m_indices;
    uint32_t m_indexCount = 0;
    ResourceHandle<StructuredBufferTag> m_blades;
    uint32_t m_bladeCount = 0;
};
/// @note A project family keeps the engine vertex, wind, coverage and 128-byte prefix in every pass.
struct FiberAppearance {
    ShaderVertexContract geometry = ShaderVertexContract::UNKNOWN;
    /// @note Color/GBuffer/Velocity/Shadow/Selection, each static then skinned; Blade has static entries only.
    std::array<ResourceHandle<ShaderTag>, 10> shaders{};
    /// @note Optional Shell companion StemFin family; Hybrid must have all ten compatible entries.
    std::array<ResourceHandle<ShaderTag>, 10> finShaders{};
    bool supportsHybrid = false;
    std::vector<uint8_t> params;
    ResourceHandle<ConstantBufferTag> materialCB;
};

[[nodiscard]] constexpr bool MatchesFiberAppearance(ShaderVertexContract geometry, FiberRenderMode mode, bool hasFinFamily = false)
{
    return (geometry == ShaderVertexContract::FIBER_SHELL
            && (mode == FiberRenderMode::SHELL || (mode == FiberRenderMode::HYBRID && hasFinFamily)))
        || (geometry == ShaderVertexContract::FIBER_FIN && mode == FiberRenderMode::FIN)
        || (geometry == ShaderVertexContract::FIBER_BLADE && mode == FiberRenderMode::BLADE);
}


/// @note These offsets form the geometry/deformation ABI. Project fields must follow the first 128 bytes.
[[nodiscard]] inline bool HasFiberMaterialPrefix(const ShaderDescriptor& desc)
{
    if (desc.cbufferSize < sizeof(FiberMaterialSettings)) return false;
    constexpr const char* names[]{"rootColor", "tipColor", "fiberLength", "fiberDensity", "fiberThickness",
        "fiberTaper", "fiberFrequency", "windResponse", "maxBend", "gravityBend", "roughness",
        "specularStrength", "transmission", "rootOcclusion", "planarShading", "worldMapping",
        "finSilhouetteWidth", "specularShift", "secondarySpecular", "colorVariation", "clumping", "clumpTwist"};
    constexpr uint32_t offsets[]{0, 16, 32, 36, 40, 44, 48, 52, 56, 60, 64, 68, 72, 76, 80, 84, 88, 92, 96, 100, 104, 108};
    for (size_t index = 0; index < std::size(names); ++index) {
        const auto* field = desc.FindVar(names[index]);
        if (!field || field->offset != offsets[index] || field->varType != ShaderVarType::Float
            || field->elements || field->rows != 1 || field->columns != (index < 2 ? 4 : 1)) return false;
    }
    for (const auto& texture : desc.textures)
        if (texture.slot == 5) return texture.constantOffset == 112;
    return false;
}

/// @note One material buffer is shared across a family; every pass and companion must agree on all reflected fields and texture indices.
[[nodiscard]] inline bool HaveMatchingFiberMaterialLayouts(const ShaderDescriptor& first, const ShaderDescriptor& second)
{
    if (first.cbufferSize != second.cbufferSize || first.textureMaskOffset != second.textureMaskOffset
        || first.vars.size() != second.vars.size() || first.textures.size() != second.textures.size()) return false;
    for (const auto& field : first.vars) {
        const auto* other = second.FindVar(field.name);
        if (!other || field.offset != other->offset || field.size != other->size
            || field.rows != other->rows || field.columns != other->columns
            || field.varClass != other->varClass || field.varType != other->varType
            || field.elements != other->elements || field.arrayStride != other->arrayStride
            || field.matrixStride != other->matrixStride || field.rowMajor != other->rowMajor) return false;
    }
    for (const auto& texture : first.textures) {
        bool matched = false;
        for (const auto& other : second.textures)
            if (texture.name == other.name && texture.slot == other.slot
                && texture.constantOffset == other.constantOffset) { matched = true; break; }
        if (!matched) return false;
    }
    return true;
}

struct RenderFiberInput {
    uint64_t identity = 0;
    uint32_t layer = 0;
    bool selected = false, skinned = false, cast = true, validPreviousSkin = true;
    bool hasBounds = false;
    float boundsRadius = 0;
    math::Vector3 boundsCenter{}, lodCenter{};
    PerObjectCB object{};
    FiberDrawSettings settings;
    FiberMaterialSettings material;
    std::shared_ptr<const FiberAppearance> appearance;
    FiberFrameCB frame;
    FiberContactCB contacts;
    ResourceHandle<BufferTag> vertices, indices, skinnedVertices;
    std::shared_ptr<PreparedSkinningTicket> preparedSkinning;
    uint32_t preparedSkinningWrite = UINT32_MAX;
    uint32_t indexCount = 0;
    ResourceHandle<ConstantBufferTag> skin, previousSkin;
    ResourceHandle<StructuredBufferTag> flows;
    ResourceHandle<TextureTag> velocityField;
    FiberResolvedGeometry fins, blades;
};
}
