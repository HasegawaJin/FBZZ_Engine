// FBZZ Engine
// GraphLayoutSerializer.cpp | fbzz::editor
// Animation Graph Editor の .animgraph サイドカーファイル読み書き
// WHAT: scenePath + ".animgraph" に TOML 形式で instanceId / stateName / 座標を書き出す。
#include <Editor/GraphLayoutSerializer.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <toml++/toml.hpp>
#include <sstream>
#include <utility>

namespace fbzz::editor {

namespace {

std::string SidecarPath(const std::string& scenePath)
{
    return scenePath.empty() ? std::string{} : scenePath + ".animgraph";
}

} // namespace

bool GraphLayoutSerializer::Save(const std::unordered_map<std::string, GraphLayout>& layouts,
                                 const std::string& scenePath)
{
    const std::string path = SidecarPath(scenePath);
    if (path.empty()) return false;

    toml::array nodes;
    for (const auto& [instanceId, layout] : layouts) {
        for (const auto& [stateName, pos] : layout.nodePositions) {
            toml::table node;
            node.insert("instanceId", instanceId);
            node.insert("stateName", stateName);
            node.insert("x", static_cast<double>(pos.x));
            node.insert("y", static_cast<double>(pos.y));
            nodes.push_back(std::move(node));
        }
    }

    toml::table root;
    root.insert("version", 1);
    root.insert("nodes", std::move(nodes));

    std::ostringstream oss;
    oss << root;
    return util::FileSystem::WriteText(path, oss.str());
}

bool GraphLayoutSerializer::Load(std::unordered_map<std::string, GraphLayout>& layouts,
                                 const std::string& scenePath)
{
    layouts.clear();
    const std::string path = SidecarPath(scenePath);
    if (path.empty() || !util::FileSystem::Exists(path)) return true;

    std::string text;
    if (!util::FileSystem::ReadText(path, text)) return false;

    toml::parse_result result = toml::parse(text);
    if (!result) return false;

    const toml::array* nodes = result["nodes"].as_array();
    if (!nodes) return true;

    nodes->for_each([&](const toml::table& node) {
        const std::string instanceId = node["instanceId"].value_or(std::string{});
        const std::string stateName  = node["stateName"].value_or(std::string{});
        if (instanceId.empty() || stateName.empty()) return;

        const float x = static_cast<float>(node["x"].value_or(0.0));
        const float y = static_cast<float>(node["y"].value_or(0.0));
        layouts[instanceId].nodePositions[stateName] = ImVec2(x, y);
    });

    return true;
}

} // namespace fbzz::editor
