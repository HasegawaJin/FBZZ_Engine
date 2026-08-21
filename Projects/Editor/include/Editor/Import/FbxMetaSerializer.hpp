// FBZZ Engine
// FbxMetaSerializer.hpp | fbzz::editor
// FBX の Unity 風 .meta サイドカーへモデルインポート設定を保存・復元する
#pragma once

#include <Editor/Import/FbxImportTool.hpp>
#include <string>

namespace fbzz::editor {

// FBX 本体の横に置く "<FBX>.meta" の [model] / [cache] セクションを扱う。
// WHY: .fzasset を UI から隠す設計では、原本 FBX の GUID と import 設定を .meta に集約すると
//      リネーム耐性と再インポート再現性を Unity と同じ粒度で維持できるため。
class FbxMetaSerializer final {
public:
    [[nodiscard]] static std::string MetaPathForSource(const std::string& fbxAbsPath);

    [[nodiscard]] static bool LoadOptions(const std::string& fbxAbsPath, FbxImportOptions& outOptions);
    [[nodiscard]] static bool SaveOptions(const std::string& fbxAbsPath, const FbxImportOptions& options);

    // import 成功後に、元 FBX と設定の fingerprint を記録する。
    // WHY: source mtime だけでは import 設定変更を検出できないため、cache 情報を .meta 側に残す。
    [[nodiscard]] static bool SaveCacheInfo(const std::string& fbxAbsPath, const FbxImportOptions& options);

    // .meta の [cache] に記録済みの fingerprint。未記録・読めない場合は空文字が入る。
    struct CacheInfo {
        std::string sourceHash;
        std::string settingsHash;
    };
    [[nodiscard]] static CacheInfo LoadCacheInfo(const std::string& fbxAbsPath);

    // 「今この瞬間の」fingerprint。SaveCacheInfo が書き込む値と同一の計算を公開する。
    // WHY: 再インポート判定を mtime 比較ではなくこの 2 値の突き合わせで行うため。
    //      mtime 比較は .meta を生成物より後に書く現在の手順では import 直後から
    //      「常に古い」と誤判定し、毎回の再インポートと ↻ バッジの出しっぱなしを招いていた。
    [[nodiscard]] static std::string SourceHash(const std::string& fbxAbsPath);
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
