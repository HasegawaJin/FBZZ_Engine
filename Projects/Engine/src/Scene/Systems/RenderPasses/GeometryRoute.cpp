/// @file    GeometryRoute.cpp
/// @brief   GBuffer と Forward のどちらへ流すかの唯一の規則。
/// @author  Hasegawa Jin
/// @date    2026-09-20
///
/// @see Docs/design/pipeline-boundary.md
#include <Engine/Scene/Systems/RenderPasses/GeometryRoute.hpp>

#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/MaterialAsset.hpp>
#include <Engine/Asset/MaterialParamBinding.hpp>
#include <Engine/Asset/ShaderCapabilities.hpp>
#include <Engine/Scene/Components/MaterialComponent.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <string>

namespace fbzz::scene {

namespace {

/// @brief 拡張ローブ (GBuffer 2 枚に入らない反射モデル) を持つか。
/// @note 判定の中身は従来の IsForwardOnly から «ローブの有無» の部分だけを切り出したもの。
/// @note シェーダーで決まる部分は IsGBufferEquivalentShader が持つ。
bool HasAdvancedLobe(const MaterialSlot& slot, const asset::MaterialAsset& material)
{
    const auto hasFeature = [&](std::string_view name) {
        const auto overrideIt = slot.paramOverrides.find(std::string(name));
        if (overrideIt != slot.paramOverrides.end())
            return !overrideIt->second.empty() && std::abs(overrideIt->second.front()) > 1.0e-4f;
        const auto* values = asset::FindMaterialParam(material, name);
        return values && !values->empty() && std::abs(values->front()) > 1.0e-4f;
    };

    if (hasFeature("clearcoat") || hasFeature("sheen") || hasFeature("anisotropy")) return true;
    /// @note Cloth は sheen=0 でも固有の反射モデルを保つ。色パラメーターの存在で見分ける。
    return asset::FindMaterialParam(material, "clothSheenColor") != nullptr;
}

} /// @note namespace

bool IsGBufferEquivalentShader(std::string_view shaderPath)
{
    return asset::ResolveShaderCapabilities(shaderPath).SupportsGBuffer();
}

renderer::GeometryMaterialInput ExtractGeometryMaterial(const MaterialSlot& slot)
{
    return ExtractGeometryMaterial(slot, asset::AssetManager::Get<asset::MaterialAsset>(slot.materialAsset));
}

renderer::GeometryMaterialInput ExtractGeometryMaterial(
    const MaterialSlot& slot, const asset::MaterialAsset* material)
{
    renderer::GeometryMaterialInput input;
    input.blend = slot.hasBlendModeOverride ? slot.blendModeOverride
        : material ? material->blendMode : renderer::BlendMode::OPAQUE_BLEND;

    if (material == nullptr) {
        /// @note 材質が解決できていない。何で描かれるか分からないものを GBuffer へ入れない。
        input.gbufferEquivalentShader = false;
        return input;
    }

    input.gbufferEquivalentShader = IsGBufferEquivalentShader(material->shaderPath);
    input.advancedLobe            = HasAdvancedLobe(slot, *material);
    return input;
}

GeometryRoute ResolveGeometryRoute(const MaterialSlot& slot, bool gbufferPipeline)
{
    return renderer::ResolveGeometryRoute(ExtractGeometryMaterial(slot), gbufferPipeline);
}

} /// @note namespace fbzz::scene
