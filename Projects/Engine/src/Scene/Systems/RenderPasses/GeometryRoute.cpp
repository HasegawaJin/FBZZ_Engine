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
#include <Engine/Scene/Components/MaterialComponent.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <string>

namespace fbzz::scene {

namespace {

/// @brief `GBuffer.hlsl` へ置き換えても絵が変わらないエンジンのシェーダー。
/// @note ここに載せてよいのは «標準 PBR のローブで、GBuffer の 2 枚に全部入る» ものだけ。
/// @note Toon / RimLight / Unlit / Dissolve / Anisotropic / Subsurface / Cloth は載せない。
/// @note 置き換えると陰影のモデルそのものが変わる。
/// @see Docs/design/pipeline-boundary.md §2.1
constexpr std::array<std::string_view, 6> kGBufferEquivalentShaders{
    "PBR.hlsl",
    "Lit.hlsl",
    "Fallback.hlsl",
    "SkinnedPBR.hlsl",
    "SkinnedLit.hlsl",
    "FallbackSkinned.hlsl",
};

/// @brief 参照文字列からシェーダーのファイル名を取り出す。
/// @note `.mat` の shader は 3 つの形を取る: 素のパス / `guid:...` / `guid:...|パス`。
/// @note 後ろ 2 つは AssetManager に解決させてからファイル名を切る。
std::string_view ShaderFileName(std::string_view path)
{
    /// @note `guid:...|パス` は縦棒の後ろが «人が読める側» の控え。解決を待たずに使える。
    if (const std::size_t bar = path.rfind('|'); bar != std::string_view::npos)
        path = path.substr(bar + 1u);

    if (const std::size_t slash = path.find_last_of("/\\"); slash != std::string_view::npos)
        path = path.substr(slash + 1u);
    return path;
}

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
    /// @note 空欄は «既定の材質» で、実際には Fallback が使われる。Forward へ落とす理由が無い。
    if (shaderPath.empty()) return true;

    std::string_view name = ShaderFileName(shaderPath);

    /// @note `guid:` だけでパスの控えが無い場合はここで初めて解決する。実在ファイルを引くので
    /// @note 毎フレーム全材質に対して呼ばれないよう、呼び出し側は解決済みの結果を使い回すこと。
    std::string resolved;
    if (name.starts_with("guid:")) {
        resolved = asset::AssetManager::ResolveAssetPath(std::string(shaderPath));
        if (resolved.empty()) return false;
        name = ShaderFileName(resolved);
    }

    return std::find(kGBufferEquivalentShaders.begin(), kGBufferEquivalentShaders.end(), name)
        != kGBufferEquivalentShaders.end();
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
