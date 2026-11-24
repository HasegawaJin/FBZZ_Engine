/// @file    DataAssetFactory.hpp
/// @brief   DataAssetRegistry が保存された型名から DataAsset 実体を復元するための型レジストリ。
/// @author  Hasegawa Jin
/// @date    2026-07-01

/// @note ScriptFactory と同方針: ユーザー定義型 (スクリプト DLL 内) を型名キーで生成する。
#pragma once
#include <Engine/Api.hpp>
#include <Engine/Asset/DataAsset.hpp>
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace fbzz::asset {

namespace detail {

/// @note 生成時に得た実体のポインターと変換関数。保持期間は CreatedDataAsset の所有権に従う。
struct NativeDataAssetType {
    void* instance = nullptr;
    void* (*query)(void*, const std::type_info&) = nullptr;
};

struct CreatedDataAsset {
    std::unique_ptr<DataAsset> asset;
    NativeDataAssetType nativeType;
};

[[nodiscard]] CreatedDataAsset CreateDataAsset(const std::string& typeName);

}

/// @brief スクリプト DLL 由来と Engine 組み込みの型を分けて管理するレジストリ。
/// @note DLL アンロード時に全消しすると、DLL 側は次ロードの静的初期化で復活するが、
/// @note Engine 側の静的初期化はプロセス起動時の 1 回きりで復活しない。区別せず消すと
/// @note PostProcessProfile 等が恒久的に失われ `.fzdata` が読めなくなる。
class DataAssetFactory {
public:
    using Factory = std::function<std::unique_ptr<DataAsset>()>;

    /// @note スクリプト DLL 側の型を登録する。UnregisterScriptTypes で外れる。
    /// @note 基底として参照する型を Bases に列挙する。T から各基底へ公開かつ曖昧でない変換が必要。
    template<typename T, typename... Bases>
    static bool Register()
    {
        return RegisterNative(T::TYPE_NAME, &CreateNative<T, Bases...>, false);
    }

    /// @note Engine 組み込みの型を登録する。DLL ホットリロードをまたいで残る。
    /// @note 基底型の登録条件は Register<T, Bases...>() と同じ。
    template<typename T, typename... Bases>
    static bool RegisterBuiltin()
    {
        return RegisterNative(T::TYPE_NAME, &CreateNative<T, Bases...>, true);
    }

    /// @note 所有権を消去済みのファクトリーでは型の来歴を保持できず、Asset<T> は厳密な実行時型だけを照合する。
    FBZZ_ENGINE_API static bool Register(const std::string& typeName, Factory factory);
    FBZZ_ENGINE_API static bool RegisterBuiltin(const std::string& typeName, Factory factory);
    FBZZ_ENGINE_API static std::unique_ptr<DataAsset> Create(const std::string& typeName);
    FBZZ_ENGINE_API static std::vector<std::string> RegisteredTypeNames();

    /// @note 登録内容が変わるたびに進む世代番号。
    /// @note DataAssetRegistry が「生成に失敗した型をもう一度試す価値があるか」の判定に使う。
    FBZZ_ENGINE_API static std::uint64_t RegistrationEpoch();

    /// @note DLL ホットリロード用: スクリプト DLL 由来の登録だけを外す。
    /// @note 組み込み型の factory は Engine 側のコードを指すため FreeLibrary の影響を受けず、
    /// @note 外す必要がない。DLL ロード時の登録コードが DLL 側の型を再登録する。
    FBZZ_ENGINE_API static void UnregisterScriptTypes();

private:
    using NativeFactory = detail::CreatedDataAsset (*)();

    FBZZ_ENGINE_API static bool RegisterNative(const std::string& typeName, NativeFactory factory, bool builtin);

    template<typename T, typename... Bases>
    static void* QueryNativeType(void* original, const std::type_info& requestedType)
    {
        auto* instance = static_cast<T*>(original);
        if (requestedType == typeid(T)) return instance;
        const std::array<std::pair<const std::type_info*, void*>, sizeof...(Bases)> bases{{
            {&typeid(Bases), static_cast<Bases*>(instance)}...
        }};
        for (const auto& [type, address] : bases) {
            if (requestedType == *type) return address;
        }
        return nullptr;
    }

    template<typename T, typename... Bases>
    static detail::CreatedDataAsset CreateNative()
    {
        static_assert(std::is_convertible_v<T*, DataAsset*>);
        static_assert((std::is_convertible_v<T*, Bases*> && ...));
        auto asset = std::make_unique<T>();
        auto* instance = asset.get();
        /// @note 所有権を消去する前の T* から変換するため、仮想基底や多重継承でも部分オブジェクトを調整できる。
        return {std::move(asset), {instance, &QueryNativeType<T, Bases...>}};
    }
};

}

/// @brief DataAsset 型を DataAssetFactory に静的登録するマクロ。
#define FBZZ_DATA_ASSET_FACTORY_CONCAT_INNER(a, b) a##b
#define FBZZ_DATA_ASSET_FACTORY_CONCAT(a, b) FBZZ_DATA_ASSET_FACTORY_CONCAT_INNER(a, b)
#define FBZZ_REGISTER_DATA_ASSET(...) \
    namespace { \
        [[maybe_unused]] const bool FBZZ_DATA_ASSET_FACTORY_CONCAT(s_fbzzDataAssetRegistered_, __COUNTER__) = []() { \
            ::fbzz::asset::DataAssetFactory::Register<__VA_ARGS__>(); \
            return true; \
        }(); \
    }

/// @brief Engine 組み込みの DataAsset 型を静的登録するマクロ。Engine 内の `.cpp` 専用。
/// @note スクリプト側は FBZZ_REGISTER_DATA_ASSET を使うこと。ここで登録すると
/// @note DLL アンロード後も解放済みコードを指す factory が残る。
#define FBZZ_REGISTER_BUILTIN_DATA_ASSET(...) \
    namespace { \
        [[maybe_unused]] const bool FBZZ_DATA_ASSET_FACTORY_CONCAT(s_fbzzBuiltinDataAssetRegistered_, __COUNTER__) = []() { \
            ::fbzz::asset::DataAssetFactory::RegisterBuiltin<__VA_ARGS__>(); \
            return true; \
        }(); \
    }
