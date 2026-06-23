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
        toml::table specialNodes;
        specialNodes.insert("instanceId", instanceId);
        specialNodes.insert("entryX", static_cast<double>(layout.entryPosition.x));
        specialNodes.insert("entryY", static_cast<double>(layout.entryPosition.y));
        specialNodes.insert("anyStateX", static_cast<double>(layout.anyStatePosition.x));
        specialNodes.insert("anyStateY", static_cast<double>(layout.anyStatePosition.y));
        nodes.push_back(std::move(specialNodes));

        for (const auto& [stateName, pos] : layout.nodePositions) {
            toml::table node;
            node.insert("instanceId", instanceId);
            node.insert("stateName", stateName);
            node.insert("x", static_cast<double>(pos.x));
            node.insert("y", static_cast<double>(pos.y));
            nodes.push_back(std::move(node));
        }
        for (const auto& [stateName, positions] : layout.blendTreeMotionPositions) {
            for (size_t motionIndex = 0; motionIndex < positions.size(); ++motionIndex) {
                toml::table node;
                node.insert("instanceId", instanceId);
                node.insert("blendStateName", stateName);
                node.insert("motionIndex", static_cast<int64_t>(motionIndex));
                node.insert("x", static_cast<double>(positions[motionIndex].x));
                node.insert("y", static_cast<double>(positions[motionIndex].y));
                nodes.push_back(std::move(node));
            }
        }
    }

    toml::table root;
    root.insert("version", 3);
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
        const std::string blendStateName =
            node["blendStateName"].value_or(std::string{});
        if (instanceId.empty()) return;
        if (!blendStateName.empty()) {
            const int motionIndex =
                static_cast<int>(node["motionIndex"].value_or(int64_t{-1}));
            if (motionIndex < 0) return;
            auto& positions =
                layouts[instanceId].blendTreeMotionPositions[blendStateName];
            if (positions.size() <= static_cast<size_t>(motionIndex))
                positions.resize(static_cast<size_t>(motionIndex) + 1);
            positions[static_cast<size_t>(motionIndex)] = ImVec2(
                static_cast<float>(node["x"].value_or(0.0)),
                static_cast<float>(node["y"].value_or(0.0)));
            return;
        }
        if (stateName.empty()) {
            GraphLayout& layout = layouts[instanceId];
            layout.entryPosition = ImVec2(
                static_cast<float>(node["entryX"].value_or(-220.0)),
                static_cast<float>(node["entryY"].value_or(80.0)));
            layout.anyStatePosition = ImVec2(
                static_cast<float>(node["anyStateX"].value_or(-220.0)),
                static_cast<float>(node["anyStateY"].value_or(260.0)));
            return;
        }

        const float x = static_cast<float>(node["x"].value_or(0.0));
        const float y = static_cast<float>(node["y"].value_or(0.0));
        layouts[instanceId].nodePositions[stateName] = ImVec2(x, y);
    });

    return true;
}

} // namespace fbzz::editor
