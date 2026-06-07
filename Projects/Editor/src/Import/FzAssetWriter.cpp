// FBZZ Engine
// FzAssetWriter.cpp | fbzz::editor
// .fzasset マニフェスト (TOML) 書き出し
#include <Editor/Import/FzAssetWriter.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <toml++/toml.hpp>
#include <sstream>

namespace fbzz::editor {

bool FzAssetWriter::Write(const FzAssetManifest& manifest,
                           const std::string& outputPath)
{
    toml::table tbl;
    tbl.insert("version",    1);
    tbl.insert("unit_scale", static_cast<double>(manifest.unitScale));
    if (!manifest.sourceHint.empty())
        tbl.insert("source_hint", manifest.sourceHint);

    // meshes + materials 配列は同じ長さであることが前提
    {
        toml::array meshArr, matArr;
        for (const auto& p : manifest.meshPaths)     meshArr.push_back(p);
        for (const auto& p : manifest.materialPaths) matArr.push_back(p);
        tbl.insert("meshes",    std::move(meshArr));
        tbl.insert("materials", std::move(matArr));
    }

    if (!manifest.skeletonPath.empty())
        tbl.insert("skeleton", manifest.skeletonPath);

    if (!manifest.animPaths.empty()) {
        toml::array animArr;
        for (const auto& p : manifest.animPaths) animArr.push_back(p);
        tbl.insert("animations", std::move(animArr));
    }

    std::ostringstream out;
    out << tbl << '\n';
    if (!util::FileSystem::WriteText(util::FileSystem::PathFromUtf8(outputPath), out.str())) {
        FBZZ_LOG_ERROR("FzAssetWriter: cannot open [%s]", outputPath.c_str());
        return false;
    }
    return true;
}

} // namespace fbzz::editor
