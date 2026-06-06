// FBZZ Engine
// FzMeshExporter.hpp | fbzz::editor
// aiMesh をエンジンネイティブバイナリ (.fzmesh) に書き出す
#pragma once
#include <string>

struct aiMesh;
struct aiScene;

namespace fbzz::editor {

class FzMeshExporter {
public:
    // aiMesh を .fzmesh バイナリとして outputPath に書き出す。
    // unitScale: ReadUnitScale() で取得したメートル換算係数。
    // isSkinned: SkinnedVertex レイアウトで書き出す場合 true。
    // @ret 成功なら true
    static bool Export(const aiMesh* mesh,
                       const aiScene* scene,
                       float unitScale,
                       bool isSkinned,
                       const std::string& outputPath);
};

} // namespace fbzz::editor
