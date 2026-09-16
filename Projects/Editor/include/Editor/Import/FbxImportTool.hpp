/// @file    FbxImportTool.hpp
/// @brief   FBX ファイルをエンジンネイティブ形式に変換するインポートツール。
/// @author  Hasegawa Jin
/// @date    2026-06-06
///
/// 新パイプライン出力構造:
/// Library/Baked/<fbx-guid>/<name>.fzasset ← 統合モデルバイナリ (FZMD)
/// outputDir/anims/<name>@<clip>.anim ← アニメーションクリップ v3
/// outputDir/materials/<MaterialName>.mat ← マテリアル TOML
/// outputDir/textures/*.png     ← テクスチャコピー + *.png.meta 自動生成
/// <source>.fbx.meta            ← GUID + モデル import 設定
///
/// BuildPipeline() で IFbxSubExporter のリストを構築する。
/// 各 Export() を順に呼び、失敗時は outputDir ごとロールバックする。
#pragma once
#include <Editor/Import/IFbxSubExporter.hpp>
#include <Engine/Asset/TextureAsset.hpp>
#include <memory>
#include <string>
#include <vector>

namespace fbzz::editor {

// NormalMapConvention は IFbxSubExporter.hpp で定義 (循環インクルード回避)

// AnimationClipImportSettings は IFbxSubExporter.hpp で定義 (循環インクルード回避)

struct FbxImportOptions {
    FbxSourceDcc                 sourceDcc              = FbxSourceDcc::Auto;
    FbxUpAxis                    upAxis                 = FbxUpAxis::Auto;
    NormalMapConvention           normalMapConvention    = NormalMapConvention::DirectX;
    float                         unitScaleMultiplier    = 1.0f;
    bool                          generateNormals        = true;
    bool                          generateTangents       = true;
    bool                          generateTexDescriptors = true;
    asset::TextureCompression     defaultCompression     = asset::TextureCompression::Auto;
    std::vector<std::string>      selectedMeshNames;
    std::vector<std::string>      selectedAnimNames;
    // ルートモーションを取り出すノード名 (空 = 候補名から自動判定)。
    std::string                   rootMotionNodeName;
    // クリップ単位の設定。既定と同じ (loop=false) のものは保存しないため、
    // ここに無いクリップは既定値として扱う。
    std::vector<AnimationClipImportSettings> clipSettings;

    // 指定クリップの設定を引く。未登録なら既定値を返す。
    [[nodiscard]] AnimationClipImportSettings ClipSettingsFor(const std::string& clipName) const
    {
        for (const AnimationClipImportSettings& settings : clipSettings)
            if (settings.name == clipName) return settings;
        AnimationClipImportSettings fallback;
        fallback.name = clipName;
        return fallback;
    }
};

struct FbxScanResult {
    std::vector<std::string> meshNames;
    std::vector<std::string> animNames;
    FbxSourceDcc             detectedSourceDcc = FbxSourceDcc::Auto;
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
