// FBZZ Engine
// EditorSettings.cpp | fbzz::editor
// エディター設定の TOML 永続化実装
#include <editor/Util/EditorSettings.hpp>
#include <toml++/toml.hpp>
#include <engine/Util/FileSystem.hpp>
#include <engine/Core/Logger.hpp>
#include <sstream>

namespace fbzz::editor {

bool EditorSettings::Load(const std::string& path)
{
    std::string text;
    if (!util::FileSystem::ReadText(path, text)) return false;

    // TOML_EXCEPTIONS=0 なので parse_result で受ける
    auto result = toml::parse(text);
    if (!result) {
        FBZZ_LOG_WARN("EditorSettings: parse failed: %s", path.c_str());
        return false;
    }
    auto& tbl = result.table();

    if (auto v = tbl["camera"]["speed"].value<float>())        cameraSpeed       = *v;
    if (auto v = tbl["camera"]["sensitivity"].value<float>())   cameraSensitivity = *v;
    if (auto v = tbl["view"]["show_grid"].value<bool>())        showGrid          = *v;
    if (auto v = tbl["snap"]["enabled"].value<bool>())          snapEnabled       = *v;
    if (auto v = tbl["snap"]["distance"].value<float>())        snapDistance      = *v;
    if (auto v = tbl["scene"]["last_path"].value<std::string>()) lastScenePath    = *v;

    return true;
}

bool EditorSettings::Save(const std::string& path) const
{
    toml::table camTbl;
    camTbl.insert("speed",       cameraSpeed);
    camTbl.insert("sensitivity", cameraSensitivity);

    toml::table viewTbl;
    viewTbl.insert("show_grid", showGrid);

    toml::table snapTbl;
    snapTbl.insert("enabled",  snapEnabled);
    snapTbl.insert("distance", snapDistance);

    toml::table sceneTbl;
    sceneTbl.insert("last_path", lastScenePath);

    toml::table root;
    root.insert("camera", std::move(camTbl));
    root.insert("view",   std::move(viewTbl));
    root.insert("snap",   std::move(snapTbl));
    root.insert("scene",  std::move(sceneTbl));

    std::ostringstream ss;
    ss << root;
    return util::FileSystem::WriteText(path, ss.str());
}

} // namespace fbzz::editor
