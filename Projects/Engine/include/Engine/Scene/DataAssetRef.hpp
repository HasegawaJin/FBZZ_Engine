// FBZZ Engine
// DataAssetRef.hpp | fbzz::scene
// DataAsset (純共有 ScriptableObject) への参照型。
// 設計意図 (WHY):
//   PrefabRef と同じく「パス文字列」をシリアライズ対象に持つ軽量参照。
//   IReflector::Field(DataAssetRef&) の既定実装が path をそのまま保存/復元するため、
//   3 リフレクタ (Inspector / TOML 読み / TOML 書き) のうち Inspector だけが
//   アセットスロット UI を上書きすればよい。
//   IReflector が完全型を必要とするため、ScriptProxy 群より前に定義できるよう独立ヘッダにする
//   (PrefabRef.hpp と同じ理由)。
#pragma once
#include <string>

namespace fbzz::scene {

struct DataAssetRef {
    // シリアライズ対象。参照先 .fzdata の相対パス。
    std::string path;
    // 期待する DataAsset 型名。Asset<T> のコンストラクタが T::TYPE_NAME で埋める。
    // Inspector のピッカー絞り込み / ドロップ型チェックに使う「ヒント」でありシリアライズしない。
    std::string type;

    // 参照同一性は path のみで判定する (type は編集補助メタ情報のため)。
    bool operator==(const DataAssetRef& o) const { return path == o.path; }
};

} // namespace fbzz::scene
