/// @file    ShaderCapabilities.hpp
/// @brief   シェーダーが明示する表面・頂点・被覆と補助パスの契約。
/// @author  Hasegawa Jin
/// @date    2026-10-02
#pragma once
#include <cstdint>

namespace fbzz::renderer {

enum class ShaderVertexContract : uint8_t { UNKNOWN, STANDARD_SURFACE, STANDARD_SKINNED };
enum class ShaderSurfaceContract : uint8_t { UNKNOWN, STANDARD_PBR, LAMBERT, UNLIT, WATER, TERRAIN };
enum class ShaderOpacityContract : uint8_t { UNKNOWN, OPAQUE_OUTPUT, ALPHA_CLIP };
enum class ShaderVariantSet : uint8_t { NONE, STANDARD_SURFACE, STANDARD_SKINNED };
enum class ShaderPreviewKind : uint8_t {
    UNKNOWN, SURFACE, SKINNED, WATER, TERRAIN, UI, PARTICLE, TRAIL, DECAL,
    POST_PROCESS, MESH_TRAIL, GPU_PARTICLE, FIBER_SHELL, FIBER_FIN, FIBER_BLADE
};

/// @note 宣言の無い独自シェーダーは Forward のみ。ファイル名・配置・GUID のヒントは能力の根拠にしない。
/// @see Docs/design/shader-capabilities.md
struct ShaderCapabilities {
    ShaderVertexContract vertex = ShaderVertexContract::UNKNOWN;
    ShaderSurfaceContract surface = ShaderSurfaceContract::UNKNOWN;
    ShaderOpacityContract opacity = ShaderOpacityContract::UNKNOWN;
    ShaderVariantSet variants = ShaderVariantSet::NONE;
    ShaderPreviewKind preview = ShaderPreviewKind::UNKNOWN;

    [[nodiscard]] constexpr bool SupportsSkinning() const { return vertex == ShaderVertexContract::STANDARD_SKINNED; }
    [[nodiscard]] constexpr bool IsStandardPbr() const { return surface == ShaderSurfaceContract::STANDARD_PBR; }
    [[nodiscard]] constexpr bool HasOpaqueOutput() const { return opacity == ShaderOpacityContract::OPAQUE_OUTPUT; }
    [[nodiscard]] constexpr bool HasStandardGeometry() const
    {
        return vertex != ShaderVertexContract::UNKNOWN && opacity != ShaderOpacityContract::UNKNOWN;
    }
    /// @note 標準の変形・UV・alpha clip を共有する static / instanced / skinned GBuffer 変種の組を要求する。
    [[nodiscard]] constexpr bool HasStandardVariants() const
    {
        return (vertex == ShaderVertexContract::STANDARD_SURFACE && variants == ShaderVariantSet::STANDARD_SURFACE)
            || (vertex == ShaderVertexContract::STANDARD_SKINNED && variants == ShaderVariantSet::STANDARD_SKINNED);
    }
    [[nodiscard]] constexpr bool SupportsGBuffer() const
    {
        return IsStandardPbr() && opacity == ShaderOpacityContract::ALPHA_CLIP && HasStandardVariants();
    }
};

} /// @note namespace fbzz::renderer
