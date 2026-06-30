// FBZZ Engine
// DataAssetFactory.hpp | fbzz::asset
// DataAssetRegistry が保存された型名から DataAsset 実体を復元するための型レジストリ。
// ScriptFactory と同方針: ユーザー定義型 (スクリプト DLL 内) を型名キーで生成する。
#pragma once
#include <Engine/Asset/DataAsset.hpp>
#include <functional>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

namespace fbzz::asset {

class DataAssetFactory {
public:
    using Factory = std::function<std::unique_ptr<DataAsset>()>;

    template<typename T>
    static bool Register()
    {
        static_assert(std::is_base_of_v<DataAsset, T>);
        return Register(T::TYPE_NAME, []() { return std::make_unique<T>(); });
    }

    static bool Register(const std::string& typeName, Factory factory);
    static std::unique_ptr<DataAsset> Create(const std::string& typeName);
    static std::vector<std::string> RegisteredTypeNames();

    // DLL ホットリロード用: レジストリを全クリアする (古い型の factory が DLL ポインタを残さないように)。
    // DLL ロード時の登録コードが再登録する。
    static void UnregisterAll();
};

} // namespace fbzz::asset

// DataAsset 型を DataAssetFactory に静的登録するマクロ (FBZZ_REGISTER_SCRIPT と同方針)。
#define FBZZ_DATA_ASSET_FACTORY_CONCAT_INNER(a, b) a##b
#define FBZZ_DATA_ASSET_FACTORY_CONCAT(a, b) FBZZ_DATA_ASSET_FACTORY_CONCAT_INNER(a, b)
#define FBZZ_REGISTER_DATA_ASSET(T) \
    namespace { \
        [[maybe_unused]] const bool FBZZ_DATA_ASSET_FACTORY_CONCAT(s_fbzzDataAssetRegistered_, __COUNTER__) = []() { \
            ::fbzz::asset::DataAssetFactory::Register<T>(); \
            return true; \
        }(); \
    }
