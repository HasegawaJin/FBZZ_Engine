/// @file    PhysicsMaterialAsset.cpp
/// @brief   .physmat の TOML 入出力。
/// @author  Hasegawa Jin
/// @date    2026-08-16
#include <Engine/Asset/PhysicsMaterialAsset.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Util/FileSystem.hpp>

#include <toml++/toml.hpp>

#include <algorithm>
#include <sstream>

namespace fbzz::asset {
namespace {

using physics::PhysicsMaterialCombine;

/// 合成規則は数値ではなく名前で保存する。
/// @note 列挙に値を追加・並べ替えしたときに、既存 .physmat の意味が黙って変わるのを防ぐ。
///       物理挙動は「後で見返して原因を追う」対象なので、ファイルが読んで分かる形であることを優先する。
constexpr const char* kCombineNames[] = {
    "average", "geometric_mean", "minimum", "multiply", "maximum"
};

const char* CombineToString(PhysicsMaterialCombine mode)
{
    const auto index = static_cast<size_t>(mode);
    return index < std::size(kCombineNames) ? kCombineNames[index] : kCombineNames[0];
}

PhysicsMaterialCombine CombineFromString(const std::string& name,
                                         PhysicsMaterialCombine fallback)
{
    for (size_t i = 0; i < std::size(kCombineNames); ++i)
        if (name == kCombineNames[i]) return static_cast<PhysicsMaterialCombine>(i);
    return fallback;
}

} // namespace

bool LoadPhysicsMaterialAssetFromFile(std::string_view path, PhysicsMaterialAsset& outAsset)
{
    const std::string pathString(path);
    std::string text;
    if (!util::FileSystem::ReadText(pathString, text)) {
        FBZZ_LOG_WARN("PhysicsMaterialAsset: cannot open [%s]", pathString.c_str());
        return false;
    }

    toml::parse_result parsed = toml::parse(text, pathString);
    if (!parsed) {
        FBZZ_LOG_WARN("PhysicsMaterialAsset: parse failed [%s]", pathString.c_str());
        return false;
    }

    const toml::table& table = parsed.table();

    /// @note 既定値は PhysicsMaterial::Default。欠損キーはそのまま既定が残る。
    ///       物理マテリアルは今後フィールドが増える可能性が高く、古い .physmat を読めなく
    ///       するより「書いていない項目は既定」の方が壊れにくい。
    PhysicsMaterialAsset asset;
    auto& material = asset.material;
    material.restitution     = static_cast<float>(table["restitution"].value_or(static_cast<double>(material.restitution)));
    material.staticFriction  = static_cast<float>(table["static_friction"].value_or(static_cast<double>(material.staticFriction)));
    material.dynamicFriction = static_cast<float>(table["dynamic_friction"].value_or(static_cast<double>(material.dynamicFriction)));
    material.density         = static_cast<float>(table["density"].value_or(static_cast<double>(material.density)));

    material.restitutionCombine = CombineFromString(
        table["restitution_combine"].value_or(std::string{}), material.restitutionCombine);
    material.frictionCombine = CombineFromString(
        table["friction_combine"].value_or(std::string{}), material.frictionCombine);

    asset.presetName = table["preset"].value_or(std::string{});

    outAsset = std::move(asset);
    return true;
}

bool SavePhysicsMaterialAssetToFile(std::string_view path, const PhysicsMaterialAsset& asset)
{
    const std::string pathString(path);
    const auto& material = asset.material;

    toml::table table;
    table.insert("restitution",      static_cast<double>(material.restitution));
    table.insert("static_friction",  static_cast<double>(material.staticFriction));
    table.insert("dynamic_friction", static_cast<double>(material.dynamicFriction));
    table.insert("density",          static_cast<double>(material.density));
    table.insert("restitution_combine", std::string(CombineToString(material.restitutionCombine)));
    table.insert("friction_combine",    std::string(CombineToString(material.frictionCombine)));
    if (!asset.presetName.empty())
        table.insert("preset", asset.presetName);

    if (!util::FileSystem::EnsureParentDirectory(util::FileSystem::PathFromUtf8(pathString))) {
        FBZZ_LOG_ERROR("PhysicsMaterialAsset: cannot create directory for [%s]", pathString.c_str());
        return false;
    }

    std::ostringstream oss;
    oss << table;
    if (!util::FileSystem::WriteText(pathString, oss.str())) {
        FBZZ_LOG_ERROR("PhysicsMaterialAsset: write failed [%s]", pathString.c_str());
        return false;
    }
    return true;
}

} // namespace fbzz::asset
