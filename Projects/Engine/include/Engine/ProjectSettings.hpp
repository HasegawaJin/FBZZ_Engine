// FBZZ Engine
// ProjectSettings.hpp | fbzz
// プロジェクト共通設定の定義と永続化
// タグ名・レイヤー名など、エディタとランタイムで共有する軽量設定。
// 読み書きは bool で成否を返し、例外は使わない。
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
