// FBZZ Engine
// ProjectSettings.cpp | fbzz
// プロジェクト設定の TOML 永続化実装
// タグ・レイヤーなどエディタとランタイムで共有する設定を読み書きする。
// 失敗時は bool で返し、例外は使わない。
#include <Engine/ProjectSettings.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <toml++/toml.hpp>
#include <sstream>

namespace fbzz {

namespace {

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

} // namespace

ProjectSettings ProjectSettings::Default()
{
    ProjectSettings ps;
    ps.tags = { "Untagged", "Respawn", "Finish", "EditorOnly",
                "MainCamera", "Player", "GameController" };
    ps.layerNames = {
        "Default", "TransparentFX", "IgnoreRaycast", "3", "Water", "UI",
        "6",  "7",  "8",  "9",  "10", "11", "12", "13", "14", "15",
        "16", "17", "18", "19", "20", "21", "22", "23", "24", "25",
        "26", "27", "28", "29", "30", "31"
    };
    ps.physics.hz       = 60;
    ps.physics.substeps = 4;
    ps.physics.gravity  = { 0.0f, -9.81f, 0.0f };
    return ps;
}

bool ProjectSettings::Load(const std::string& path)
{
    std::string text;
    if (!util::FileSystem::ReadText(path, text)) {
        *this = Default();
        return false;
    }

    auto result = toml::parse(text);
    if (!result) {
        FBZZ_LOG_WARN("ProjectSettings: parse failed: %s", path.c_str());
        *this = Default();
        return false;
    }
    auto& tbl = result.table();

    *this = Default();

    if (auto* tagArr = tbl["tags"]["list"].as_array()) {
        tags.clear();
        for (auto& elem : *tagArr)
            if (auto v = elem.value<std::string>())
                tags.push_back(*v);
    }

    if (auto* layArr = tbl["layers"]["names"].as_array()) {
        for (int i = 0; i < 32 && i < (int)layArr->size(); ++i)
            if (auto v = (*layArr)[i].value<std::string>())
                layerNames[i] = *v;
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

    if (auto* renderTbl = tbl["render"].as_table()) {
        render.wireframeMode = (*renderTbl)["wireframe"].value_or(render.wireframeMode);
        render.shadowEnabled = (*renderTbl)["shadow"].value_or(render.shadowEnabled);
        render.bloomEnabled  = (*renderTbl)["bloom"].value_or(render.bloomEnabled);
        render.fxaaEnabled   = (*renderTbl)["fxaa"].value_or(render.fxaaEnabled);
        render.fogEnabled    = (*renderTbl)["fog"].value_or(render.fogEnabled);
        render.showColliders = (*renderTbl)["showColliders"].value_or(render.showColliders);
        render.exposure      = (float)(*renderTbl)["exposure"].value_or((double)render.exposure);
        render.fogDensity    = (float)(*renderTbl)["fogDensity"].value_or((double)render.fogDensity);
        render.fogFar        = (float)(*renderTbl)["fogFar"].value_or((double)render.fogFar);
        if (auto* fogColorArr = (*renderTbl)["fogColor"].as_array(); fogColorArr && fogColorArr->size() >= 3) {
            render.fogColor[0] = (float)(*fogColorArr)[0].value_or((double)render.fogColor[0]);
            render.fogColor[1] = (float)(*fogColorArr)[1].value_or((double)render.fogColor[1]);
            render.fogColor[2] = (float)(*fogColorArr)[2].value_or((double)render.fogColor[2]);
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
    }

    return true;
}

bool ProjectSettings::Save(const std::string& path) const
{
    toml::array tagArr;
    for (const auto& t : tags)
        tagArr.push_back(t);

    toml::array layArr;
    for (const auto& n : layerNames)
        layArr.push_back(n);

    toml::table tagTbl;
    tagTbl.insert("list", std::move(tagArr));

    toml::table layTbl;
    layTbl.insert("names", std::move(layArr));

    toml::table physicsTbl;
    physicsTbl.insert("hz",       (int64_t)physics.hz);
    physicsTbl.insert("substeps", (int64_t)physics.substeps);
    physicsTbl.insert("gravity",  Vec3ToArr(physics.gravity));

    toml::array fogColorArr;
    fogColorArr.push_back((double)render.fogColor[0]);
    fogColorArr.push_back((double)render.fogColor[1]);
    fogColorArr.push_back((double)render.fogColor[2]);

    toml::table renderTbl;
    renderTbl.insert("wireframe",    render.wireframeMode);
    renderTbl.insert("shadow",       render.shadowEnabled);
    renderTbl.insert("bloom",        render.bloomEnabled);
    renderTbl.insert("fxaa",         render.fxaaEnabled);
    renderTbl.insert("fog",          render.fogEnabled);
    renderTbl.insert("showColliders",render.showColliders);
    renderTbl.insert("exposure",     (double)render.exposure);
    renderTbl.insert("fogDensity",   (double)render.fogDensity);
    renderTbl.insert("fogFar",       (double)render.fogFar);
    renderTbl.insert("fogColor",     std::move(fogColorArr));

    toml::table audioTbl;
    audioTbl.insert("bgmVolume", (double)audio.bgmVolume);
    audioTbl.insert("seVolume",  (double)audio.seVolume);

    toml::table screenTbl;
    screenTbl.insert("width",  (int64_t)screen.width);
    screenTbl.insert("height", (int64_t)screen.height);

    toml::table appTbl;
    appTbl.insert("targetFps", (int64_t)app.targetFps);

    toml::table root;
    root.insert("tags",    std::move(tagTbl));
    root.insert("layers",  std::move(layTbl));
    root.insert("physics", std::move(physicsTbl));
    root.insert("render",  std::move(renderTbl));
    root.insert("audio",   std::move(audioTbl));
    root.insert("screen",  std::move(screenTbl));
    root.insert("app",     std::move(appTbl));

    std::ostringstream ss;
    ss << root;
    return util::FileSystem::WriteText(path, ss.str());
}

} // namespace fbzz
