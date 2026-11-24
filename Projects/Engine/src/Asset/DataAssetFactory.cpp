/// @file    DataAssetFactory.cpp
/// @brief   DataAsset 型名から生成関数を引くレジストリ実装 (ScriptFactory と同構造)。
/// @author  Hasegawa Jin
/// @date    2026-07-01
///
/// @note DataAssetRegistry は .fzdata の型名から復元に使うファクトリーを選ぶ。
#include <Engine/Asset/DataAssetFactory.hpp>
#include <Engine/Core/Logger.hpp>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <utility>

namespace fbzz::asset {

namespace {

struct Entry {
    std::function<detail::CreatedDataAsset()> factory;
    bool builtin = false; ///< Engine 組み込みの登録は DLL アンロードでも外さない。
};

std::unordered_map<std::string, Entry>& Registry()
{
    static std::unordered_map<std::string, Entry> registry;
    return registry;
}

std::uint64_t& Epoch()
{
    static std::uint64_t epoch = 0;
    return epoch;
}

bool RegisterImpl(const std::string& typeName, Entry entry)
{
    if (typeName.empty() || !entry.factory) return false;

    auto& registry = Registry();
    /// @note 同名のスクリプト型で組み込み登録を上書きすると UnregisterScriptTypes で組み込み型まで失われる。
    if (const auto it = registry.find(typeName);
        it != registry.end() && it->second.builtin && !entry.builtin) {
        FBZZ_LOG_WARN("DataAssetFactory: '%s' is a built-in type; script registration ignored",
                      typeName.c_str());
        return false;
    }

    registry[typeName] = std::move(entry);
    ++Epoch();
    return true;
}

std::function<detail::CreatedDataAsset()> WrapLegacyFactory(DataAssetFactory::Factory factory)
{
    if (!factory) return {};
    return [factory = std::move(factory)] { return detail::CreatedDataAsset{factory(), {}}; };
}

}

bool DataAssetFactory::Register(const std::string& typeName, Factory factory)
{
    return RegisterImpl(typeName, {WrapLegacyFactory(std::move(factory)), false});
}

bool DataAssetFactory::RegisterBuiltin(const std::string& typeName, Factory factory)
{
    return RegisterImpl(typeName, {WrapLegacyFactory(std::move(factory)), true});
}

bool DataAssetFactory::RegisterNative(const std::string& typeName, NativeFactory factory, bool builtin)
{
    return RegisterImpl(typeName, {factory, builtin});
}

detail::CreatedDataAsset detail::CreateDataAsset(const std::string& typeName)
{
    const auto& registry = Registry();
    const auto found = registry.find(typeName);
    if (found == registry.end()) return {};
    return found->second.factory();
}

std::unique_ptr<DataAsset> DataAssetFactory::Create(const std::string& typeName)
{
    return detail::CreateDataAsset(typeName).asset;
}

std::uint64_t DataAssetFactory::RegistrationEpoch()
{
    return Epoch();
}

void DataAssetFactory::UnregisterScriptTypes()
{
    auto& registry = Registry();
    const std::size_t before = registry.size();
    std::erase_if(registry, [](const auto& pair) { return !pair.second.builtin; });
    if (registry.size() != before) ++Epoch();
}

std::vector<std::string> DataAssetFactory::RegisteredTypeNames()
{
    std::vector<std::string> names;
    auto& registry = Registry();
    names.reserve(registry.size());
    for (const auto& [typeName, entry] : registry) {
        if (!typeName.empty() && entry.factory)
            names.push_back(typeName);
    }
    std::sort(names.begin(), names.end());
    return names;
}

}
