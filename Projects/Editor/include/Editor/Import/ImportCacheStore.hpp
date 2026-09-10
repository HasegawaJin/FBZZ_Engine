/// @file    ImportCacheStore.hpp
/// @brief   再インポート判定に使う fingerprint を Library/ImportCache.toml へ集約する。
/// @author  Hasegawa Jin
/// @date    2026-09-09
#pragma once

#include <string>
#include <vector>

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
        std::string contentHash;   ///< 原本の «中身» の fingerprint
        std::string settingsHash;  ///< import 設定の fingerprint

        // 中身を読まずに «変わっていない» と言い切るための目印。
        // WHY 2 段構えか: 中身のハッシュは原本を丸ごと読むため、起動のたびに全 FBX へ
        //     掛けると重い。サイズと更新時刻が前回と一致していれば触られてすらいないので、
        //     そこで打ち切れる。動いていたときだけ中身を読んで «本当に変わったか» を見る。
        uint64_t size  = 0;
        int64_t  mtime = 0;

        /// 旧形式 (パス + サイズ + 更新時刻から作る fingerprint)。移行判定にだけ使う。
        /// WHY 残すか: 旧記録しか無い既存プロジェクトで、中身が同じなのに «未記録» 扱いして
        ///     全件焼き直すのを避ける。新しい記録を書くときは常に空。
        std::string legacyStamp;

        /// 判定に使えない (中身の目印も旧記録も無い / 設定が無い) か。
        [[nodiscard]] bool Empty() const {
            return (contentHash.empty() && legacyStamp.empty()) || settingsHash.empty();
        }
    };

    /// 記録済みの fingerprint。未記録・guid が空なら Empty() な Entry。
    [[nodiscard]] static Entry Load(const std::string& assetGuid);

    /// fingerprint を記録してファイルへ書き出す。
    /// 内容が変わっていなければ書き出しを省く。guid が 32 桁 hex でなければ何もしない。
    static bool Save(const std::string& assetGuid, const Entry& entry);

    /// 記録を別の guid の名前へ移す。guid を振り直したときに呼ぶ。
    ///
    /// WHY 要るか: この索引のキーは guid で、生成物の置き場所 Library/Baked/<guid>/ と
    ///     対になっている。guid だけ振り直すと記録が引けなくなり、生成物が揃っているのに
    ///     «未 import» と判定されて焼き直しが走る。
    static bool Rekey(const std::string& oldGuid, const std::string& newGuid);

    /// 指定した guid の記録を捨てる。生成物を消したときに揃えて呼ぶ。
    /// @return 実際に落とした件数。
    static size_t Forget(const std::vector<std::string>& guids);

    /// サイズ・更新時刻の目印だけを今の値へ更新する (fingerprint は触らない)。
    ///
    /// WHY 要るか: 中身を読んで «変わっていなかった» と分かった直後に呼ぶ。
    ///     ここで新しい更新時刻を覚えておかないと、touch されたファイルを
    ///     以降ずっと読み直すことになる (git pull のたびに全 FBX を読む)。
    static bool RefreshStamp(const std::string& assetGuid, uint64_t size, int64_t mtime);
};

} // namespace fbzz::editor
