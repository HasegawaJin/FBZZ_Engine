// FBZZ Engine
// ProjectSettings.cpp | fbzz
// プロジェクト設定の TOML 永続化実装
// タグ・レイヤーなどエディタとランタイムで共有する設定を読み書きする。
// 失敗時は bool で返し、例外は使わない。
#include <Engine/ProjectSettings.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Engine/Input/InputActionMap.hpp>
#include <toml++/toml.hpp>
#include <cmath>
#include <filesystem>
#include <sstream>
#include <string_view>
#include <utility>

namespace fbzz {

namespace {

double RoundTomlFloat(double value)
{
    constexpr double SCALE = 1000000.0;
    const double rounded = std::round(value * SCALE) / SCALE;
    return rounded == 0.0 ? 0.0 : rounded;
}

void NormalizeTomlFloats(toml::node& node)
{
    if (auto* value = node.as_floating_point()) {
        value->get() = RoundTomlFloat(value->get());
        return;
    }

    if (auto* table = node.as_table()) {
        for (auto&& [key, child] : *table) {
            (void)key;
            NormalizeTomlFloats(child);
        }
        return;
    }

    if (auto* array = node.as_array()) {
        for (auto& child : *array)
            NormalizeTomlFloats(child);
    }
}

toml::array Vec3ToArr(const math::Vector3& v)
{
    toml::array a;
    a.push_back((double)v.x);
    a.push_back((double)v.y);
    a.push_back((double)v.z);
    return a;
}

math::Vector3 ArrToVec3(const toml::array* arr, const math::Vector3& def)
{
    if (!arr || arr->size() < 3) return def;
    return {
        (float)(*arr)[0].value_or((double)def.x),
        (float)(*arr)[1].value_or((double)def.y),
        (float)(*arr)[2].value_or((double)def.z)
    };
}

bool ReadBool(const toml::table& table, const char* key, bool fallback)
{
    if (auto value = table[key].value<bool>())
        return *value;
    return fallback;
}

float ReadFloat(const toml::table& table, const char* key, float fallback)
{
    if (auto value = table[key].value<double>())
        return static_cast<float>(*value);
    if (auto value = table[key].value<int64_t>())
        return static_cast<float>(*value);
    return fallback;
}

renderer::RenderingPipeline RenderingPipelineFromString(std::string_view value,
                                                        renderer::RenderingPipeline fallback)
{
    // 手編集された旧表記も受け付け、既存の ProjectSettings.toml を壊さず移行する。
    if (value == "forward" || value == "Forward")
        return renderer::RenderingPipeline::Forward;
    if (value == "deferred" || value == "Deferred")
        return renderer::RenderingPipeline::Deferred;
    if (value == "forward_plus" || value == "forward+" || value == "Forward+")
        return renderer::RenderingPipeline::ForwardPlus;
    if (value == "deferred_plus" || value == "deferred+" || value == "Deferred+")
        return renderer::RenderingPipeline::DeferredPlus;
    return fallback;
}

const char* RenderingPipelineToString(renderer::RenderingPipeline pipeline)
{
    switch (pipeline) {
    // 保存値も Editor の表示名に揃える。Load 側は旧 lowercase / underscore 表記も受け付ける。
    case renderer::RenderingPipeline::Forward:      return "Forward";
    case renderer::RenderingPipeline::Deferred:     return "Deferred";
    case renderer::RenderingPipeline::ForwardPlus:  return "Forward+";
    case renderer::RenderingPipeline::DeferredPlus: return "Deferred+";
    }
    return "Forward";
}

} // namespace

ProjectSettings ProjectSettings::Default()
{
    ProjectSettings ps;
    ps.game.project.defaultScene = "Assets/Scenes/Main.scene";
    ps.game.runtime.startScene   = "Assets/Scenes/Main.scene";
    ps.game.tags = { "Untagged", "Respawn", "Finish", "EditorOnly",
                     "MainCamera", "Player", "GameController" };
    ps.game.layerNames = {
        "Default", "TransparentFX", "Ignore Raycast", "", "Water", "UI",
        "", "", "", "", "", "", "", "", "", "",
        "", "", "", "", "", "", "", "", "", "",
        "", "", "", "", "", ""
    };
    ps.physics.hz       = 60;
    ps.physics.substeps = 1;
    ps.physics.gravity  = { 0.0f, -9.81f, 0.0f };
    return ps;
}

bool ProjectSettings::Load(const std::string& path)
{
    std::string text;
    if (!util::FileSystem::ReadText(path, text)) {
        // WHY: Load 失敗時に既存設定を代入で破棄すると、呼び出し側が保持していた設定や UI 参照まで巻き戻る。
        //      失敗は bool で伝え、現在の設定はそのまま残す。
        FBZZ_LOG_WARN("ProjectSettings: read failed: %s", path.c_str());
        return false;
    }

    auto result = toml::parse(text);
    if (!result) {
        FBZZ_LOG_WARN("ProjectSettings: parse failed: %s", path.c_str());
        return false;
    }
    auto& tbl = result.table();

    if (auto* projectTbl = tbl["project"].as_table()) {
        game.project.name         = (*projectTbl)["name"].value_or(game.project.name);
        game.project.defaultScene = (*projectTbl)["default_scene"].value_or(game.project.defaultScene);
    }

    if (auto* runtimeTbl = tbl["runtime"].as_table()) {
        game.runtime.startScene = (*runtimeTbl)["start_scene"].value_or(game.runtime.startScene);
    }

    if (auto* tagArr = tbl["tags"]["list"].as_array()) {
        game.tags.clear();
        for (auto& elem : *tagArr)
            if (auto v = elem.value<std::string>())
                game.tags.push_back(*v);
    }

    if (auto* layArr = tbl["layers"]["names"].as_array()) {
        for (int i = 0; i < 32 && i < (int)layArr->size(); ++i)
            if (auto v = (*layArr)[i].value<std::string>())
                game.layerNames[i] = *v;
    }

    if (auto* physicsTbl = tbl["physics"].as_table()) {
        physics.hz      = (int)(*physicsTbl)["hz"].value_or((int64_t)physics.hz);
        physics.substeps = (int)(*physicsTbl)["substeps"].value_or((int64_t)physics.substeps);
        physics.gravity = ArrToVec3((*physicsTbl)["gravity"].as_array(), physics.gravity);
    }

    if (physics.hz < 1)        physics.hz = 1;
    if (physics.hz > 1000)     physics.hz = 1000;
    if (physics.substeps < 1)  physics.substeps = 1;
    if (physics.substeps > 32) physics.substeps = 32;

    // [render] — プロジェクト全体で固定の描画構成のみを扱う。
    // WHY ポストプロセス / 高度グラフィクスが無いか:
    //     Bloom や SSR のような「ルック」は場所ごとに変わるものなので、
    //     PostProcessVolume + PostProcessProfile (.fzdata) を唯一の所有者にした。
    //     ここに残すのは、シーンをまたいでも変わらない構成
    //     (パイプライン・シャドウ品質・デバッグ表示・パーティクル予算) だけ。
    //     旧 .toml に残っている bloom = … 等のキーは単に無視される。
    if (auto* renderTbl = tbl["render"].as_table()) {
        {
            const auto s = (*renderTbl)["pipeline"].value_or(std::string("forward"));
            render.pipeline = RenderingPipelineFromString(s, render.pipeline);
        }
        {
            const int vm = (*renderTbl)["viewMode"].value_or(static_cast<int>(render.viewMode));
            render.viewMode = static_cast<renderer::ViewMode>(vm);
        }

        // ── シャドウ ──────────────────────────────────────────────
        render.shadowEnabled = (*renderTbl)["shadow"].value_or(render.shadowEnabled);
        render.shadow.mapResolution = static_cast<uint32_t>(
            (*renderTbl)["shadowResolution"].value_or(static_cast<int64_t>(render.shadow.mapResolution)));
        render.shadow.pcfRadius = static_cast<int>(
            (*renderTbl)["shadowPcfRadius"].value_or(static_cast<int64_t>(render.shadow.pcfRadius)));
        render.shadow.autoFitDistance =
            ReadFloat(*renderTbl, "shadowAutoFitDistance", render.shadow.autoFitDistance);
        render.shadow.cascadeCount = static_cast<int>(
            (*renderTbl)["shadowCascadeCount"].value_or(static_cast<int64_t>(render.shadow.cascadeCount)));
        render.shadow.cascadeSplitLambda =
            ReadFloat(*renderTbl, "shadowCascadeSplitLambda", render.shadow.cascadeSplitLambda);
        render.shadow.cascadeBlend =
            ReadFloat(*renderTbl, "shadowCascadeBlend", render.shadow.cascadeBlend);
        render.shadow.pcssEnabled     = ReadBool(*renderTbl,  "pcssEnabled",     render.shadow.pcssEnabled);
        render.shadow.pcssLightRadius = ReadFloat(*renderTbl, "pcssLightRadius", render.shadow.pcssLightRadius);

        render.clustered.enabled = ReadBool(*renderTbl, "clusteredEnabled", render.clustered.enabled);
        render.clustered.maxDistance = ReadFloat(
            *renderTbl, "clusteredMaxDistance", render.clustered.maxDistance);
        render.clustered.debugHeatmap = ReadBool(
            *renderTbl, "clusteredDebugHeatmap", render.clustered.debugHeatmap);
        render.clustered.forceAllLights = ReadBool(
            *renderTbl, "clusteredForceAllLights", render.clustered.forceAllLights);

        // ── デバッグ表示 ──────────────────────────────────────────
        render.showColliders        = (*renderTbl)["showColliders"].value_or(render.showColliders);
        render.showDecalBounds      = (*renderTbl)["showDecalBounds"].value_or(render.showDecalBounds);
        render.showSelectionOutline = (*renderTbl)["showSelectionOutline"].value_or(render.showSelectionOutline);
        render.passViewerEnabled    = (*renderTbl)["passViewerEnabled"].value_or(render.passViewerEnabled);

        // ── パーティクル予算 ──────────────────────────────────────
        render.particleBudget        = (int)(*renderTbl)["particleBudget"].value_or((int64_t)render.particleBudget);
        render.particleBudgetEnabled = (*renderTbl)["particleBudgetEnabled"].value_or(render.particleBudgetEnabled);

        // ── 選択アウトライン ──────────────────────────────────────
        render.outlineWidth = (float)(*renderTbl)["outlineWidth"].value_or((double)render.outlineWidth);
        if (auto* outlineColorArr = (*renderTbl)["outlineColor"].as_array(); outlineColorArr && outlineColorArr->size() >= 4) {
            render.outlineColor[0] = (float)(*outlineColorArr)[0].value_or((double)render.outlineColor[0]);
            render.outlineColor[1] = (float)(*outlineColorArr)[1].value_or((double)render.outlineColor[1]);
            render.outlineColor[2] = (float)(*outlineColorArr)[2].value_or((double)render.outlineColor[2]);
            render.outlineColor[3] = (float)(*outlineColorArr)[3].value_or((double)render.outlineColor[3]);
        }
    }

    if (auto* audioTbl = tbl["audio"].as_table()) {
        audio.bgmVolume = (float)(*audioTbl)["bgmVolume"].value_or((double)audio.bgmVolume);
        audio.seVolume  = (float)(*audioTbl)["seVolume"].value_or((double)audio.seVolume);
        if (audio.bgmVolume < 0.0f) audio.bgmVolume = 0.0f;
        if (audio.bgmVolume > 1.0f) audio.bgmVolume = 1.0f;
        if (audio.seVolume  < 0.0f) audio.seVolume  = 0.0f;
        if (audio.seVolume  > 1.0f) audio.seVolume  = 1.0f;
    }

    if (auto* screenTbl = tbl["screen"].as_table()) {
        screen.width  = (int)(*screenTbl)["width"].value_or((int64_t)screen.width);
        screen.height = (int)(*screenTbl)["height"].value_or((int64_t)screen.height);
        if (screen.width  < 1) screen.width  = 1;
        if (screen.height < 1) screen.height = 1;
    }

    if (auto* appTbl = tbl["app"].as_table()) {
        app.targetFps = (int)(*appTbl)["targetFps"].value_or((int64_t)app.targetFps);
        if (app.targetFps < 0) app.targetFps = 0;
        if (auto backend = (*appTbl)["renderer"].value<std::string>())
            app.rendererBackend = renderer::BackendFromString(*backend);
    }

    if (auto* windowTbl = tbl["window"].as_table()) {
        window.title      = (*windowTbl)["title"].value_or(window.title);
        window.width      = (int)(*windowTbl)["width"].value_or((int64_t)window.width);
        window.height     = (int)(*windowTbl)["height"].value_or((int64_t)window.height);
        window.fullscreen = (*windowTbl)["fullscreen"].value_or(window.fullscreen);
        if (window.width  < 1) window.width  = 1;
        if (window.height < 1) window.height = 1;
    }

    if (auto* uiTbl = tbl["ui"].as_table())
        ui.defaultFontPath = (*uiTbl)["default_font"].value_or(ui.defaultFontPath);

    // ── 入力バインド ─────────────────────────────────────────────────────────
    // ProjectSettings.toml と同じディレクトリの Input.inputactions を読む。
    // WHY 別ファイルにするか: バインド定義は配列の入れ子が深く、ProjectSettings.toml へ
    //     混ぜると設定全体が読みにくくなる。またキーコンフィグはプレイヤーが実行時に
    //     書き換える対象で、開発者が編集する他の設定とは更新頻度も責務も異なる。
    // WHY 失敗しても Load 全体を失敗させないか: 入力ファイルは無くて当然 (既定バインドで動く)。
    //     ここで false を返すとプロジェクト設定そのものが読めなかった扱いになってしまう。
    {
        const std::filesystem::path settingsPath(path);
        const std::filesystem::path inputPath =
            settingsPath.has_parent_path()
                ? settingsPath.parent_path() / "Input.inputactions"
                : std::filesystem::path("Input.inputactions");
        (void)input::InputActionMap::LoadFromFile(inputPath.string());
    }

    return true;
}

bool ProjectSettings::Save(const std::string& path) const
{
    toml::array tagArr;
    for (const auto& t : game.tags)
        tagArr.push_back(t);

    toml::array layArr;
    for (const auto& n : game.layerNames)
        layArr.push_back(n);

    toml::table tagTbl;
    tagTbl.insert("list", std::move(tagArr));

    toml::table layTbl;
    layTbl.insert("names", std::move(layArr));

    toml::table physicsTbl;
    physicsTbl.insert("hz",       (int64_t)physics.hz);
    physicsTbl.insert("substeps", (int64_t)physics.substeps);
    physicsTbl.insert("gravity",  Vec3ToArr(physics.gravity));

    toml::array outlineColorArr;
    outlineColorArr.push_back((double)render.outlineColor[0]);
    outlineColorArr.push_back((double)render.outlineColor[1]);
    outlineColorArr.push_back((double)render.outlineColor[2]);
    outlineColorArr.push_back((double)render.outlineColor[3]);

    // [render] の保存対象は Load と対になる「プロジェクト全体で固定の構成」のみ。
    // ルック (ポストプロセス / 高度グラフィクス) は PostProcessProfile (.fzdata) が保存する。
    toml::table renderTbl;
    renderTbl.insert("pipeline", RenderingPipelineToString(render.pipeline));
    renderTbl.insert("viewMode", static_cast<int>(render.viewMode));

    // ── Shadow 品質 ─────────────────────────────────────────────────────────
    renderTbl.insert("shadow",            render.shadowEnabled);
    renderTbl.insert("shadowResolution",  (int64_t)render.shadow.mapResolution);
    renderTbl.insert("shadowPcfRadius",   (int64_t)render.shadow.pcfRadius);
    renderTbl.insert("shadowAutoFitDistance", (double)render.shadow.autoFitDistance);
    renderTbl.insert("shadowCascadeCount",       (int64_t)render.shadow.cascadeCount);
    renderTbl.insert("shadowCascadeSplitLambda", (double)render.shadow.cascadeSplitLambda);
    renderTbl.insert("shadowCascadeBlend",       (double)render.shadow.cascadeBlend);
    renderTbl.insert("pcssEnabled",       render.shadow.pcssEnabled);
    renderTbl.insert("pcssLightRadius",   (double)render.shadow.pcssLightRadius);

    renderTbl.insert("clusteredEnabled",        render.clustered.enabled);
    renderTbl.insert("clusteredMaxDistance",    (double)render.clustered.maxDistance);
    renderTbl.insert("clusteredDebugHeatmap",   render.clustered.debugHeatmap);
    renderTbl.insert("clusteredForceAllLights", render.clustered.forceAllLights);

    // ── デバッグ表示 ────────────────────────────────────────────────────────
    renderTbl.insert("showColliders",        render.showColliders);
    renderTbl.insert("showDecalBounds",      render.showDecalBounds);
    renderTbl.insert("showSelectionOutline", render.showSelectionOutline);
    renderTbl.insert("passViewerEnabled",    render.passViewerEnabled);

    // ── パーティクル予算 ────────────────────────────────────────────────────
    renderTbl.insert("particleBudget",        (int64_t)render.particleBudget);
    renderTbl.insert("particleBudgetEnabled", render.particleBudgetEnabled);

    // ── 選択アウトライン ────────────────────────────────────────────────────
    renderTbl.insert("outlineWidth", (double)render.outlineWidth);
    renderTbl.insert("outlineColor", std::move(outlineColorArr));

    toml::table audioTbl;
    audioTbl.insert("bgmVolume", (double)audio.bgmVolume);
    audioTbl.insert("seVolume",  (double)audio.seVolume);

    toml::table screenTbl;
    screenTbl.insert("width",  (int64_t)screen.width);
    screenTbl.insert("height", (int64_t)screen.height);

    toml::table appTbl;
    appTbl.insert("targetFps", (int64_t)app.targetFps);
    appTbl.insert("renderer", renderer::ToString(app.rendererBackend));

    toml::table windowTbl;
    windowTbl.insert("title",      window.title);
    windowTbl.insert("width",      (int64_t)window.width);
    windowTbl.insert("height",     (int64_t)window.height);
    windowTbl.insert("fullscreen", window.fullscreen);

    toml::table projectTbl;
    projectTbl.insert("name", game.project.name);
    projectTbl.insert("default_scene", game.project.defaultScene);

    toml::table runtimeTbl;
    runtimeTbl.insert("start_scene", game.runtime.startScene);

    toml::table uiTbl;
    uiTbl.insert("default_font", ui.defaultFontPath);

    toml::table root;
    root.insert("project", std::move(projectTbl));
    root.insert("runtime", std::move(runtimeTbl));
    root.insert("tags",    std::move(tagTbl));
    root.insert("layers",  std::move(layTbl));
    root.insert("physics", std::move(physicsTbl));
    root.insert("render",  std::move(renderTbl));
    root.insert("audio",   std::move(audioTbl));
    root.insert("screen",  std::move(screenTbl));
    root.insert("app",     std::move(appTbl));
    root.insert("window",  std::move(windowTbl));
    root.insert("ui",      std::move(uiTbl));

    NormalizeTomlFloats(root);

    std::ostringstream ss;
    ss << root;
    return util::FileSystem::WriteText(path, ss.str());
}

} // namespace fbzz
