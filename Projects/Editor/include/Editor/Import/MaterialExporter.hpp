/// @file    MaterialExporter.hpp
/// @brief   aiMaterial を .mat (TOML) に書き出し、テクスチャを outputDir/textures/ にコピーする。
/// @author  Hasegawa Jin
/// @date    2026-06-18
#pragma once
#include <string>

struct aiMaterial;
struct aiScene;

namespace fbzz::editor {

class MaterialExporter {
public:
    // aiMaterial を .mat TOML として outputPath に書き出す。
    // テクスチャファイルを texturesDir にコピーし、.mat 内のパスを相対化する。
    // fbxDir: FBX ファイルが置かれているディレクトリ (テクスチャ解決用)。
    // fbxBaseName: FBX の拡張子なしファイル名。解決先の "<fbxBaseName>.fbm/" を探すのに使う。
    // .mat は元画像を直接参照する。インポート設定は元画像隣の "<画像>.meta" が担う。
    // @ret 成功なら true
    static bool Export(const aiMaterial* material,
                       const aiScene* scene,
                       const std::string& fbxDir,
                       const std::string& fbxBaseName,
                       const std::string& texturesDir,
                       const std::string& outputPath,
                       bool skinned = false,
                       bool flipGreenChannel = false);
};

} // namespace fbzz::editor
