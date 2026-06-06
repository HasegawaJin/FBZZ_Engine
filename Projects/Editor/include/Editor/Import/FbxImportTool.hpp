// FBZZ Engine
// FbxImportTool.hpp | fbzz::editor
// FBX ファイルをエンジンネイティブ形式 (fz*) に分解するインポートツール
// WHY: Assimp によるパースをインポート時の一度だけに限定し、
//      ランタイムは軽量バイナリのみをロードするようにする。
//      フリーアセット規約上、FBX 原本を git に含めない設計にも対応する。
#pragma once
#include <string>

namespace fbzz::editor {

class FbxImportTool {
public:
    // FBX を分解して outputDir に fz* ファイル群を書き出す。
    //
    // 出力構造:
    //   outputDir/
    //     <name>.fzasset          ← マニフェスト
    //     meshes/mesh_N.fzmesh
    //     materials/mat_N.fzmat
    //     textures/               ← FBX 埋め込みテクスチャのコピー先
    //     <name>.fzskel           (スキンメッシュ時のみ)
    //     anims/clip_N.fzanim     (アニメーション時のみ)
    //
    // 失敗時は outputDir ごとロールバック (削除) して false を返す。
    // @param fbxPath   FBX ファイルの絶対パス
    // @param outputDir 出力ディレクトリの絶対パス (存在しなければ作成する)
    // @param sourceHint .fzasset に記録する原本パスのヒント (参考情報)
    static bool Import(const std::string& fbxPath,
                       const std::string& outputDir,
                       const std::string& sourceHint = {});
};

} // namespace fbzz::editor
