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

// FBX の作成元 DCC。Auto は FBX メタデータから判定し、Maya は FBX SDK 系として扱う。
// WHY: Blender はルートノードに座標系変換を焼き込みやすいため、手動指定で補正を固定できるようにする。
enum class FbxSourceDcc { Auto, Maya, Blender };

struct FbxImportContext {
    const aiScene* scene       = nullptr; // アニメーション用 (ボーン階層あり)
    const aiScene* meshScene   = nullptr; // 静的メッシュ用 (PreTransformVertices 済み)
    std::string    fbxPath;
    std::string    fbxDir;
    std::string    baseName;   // FBX ファイルのステム名
    std::string    outputDir;  // stem/ サブフォルダへの絶対パス
    std::string    manifestDir;// import 生成物のルートフォルダ (stem/stem.fzasset をここに置く)
    float          unitScale             = 0.01f;
    bool           hasSkin              = false;
    // テクスチャ生成オプション (FbxImportOptions から伝播)
    NormalMapConvention       normalMapConvention    = NormalMapConvention::DirectX;
    bool                      generateTexDescriptors = true;
    asset::TextureCompression defaultCompression     = asset::TextureCompression::Auto;
    // DCC 由来のルート焼き込み補正 (Blender の "Apply Transform" 相当をインポート時に実行)。
    // Blender 製 FBX は RootNode 直下ノードに +90°X 回転と均一スケール 100 が焼かれている
    // ことが多く、エンジンの前提 (Y-up / m / scale1 の骨階層) を破る。FbxImportTool が
    // シーンのノードからこれを除去し、除去内容をここへ記録する。
    // AnimSubExporter は axisFixNodes と同名のトラックへ同じ補正を適用して整合させる。
    std::vector<std::string> axisFixNodes;          // 正規化した RootNode 直下ノード名
    float axisFixRotation[4] = { 0.f, 0.f, 0.f, 1.f }; // 除去した回転 q (x,y,z,w)
    float axisFixScale = 1.0f;                      // 除去した均一スケール s (unitScale へ移動済み)
    bool  applyStaticNodeTransforms = false;        // 静的 Blender FBX は補正後ノード transform を頂点へ焼く

    // 選択的インポート (空 = 全選択)
    std::vector<std::string> selectedMeshNames;
    std::vector<std::string> selectedAnimNames;
    // パイプライン内で共有する出力情報
    std::string outputModelPath; // ModelSubExporter が書き込む .fzasset パス
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
