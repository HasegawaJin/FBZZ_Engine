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
    // Blender 製 FBX は RootNode 直下ノードに -90°X 回転と均一スケール 100 が焼かれている
    // ことが多い。このうち scale 100 だけがエンジンの前提 (m / scale1 の骨階層) を破るので
    // FbxImportTool が除去し、除去内容をここへ記録する。
    // AnimSubExporter は axisFixNodes と同名のトラックへ同じ補正を適用して整合させる。
    //
    // 回転は除去しない: 頂点・ボーンの生データは Blender の Z-up のままで、Y-up への
    // 変換はこの -90°X が担っている。除去すると Y-up ランタイムで 90° 倒れる。
    std::vector<std::string> axisFixNodes;          // 正規化した RootNode 直下ノード名
    float axisFixScale = 1.0f;                      // 除去した均一スケール s (unitScale へ移動済み)

    // NOTE (2026-08): ルート直下ノードから軸補正 R を剥がして入れ子時の二重掛けを
    //   無くす試みを 2 通り行ったが、いずれも revert した。
    //     - 左掛け   (L→R·L)     : W(bone) を保つ変換のため R が 1 段下へ移るだけ
    //     - 基底変換 (L→R·L·R⁻¹) : 理屈は合うが offset・アニメキー・ルートモーションを
    //                              一斉に整合させる必要があり不整合が出た
    //   入れ子時の二重掛けは scene::AttachToSocket() が実測で吸収する方針とし、
    //   インポート層は「回転は残す」初版の挙動を維持する。

    // スキンメッシュ頂点へ焼き込むバインド回転 R (x,y,z,w)。Blender の -90°X。
    //
    // WHY: エンジンは「ボーン行列 = identity のときが正しい静止姿勢」を前提にしている
    //   (AssetBrowser のサムネイル、AnimatorComponent を持たない SkinnedMeshRenderer、
    //    クリップ未解決時のフォールバック、すべてが identity を入れる)。
    //   Blender 製 FBX は頂点が Z-up 生データのままで Y-up 化を root ノードの回転が
    //   担うため、この前提が崩れて 90° 倒れる。そこでインポート時に頂点へ R を焼き、
    //   offsetMatrix を R⁻¹ で補正する。
    //     アニメ時 : globalBone · (offset·R⁻¹) · (R·v) = globalBone · offset · v (不変)
    //     identity : R·v = Y-up (= バインドポーズ)
    float bindBakeRotation[4] = { 0.f, 0.f, 0.f, 1.f };
    bool  applyStaticNodeTransforms = false;        // 静的 Blender FBX は補正後ノード transform を頂点へ焼く

    // ルートモーションを取り出すノード名の明示指定 (空 = 自動判定)。
    // WHY: リグによっては専用ノードもよくある候補名も持たない。名前を指定できないと
    //      「エクスポーターが知っている固定名のリグしかルートモーションを扱えない」
    //      という制約がそのまま残るため、インポート設定から上書きできるようにする。
    std::string rootMotionNodeName;

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
