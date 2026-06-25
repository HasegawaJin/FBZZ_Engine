// FBZZ Engine
// MaterialExporter.hpp | fbzz::editor
// aiMaterial を .mat (TOML) に書き出し、テクスチャを outputDir/textures/ にコピーする
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
    // useTexDescriptors: true なら生成画像ではなく同名 .tex をマテリアルから参照する。
    // @ret 成功なら true
    static bool Export(const aiMaterial* material,
                       const aiScene* scene,
                       const std::string& fbxDir,
                       const std::string& texturesDir,
                       const std::string& outputPath,
                       bool skinned = false,
                       bool flipGreenChannel = false,
                       bool useTexDescriptors = false);
};

} // namespace fbzz::editor
