// FBZZ Engine
// FbxImportTool.hpp | fbzz::editor
// FBX ファイルをエンジンネイティブ形式に変換するインポートツール
//
// 新パイプライン出力構造:
//   outputDir/<name>.fzasset     ← 統合モデルバイナリ (FZMD)
//   outputDir/anims/<name>@<clip>.anim ← アニメーションクリップ v2
//   outputDir/materials/<MaterialName>.mat ← マテリアル TOML
//   outputDir/textures/*.png     ← テクスチャコピー + *.tex 自動生成
//
// BuildPipeline() で IFbxSubExporter のリストを構築する。
// 各 Export() を順に呼び、失敗時は outputDir ごとロールバックする。
#pragma once
#include <Editor/Import/IFbxSubExporter.hpp>
#include <Engine/Asset/TextureAsset.hpp>
#include <memory>
#include <string>
#include <vector>

namespace fbzz::editor {

// NormalMapConvention は IFbxSubExporter.hpp で定義 (循環インクルード回避)

struct FbxImportOptions {
    NormalMapConvention           normalMapConvention    = NormalMapConvention::DirectX;
    bool                          generateTexDescriptors = true;
    asset::TextureCompression     defaultCompression     = asset::TextureCompression::Auto;
    std::vector<std::string>      selectedMeshNames;
    std::vector<std::string>      selectedAnimNames;
};

struct FbxScanResult {
    std::vector<std::string> meshNames;
    std::vector<std::string> animNames;
    bool                     valid = false;
};

class FbxImportTool {
public:
    // FBX を分解して outputDir に fz* ファイル群を書き出す。
    // @param fbxPath   FBX ファイルの絶対パス
    // @param outputDir 出力サブフォルダの絶対パス (stem/ — 存在しなければ作成する)
    static bool Import(const std::string& fbxPath,
                       const std::string& outputDir,
                       const std::string& sourceHint = {},
                       const FbxImportOptions& options = {});

    // FBX を軽量パースしてメッシュ名・アニメーション名だけを返す。
    static FbxScanResult Scan(const std::string& fbxPath);

private:
    // インポートパイプラインを構築して返す。FbxImportContext を受け取る。
    // テスト・拡張のために virtual でなく static にしておく。
    static std::vector<std::unique_ptr<IFbxSubExporter>> BuildPipeline();
};

} // namespace fbzz::editor
