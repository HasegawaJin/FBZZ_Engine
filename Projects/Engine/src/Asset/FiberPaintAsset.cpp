/// @file    FiberPaintAsset.cpp
/// @brief   表面ペイントの検証付き TOML 読込と原子的保存。
/// @author  Hasegawa Jin
/// @date    2026-10-11
#include <Engine/Asset/FiberPaintAsset.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <toml++/toml.hpp>
#include <atomic>
#include <charconv>
#include <sstream>

namespace fbzz::asset {
namespace {
std::string Hex(uint64_t value)
{
    char buffer[16];
    const auto result = std::to_chars(buffer,buffer+sizeof(buffer),value,16);
    return {buffer,result.ptr};
}
bool ReadHex(const toml::node_view<const toml::node>& node, uint64_t& out)
{
    const auto text = node.value<std::string>();
    if (!text || text->empty() || text->size() > 16) return false;
    const auto result = std::from_chars(text->data(),text->data()+text->size(),out,16);
    return result.ec == std::errc{} && result.ptr == text->data()+text->size();
}
} /// @note namespace
void TouchFiberPaintAsset(FiberPaintAsset& asset)
{
    static std::atomic<uint64_t> next{1};
    asset.revision = next.fetch_add(1,std::memory_order_relaxed);
}
bool LoadFiberPaintAssetFromFile(std::string_view path, FiberPaintAsset& out)
{
    std::string text;
    const std::string filename(path);
    if (!util::FileSystem::ReadText(filename,text)) return false;
    const auto parsed = toml::parse(text,filename);
    if (!parsed) return false;
    const auto& table = parsed.table();
    if (table["version"].value<int64_t>() != 1) return false;
    FiberPaintAsset result;
    const auto cellSize = table["cell_size"].value<float>();
    const auto density = table["default_density"].value<float>();
    const auto length = table["default_length"].value<float>();
    const auto* cells = table["cells"].as_array();
    if (!cellSize || !density || !length || !cells || cells->size() > renderer::FIBER_MAX_PAINT_CELLS
        || !ReadHex(table["mesh_signature"],result.data.meshSignature)) return false;
    result.data.cellSize = *cellSize; result.data.defaults = {*density,*length};
    for (const auto& node : *cells) {
        const auto* record = node.as_table();
        if (!record) return false;
        uint64_t key = 0;
        const auto d = (*record)["density"].value<float>(), h = (*record)["length"].value<float>();
        if (!d || !h || !ReadHex((*record)["key"],key)
            || !result.data.cells.emplace(key,renderer::FiberPaintValue{*d,*h}).second) return false;
    }
    if (!renderer::ValidateFiberPaintData(result.data)) return false;
    TouchFiberPaintAsset(result);
    out = std::move(result);
    return true;
}
bool SaveFiberPaintAssetToFile(std::string_view path, const FiberPaintAsset& asset)
{
    if (!renderer::ValidateFiberPaintData(asset.data)) return false;
    toml::table table;
    table.insert("version",1);
    table.insert("mesh_signature",Hex(asset.data.meshSignature));
    table.insert("cell_size",asset.data.cellSize);
    table.insert("default_density",asset.data.defaults.density);
    table.insert("default_length",asset.data.defaults.length);
    toml::array cells;
    for (const auto& [key,value] : asset.data.cells)
        cells.push_back(toml::table{{"key",Hex(key)}, {"density",value.density}, {"length",value.length}});
    table.insert("cells",std::move(cells));
    std::ostringstream text;
    text << table;
    const std::string filename(path);
    return util::FileSystem::EnsureParentDirectory(util::FileSystem::PathFromUtf8(filename))
        && util::FileSystem::WriteTextAtomic(filename,text.str());
}
} /// @note namespace fbzz::asset
