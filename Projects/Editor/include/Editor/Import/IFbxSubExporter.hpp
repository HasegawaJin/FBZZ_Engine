// FBZZ Engine
// IFbxSubExporter.hpp | fbzz::editor
// FBX インポートパイプラインの部品インターフェース
// FbxImportTool::BuildPipeline() が IFbxSubExporter のリストを構築し、
// 各 Export() を順に呼ぶ。サブエクスポーターはそれぞれ独立した責務を持つ。
#pragma once
#include <Engine/Asset/TextureAsset.hpp>
#include <string>
#include <vector>

struct aiScene;

namespace fbzz::editor {

// DirectX 法線マップ: G を反転しない / OpenGL 法線マップ: G を反転する
enum class NormalMapConvention { DirectX, OpenGL };

struct FbxImportContext {
    const aiScene* scene       = nullptr; // アニメーション用 (ボーン階層あり)
    const aiScene* meshScene   = nullptr; // 静的メッシュ用 (PreTransformVertices 済み)
    std::string    fbxPath;
    std::string    fbxDir;
    std::string    baseName;   // FBX ファイルのステム名
    std::string    outputDir;  // stem/ サブフォルダへの絶対パス
    std::string    manifestDir;// FBX と同じフォルダ (stem.model をここに置く)
    float          unitScale             = 0.01f;
    bool           hasSkin              = false;
    // テクスチャ生成オプション (FbxImportOptions から伝播)
    NormalMapConvention       normalMapConvention    = NormalMapConvention::DirectX;
    bool                      generateTexDescriptors = true;
    asset::TextureCompression defaultCompression     = asset::TextureCompression::Auto;
    // 選択的インポート (空 = 全選択)
    std::vector<std::string> selectedMeshNames;
    std::vector<std::string> selectedAnimNames;
    // パイプライン内で共有する出力情報
    std::string outputModelPath; // FzModelSubExporter が書き込む .model パス
};

class IFbxSubExporter {
public:
    virtual ~IFbxSubExporter() = default;

    // このサブエクスポーターの処理を実行する。
    // 失敗時は false を返す。FbxImportTool はパイプラインを中断してロールバックする。
    [[nodiscard]] virtual bool Export(FbxImportContext& ctx) = 0;

    // デバッグ用の名前
    virtual const char* Name() const = 0;
};

} // namespace fbzz::editor
