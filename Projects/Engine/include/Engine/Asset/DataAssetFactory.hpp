// FBZZ Engine
// DataAssetFactory.hpp | fbzz::asset
// DataAssetRegistry が保存された型名から DataAsset 実体を復元するための型レジストリ。
// ScriptFactory と同方針: ユーザー定義型 (スクリプト DLL 内) を型名キーで生成する。
#pragma once
#include <Engine/Asset/DataAsset.hpp>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

namespace fbzz::asset {

/// 登録には「スクリプト DLL 由来」と「Engine 組み込み」の 2 系統がある。
///
/// WHY 区別が要るか: DLL アンロード時にレジストリを全消しすると、DLL 側の型は
///     次のロードで静的初期化が走り直して復活するが、Engine 側の静的初期化は
///     プロセス起動時の 1 回きりで二度と走らない。区別せずに消していたため、
///     スクリプトをホットリロードすると PostProcessProfile が恒久的に失われ、
///     .fzdata が「型が未登録」で読めなくなっていた。
class DataAssetFactory {
public:
    using Factory = std::function<std::unique_ptr<DataAsset>()>;

    /// スクリプト DLL 側の型を登録する。UnregisterScriptTypes で外れる。
    template<typename T>
    static bool Register()
    {
        static_assert(std::is_base_of_v<DataAsset, T>);
        return Register(T::TYPE_NAME, []() { return std::make_unique<T>(); });
    }

    /// Engine 組み込みの型を登録する。DLL ホットリロードをまたいで残る。
    template<typename T>
    static bool RegisterBuiltin()
    {
        static_assert(std::is_base_of_v<DataAsset, T>);
        return RegisterBuiltin(T::TYPE_NAME, []() { return std::make_unique<T>(); });
    }

    static bool Register(const std::string& typeName, Factory factory);
    static bool RegisterBuiltin(const std::string& typeName, Factory factory);
    static std::unique_ptr<DataAsset> Create(const std::string& typeName);
    static std::vector<std::string> RegisteredTypeNames();

    /// 登録内容が変わるたびに進む世代番号。
    /// DataAssetRegistry が「生成に失敗した型をもう一度試す価値があるか」の判定に使う。
    static std::uint64_t RegistrationEpoch();

    /// DLL ホットリロード用: スクリプト DLL 由来の登録だけを外す。
    /// 組み込み型の factory は Engine 側のコードを指すため FreeLibrary の影響を受けず、
    /// 外す必要がない。DLL ロード時の登録コードが DLL 側の型を再登録する。
    static void UnregisterScriptTypes();
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

// Engine 組み込みの DataAsset 型を静的登録するマクロ。
// Engine 内の .cpp からのみ使う。スクリプト側 (Assets/Scripts) は
// FBZZ_REGISTER_DATA_ASSET を使うこと — こちらで登録すると
// DLL アンロード後も解放済みコードを指す factory が残る。
#define FBZZ_REGISTER_BUILTIN_DATA_ASSET(T) \
    namespace { \
        [[maybe_unused]] const bool FBZZ_DATA_ASSET_FACTORY_CONCAT(s_fbzzBuiltinDataAssetRegistered_, __COUNTER__) = []() { \
            ::fbzz::asset::DataAssetFactory::RegisterBuiltin<T>(); \
            return true; \
        }(); \
    }
