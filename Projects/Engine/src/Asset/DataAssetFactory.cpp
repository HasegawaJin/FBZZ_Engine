// FBZZ Engine
// DataAssetFactory.cpp | fbzz::asset
// DataAsset 型名から生成関数を引くレジストリ実装 (ScriptFactory と同構造)。
// DataAssetRegistry が .fzdata の "type" キーからユーザー定義 DataAsset を復元するために使う。
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
    DataAssetFactory::Factory factory;
    bool builtin = false;   // Engine 側の登録。DLL アンロードでも外さない
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

bool RegisterImpl(const std::string& typeName, DataAssetFactory::Factory factory, bool builtin)
{
    if (typeName.empty() || !factory) return false;

    auto& registry = Registry();
    // 組み込み型を同名のスクリプト型で上書きさせない。上書きを許すと builtin 印が落ち、
    // 次の UnregisterScriptTypes で組み込み型ごと消えて復活しなくなる。
    if (const auto it = registry.find(typeName);
        it != registry.end() && it->second.builtin && !builtin) {
        FBZZ_LOG_WARN("DataAssetFactory: '%s' is a built-in type; script registration ignored",
                      typeName.c_str());
        return false;
    }

    registry[typeName] = Entry{ std::move(factory), builtin };
    ++Epoch();
    return true;
}

} // namespace

bool DataAssetFactory::Register(const std::string& typeName, Factory factory)
{
    return RegisterImpl(typeName, std::move(factory), false);
}

bool DataAssetFactory::RegisterBuiltin(const std::string& typeName, Factory factory)
{
    return RegisterImpl(typeName, std::move(factory), true);
}

std::unique_ptr<DataAsset> DataAssetFactory::Create(const std::string& typeName)
{
    auto& registry = Registry();
    auto it = registry.find(typeName);
    if (it == registry.end()) return nullptr;
    return it->second.factory();
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

} // namespace fbzz::asset
