// FBZZ Engine
// FbxImportTool.hpp | fbzz::editor
// FBX ファイルをエンジンネイティブ形式 (fz*) に分解するインポートツール
// WHY: Assimp によるパースをインポート時の一度だけに限定し、
//      ランタイムは軽量バイナリのみをロードするようにする。
//      フリーアセット規約上、FBX 原本を git に含めない設計にも対応する。
#pragma once
#include <string>
#include <vector>

namespace fbzz::editor {

struct FbxImportOptions {
    // WHY: DirectX 法線マップは Y 上向き、OpenGL は Y 下向き。
    //      Blender/Maya のデフォルト書き出しが OpenGL 形式の場合に G チャンネルを反転する。
    bool flipGreenChannel = false;

    // 選択的インポート: 空のとき全メッシュ/全アニメーションを対象にする。
    std::vector<std::string> selectedMeshNames;
    std::vector<std::string> selectedAnimNames;
};

// FBX ファイル内のメッシュ名とアニメーション名を列挙する (インポートは行わない)。
struct FbxScanResult {
    std::vector<std::string> meshNames;
    std::vector<std::string> animNames;
    bool                     valid = false; // Assimp パース成功か
};

class FbxImportTool {
public:
    // FBX を分解して outputDir に fz* ファイル群を書き出す。
    //
    // 出力構造:
    //   outputDir/
    //     <name>.asset          ← マニフェスト
    //     meshes/mesh_N.mesh
    //     materials/mat_N.mat
    //     textures/               ← FBX 埋め込みテクスチャのコピー先
    //     <name>.skel           (スキンメッシュ時のみ)
    //     anims/clip_N.anim     (アニメーション時のみ)
    //
    // 失敗時は outputDir ごとロールバック (削除) して false を返す。
    // @param fbxPath   FBX ファイルの絶対パス
    // @param outputDir 出力ディレクトリの絶対パス (存在しなければ作成する)
    // @param sourceHint .asset に記録する原本パスのヒント (参考情報)
    static bool         Import(const std::string& fbxPath,
                               const std::string& outputDir,
                               const std::string& sourceHint = {},
                               const FbxImportOptions& options = {});

    // FBX を軽量パースしてメッシュ名・アニメーション名だけを返す。
    static FbxScanResult Scan(const std::string& fbxPath);
};

} // namespace fbzz::editor
