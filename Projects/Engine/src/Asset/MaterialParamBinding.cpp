/// @file    MaterialParamBinding.cpp
/// @brief   MaterialParamBinding.hpp の実装
/// @author  Hasegawa Jin
/// @date    2026-08-22
#include <Engine/Asset/MaterialParamBinding.hpp>

#include <algorithm>
#include <cstring>
#include <cmath>
#include <limits>
#include <Engine/Core/Logger.hpp>

namespace fbzz::asset {

namespace {

template<typename T>
bool ValidateValues(const renderer::ShaderVarDesc& variable, std::span<const T> values)
{
    if (!variable.IsWritable() || values.size() != variable.ValueCount()) return false;
    for (const T source : values) {
        const double value = static_cast<double>(source);
        if (!std::isfinite(value)) return false;
        switch (variable.varType) {
        case renderer::ShaderVarType::Float:
            if (std::abs(value) > std::numeric_limits<float>::max()) return false;
            break;
        case renderer::ShaderVarType::Int:
            if (std::trunc(value) != value || value < INT32_MIN || value > INT32_MAX) return false;
            break;
        case renderer::ShaderVarType::UInt:
            if (std::trunc(value) != value || value < 0 || value > UINT32_MAX) return false;
            break;
        case renderer::ShaderVarType::Bool:
            if (value != 0 && value != 1) return false;
            break;
        default: return false;
        }
    }
    return true;
}

template<typename T>
bool WriteValues(const renderer::ShaderVarDesc& variable, std::span<const T> values,
                 std::span<uint8_t> destination)
{
    if (!ValidateValues(variable, values)) return false;
    if (variable.elements && variable.arrayStride == 0) return false;
    for (uint32_t i = 0; i < values.size(); ++i) {
        const uint64_t offset = variable.ValueOffset(i);
        if (offset < variable.offset || offset + 4 > destination.size()
            || offset + 4 > static_cast<uint64_t>(variable.offset) + variable.size) return false;
    }
    for (uint32_t i = 0; i < values.size(); ++i) {
        auto* target = destination.data() + variable.ValueOffset(i);
        if (variable.varType == renderer::ShaderVarType::Float) {
            const float value = static_cast<float>(values[i]);
            std::memcpy(target, &value, sizeof(value));
        } else if (variable.varType == renderer::ShaderVarType::Int) {
            const int32_t value = static_cast<int32_t>(values[i]);
            std::memcpy(target, &value, sizeof(value));
        } else {
            const uint32_t value = static_cast<uint32_t>(values[i]);
            std::memcpy(target, &value, sizeof(value));
        }
    }
    return true;
}

template<typename T>
void ApplyValues(const renderer::ShaderVarDesc& variable, const std::vector<T>& values,
                 std::vector<uint8_t>& data)
{
    /// @note 旧 .mat の短い float ベクトルは未指定成分を保持する。配列・行列は要素数を厳密に扱う。
    bool written = false;
    if (variable.IsWritable() && variable.varType == renderer::ShaderVarType::Float
        && !variable.elements && variable.varClass != renderer::ShaderVarClass::Matrix
        && !values.empty() && values.size() < variable.ValueCount()) {
        std::array<double, 4> converted{};
        for (size_t i = 0; i < values.size(); ++i) converted[i] = static_cast<double>(values[i]);
        for (uint32_t i = static_cast<uint32_t>(values.size()); i < variable.ValueCount(); ++i) {
            const uint64_t offset = variable.ValueOffset(i);
            if (offset + 4 > data.size()) return;
            float value = 0;
            std::memcpy(&value, data.data() + offset, sizeof(value));
            converted[i] = value;
        }
        written = WriteValues(variable, std::span<const double>(converted.data(), variable.ValueCount()), data);
    } else written = WriteValues(variable, std::span<const T>(values), data);
    if (!written)
        FBZZ_LOG_WARN("Material parameter rejected (type/count/range/layout): %s", variable.name.c_str());
}
}

bool ValidateMaterialValues(const renderer::ShaderVarDesc& variable, std::span<const double> values)
{
    return ValidateValues(variable, values);
}

bool WriteMaterialValues(const renderer::ShaderVarDesc& variable, std::span<const double> values,
                         std::span<uint8_t> destination)
{
    return WriteValues(variable, values, destination);
}

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
    /// @note 0 のままだと乗算スケール系変数が非表示になるため。1.0f は乗算の単位元であり、
    /// @note 加算オフセット (uvOffset 等) は次ステップで 0 に上書きされる。
    for (const auto& v : desc.vars) {
        if (!v.IsWritable()) continue;
        for (uint32_t i = 0; i < v.ValueCount(); ++i) {
            const uint64_t offset = v.ValueOffset(i);
            if (offset + 4 > paramData.size() || offset + 4 > static_cast<uint64_t>(v.offset) + v.size) break;
            float value = v.varType == renderer::ShaderVarType::Float ? 1.0f : 0.0f;
            if (v.varClass == renderer::ShaderVarClass::Matrix) {
                const uint32_t component = i % (v.rows * v.columns);
                value = component / v.columns == component % v.columns ? 1.0f : 0.0f;
            }
            if (v.varType == renderer::ShaderVarType::Float)
                std::memcpy(paramData.data() + offset, &value, sizeof(value));
            else {
                const uint32_t integer = static_cast<uint32_t>(value);
                std::memcpy(paramData.data() + offset, &integer, sizeof(integer));
            }
        }
    }

    /// @note Step 2: 標準 PBR パラメータを正しいデフォルト値で上書きする。
    auto setFloat = [&](std::string_view name, float value) {
        const auto* v = desc.FindVar(name);
        if (!v || !v->IsWritable() || v->elements || v->varClass != renderer::ShaderVarClass::Scalar
            || v->varType != renderer::ShaderVarType::Float || v->columns != 1) return;
        if (v->offset + sizeof(float) > static_cast<uint32_t>(paramData.size())) return;
        std::memcpy(paramData.data() + v->offset, &value, sizeof(float));
    };
    auto setFloat2 = [&](std::string_view name, const float value[2]) {
        const auto* v = desc.FindVar(name);
        if (!v || !v->IsWritable() || v->elements || v->varClass != renderer::ShaderVarClass::Vector
            || v->varType != renderer::ShaderVarType::Float || v->columns != 2) return;
        if (v->offset + 2u * sizeof(float) > static_cast<uint32_t>(paramData.size())) return;
        std::memcpy(paramData.data() + v->offset, value, 2u * sizeof(float));
    };
    auto setFloat3 = [&](std::string_view name, const float value[3]) {
        const auto* v = desc.FindVar(name);
        if (!v || !v->IsWritable() || v->elements || v->varClass != renderer::ShaderVarClass::Vector
            || v->varType != renderer::ShaderVarType::Float || v->columns < 3) return;
        if (v->offset + 3u * sizeof(float) > static_cast<uint32_t>(paramData.size())) return;
        std::memcpy(paramData.data() + v->offset, value, 3u * sizeof(float));
    };

    const float uvTiling[2] = { 1.0f, 1.0f };
    const float uvOffset[2] = { 0.0f, 0.0f };

    const float white3[3] = { 1.0f, 1.0f, 1.0f };
    setFloat("metallic",       0.0f);
    setFloat("roughness",      0.65f);
    /// @note 拡張 PBR ローブは既定で無効にする。1.0f のままだと既存マテリアルの見た目と
    /// @note エネルギー配分が変わるため、明示的に有効化された場合だけ Forward へ送る。
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
        if (const auto it = asset.integerParams.find(v.name); it != asset.integerParams.end()) {
            ApplyValues(v, it->second, paramData);
            continue;
        }
        const auto* values = FindMaterialParam(asset, v.name);
        if (!values || values->empty()) continue;
        ApplyValues(v, *values, paramData);
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
        if (v) ApplyValues(*v, values, paramData);
    }
}

void ApplyMaterialIntegerOverrides(
    const std::unordered_map<std::string, std::vector<int64_t>>& overrides,
    const renderer::ShaderDescriptor& desc, std::vector<uint8_t>& paramData)
{
    for (const auto& [name, values] : overrides)
        if (const auto* variable = desc.FindVar(name)) ApplyValues(*variable, values, paramData);
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
