/// @file    DataAsset.hpp
/// @brief   純共有データアセット (Unity の ScriptableObject 相当) の基底とユーザー向けマクロ。
/// @author  Hasegawa Jin
/// @date    2026-07-01
///
/// @note  .fzdata に値を切り出し、複数スクリプトが Asset<T> で同じ実体を共有する (純共有、Unity の
/// @note  ScriptableObject 相当)。Script と同じ GetTypeName()/Reflect(IReflector&) を持つため、
/// @note  FBZZ_FIELD 等の登録マクロ・Inspector・TOML リフレクタをそのまま使い回せる。
/// @note  宣言は FBZZ_DATA_ASSET、参照側は FBZZ_ASSET / FBZZ_REQUIRED_ASSET を使う (下記マクロ参照)。
#pragma once
/// @note IReflector / FBZZ_* 登録マクロ / DataAssetRef を持ち込む。
#include <Engine/Reflection/Reflection.hpp>
/// @note Asset<T>::Get() の解決先。
#include <Engine/Asset/Data/DataAssetRegistry.hpp>
#include <string>
#include <typeinfo>

namespace fbzz {

/// @brief ユーザー向け公開名 (fbzz::DataAsset 等) の実体を置く名前空間。
namespace asset {

class DataAsset {
public:
    virtual ~DataAsset() = default;

    /// @brief 型名 (TYPE_NAME)。FBZZ_DATA_ASSET が定義する。.fzdata の `"type"` キーに保存される。
    virtual const char* GetTypeName() const = 0;

    /// @brief フィールドを IReflector へ反映する。FBZZ_REFLECT がフィールド宣言から自動生成する。
    virtual void Reflect(scene::IReflector& r) = 0;
};

/// @return 実行時の型が T と一致し、DataAsset からの公開かつ非仮想の変換経路があれば T のポインター。それ以外は nullptr。
template<typename T>
[[nodiscard]] T* TryAsExactDataAsset(DataAsset* source)
{
    if constexpr (requires { static_cast<T*>(source); }) {
        if (source && typeid(*source) == typeid(T)) return static_cast<T*>(source);
    }
    return nullptr;
}

/// @brief 参照側スクリプトのフィールド型。path で共有実体を解決する型安全ハンドル。
/// @note Get() は未アサイン/型不一致/未存在なら nullptr。operator->/* で `if (asset) asset->field;` のように Ref<T> と同じ書き味で使える。
/// @note ネイティブ派生型を基底型で参照する場合は DataAssetFactory へ基底型も登録する。
template<typename T>
struct Asset {
    scene::DataAssetRef ref;

    /// @brief 期待型名を埋めておくことで、Inspector のピッカー絞り込み・ドロップ型チェックが効く。
    Asset() { ref.type = T::TYPE_NAME; }

    [[nodiscard]] T* Get() const
    {
        if (ref.path.empty()) return nullptr;
        DataAsset* legacy = nullptr;
        if (void* typed = DataAssetRegistry::ResolveNative(ref.path, typeid(T), legacy))
            return static_cast<T*>(typed);
        return TryAsExactDataAsset<T>(legacy);
    }

    T* operator->() const { return Get(); }
    T& operator*()  const { return *Get(); }
    explicit operator bool() const { return Get() != nullptr; }
    [[nodiscard]] bool IsAssigned() const { return !ref.path.empty(); }
    void Clear() { ref.path.clear(); }
};

}

/// @brief 公開エイリアス: `fbzz::DataAsset` / `fbzz::Asset<T>` で使えるようにする。
using asset::DataAsset;
template<typename T> using Asset = asset::Asset<T>;

}

/// @name ユーザー向けマクロ
/// @{

/// @brief クラス先頭に置く。Reflect() 連鎖の土台・型名・仮想 override を生成する。
/// @note DataAsset は Script を継承せず、共通の FBZZ_REFLECT_CORE_ でフィールド反射を宣言する。
#define FBZZ_DATA_ASSET(T)                                                      \
    FBZZ_REFLECT_CORE_(T)                                                       \
    void _fbzz_reflect(::fbzz::scene::detail::ReflectTag<0>,                    \
                       ::fbzz::scene::IReflector&) {}

/// @brief 参照側スクリプトのフィールド宣言。Inspector に .fzdata ドラッグ&ドロップスロットを出す。
/// @note 既定値は不要 (空参照)。Reflect では内包する DataAssetRef を対象にする。
#define FBZZ_ASSET(Type, Name, Display)                                         \
    ::fbzz::asset::Asset<Type> Name{};                                          \
    FBZZ_REFLECT_ENTRY_(Name, r_.Field(FBZZ_DISP_(Display, Name), Name.ref))

/// @brief 必須アセットの設定と型を共通検証へ通知する。保存形式は FBZZ_ASSET と同じ。
#define FBZZ_REQUIRED_ASSET(Type, Name, Display)                                \
    ::fbzz::asset::Asset<Type> Name{};                                          \
    FBZZ_REFLECT_ENTRY_(Name, {                                                 \
        r_.BeginField(#Name, FBZZ_DISP_(Display, Name));                        \
        r_.RequireAsset(FBZZ_DISP_(Display, Name), Name.ref);                   \
        r_.Field(FBZZ_DISP_(Display, Name), Name.ref);                          \
        r_.EndField();                                                        \
    })

/// @}
