// FBZZ Engine
// ProjectSettings.hpp | fbzz
// プロジェクト共通設定（タグ・レイヤー名）の定義と永続化
#pragma once
#include <array>
#include <string>
#include <vector>

namespace fbzz {

struct ProjectSettings {
    std::vector<std::string>    tags;
    std::array<std::string, 32> layerNames;

    static ProjectSettings Default();
    bool Load(const std::string& path);
    bool Save(const std::string& path) const;
};

} // namespace fbzz
