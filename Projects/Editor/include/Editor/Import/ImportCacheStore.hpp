/// @file    ImportCacheStore.hpp
/// @brief   再インポート判定に使う fingerprint を Library/ImportCache.toml へ集約する。
/// @author  Hasegawa Jin
/// @date    2026-09-09
#pragma once

#include <string>

namespace fbzz::editor {

// アセット GUID → 前回 import 時の fingerprint。実体はプロジェクトごとの
// "Library/ImportCache.toml" で、開いているプロジェクトを跨いで持ち越さない。
//
// WHY .meta から出したか: source fingerprint は「原本の絶対パス + サイズ + 更新時刻」から
//     作るため、人・マシン・clone ごとに必ず違う値になる。これを git 追跡下の .meta へ
//     書いていたので、同じプロジェクトを触る全員の手元で FBX の .meta が常に差分を持ち、
//     pull のたびに衝突していた。値の寿命はローカルの Library/Baked と同じなので、
//     置き場所も Library へ揃える (Library は .gitignore 済み)。
class ImportCacheStore final {
public:
    struct Entry {
        std::string sourceHash;    ///< 原本の fingerprint
        std::string settingsHash;  ///< import 設定の fingerprint

        /// 判定に使えない (片方でも欠けている) か。
        [[nodiscard]] bool Empty() const {
            return sourceHash.empty() || settingsHash.empty();
        }
    };

    /// 記録済みの fingerprint。未記録・guid が空なら Empty() な Entry。
    [[nodiscard]] static Entry Load(const std::string& assetGuid);

    /// fingerprint を記録してファイルへ書き出す。
    /// 内容が変わっていなければ書き出しを省く。guid が 32 桁 hex でなければ何もしない。
    static bool Save(const std::string& assetGuid, const Entry& entry);
};

} // namespace fbzz::editor
