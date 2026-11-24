/// @file    RenderSceneMaterialBindings.cpp
/// @brief   同期済み材質を GBuffer とレイトレーシングの入力に変換する。
/// @author  Hasegawa Jin
/// @date    2026-11-21
#include "RenderSceneMaterialBindings.hpp"
#include "RenderSceneMaterialResolver.hpp"

#include <Engine/Renderer/IShader.hpp>
#include <Graphics/Pipeline/RayTracingPipeline.hpp>
#include <Graphics/Renderer/BuiltinShader.hpp>
#include <Graphics/Renderer/Material.hpp>
#include <Graphics/Renderer/ResourceManager.hpp>
#include <cmath>
#include <cstring>

namespace fbzz::scene::render_extraction_detail {

namespace {

struct GBufferFieldSpec {
    std::string_view name;
    uint32_t offset;
    uint32_t bytes;
};

/// @note オフセットは標準 GBuffer の MaterialCB に対応する。
constexpr std::array GBUFFER_FIELDS{
    GBufferFieldSpec{"albedo", 0, 16}, GBufferFieldSpec{"metallic", 16, 4},
    GBufferFieldSpec{"roughness", 20, 4}, GBufferFieldSpec{"normalStrength", 24, 4},
    GBufferFieldSpec{"occlusionStrength", 28, 4}, GBufferFieldSpec{"emissiveColor", 32, 12},
    GBufferFieldSpec{"emissiveScale", 44, 4}, GBufferFieldSpec{"uvTiling", 48, 8},
    GBufferFieldSpec{"uvOffset", 56, 8}, GBufferFieldSpec{"alphaCutoff", 64, 4},
    GBufferFieldSpec{"leafWindStrength", 68, 4}, GBufferFieldSpec{"leafWindFrequency", 72, 4},
    GBufferFieldSpec{"leafWindRootHeight", 76, 4}, GBufferFieldSpec{"parallaxDepth", 92, 4}};

bool IsGBufferCopyVariable(const renderer::ShaderVarDesc& variable, uint32_t bytes)
{
    return variable.IsWritable() && variable.varType == renderer::ShaderVarType::Float
        && variable.varClass == (bytes == sizeof(float) ? renderer::ShaderVarClass::Scalar : renderer::ShaderVarClass::Vector)
        && variable.rows == 1 && variable.elements == 0 && variable.size == bytes;
}

void CopyGBufferField(const std::vector<uint8_t>& source, uint32_t sourceOffset,
    std::array<uint8_t, 96>& output, uint32_t destinationOffset, uint32_t bytes)
{
    if (sourceOffset <= source.size() && bytes <= source.size() - sourceOffset)
        std::memcpy(output.data() + destinationOffset, source.data() + sourceOffset, bytes);
}

}

GBufferCopyPlan::GBufferCopyPlan(const renderer::ShaderDescriptor& descriptor)
{
    static_assert(GBUFFER_FIELDS.size() == std::tuple_size_v<decltype(fields)>);
    for (size_t index = 0; index < fields.size(); ++index) {
        const auto& spec = GBUFFER_FIELDS[index];
        auto& field = fields[index];
        field.destinationOffset = spec.offset;
        field.bytes = spec.bytes;
        if (const auto* variable = descriptor.FindVar(spec.name)) {
            field.variableIndex = static_cast<size_t>(variable - descriptor.vars.data());
            field.sourceOffset = variable->offset;
            field.writable = IsGBufferCopyVariable(*variable, field.bytes);
        }
    }
}

bool GBufferCopyPlan::Matches(const renderer::ShaderDescriptor& prepared, const renderer::ShaderDescriptor& current) const
{
    if (prepared.cbufferSize != current.cbufferSize || prepared.vars.size() != current.vars.size()) return false;
    /// @note 同じ配列を直接編集しても、欠落・同名変数を含む FindVar の先頭一致を維持する。
    for (size_t index = 0; index < prepared.vars.size(); ++index)
        if (prepared.vars[index].name != current.vars[index].name) return false;
    for (const auto& field : fields) {
        if (field.variableIndex == SIZE_MAX) continue;
        const auto& variable = current.vars[field.variableIndex];
        if (IsGBufferCopyVariable(variable, field.bytes) != field.writable
            || (field.writable && variable.offset != field.sourceOffset)) return false;
    }
    return true;
}

void GBufferCopyPlan::Copy(const std::vector<uint8_t>& source, std::array<uint8_t, 96>& output) const
{
    for (const auto& field : fields)
        if (field.writable)
            CopyGBufferField(source, field.sourceOffset, output, field.destinationOffset, field.bytes);
}

void CopyResolvedMaterial(renderer::RenderMaterial& output, const renderer::Material* material,
                  renderer::ResourceManager& resources, const MaterialSlot& slot, RenderMaterialResolver& materialResolver)
{
    output.valid = material != nullptr;
    if (!material) {
        output.capabilities.gbufferEquivalentShader = resources.GetShaderCapabilities(output.shader).SupportsGBuffer();
        return;
    }
    output.shader = material->shader;
    /// @note 失敗したリロードの宣言を混ぜないよう、実行中のシェーダーが公開した能力を使う。
    output.shaderCapabilities = resources.GetShaderCapabilities(material->shader);
    output.paramsBuffer = material->paramsBuffer;
    for (size_t textureSlot = 0; textureSlot < output.textures.size() && textureSlot < material->textures.size();
         ++textureSlot) {
        output.textures[textureSlot] = material->textures[textureSlot];
    }
    /// @note 異なるシェーダーの CB レイアウトを GBuffer の 96 バイトへ正規化する。
    output.directGBufferParams = false;
    output.gbufferParams = {};
    if (renderer::IsErrorShader(material->shaderPath)) {
        /// @note 壊れた材質の透過・色・テクスチャ上書きで診断表示が消えないよう、実効状態を揃える。
        output.capabilities = {};
        output.capabilities.blend = renderer::BlendMode::OPAQUE_BLEND;
        output.capabilities.gbufferEquivalentShader = output.shaderCapabilities.SupportsGBuffer();
        output.renderQueue = renderer::RenderQueue::GEOMETRY;
        output.doubleSided = true;
        output.depthBias = 0;
        output.depthBiasSlope = 0;
        output.paramsBuffer = {};
        output.textures = {};
        const float magenta[] = {1, 0, 1, 1};
        std::memcpy(output.gbufferParams.data(), magenta, sizeof(magenta));
        return;
    }
    output.capabilities.gbufferEquivalentShader = output.shaderCapabilities.SupportsGBuffer();
    const GBufferCopyPlan* copyPlan = nullptr;
    const renderer::ShaderDescriptor* preparedLayout = nullptr;
    if (materialResolver.CopyPacked(slot, *material, output, resources, copyPlan, preparedLayout)) return;
    const float roughness = 0.5f;
    const float tiling[] = { 1.0f, 1.0f };
    std::memcpy(output.gbufferParams.data() + 20, &roughness, sizeof(roughness));
    std::memcpy(output.gbufferParams.data() + 48, tiling, sizeof(tiling));
    const auto* shader = resources.Get(material->shader);
    if (!shader) return;
    const auto& descriptor = shader->GetDescriptor();
    if (copyPlan && preparedLayout && copyPlan->Matches(*preparedLayout, descriptor))
        copyPlan->Copy(material->paramData, output.gbufferParams);
    else {
        for (const auto& field : GBUFFER_FIELDS) {
            const auto* variable = descriptor.FindVar(field.name);
            if (variable && IsGBufferCopyVariable(*variable, field.bytes))
                CopyGBufferField(material->paramData, variable->offset, output.gbufferParams, field.offset, field.bytes);
        }
    }
    if (descriptor.textureMaskOffset != UINT32_MAX)
        CopyGBufferField(material->paramData, descriptor.textureMaskOffset, output.gbufferParams, 80, sizeof(uint32_t));
    /// @note 同じ 96 bytes でも変数の位置は異なり得る。正規化した値が一致するときだけ元 CB を共有する。
    output.directGBufferParams = material->paramData.size() == output.gbufferParams.size()
        && std::memcmp(material->paramData.data(), output.gbufferParams.data(), output.gbufferParams.size()) == 0;
    materialResolver.RememberPacked(slot, *material, output, resources.GetShaderVersion());
}

void ExtractRayMaterialInputs(renderer::RenderMaterial& material,
    const renderer::Material* source, const MaterialSlot& slot, renderer::ResourceManager& resources,
    bool currentDeformationVerified)
{
    const auto contract = material.shaderCapabilities;
    if (!source) material.capabilities.gbufferEquivalentShader = contract.SupportsGBuffer();
    const bool deformationVerified = !contract.SupportsSkinning() || currentDeformationVerified;
    const bool pbr = contract.IsStandardPbr() && deformationVerified;
    const bool unclipped = contract.HasOpaqueOutput();
    const auto* shader = resources.Get(material.shader);
    const bool standardGeometry = material.valid && shader && contract.HasStandardGeometry() && deformationVerified;
    uint32_t declaredTextureMask = 0;
    bool unresolvedTexture = false;
    const auto declareTexture = [&](const std::string& name, const std::string& texturePath) {
        if (texturePath.empty()) return;
        for (uint32_t i = 0; i < renderer::kMaterialTextureSlotCount; ++i) {
            if (name != renderer::kMaterialTextureSlots[i].key) continue;
            declaredTextureMask |= 1u << i;
            return;
        }
        unresolvedTexture = true;
    };
    const auto materialAssetSnapshot = asset::ReadMaterialAuthoring(slot.GetMaterialAsset());
    const auto slotAuthoringSnapshot = slot.ReadAuthoring();
    const auto* materialAsset = materialAssetSnapshot ? &materialAssetSnapshot->Data() : nullptr;
    const auto& textureOverrides = slotAuthoringSnapshot->Data().textureOverrides;
    /// @note 未ロードや無効な GPU handle でも、元 .mat の texture 指定を定数材質と誤認しない。
    if (materialAsset) {
        for (const auto& [name, texturePath] : materialAsset->textures)
            if (!textureOverrides.contains(name)) declareTexture(name, texturePath);
    }
    for (const auto& [name, texturePath] : textureOverrides)
        declareTexture(name, texturePath);
    bool advancedLobes = false;
    if (pbr && shader && source) {
        /// @note GBuffer の 96 bytes 外にある追加ローブを、定数 metallic / roughness と誤認しない。
        for (const auto* name : {"clearcoat", "sheen", "anisotropy"}) {
            const auto* variable = shader->GetDescriptor().FindVar(name);
            if (!variable) continue;
            if (variable->offset > source->paramData.size()
                || sizeof(float) > source->paramData.size() - variable->offset) {
                advancedLobes = true;
                continue;
            }
            float amount = 0;
            std::memcpy(&amount, source->paramData.data() + variable->offset, sizeof(amount));
            advancedLobes |= !std::isfinite(amount) || amount != 0;
        }
    }
    material.surface = source ? source->ResolveRaySurface(material.gbufferParams, standardGeometry && pbr,
        declaredTextureMask, unresolvedTexture, advancedLobes, resources)
        : renderer::ResolveConstantSurfaceMaterial(material.gbufferParams, false);
    /// @note 表面解決では GPU 未転送のテクスチャ指定も残るため、被覆判定にはそのマスクを使う。
    if (unclipped) {
        /// @note Lit / Fallback はテクスチャのアルファ値にかかわらず alpha=1 を返し、clip を実行しない。
        material.rayCapabilities = standardGeometry
            ? renderer::RayMaterialCapabilities{true, renderer::RayOpacity::OPAQUE_SURFACE}
            : renderer::RayMaterialCapabilities{};
    } else {
        float alpha = 0;
        float cutoff = 0;
        std::memcpy(&alpha, material.gbufferParams.data() + 12, sizeof(alpha));
        std::memcpy(&cutoff, material.gbufferParams.data() + 64, sizeof(cutoff));
        material.rayCapabilities = renderer::ResolveStaticRayMaterialCapabilities(standardGeometry,
            material.capabilities.blend, alpha, cutoff, (material.surface.textureMask & 1u) != 0);
    }
}

}
