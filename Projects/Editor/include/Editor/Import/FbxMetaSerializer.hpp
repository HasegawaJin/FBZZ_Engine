/// @file    FbxMetaSerializer.hpp
/// @brief   FBX の Unity 風 .meta サイドカーへモデルインポート設定を保存・復元する。
/// @author  Hasegawa Jin
/// @date    2026-07-08
#pragma once

#include <Editor/Import/FbxImportTool.hpp>
#include <cstdint>
#include <string>

namespace fbzz::editor {

/// FBX 本体の横に置く `"<FBX>.meta"` の [model] セクションを扱う。
/// @note .fzasset を UI から隠す設計では、原本 FBX の GUID と import 設定を .meta に集約するとリネーム耐性と再インポート再現性を Unity と同じ粒度で維持できる。
///
/// 再インポート判定に使う fingerprint はここではなく ImportCacheStore
/// (Library/ImportCache.toml) が持つ。マシンごとに違う値なので追跡ファイルへ置けない。
class FbxMetaSerializer final {
public:
    [[nodiscard]] static std::string MetaPathForSource(const std::string& fbxAbsPath);

    [[nodiscard]] static bool LoadOptions(const std::string& fbxAbsPath, FbxImportOptions& outOptions);
    [[nodiscard]] static bool SaveOptions(const std::string& fbxAbsPath, const FbxImportOptions& options);

    /// import 成功後に、元 FBX と設定の fingerprint を ImportCacheStore へ記録する。
    /// @note source mtime だけでは import 設定変更を検出できないため、設定の fingerprint も併せて記録する。旧形式で .meta に残っている [cache] はここで落とす。
    [[nodiscard]] static bool SaveCacheInfo(const std::string& fbxAbsPath, const FbxImportOptions& options);

    /// 記録済みの fingerprint。未記録・読めない場合は空文字が入る。
    /// ImportCacheStore を引き、無ければ旧形式 (.meta の [cache]) へフォールバックする。
    struct CacheInfo {
        std::string contentHash;   ///< 原本の «中身» のハッシュ
        std::string settingsHash;
        uint64_t    size  = 0;     ///< 前回 import 時のファイルサイズ
        int64_t     mtime = 0;     ///< 前回 import 時の更新時刻
        std::string legacyStamp;   ///< 旧形式の記録。移行判定にだけ使う
    };
    [[nodiscard]] static CacheInfo LoadCacheInfo(const std::string& fbxAbsPath);

    /// 原本の «中身» のハッシュ。ファイルを丸ごと読むので、呼ぶ前に size / mtime で絞ること。読めなければ空。
    /// @note 以前はパス・サイズ・更新時刻から作っていたため、git pull・clone・ファイル移動のように中身が変わっていなくても値が変わり、全 FBX の焼き直しが走っていた。
    [[nodiscard]] static std::string SourceContentHash(const std::string& fbxAbsPath);

    /// 中身を読まずに取れる目印。取得できなければ false。
    [[nodiscard]] static bool SourceStamp(const std::string& fbxAbsPath,
                                          uint64_t& outSize, int64_t& outMTime);

    /// 旧形式の fingerprint (パス + サイズ + 更新時刻)。移行判定にだけ使う。
    [[nodiscard]] static std::string LegacyStampHash(const std::string& fbxAbsPath);

    [[nodiscard]] static std::string SettingsHash(const FbxImportOptions& options);

    /// @name インポータ版数
    /// @{
    /// インポータのコードを変更して「同じ FBX から違う結果が出るようになった」ときにこの値を +1 する。既存アセットの .meta に書かれた版数と食い違ったら IsOutdated() が true を返し、再インポートが走る。
    /// @note 再インポート判定は従来「生成物の有無」と「FBX の更新時刻」だけで、インポータ側の変更を検知できず、直しても古い Bake が使われ続ける事故があった。FBX を入れ直しても source_hash が同じなので効かない。
    static constexpr int kModelImporterVersion = 4;

    /// .meta に記録された版数を返す。未記録・読めない場合は 0。
    [[nodiscard]] static int LoadImporterVersion(const std::string& fbxAbsPath);
    /// @}
};

} // namespace fbzz::editor
