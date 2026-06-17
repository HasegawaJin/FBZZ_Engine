// FBZZ Engine
// FzAssetWriter.hpp | fbzz::editor
// .asset マニフェスト (TOML) を書き出す
#pragma once
#include <string>
#include <vector>

namespace fbzz::editor {

struct FzAssetManifest {
    float       unitScale    = 0.01f;
    std::string sourceHint;                  // 元の FBX パス (参考情報)
    std::vector<std::string> meshPaths;      // Assets/ からの相対パス
    std::vector<std::string> materialPaths;  // meshPaths[i] に対応
    std::string skeletonPath;                // 空 = スキンなし
    std::vector<std::string> animPaths;
};

class FzAssetWriter {
public:
    // マニフェストを outputPath に TOML として書き出す。
    // @ret 成功なら true
    static bool Write(const FzAssetManifest& manifest,
                      const std::string& outputPath);
};

} // namespace fbzz::editor
