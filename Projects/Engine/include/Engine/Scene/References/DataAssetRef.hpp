/// @file    DataAssetRef.hpp
/// @brief   DataAsset (純共有 ScriptableObject) への参照型。
/// @author  Hasegawa Jin
/// @date    2026-07-01
///
/// @note PrefabRef と同じく「パス文字列」をシリアライズ対象に持つ軽量参照。IReflector::Field(DataAssetRef&) の既定実装が path をそのまま保存/復元するため、3 リフレクタ (Inspector / TOML 読み / TOML 書き) のうち Inspector だけがアセットスロット UI を上書きすればよい。
/// @note IReflector が完全型を必要とするため、ScriptProxy 群より前に定義できるよう独立ヘッダにする (PrefabRef.hpp と同じ理由)。
#pragma once
#include <string>

namespace fbzz::scene {

struct DataAssetRef {
    std::string path; ///< シリアライズ対象。参照先 .fzdata の相対パス。
    std::string type; ///< 期待する DataAsset 型名 (Asset<T> のコンストラクタが T::TYPE_NAME で埋める)。Inspector のピッカー絞り込み/ドロップ型チェックに使う「ヒント」でシリアライズしない。

    /// @note 参照同一性は path のみで判定する (type は編集補助メタ情報のため)。
    bool operator==(const DataAssetRef& o) const { return path == o.path; }
};

} // namespace fbzz::scene
