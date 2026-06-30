// FBZZ Engine
// DataAssetFactory.cpp | fbzz::asset
// DataAsset 型名から生成関数を引くレジストリ実装 (ScriptFactory と同構造)。
// DataAssetRegistry が .fzdata の "type" キーからユーザー定義 DataAsset を復元するために使う。
#include <Engine/Asset/DataAssetFactory.hpp>
#include <algorithm>
#include <unordered_map>
#include <utility>

namespace fbzz::asset {

namespace {

std::unordered_map<std::string, DataAssetFactory::Factory>& Registry()
{
    static std::unordered_map<std::string, DataAssetFactory::Factory> registry;
    return registry;
}

} // namespace

bool DataAssetFactory::Register(const std::string& typeName, Factory factory)
{
    if (typeName.empty() || !factory) return false;
    Registry()[typeName] = std::move(factory);
    return true;
}

std::unique_ptr<DataAsset> DataAssetFactory::Create(const std::string& typeName)
{
    auto& registry = Registry();
    auto it = registry.find(typeName);
    if (it == registry.end()) return nullptr;
    return it->second();
}

void DataAssetFactory::UnregisterAll()
{
    Registry().clear();
}

std::vector<std::string> DataAssetFactory::RegisteredTypeNames()
{
    std::vector<std::string> names;
    auto& registry = Registry();
    names.reserve(registry.size());
    for (const auto& [typeName, factory] : registry) {
        if (!typeName.empty() && factory)
            names.push_back(typeName);
    }
    std::sort(names.begin(), names.end());
    return names;
}

} // namespace fbzz::asset
