// FBZZ Engine
// GraphLayoutSerializer.hpp | fbzz::editor
// Animation Graph Editor の .animgraph サイドカーファイル読み書き
// WHY: シーン本体を汚さず、チームで共有できる編集用レイアウトだけを別ファイルに保存する。
#pragma once
#include <Editor/GraphLayout.hpp>
#include <string>
#include <unordered_map>

namespace fbzz::editor {

class GraphLayoutSerializer {
public:
    static bool Save(const std::unordered_map<std::string, GraphLayout>& layouts,
                     const std::string& scenePath);
    static bool Load(std::unordered_map<std::string, GraphLayout>& layouts,
                     const std::string& scenePath);
};

} // namespace fbzz::editor
