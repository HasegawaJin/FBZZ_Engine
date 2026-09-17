/// @file    MaterialParamBinding.cpp
/// @brief   MaterialParamBinding.hpp の実装
/// @author  Hasegawa Jin
/// @date    2026-08-22
#include <Engine/Asset/MaterialParamBinding.hpp>

#include <algorithm>
#include <cstring>

namespace fbzz::asset {

const std::vector<float>* FindMaterialParam(const MaterialAsset& asset,
                                            std::string_view shaderVarName)
{
    auto it = asset.params.find(std::string(shaderVarName));
    if (it != asset.params.end()) return &it->second;

    /// @note snake_case 別名 (手書き .mat 向け)
    if (shaderVarName == "normalStrength") it = asset.params.find("normal_strength");
    else if (shaderVarName == "emissiveColor")  it = asset.params.find("emissive_color");
    else if (shaderVarName == "emissiveScale")  it = asset.params.find("emissive_scale");
    else if (shaderVarName == "clearcoatRoughness") it = asset.params.find("clearcoat_roughness");
    else if (shaderVarName == "sheenColor") it = asset.params.find("sheen_color");

    return it != asset.params.end() ? &it->second : nullptr;
}

void InitDefaultMaterialParams(const renderer::ShaderDescriptor& desc,
                               std::vector<uint8_t>& paramData)
{
    /// @note シェーダーの全 float 変数をまず 1.0f で初期化する。未知のカスタムパラメータが
    ///       0 のままだと乗算スケール系変数が非表示になるため。1.0f は乗算の単位元であり、
    ///       加算オフセット (uvOffset 等) は次ステップで 0 に上書きされる。
    const float one = 1.0f;
    for (const auto& v : desc.vars) {
        if (v.varType != renderer::ShaderVarType::Float) continue;
        for (uint32_t col = 0; col < v.columns; ++col) {
            const uint32_t byteOff = v.offset + col * sizeof(float);
            if (byteOff + sizeof(float) <= static_cast<uint32_t>(paramData.size()))
                std::memcpy(paramData.data() + byteOff, &one, sizeof(float));
        }
    }

    /// @note Step 2: 標準 PBR パラメータを正しいデフォルト値で上書きする。
    auto setFloat = [&](std::string_view name, float value) {
        const auto* v = desc.FindVar(name);
        if (!v || v->varType != renderer::ShaderVarType::Float || v->columns != 1) return;
        if (v->offset + sizeof(float) > static_cast<uint32_t>(paramData.size())) return;
        std::memcpy(paramData.data() + v->offset, &value, sizeof(float));
    };
    auto setFloat2 = [&](std::string_view name, const float value[2]) {
        const auto* v = desc.FindVar(name);
        if (!v || v->varType != renderer::ShaderVarType::Float || v->columns < 2) return;
        if (v->offset + 2u * sizeof(float) > static_cast<uint32_t>(paramData.size())) return;
        std::memcpy(paramData.data() + v->offset, value, 2u * sizeof(float));
    };
    auto setFloat3 = [&](std::string_view name, const float value[3]) {
        const auto* v = desc.FindVar(name);
        if (!v || v->varType != renderer::ShaderVarType::Float || v->columns < 3) return;
        if (v->offset + 3u * sizeof(float) > static_cast<uint32_t>(paramData.size())) return;
        std::memcpy(paramData.data() + v->offset, value, 3u * sizeof(float));
    };

    const float uvTiling[2] = { 1.0f, 1.0f };
    const float uvOffset[2] = { 0.0f, 0.0f };

    const float white3[3] = { 1.0f, 1.0f, 1.0f };
    setFloat("metallic",       0.0f);
    setFloat("roughness",      0.65f);
    /// @note 拡張 PBR ローブは既定で無効にする。1.0f のままだと既存マテリアルの見た目と
    ///       エネルギー配分が変わるため、明示的に有効化された場合だけ Forward へ送る。
    setFloat("clearcoat",             0.0f);
    setFloat("clearcoatRoughness",    0.10f);
    setFloat("sheen",                 0.0f);
    setFloat("anisotropy",             0.0f);
    setFloat3("sheenColor",           white3);
    setFloat("emissiveScale",  0.0f);
    setFloat2("uvTiling",      uvTiling);
    setFloat2("uvOffset",      uvOffset);
    setFloat("alphaCutoff",    0.5f);
    setFloat3("emissiveColor", white3);
}

void ApplyMaterialAssetParams(const MaterialAsset& asset,
                              const renderer::ShaderDescriptor& desc,
                              std::vector<uint8_t>& paramData)
{
    for (const auto& v : desc.vars) {
        if (v.varType != renderer::ShaderVarType::Float) continue;
        if (v.offset + v.size > static_cast<uint32_t>(paramData.size())) continue;

        const auto* values = FindMaterialParam(asset, v.name);
        if (!values || values->empty()) continue;

        const size_t count = (std::min<size_t>)(v.columns, values->size());
        std::memcpy(paramData.data() + v.offset, values->data(), count * sizeof(float));
    }
}

void ApplyMaterialParamOverrides(
    const std::unordered_map<std::string, std::vector<float>>& overrides,
    const renderer::ShaderDescriptor& desc,
    std::vector<uint8_t>& paramData)
{
    for (const auto& [name, values] : overrides) {
        if (values.empty()) continue;
        const auto* v = desc.FindVar(name);
        if (!v || v->varType != renderer::ShaderVarType::Float) continue;
        if (v->offset + v->size > static_cast<uint32_t>(paramData.size())) continue;
        const size_t count = (std::min<size_t>)(v->columns, values.size());
        std::memcpy(paramData.data() + v->offset, values.data(), count * sizeof(float));
    }
}

std::array<std::string, kMaterialTextureSlotNames.size()>
ResolveMaterialTexturePaths(const MaterialAsset& asset)
{
    std::array<std::string, kMaterialTextureSlotNames.size()> paths{};
    for (size_t i = 0; i < kMaterialTextureSlotNames.size(); ++i) {
        const auto it = asset.textures.find(kMaterialTextureSlotNames[i]);
        paths[i] = it != asset.textures.end() ? it->second : std::string{};
    }
    return paths;
}

} // namespace fbzz::asset
