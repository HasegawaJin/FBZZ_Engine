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

    toml::table root;
    root.insert("tags",   std::move(tagTbl));
    root.insert("layers", std::move(layTbl));

    std::ostringstream ss;
    ss << root;
    return util::FileSystem::WriteText(path, ss.str());
}

} // namespace fbzz
