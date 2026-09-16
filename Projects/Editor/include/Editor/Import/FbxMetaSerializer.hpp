/// @file    FbxMetaSerializer.hpp
/// @brief   FBX の Unity 風 .meta サイドカーへモデルインポート設定を保存・復元する。
/// @author  Hasegawa Jin
/// @date    2026-07-08
#pragma once

#include <Editor/Import/FbxImportTool.hpp>
#include <cstdint>
#include <string>

namespace fbzz::editor {

// FBX 本体の横に置く "<FBX>.meta" の [model] セクションを扱う。
// WHY: .fzasset を UI から隠す設計では、原本 FBX の GUID と import 設定を .meta に集約すると
//      リネーム耐性と再インポート再現性を Unity と同じ粒度で維持できるため。
//
// 再インポート判定に使う fingerprint はここではなく ImportCacheStore
// (Library/ImportCache.toml) が持つ。マシンごとに違う値なので追跡ファイルへ置けない。
class FbxMetaSerializer final {
public:
    [[nodiscard]] static std::string MetaPathForSource(const std::string& fbxAbsPath);

    [[nodiscard]] static bool LoadOptions(const std::string& fbxAbsPath, FbxImportOptions& outOptions);
    [[nodiscard]] static bool SaveOptions(const std::string& fbxAbsPath, const FbxImportOptions& options);

    // import 成功後に、元 FBX と設定の fingerprint を ImportCacheStore へ記録する。
    // WHY: source mtime だけでは import 設定変更を検出できないため。
    // 併せて、旧形式で .meta に残っている [cache] を落とす。
    [[nodiscard]] static bool SaveCacheInfo(const std::string& fbxAbsPath, const FbxImportOptions& options);

    // 記録済みの fingerprint。未記録・読めない場合は空文字が入る。
    // ImportCacheStore を引き、無ければ旧形式 (.meta の [cache]) へフォールバックする。
    struct CacheInfo {
        std::string contentHash;   ///< 原本の «中身» のハッシュ
        std::string settingsHash;
        uint64_t    size  = 0;     ///< 前回 import 時のファイルサイズ
        int64_t     mtime = 0;     ///< 前回 import 時の更新時刻
        std::string legacyStamp;   ///< 旧形式の記録。移行判定にだけ使う
    };
    [[nodiscard]] static CacheInfo LoadCacheInfo(const std::string& fbxAbsPath);

    // 原本の «中身» のハッシュ。ファイルを丸ごと読むので、呼ぶ前に size / mtime で
    // 絞ること。読めなければ空。
    //
    // WHY 中身を見るか: 以前はパスとサイズと更新時刻から作っていたため、
    //     git pull・clone・ファイル移動のように «中身が 1 バイトも変わっていない» 場合でも
    //     値が変わり、全 FBX の焼き直しが走っていた。
    [[nodiscard]] static std::string SourceContentHash(const std::string& fbxAbsPath);

    // 中身を読まずに取れる目印。取得できなければ false。
    [[nodiscard]] static bool SourceStamp(const std::string& fbxAbsPath,
                                          uint64_t& outSize, int64_t& outMTime);

    // 旧形式の fingerprint (パス + サイズ + 更新時刻)。移行判定にだけ使う。
    [[nodiscard]] static std::string LegacyStampHash(const std::string& fbxAbsPath);

    [[nodiscard]] static std::string SettingsHash(const FbxImportOptions& options);

    // ── インポータ版数 ────────────────────────────────────────────────────
    // インポータのコードを変更して「同じ FBX から違う結果が出るようになった」ときに
    // この値を +1 する。既存アセットの .meta に書かれた版数と食い違ったら
    // IsOutdated() が true を返し、再インポートが走る。
    //
    // WHY: 再インポート判定は従来「生成物の有無」と「FBX の更新時刻」だけで、
    //   インポータ側の変更を検知できなかった。そのため軸補正の実装を直しても
    //   古い Bake が使われ続け、「直したのに何も変わらない」状態になっていた。
    //   FBX を消して入れ直しても source_hash が同じなので同様に効かない。
    //
    // 履歴:
    //   1 : 初版 (軸補正の回転はルート直下ノードに残す方針)。
    //   2 : Unit Scale / Up Axis / Normals / Tangents とクリップ範囲・名前変更を追加。
    //   3 : Assimp / TOML の float メタデータを正しく読み、Unit Scale を実値へ反映。
    //   4 : AnimationClip の Bone targetPath を Skeleton の正規パスとして保存。
    static constexpr int kModelImporterVersion = 4;

    // .meta に記録された版数を返す。未記録・読めない場合は 0。
    [[nodiscard]] static int LoadImporterVersion(const std::string& fbxAbsPath);
};

} // namespace fbzz::editor
