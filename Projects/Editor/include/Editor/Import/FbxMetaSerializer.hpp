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
    static constexpr int kModelImporterVersion = 3;

    // .meta に記録された版数を返す。未記録・読めない場合は 0。
    [[nodiscard]] static int LoadImporterVersion(const std::string& fbxAbsPath);
};

} // namespace fbzz::editor
