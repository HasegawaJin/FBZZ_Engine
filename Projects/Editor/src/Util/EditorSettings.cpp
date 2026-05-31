// FBZZ Engine
// EditorSettings.cpp | fbzz::editor
// エディター設定の TOML 永続化実装
#include <Editor/Util/EditorSettings.hpp>
#include <toml++/toml.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Engine/Core/Logger.hpp>
#include <filesystem>
#include <sstream>

namespace fbzz::editor {

bool EditorSettings::Load(const std::string& path, const std::string& projectRoot)
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

    // カメラ
    if (auto v = tbl["camera"]["speed"].value<float>())       cameraSpeed       = *v;
    if (auto v = tbl["camera"]["sensitivity"].value<float>()) cameraSensitivity = *v;

    // ビュー
    if (auto v = tbl["view"]["show_grid"].value<bool>())      showGrid      = *v;
    if (auto v = tbl["view"]["grid_size"].value<float>())     gridSize      = *v;
    if (auto v = tbl["view"]["show_light_range"].value<bool>()) showLightRange = *v;
    if (auto v = tbl["view"]["show_skeleton"].value<bool>())  showSkeleton  = *v;
    if (auto v = tbl["view"]["show_stats"].value<bool>())     showStats = *v;

    // スナップ
    if (auto v = tbl["snap"]["enabled"].value<bool>())        snapEnabled   = *v;
    if (auto v = tbl["snap"]["distance"].value<float>())      snapDistance  = *v;

    // ギズモ
    if (auto v = tbl["gizmo"]["mode"].value<int64_t>())       gizmoMode     = static_cast<int>(*v);
    if (auto v = tbl["gizmo"]["space"].value<int64_t>())      gizmoSpace    = static_cast<int>(*v);

    // ゲームビュー
    if (auto v = tbl["game_view"]["aspect"].value<int64_t>()) gameViewportAspect = static_cast<int>(*v);

    // その他
    if (auto v = tbl["misc"]["hot_reload"].value<bool>())     hotReloadEnabled = *v;
    if (auto v = tbl["scene"]["last_path"].value<std::string>()) {
        lastScenePath = *v;
        // 相対パスで保存されていた場合は projectRoot と組み合わせて絶対パスに戻す
        if (!projectRoot.empty() && !lastScenePath.empty()) {
            std::error_code ec;
            std::filesystem::path p(lastScenePath);
            if (p.is_relative()) {
                const auto abs = std::filesystem::absolute(
                    std::filesystem::path(projectRoot) / p, ec);
                if (!ec) lastScenePath = abs.string();
            }
        }
    }

    return true;
}

bool EditorSettings::Save(const std::string& path, const std::string& projectRoot) const
{
    toml::table camTbl;
    camTbl.insert("speed",       cameraSpeed);
    camTbl.insert("sensitivity", cameraSensitivity);

    toml::table viewTbl;
    viewTbl.insert("show_grid",       showGrid);
    viewTbl.insert("grid_size",       gridSize);
    viewTbl.insert("show_light_range", showLightRange);
    viewTbl.insert("show_skeleton",   showSkeleton);
    viewTbl.insert("show_stats",      showStats);

    toml::table snapTbl;
    snapTbl.insert("enabled",  snapEnabled);
    snapTbl.insert("distance", snapDistance);

    toml::table gizmoTbl;
    gizmoTbl.insert("mode",  static_cast<int64_t>(gizmoMode));
    gizmoTbl.insert("space", static_cast<int64_t>(gizmoSpace));

    toml::table gameViewTbl;
    gameViewTbl.insert("aspect", static_cast<int64_t>(gameViewportAspect));

    toml::table miscTbl;
    miscTbl.insert("hot_reload", hotReloadEnabled);

    // lastScenePath を projectRoot 相対パスに変換して保存する
    // WHY: 絶対パスのまま保存するとプロジェクトフォルダを移動した後にシーンが見つからなくなる
    std::string scenePathToSave = lastScenePath;
    if (!projectRoot.empty() && !lastScenePath.empty()) {
        std::error_code ec;
        const std::filesystem::path rel = std::filesystem::relative(
            std::filesystem::path(lastScenePath), std::filesystem::path(projectRoot), ec);
        if (!ec && !rel.empty())
            scenePathToSave = rel.generic_string();
    }

    toml::table sceneTbl;
    sceneTbl.insert("last_path", scenePathToSave);

    toml::table root;
    root.insert("camera",    std::move(camTbl));
    root.insert("view",      std::move(viewTbl));
    root.insert("snap",      std::move(snapTbl));
    root.insert("gizmo",     std::move(gizmoTbl));
    root.insert("game_view", std::move(gameViewTbl));
    root.insert("misc",      std::move(miscTbl));
    root.insert("scene",     std::move(sceneTbl));

    std::ostringstream ss;
    ss << root;
    return util::FileSystem::WriteText(path, ss.str());
}

} // namespace fbzz::editor
