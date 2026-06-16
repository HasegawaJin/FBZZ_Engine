// FBZZ Engine
// FzMaterialExporter.hpp | fbzz::editor
// aiMaterial を .fzmat (TOML) に書き出し、テクスチャを outputDir/textures/ にコピーする
#pragma once
#include <string>

struct aiMaterial;
struct aiScene;

namespace fbzz::editor {

class FzMaterialExporter {
public:
    // aiMaterial を .fzmat TOML として outputPath に書き出す。
    // テクスチャファイルを texturesDir にコピーし、.fzmat 内のパスを相対化する。
    // fbxDir: FBX ファイルが置かれているディレクトリ (テクスチャ解決用)。
    // @ret 成功なら true
    static bool Export(const aiMaterial* material,
                       const aiScene* scene,
                       const std::string& fbxDir,
                       const std::string& texturesDir,
                       const std::string& outputPath,
                       bool skinned = false,
                       bool flipGreenChannel = false);
};

} // namespace fbzz::editor
