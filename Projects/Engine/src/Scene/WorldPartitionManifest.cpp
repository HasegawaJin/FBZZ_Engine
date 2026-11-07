/// @file    WorldPartitionManifest.cpp
/// @brief   Bounded TOML manifests with canonical cell ordering and atomic persistence.
/// @author  Hasegawa Jin
/// @date    2026-11-07
#include <Engine/Scene/WorldPartitionManifest.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <toml++/toml.hpp>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <initializer_list>
#include <limits>
#include <set>
#include <sstream>
#include <utility>

namespace fbzz::scene {
namespace {
constexpr std::size_t MAX_CELLS = 65536;
constexpr std::size_t MAX_DEPENDENCIES_PER_CELL = 1024;
constexpr std::size_t MAX_TOTAL_DEPENDENCIES = 1048576;
constexpr std::size_t MAX_MANIFEST_BYTES = 64u * 1024u * 1024u;
constexpr std::size_t MAX_PATH_BYTES = 4096;
constexpr std::size_t MAX_SOURCE_IDS = 1048576;

bool Fail(std::string& diagnostic, std::string value)
{
    diagnostic = std::move(value);
    return false;
}

bool Finite(const math::Vector3& value)
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

std::string FoldAscii(std::string_view value)
{
    std::string folded(value);
    for (char& character : folded)
        if (character >= 'A' && character <= 'Z') character = static_cast<char>(character + ('a' - 'A'));
    return folded;
}

bool Utf8Text(std::string_view value)
{
    std::size_t offset = 0;
    while (offset < value.size()) {
        const auto first = static_cast<unsigned char>(value[offset++]);
        if (first < 0x20 || first == 0x7f) return false;
        if (first < 0x80) continue;
        uint32_t codepoint = 0;
        uint32_t minimum = 0;
        int remaining = 0;
        if (first >= 0xc2 && first <= 0xdf) { codepoint = first & 0x1fu; minimum = 0x80; remaining = 1; }
        else if (first >= 0xe0 && first <= 0xef) { codepoint = first & 0x0fu; minimum = 0x800; remaining = 2; }
        else if (first >= 0xf0 && first <= 0xf4) { codepoint = first & 0x07u; minimum = 0x10000; remaining = 3; }
        else return false;
        if (value.size() - offset < static_cast<std::size_t>(remaining)) return false;
        for (int index = 0; index < remaining; ++index) {
            const auto next = static_cast<unsigned char>(value[offset++]);
            if ((next & 0xc0u) != 0x80u) return false;
            codepoint = (codepoint << 6) | (next & 0x3fu);
        }
        if (codepoint < minimum || codepoint > 0x10ffffu || (codepoint >= 0xd800u && codepoint <= 0xdfffu)) return false;
    }
    return true;
}

bool ReservedFilename(std::string_view segment)
{
    const std::string stem = FoldAscii(segment.substr(0, segment.find('.')));
    if (stem == "con" || stem == "prn" || stem == "aux" || stem == "nul") return true;
    return stem.size() == 4 && stem[3] >= '1' && stem[3] <= '9'
        && (stem.substr(0, 3) == "com" || stem.substr(0, 3) == "lpt");
}

bool RelativeAssetPath(std::string_view value, bool payload)
{
    if (value.empty() || value.size() > MAX_PATH_BYTES || !Utf8Text(value)) return false;
    if (!value.starts_with("Assets/") && (payload || !value.starts_with("Library/Baked/"))) return false;
    if (value.find_first_of("\\:<>\"|?*") != std::string_view::npos) return false;
    std::size_t first = 0;
    while (first <= value.size()) {
        const std::size_t slash = value.find('/', first);
        const auto segment = value.substr(first, slash == std::string_view::npos ? value.size() - first : slash - first);
        if (segment.empty() || segment == "." || segment == ".." || segment.back() == '.' || segment.back() == ' '
            || ReservedFilename(segment)) return false;
        if (slash == std::string_view::npos) break;
        first = slash + 1;
    }
    return true;
}

bool DependencyReference(std::string_view value)
{
    if (!value.starts_with("guid:")) return RelativeAssetPath(value, false);
    const std::size_t separator = value.find('|');
    const std::string_view guid = value.substr(5, separator == std::string_view::npos ? value.size() - 5 : separator - 5);
    if (guid.size() != 32) return false;
    for (char digit : guid)
        if (!((digit >= '0' && digit <= '9') || (digit >= 'a' && digit <= 'f') || (digit >= 'A' && digit <= 'F'))) return false;
    return separator == std::string_view::npos || RelativeAssetPath(value.substr(separator + 1), false);
}

bool DependencyType(std::string_view value)
{
    return !value.empty() && value.size() <= 256 && Utf8Text(value);
}

std::string DependencyKey(std::string_view value)
{
    return FoldAscii(value.starts_with("guid:") ? value.substr(0, value.find('|')) : value);
}

bool Keys(const toml::table& table, std::initializer_list<std::string_view> allowed)
{
    for (const auto& [key, value] : table) {
        static_cast<void>(value);
        if (std::find(allowed.begin(), allowed.end(), key.str()) == allowed.end()) return false;
    }
    return true;
}

bool ReadVector(const toml::node* node, math::Vector3& out)
{
    const auto* values = node ? node->as_array() : nullptr;
    if (!values || values->size() != 3) return false;
    const auto x = (*values)[0].value<float>(), y = (*values)[1].value<float>(), z = (*values)[2].value<float>();
    if (!x || !y || !z) return false;
    out = {*x, *y, *z};
    return Finite(out);
}

toml::array WriteVector(const math::Vector3& value) { return toml::array{value.x, value.y, value.z}; }

bool ReadRevision(const toml::node* node, uint64_t& out)
{
    if (!node) return false;
    if (const auto text = node->value<std::string>()) {
        if (text->empty() || (*text)[0] < '0' || (*text)[0] > '9') return false;
        const auto result = std::from_chars(text->data(), text->data() + text->size(), out);
        return result.ec == std::errc{} && result.ptr == text->data() + text->size() && out != 0;
    }
    if (!node->is_integer()) return false;
    const auto number = node->value<int64_t>();
    if (!number || *number <= 0) return false;
    out = static_cast<uint64_t>(*number);
    return true;
}

bool ReadCellId(const toml::node* node, WorldCellId& out)
{
    const auto* values = node ? node->as_array() : nullptr;
    if (!values || values->size() != 2 || !(*values)[0].is_integer() || !(*values)[1].is_integer()) return false;
    const auto x = (*values)[0].value<int64_t>(), z = (*values)[1].value<int64_t>();
    if (!x || !z || *x < (std::numeric_limits<int32_t>::min)() || *x > (std::numeric_limits<int32_t>::max)()
        || *z < (std::numeric_limits<int32_t>::min)() || *z > (std::numeric_limits<int32_t>::max)()) return false;
    out = {static_cast<int32_t>(*x), static_cast<int32_t>(*z)};
    return true;
}

} /// @note namespace

bool ValidateWorldPartitionManifest(const WorldPartitionManifest& manifest, std::string& diagnostic)
{
    if (!ValidateWorldCellGrid(manifest.grid, diagnostic)) return false;
    if (manifest.revision == 0 || manifest.cells.size() > MAX_CELLS)
        return Fail(diagnostic, "Manifest revision must be positive and cell count must not exceed 65536.");
    if (manifest.sourceInstanceIds.size() > MAX_SOURCE_IDS) return Fail(diagnostic, "Source identity count exceeds 1048576.");
    std::set<std::string> sourceIds;
    for (const auto& id : manifest.sourceInstanceIds) {
        if (id.empty() || id.size() > 128 || !Utf8Text(id)) return Fail(diagnostic, "Source instance identity is empty, invalid, or too long.");
        if (!sourceIds.insert(id).second) return Fail(diagnostic, "Duplicate source instance identity.");
    }
    std::set<WorldCellId> ids;
    std::set<std::string> payloads;
    std::size_t totalDependencies = 0;
    for (const auto& cell : manifest.cells) {
        const std::string label = "Cell " + WorldCellName(cell.id) + ": ";
        if (!ids.insert(cell.id).second) return Fail(diagnostic, label + "duplicate coordinates.");
        const auto footprint = WorldCellFootprint(manifest.grid, cell.id);
        if (!Finite(footprint.min) || !Finite(footprint.max) || footprint.min.x >= footprint.max.x || footprint.min.z >= footprint.max.z)
            return Fail(diagnostic, label + "coordinates exceed the representable world grid.");
        if (!Finite(cell.bounds.min) || !Finite(cell.bounds.max) || cell.bounds.min.x >= cell.bounds.max.x
            || cell.bounds.min.y > cell.bounds.max.y || cell.bounds.min.z >= cell.bounds.max.z
            || !Finite(cell.bounds.max - cell.bounds.min))
            return Fail(diagnostic, label + "bounds must be finite and ordered, with positive XZ extent.");
        if (cell.revision == 0) return Fail(diagnostic, label + "revision must be positive.");
        if (!RelativeAssetPath(cell.nearPath, true) || (!cell.hlodPath.empty() && !RelativeAssetPath(cell.hlodPath, true)))
            return Fail(diagnostic, label + "payloads must be normalized project-relative Assets/... paths without traversal.");
        if (!payloads.insert(FoldAscii(cell.nearPath)).second
            || (!cell.hlodPath.empty() && !payloads.insert(FoldAscii(cell.hlodPath)).second))
            return Fail(diagnostic, label + "a payload path is assigned to multiple representations.");
        if (cell.dependencies.size() > MAX_DEPENDENCIES_PER_CELL)
            return Fail(diagnostic, label + "dependency count exceeds 1024.");
        totalDependencies += cell.dependencies.size();
        if (totalDependencies > MAX_TOTAL_DEPENDENCIES) return Fail(diagnostic, "Manifest dependency count exceeds 1048576.");
        std::set<std::pair<std::string, std::string>> dependencies;
        for (const auto& dependency : cell.dependencies) {
            if (!DependencyType(dependency.typeName) || !DependencyReference(dependency.reference))
                return Fail(diagnostic, label + "dependency type or asset reference is invalid.");
            if (!dependencies.emplace(dependency.typeName, DependencyKey(dependency.reference)).second)
                return Fail(diagnostic, label + "duplicate dependency.");
        }
    }
    diagnostic.clear();
    return true;
}

bool SerializeWorldPartitionManifest(const WorldPartitionManifest& manifest, std::string& out, std::string& diagnostic)
{
    if (!ValidateWorldPartitionManifest(manifest, diagnostic)) return false;
    std::vector<const WorldPartitionCell*> ordered;
    ordered.reserve(manifest.cells.size());
    for (const auto& cell : manifest.cells) ordered.push_back(&cell);
    std::sort(ordered.begin(), ordered.end(), [](const auto* first, const auto* second) { return first->id < second->id; });
    toml::array cells;
    for (const auto* cell : ordered) {
        std::vector<WorldPartitionDependency> dependencies = cell->dependencies;
        std::sort(dependencies.begin(), dependencies.end(), [](const auto& first, const auto& second) {
            return first.typeName == second.typeName ? first.reference < second.reference : first.typeName < second.typeName;
        });
        toml::array dependencyTables;
        for (const auto& dependency : dependencies)
            dependencyTables.push_back(toml::table{{"typeName", dependency.typeName}, {"reference", dependency.reference}});
        cells.push_back(toml::table{{"id", toml::array{cell->id.x, cell->id.z}}, {"boundsMin", WriteVector(cell->bounds.min)},
            {"boundsMax", WriteVector(cell->bounds.max)}, {"revision", std::to_string(cell->revision)},
            {"nearPath", cell->nearPath}, {"hlodPath", cell->hlodPath}, {"dependencies", std::move(dependencyTables)}});
    }
    toml::table table{{"format", "fbzz.worldpartition"}, {"version", 1}, {"revision", std::to_string(manifest.revision)},
        {"grid", toml::table{{"origin", WriteVector(manifest.grid.origin)}, {"cellSize", manifest.grid.cellSize}}},
        {"cells", std::move(cells)}};
    if (!manifest.sourceInstanceIds.empty()) {
        auto orderedSources = manifest.sourceInstanceIds;
        std::sort(orderedSources.begin(), orderedSources.end());
        toml::array sources;
        for (const auto& id : orderedSources) sources.push_back(id);
        table.insert("sourceInstanceIds", std::move(sources));
    }
    std::ostringstream text;
    text << table;
    std::string candidate = text.str();
    if (candidate.size() > MAX_MANIFEST_BYTES) return Fail(diagnostic, "Serialized manifest exceeds 64 MiB.");
    out = std::move(candidate);
    diagnostic.clear();
    return true;
}

bool DeserializeWorldPartitionManifest(std::string_view text, WorldPartitionManifest& out, std::string& diagnostic)
{
    if (text.empty() || text.size() > MAX_MANIFEST_BYTES) return Fail(diagnostic, "Manifest is empty or exceeds 64 MiB.");
    const auto parsed = toml::parse(text);
    if (!parsed) return Fail(diagnostic, "Manifest TOML is malformed.");
    const auto& table = parsed.table();
    if (table["format"].value<std::string>() != "fbzz.worldpartition" || table["version"].value<int64_t>() != 1)
        return Fail(diagnostic, "Unsupported world partition format or version.");
    if (!Keys(table, {"format", "version", "revision", "grid", "cells", "sourceInstanceIds"})) return Fail(diagnostic, "Unknown manifest field.");
    const auto* grid = table["grid"].as_table();
    const auto* cells = table["cells"].as_array();
    WorldPartitionManifest candidate;
    if (!ReadRevision(table.get("revision"), candidate.revision) || !grid || !Keys(*grid, {"origin", "cellSize"})
        || !ReadVector(grid->get("origin"), candidate.grid.origin) || !cells || cells->size() > MAX_CELLS)
        return Fail(diagnostic, "Invalid manifest revision, grid, or cell array.");
    const auto cellSize = (*grid)["cellSize"].value<float>();
    if (!cellSize) return Fail(diagnostic, "Grid cellSize is missing or invalid.");
    candidate.grid.cellSize = *cellSize;
    if (const auto* sourceNode = table.get("sourceInstanceIds")) {
        const auto* sources = sourceNode->as_array();
        if (!sources || sources->size() > MAX_SOURCE_IDS) return Fail(diagnostic, "Invalid source instance identity array.");
        candidate.sourceInstanceIds.reserve(sources->size());
        for (const auto& item : *sources) {
            const auto id = item.value<std::string>();
            if (!id) return Fail(diagnostic, "Source instance identity must be a string.");
            candidate.sourceInstanceIds.push_back(*id);
        }
    }
    candidate.cells.reserve(cells->size());
    std::size_t totalDependencies = 0;
    for (const auto& entry : *cells) {
        const auto* row = entry.as_table();
        WorldPartitionCell cell;
        if (!row || !Keys(*row, {"id", "boundsMin", "boundsMax", "revision", "nearPath", "hlodPath", "dependencies"})
            || !ReadCellId(row->get("id"), cell.id) || !ReadVector(row->get("boundsMin"), cell.bounds.min)
            || !ReadVector(row->get("boundsMax"), cell.bounds.max) || !ReadRevision(row->get("revision"), cell.revision))
            return Fail(diagnostic, "Invalid cell coordinates, bounds or revision.");
        const auto nearPath = (*row)["nearPath"].value<std::string>(), hlodPath = (*row)["hlodPath"].value<std::string>();
        const auto* dependencies = (*row)["dependencies"].as_array();
        if (!nearPath || !hlodPath || !dependencies || dependencies->size() > MAX_DEPENDENCIES_PER_CELL)
            return Fail(diagnostic, "Cell payload paths or dependency array are missing or invalid.");
        totalDependencies += dependencies->size();
        if (totalDependencies > MAX_TOTAL_DEPENDENCIES) return Fail(diagnostic, "Manifest dependency count exceeds 1048576.");
        cell.nearPath = *nearPath;
        cell.hlodPath = *hlodPath;
        cell.dependencies.reserve(dependencies->size());
        for (const auto& dependencyEntry : *dependencies) {
            const auto* dependencyRow = dependencyEntry.as_table();
            if (!dependencyRow || !Keys(*dependencyRow, {"typeName", "reference"})) return Fail(diagnostic, "Invalid dependency row.");
            const auto typeName = (*dependencyRow)["typeName"].value<std::string>(), reference = (*dependencyRow)["reference"].value<std::string>();
            if (!typeName || !reference) return Fail(diagnostic, "Dependency type or reference is missing.");
            cell.dependencies.push_back({*typeName, *reference});
        }
        candidate.cells.push_back(std::move(cell));
    }
    if (!ValidateWorldPartitionManifest(candidate, diagnostic)) return false;
    out = std::move(candidate);
    diagnostic.clear();
    return true;
}

bool LoadWorldPartitionManifest(std::string_view path, WorldPartitionManifest& out, std::string& diagnostic)
{
    if (path.empty() || path.size() > MAX_PATH_BYTES || !Utf8Text(path)) return Fail(diagnostic, "Manifest file path is invalid.");
    const std::string filename(path);
    std::error_code error;
    const auto size = std::filesystem::file_size(util::FileSystem::PathFromUtf8(filename), error);
    if (error || size == 0 || size > MAX_MANIFEST_BYTES) return Fail(diagnostic, "Manifest file is missing, empty, or exceeds 64 MiB.");
    std::string text;
    if (!util::FileSystem::ReadText(filename, text)) return Fail(diagnostic, "Manifest file could not be read.");
    return DeserializeWorldPartitionManifest(text, out, diagnostic);
}

bool SaveWorldPartitionManifest(std::string_view path, const WorldPartitionManifest& manifest, std::string& diagnostic)
{
    if (path.empty() || path.size() > MAX_PATH_BYTES || !Utf8Text(path)) return Fail(diagnostic, "Manifest file path is invalid.");
    std::string text;
    if (!SerializeWorldPartitionManifest(manifest, text, diagnostic)) return false;
    const std::string filename(path);
    if (!util::FileSystem::EnsureParentDirectory(util::FileSystem::PathFromUtf8(filename))
        || !util::FileSystem::WriteTextAtomic(filename, text)) return Fail(diagnostic, "Manifest could not be saved atomically.");
    diagnostic.clear();
    return true;
}

} /// @note namespace fbzz::scene
