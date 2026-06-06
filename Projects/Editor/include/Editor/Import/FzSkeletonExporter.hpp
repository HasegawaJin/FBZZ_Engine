// FBZZ Engine
// FzSkeletonExporter.hpp | fbzz::editor
// aiScene のスケルトン情報を .fzskel バイナリに書き出す
#pragma once
#include <string>

struct aiScene;

namespace fbzz::editor {

class FzSkeletonExporter {
public:
    // aiScene からスケルトンを抽出して outputPath に書き出す。
    // unitScale: 平行移動成分のスケール係数。
    // @ret 成功なら true
    static bool Export(const aiScene* scene,
                       float unitScale,
                       const std::string& outputPath);
};

} // namespace fbzz::editor
