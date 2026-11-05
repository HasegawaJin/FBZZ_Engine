/// @file    MeshInstancesSerialization.hpp
/// @brief   専用の素材定義・配置形式で共有する非公開 TOML 変換。
/// @author  Hasegawa Jin
/// @date    2026-11-05
#pragma once
#include <Engine/Asset/InstanceSetAsset.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <toml++/toml.hpp>
#include <filesystem>
#include <sstream>

namespace fbzz::asset::mesh_instances_serialization {

inline bool ReadFile(std::string_view path, std::string& text, uint64_t limit = 512ull * 1024 * 1024)
{
    std::error_code error;
    const auto size = std::filesystem::file_size(util::FileSystem::PathFromUtf8(std::string(path)), error);
    return !error && size <= limit && util::FileSystem::ReadText(std::string(path), text);
}

inline bool WriteFile(std::string_view path, const toml::table& table)
{
    std::ostringstream text;
    text << table;
    const std::string filename(path);
    return util::FileSystem::EnsureParentDirectory(util::FileSystem::PathFromUtf8(filename))
        && util::FileSystem::WriteTextAtomic(filename, text.str());
}

inline bool ReadDefinition(const toml::table& table, std::vector<InstanceMeshPart>& parts, std::vector<float>& heights)
{
    const auto* mesh = table["mesh"].as_table();
    const auto* partArray = mesh ? (*mesh)["parts"].as_array() : nullptr;
    const auto* lods = mesh ? (*mesh)["lodHeights"].as_array() : nullptr;
    if (!partArray || partArray->empty() || partArray->size() > 64 || !lods || lods->empty() || lods->size() > 4) return false;
    for (const auto& entry : *partArray) {
        const auto* row = entry.as_table();
        if (!row) return false;
        const auto meshRef = (*row)["mesh"].value<std::string>();
        const auto material = (*row)["material"].value<std::string>();
        const auto lod = (*row)["lod"].value<int64_t>();
        if (!meshRef || !material || !lod || *lod < 0 || *lod > 3) return false;
        parts.push_back({*meshRef, *material, static_cast<uint32_t>(*lod)});
    }
    for (const auto& entry : *lods) {
        const auto value = entry.value<float>();
        if (!value) return false;
        heights.push_back(*value);
    }
    return true;
}

inline void WriteDefinition(toml::table& table, const InstanceSetAsset& definition)
{
    toml::array parts, lods;
    for (const auto& part : definition.Parts())
        parts.push_back(toml::table{{"mesh", part.meshPath}, {"material", part.materialPath}, {"lod", part.lod}});
    for (float height : definition.LodHeights()) lods.push_back(height);
    table.insert("mesh", toml::table{{"parts", std::move(parts)}, {"lodHeights", std::move(lods)}});
}

inline bool ReadVector(const toml::node& node, math::Vector3& vector)
{
    const auto* array = node.as_array();
    if (!array || array->size() != 3) return false;
    const auto x = (*array)[0].value<float>(), y = (*array)[1].value<float>(), z = (*array)[2].value<float>();
    if (!x || !y || !z) return false;
    vector = {*x, *y, *z};
    return true;
}

inline toml::array WriteVector(const math::Vector3& vector) { return toml::array{vector.x, vector.y, vector.z}; }
} /// @note namespace fbzz::asset::mesh_instances_serialization
