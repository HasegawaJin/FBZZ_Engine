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
    /// @brief aiMaterial と元画像を .mat と texturesDir へ書き出す。
    /// @param fbxDir FBX の配置ディレクトリ。
    /// @param fbxBaseName 拡張子なしの FBX 名。隣接する .fbm の探索に使う。
    /// @note OpenGL 法線の G 反転は元画像を書き換えず .meta の flip_green で指定する。
    /// @return 成功なら true
    static bool Export(const aiMaterial* material,
                       const aiScene* scene,
                       const std::string& fbxDir,
                       const std::string& fbxBaseName,
                       const std::string& texturesDir,
                       const std::string& outputPath,
                       bool skinned = false);
};

} /// @note namespace fbzz::editor
