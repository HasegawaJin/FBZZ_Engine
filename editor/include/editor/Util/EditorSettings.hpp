// FBZZ Engine
// EditorSettings.hpp | fbzz::editor
// エディター設定の永続化 (toml++ 使用)
#pragma once
#include <string>

namespace fbzz::editor {

struct EditorSettings {
    float cameraSpeed       = 5.0f;
    float cameraSensitivity = 0.3f;
    bool  showGrid          = true;
    bool  snapEnabled       = false;
    float snapDistance      = 1.0f;
    std::string lastScenePath;

    bool Load(const std::string& path);
    bool Save(const std::string& path) const;
};

} // namespace fbzz::editor
