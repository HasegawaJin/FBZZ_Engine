/// @file    AssetReferenceScan.cpp
/// @brief   TOML の完全な文字列値から参照根拠を段階的に収集する。
/// @author  Hasegawa Jin
/// @date    2026-10-24
#include "AssetReferenceScan.hpp"
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/Texture/TexDescSerializer.hpp>
#include <Engine/Util/Files/FileSystem.hpp>
#include <toml++/toml.hpp>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <string_view>
#include <system_error>
#include <utility>

namespace fbzz::editor::assetrefs {
namespace {

constexpr size_t MAX_FILE_BYTES = 2 * 1024 * 1024;
constexpr size_t MAX_TOTAL_BYTES = 64 * 1024 * 1024;
constexpr size_t MAX_CANDIDATES = 8192;
constexpr size_t MAX_EVIDENCE = 4096;
constexpr size_t MAX_NODES = 65536;

std::string Lower(std::string value)
{
    for (char& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
}

struct Parts { std::string base; std::string suffix; };

/// @note GuidRefCodec と同じ sprite / 数字 submesh 接尾辞だけを認める。
Parts Split(std::string value)
{
    if (const size_t pos = value.find("::sprite::"); pos != std::string::npos)
        return {value.substr(0, pos), value.substr(pos)};
    const size_t pos = value.find_last_of(':');
    const size_t slash = value.find_last_of("/\\");
    if (pos != std::string::npos && pos + 1 < value.size()
        && (slash == std::string::npos || pos > slash)
        && std::all_of(value.begin() + pos + 1, value.end(),
            [](unsigned char c) { return std::isdigit(c) != 0; }))
        return {value.substr(0, pos), value.substr(pos)};
    return {std::move(value), {}};
}

std::string PathKey(std::string value, const std::string& root)
{
    std::replace(value.begin(), value.end(), '\\', '/');
    auto path = util::FileSystem::PathFromUtf8(value);
    if (!path.is_absolute()) path = util::FileSystem::PathFromUtf8(root) / path;
    return Lower(util::FileSystem::PathToUtf8(path.lexically_normal()));
}

bool IsImage(std::string_view path)
{
    const std::string ext = Lower(util::FileSystem::GetExtension(std::string(path)));
    return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga"
        || ext == ".dds" || ext == ".bmp" || ext == ".hdr" || ext == ".exr"
        || ext == ".ico";
}

/// @note TexDescSerializer が定義する画像 .meta の別名だけを元画像へ揃える。
std::string SourceBase(std::string base)
{
    if (Lower(util::FileSystem::GetExtension(base)) != ".meta") return base;
    std::string source;
    if (asset::TexDescSerializer::ResolveSourcePath(base, source) && IsImage(source)) return source;
    return base;
}

const char* Kind(std::string_view extension)
{
    if (extension == ".scene") return "Scene";
    if (extension == ".mat") return "Material";
    if (extension == ".prefab") return "Prefab";
    if (extension == ".animcontroller") return "Animation controller";
    if (extension == ".vfx") return "VFX";
    if (extension == ".terrain") return "Terrain";
    if (extension == ".fzdata") return "Data asset";
    if (extension == ".terrainpaint") return "Terrain paint";
    if (extension == ".meshinstances") return "Mesh instances";
    return nullptr;
}

enum class ReadResult { OK, UNREADABLE, OVERSIZED };

ReadResult ReadBounded(const std::string& path, size_t limit, std::string& text)
{
    const auto native = util::FileSystem::PathFromUtf8(path);
    std::error_code error;
    const auto size = std::filesystem::file_size(native, error);
    if (error) return ReadResult::UNREADABLE;
    if (size > limit) return ReadResult::OVERSIZED;
    const auto before = std::filesystem::last_write_time(native, error);
    if (error) return ReadResult::UNREADABLE;
    std::ifstream input(native, std::ios::binary);
    if (!input) return ReadResult::UNREADABLE;
    text.resize(static_cast<size_t>(size));
    input.read(text.data(), static_cast<std::streamsize>(size));
    if (input.gcount() != static_cast<std::streamsize>(size)
        || input.peek() != std::char_traits<char>::eof()) return ReadResult::UNREADABLE;
    const auto after = std::filesystem::last_write_time(native, error);
    return !error && before == after ? ReadResult::OK : ReadResult::UNREADABLE;
}

ImportMetadata ReadMetadata(const std::string& base)
{
    ImportMetadata metadata;
    const std::string path = base + ".meta";
    std::error_code error;
    if (!std::filesystem::exists(util::FileSystem::PathFromUtf8(path), error) && !error) return metadata;
    std::string text;
    if (ReadBounded(path, 64 * 1024, text) != ReadResult::OK) {
        metadata.unreadable = true;
        return metadata;
    }
    const auto parsed = toml::parse(text);
    if (!parsed) { metadata.unreadable = true; return metadata; }
    metadata.sourceDcc = parsed.table()["model"]["source_dcc"].value_or(std::string{});
    metadata.importer = parsed.table()["model"]["importer"].value_or(std::string{});
    metadata.importerVersion = parsed.table()["model"]["importer_version"].value_or(int64_t{-1});
    return metadata;
}

bool ValidSuffix(std::string_view suffix)
{
    if (suffix.empty()) return true;
    if (suffix.starts_with("::sprite::")) return suffix.size() > std::string_view("::sprite::").size();
    return suffix.size() > 1 && suffix.front() == ':'
        && std::all_of(suffix.begin() + 1, suffix.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
}

struct GuidReference {
    std::string guid;
    std::string suffix;
    std::string hint;
};

std::optional<GuidReference> ParseGuidReference(std::string_view value)
{
    const auto prefix = asset::AssetDatabase::kGuidPrefix;
    if (!value.starts_with(prefix)) return std::nullopt;
    const size_t separator = value.find('|');
    const auto authority = value.substr(0, separator);
    constexpr size_t AUTHORITY_LENGTH = 5 + 32;
    if (authority.size() < AUTHORITY_LENGTH) return std::nullopt;
    const auto guid = NormalizeGraphGuid(authority.substr(prefix.size(), 32));
    const auto suffix = authority.substr(AUTHORITY_LENGTH);
    if (guid.empty() || !ValidSuffix(suffix)) return std::nullopt;
    return GuidReference{guid, std::string(suffix), separator == value.npos ? "" : std::string(value.substr(separator + 1))};
}

std::optional<toml::table> ReadCandidate(const std::string& path, ScanCoverage& coverage)
{
    std::string text;
    const auto read = ReadBounded(path, (std::min)(MAX_FILE_BYTES, MAX_TOTAL_BYTES - coverage.bytesRead), text);
    if (read == ReadResult::OVERSIZED) {
        ++coverage.oversized;
        coverage.limited = true;
        return std::nullopt;
    }
    if (read != ReadResult::OK) {
        ++coverage.unreadable;
        return std::nullopt;
    }
    coverage.bytesRead += text.size();
    auto parsed = toml::parse(text);
    if (!parsed) {
        ++coverage.malformed;
        return std::nullopt;
    }
    ++coverage.parsed;
    return std::move(parsed).table();
}

using StringVisitor = std::function<bool(const std::string& value, const std::string& field, uint32_t line)>;

/// @return 走査上限を超えず、呼び出し側が全ての値を受理した場合に true。
bool VisitStrings(const toml::table& root, const StringVisitor& accept)
{
    size_t nodes = 0;
    bool depthLimited = false;
    const auto visit = [&](auto&& self, const toml::node& node, const std::string& field, size_t depth) -> bool {
        if (++nodes > MAX_NODES) return false;
        if (depth > 64) {
            depthLimited = true;
            return true;
        }
        if (const auto* value = node.as_string())
            return accept(value->get(), field, static_cast<uint32_t>(node.source().begin.line));
        if (const auto* table = node.as_table()) {
            for (const auto& [key, child] : *table) {
                const auto childField = field.empty() ? std::string(key.str()) : field + "." + std::string(key.str());
                if (!self(self, child, childField.substr(0, 512), depth + 1)) return false;
            }
        } else if (const auto* array = node.as_array()) {
            for (size_t index = 0; index < array->size(); ++index)
                if (!self(self, (*array)[index], field + "[" + std::to_string(index) + "]", depth + 1)) return false;
        }
        return true;
    };
    return visit(visit, root, {}, 0) && !depthLimited;
}

} /// @note namespace

std::string NormalizeGraphGuid(std::string_view input)
{
    if (input.starts_with(asset::AssetDatabase::kGuidPrefix))
        input.remove_prefix(asset::AssetDatabase::kGuidPrefix.size());
    if (input.size() != 32 || !std::all_of(input.begin(), input.end(), [](unsigned char digit) {
            return std::isxdigit(digit) != 0;
        })) return {};
    return Lower(std::string(input));
}

void Scan::Begin(Request request)
{
    const uint64_t generation = m_report.generation + 1;
    m_report = {};
    m_report.generation = generation;
    m_report.projectRoot = std::move(request.projectRoot);
    m_report.targetPath = std::move(request.targetPath);
    m_report.searchRevision = request.searchRevision;
    m_report.databaseRevision = request.databaseRevision;
    m_candidates.clear();
    m_evidenceCount = 0;
    const Parts target = Split(m_report.targetPath);
    const std::string source = SourceBase(target.base);
    m_targetBase = PathKey(source, m_report.projectRoot);
    m_targetSuffix = target.suffix;
    m_report.metadata = ReadMetadata(source);
    m_targetGuid = m_report.metadata.unreadable ? std::string{}
        : Lower(asset::AssetDatabase::TryGetGuidFromPath(source));
    const std::string assetsKey = PathKey("Assets", m_report.projectRoot) + "/";
    for (auto& entry : request.entries) {
        const std::string key = PathKey(entry.absolutePath, m_report.projectRoot);
        if (!key.starts_with(assetsKey) || key == m_targetBase) continue;
        if (!Kind(entry.extension)) { ++m_report.unsupported; continue; }
        if (m_candidates.size() == MAX_CANDIDATES) { m_report.limited = true; continue; }
        m_candidates.push_back(std::move(entry));
    }
    std::sort(m_candidates.begin(), m_candidates.end(), [](const auto& left, const auto& right) {
        return left.absolutePath < right.absolutePath;
    });
    m_report.candidates = m_candidates.size();
    m_report.finished = m_candidates.empty();
}

bool Scan::MatchesSnapshot(const std::string& projectRoot,
                            uint64_t searchRevision, uint64_t databaseRevision) const
{
    return util::FileSystem::SamePathText(projectRoot, m_report.projectRoot)
        && m_report.searchRevision == searchRevision && m_report.databaseRevision == databaseRevision;
}

void Scan::Advance(size_t maxFiles)
{
    if (m_report.finished) return;
    for (size_t count = 0; count < maxFiles && m_report.processed < m_candidates.size(); ++count) {
        if (m_report.bytesRead >= MAX_TOTAL_BYTES || m_evidenceCount >= MAX_EVIDENCE) {
            m_report.limited = true;
            m_report.finished = true;
            return;
        }
        const auto& entry = m_candidates[m_report.processed++];
        const auto parsed = ReadCandidate(entry.absolutePath, m_report);
        if (!parsed) continue;
        Owner owner{entry.absolutePath, Kind(entry.extension), {}, {}};
        const auto match = [&](const std::string& value) -> const char* {
            if (value.starts_with(asset::AssetDatabase::kGuidPrefix)) {
                const auto reference = ParseGuidReference(value);
                if (!reference || (!m_targetSuffix.empty() && m_targetSuffix != reference->suffix)) return nullptr;
                if (!m_targetGuid.empty() && reference->guid == m_targetGuid) return "GUID value";
                /// @note 別 GUID の古いパスヒントを、現在の参照先だと誤認させない。
                if (!asset::AssetDatabase::PathFromGuid(reference->guid).empty() || reference->hint.empty())
                    return nullptr;
                const Parts hint = Split(reference->hint);
                if (PathKey(SourceBase(hint.base), m_report.projectRoot) == m_targetBase)
                    return "Unresolved GUID hint (unverified)";
                return nullptr;
            }
            const Parts path = Split(value);
            const std::string normalized = util::FileSystem::NormalizePathSeparators(path.base);
            const std::string lower = Lower(normalized);
            if (!lower.starts_with("assets/") && !lower.starts_with("library/")
                && !util::FileSystem::PathFromUtf8(normalized).is_absolute()) return nullptr;
            if (!m_targetSuffix.empty() && m_targetSuffix != path.suffix) return nullptr;
            return PathKey(SourceBase(normalized), m_report.projectRoot) == m_targetBase ? "Path value" : nullptr;
        };
        if (!VisitStrings(*parsed, [&](const auto& value, const auto& field, uint32_t line) {
            if (m_evidenceCount >= MAX_EVIDENCE) return false;
            if (const char* kind = match(value)) {
                owner.evidence.push_back({field, kind, line});
                ++m_evidenceCount;
                if (std::string_view(kind).starts_with("Unresolved")) ++m_report.unresolvedHints;
            }
            return true;
        })) m_report.limited = true;
        if (!owner.evidence.empty()) {
            owner.metadata = ReadMetadata(entry.absolutePath);
            m_report.owners.push_back(std::move(owner));
        }
    }
    m_report.finished = m_report.processed == m_candidates.size();
}

namespace {
constexpr size_t MAX_REFERENCE_BYTES = 1024;

std::string CanonicalPath(const std::string& path)
{
    if (path.empty()) return {};
    std::error_code error;
    const auto canonical = std::filesystem::weakly_canonical(util::FileSystem::PathFromUtf8(path), error);
    return error ? "" : util::FileSystem::PathToUtf8(canonical);
}

std::string DisplayPath(const std::string& path, const std::string& project)
{
    const auto relative = util::FileSystem::PathFromUtf8(path).lexically_relative(util::FileSystem::PathFromUtf8(project));
    if (!relative.empty() && !relative.is_absolute() && *relative.begin() != "..")
        return util::FileSystem::PathToUtf8(relative);
    return path;
}

const char* GraphKind(const std::string& path)
{
    if (path.empty()) return "Unknown";
    if (const auto* kind = Kind(Lower(util::FileSystem::GetExtension(path)))) return kind;
    return "Asset";
}

GraphNodeState FileState(const std::string& path)
{
    if (path.empty()) return GraphNodeState::MISSING_PATH;
    std::error_code error;
    const auto status = std::filesystem::status(util::FileSystem::PathFromUtf8(path), error);
    if (status.type() == std::filesystem::file_type::not_found) return GraphNodeState::MISSING_PATH;
    if (!error && (std::filesystem::is_regular_file(status) || std::filesystem::is_directory(status)))
        return GraphNodeState::RESOLVED;
    return GraphNodeState::UNVERIFIED;
}

std::string StoredGuid(const std::string& path)
{
    std::string text;
    if (ReadBounded(path + ".meta", 64 * 1024, text) != ReadResult::OK) return {};
    const auto parsed = toml::parse(text);
    return parsed ? NormalizeGraphGuid(parsed.table()["meta"]["guid"].value_or(std::string{})) : "";
}

GraphDirection TraceGraph(const GraphReport& report, bool incoming)
{
    std::vector<std::vector<size_t>> adjacent(report.nodes.size());
    for (size_t index = 0; index < report.edges.size(); ++index) {
        const auto& edge = report.edges[index];
        adjacent[incoming ? edge.target : edge.source].push_back(index);
    }
    GraphDirection result;
    std::vector<bool> visited(report.nodes.size(), false);
    std::vector<std::pair<size_t, size_t>> queue{{report.root, 0}};
    visited[report.root] = true;
    for (size_t cursor = 0; cursor < queue.size(); ++cursor) {
        const auto [node, distance] = queue[cursor];
        if (distance == report.maxDepth) {
            result.depthLimited = result.depthLimited || !adjacent[node].empty();
            continue;
        }
        for (size_t index : adjacent[node]) {
            const auto& edge = report.edges[index];
            const auto targetState = report.nodes[edge.target].state;
            const auto next = incoming ? edge.source : edge.target;
            result.hops.push_back({index, distance + 1});
            if (targetState == GraphNodeState::MISSING_GUID || targetState == GraphNodeState::MISSING_PATH) ++result.brokenCount;
            if (targetState == GraphNodeState::UNVERIFIED) ++result.unverifiedCount;
            if (visited[next]) continue;
            visited[next] = true;
            queue.emplace_back(next, distance + 1);
        }
    }
    result.relatedCount = queue.size() - 1;
    return result;
}
} /// @note namespace

void GraphScan::Begin(GraphRequest request, size_t maxDepth)
{
    m_report = {};
    m_report.projectRoot = std::move(request.projectRoot);
    m_report.searchRevision = request.searchRevision;
    m_report.databaseRevision = request.databaseRevision;
    m_report.maxDepth = maxDepth;
    m_projectDirectory = CanonicalPath(m_report.projectRoot);
    m_assetsDirectory = PathKey("Assets", m_projectDirectory) + "/";
    m_bakedDirectory = PathKey("Library/Baked", m_projectDirectory) + "/";
    m_candidates.clear();
    m_guidNodes.clear();
    m_pathNodes.clear();
    m_report.root = FindGuidNode(request.guid);
    for (auto& entry : request.entries) {
        if (!PathKey(entry.absolutePath, m_projectDirectory).starts_with(m_assetsDirectory)) continue;
        if (!Kind(entry.extension)) { ++m_report.unsupported; continue; }
        if (m_candidates.size() == MAX_CANDIDATES) { m_report.limited = true; break; }
        m_candidates.push_back(std::move(entry));
    }
    std::sort(m_candidates.begin(), m_candidates.end(), [](const auto& left, const auto& right) {
        return left.absolutePath < right.absolutePath;
    });
    m_report.candidates = m_candidates.size();
    if (m_candidates.empty()) Complete();
}

size_t GraphScan::AddNode(GraphNode node)
{
    const auto index = m_report.nodes.size();
    if (!node.guid.empty()) m_guidNodes.emplace(node.guid, index);
    if (!node.path.empty()) m_pathNodes.emplace(PathKey(node.path, m_projectDirectory), index);
    m_report.nodes.push_back(std::move(node));
    return index;
}

size_t GraphScan::FindGuidNode(const std::string& guid)
{
    if (const auto found = m_guidNodes.find(guid); found != m_guidNodes.end()) return found->second;
    const auto indexedPath = asset::AssetDatabase::PathFromGuid(guid);
    const auto state = FileState(indexedPath);
    if (state == GraphNodeState::MISSING_PATH) return AddNode({guid, {}, "Unknown", GraphNodeState::MISSING_GUID});
    const auto path = CanonicalPath(indexedPath);
    if (path.empty()) return AddNode({guid, {}, "Unknown", GraphNodeState::UNVERIFIED});
    const auto key = PathKey(path, m_projectDirectory);
    if (const auto found = m_pathNodes.find(key); found != m_pathNodes.end() && m_report.nodes[found->second].guid.empty()) {
        m_report.nodes[found->second].guid = guid;
        m_guidNodes.emplace(guid, found->second);
        return found->second;
    }
    return AddNode({guid, DisplayPath(path, m_projectDirectory), GraphKind(path), state});
}

size_t GraphScan::FindPathNode(const std::string& path)
{
    const auto key = PathKey(path, m_projectDirectory);
    if (const auto found = m_pathNodes.find(key); found != m_pathNodes.end()) return found->second;
    auto resolved = CanonicalPath(path);
    const auto resolvedKey = PathKey(resolved, m_projectDirectory);
    if (resolved.empty() || (!resolvedKey.starts_with(m_assetsDirectory) && !resolvedKey.starts_with(m_bakedDirectory)))
        return AddNode({{}, DisplayPath(path, m_projectDirectory), GraphKind(path), GraphNodeState::UNVERIFIED});

    auto state = FileState(resolved);
    if (state == GraphNodeState::MISSING_PATH && key.starts_with(m_assetsDirectory)) {
        /// @note 旧形式のパスには既存の移動別名と共有 SDK アセットの解決規則が適用される。
        const auto legacy = CanonicalPath(asset::AssetManager::ResolveAssetPath(DisplayPath(path, m_projectDirectory)));
        if (FileState(legacy) == GraphNodeState::RESOLVED) {
            resolved = legacy;
            state = GraphNodeState::RESOLVED;
        }
    }
    const auto canonicalKey = PathKey(resolved, m_projectDirectory);
    size_t index = m_report.nodes.size();
    if (const auto found = m_pathNodes.find(canonicalKey); found != m_pathNodes.end()) {
        index = found->second;
    } else if (state == GraphNodeState::RESOLVED) {
        /// @note 保存済み meta を読むだけ。索引に別アセットが登録された GUID は owner に割り当てない。
        const auto guid = StoredGuid(resolved);
        if (!guid.empty() && util::FileSystem::SamePathText(CanonicalPath(asset::AssetDatabase::PathFromGuid(guid)), resolved))
            index = FindGuidNode(guid);
    }
    if (index == m_report.nodes.size())
        index = AddNode({{}, DisplayPath(resolved, m_projectDirectory), GraphKind(resolved), state});
    m_pathNodes.emplace(key, index);
    return index;
}

void GraphScan::Advance(size_t maxFiles)
{
    if (m_report.finished) return;
    for (size_t count = 0; count < maxFiles && m_report.processed < m_candidates.size(); ++count) {
        if (m_report.bytesRead >= MAX_TOTAL_BYTES || m_report.edges.size() >= MAX_EVIDENCE) {
            m_report.limited = true;
            Complete();
            return;
        }
        const auto& entry = m_candidates[m_report.processed++];
        const auto path = CanonicalPath(entry.absolutePath);
        if (path.empty() || !PathKey(path, m_projectDirectory).starts_with(m_assetsDirectory)) {
            ++m_report.unreadable;
            continue;
        }
        const auto parsed = ReadCandidate(path, m_report);
        if (!parsed) continue;
        std::optional<size_t> source;
        if (!VisitStrings(*parsed, [&](const std::string& value, const std::string& field, uint32_t line) {
            if (m_report.edges.size() >= MAX_EVIDENCE) return false;
            const bool guidReference = asset::AssetDatabase::IsGuidRef(value);
            const auto normalized = util::FileSystem::NormalizePathSeparators(value);
            const auto lower = Lower(normalized);
            const bool projectPath = lower.starts_with("assets/") || lower.starts_with("library/baked/");
            if (value.size() > MAX_REFERENCE_BYTES) {
                if (guidReference || projectPath) m_report.limited = true;
                return true;
            }
            GraphEdge edge;
            if (guidReference) {
                const auto guid = ParseGuidReference(value);
                if (!guid) return true;
                edge.target = FindGuidNode(guid->guid);
                edge.kind = "guid";
                edge.suffix = guid->suffix;
                edge.hint = guid->hint;
            } else {
                if (!projectPath && !normalized.starts_with('/') && (normalized.size() < 2 || normalized[1] != ':')) return true;
                if (std::any_of(value.begin(), value.end(), [](unsigned char c) { return c < 32; })) return true;
                const auto parts = Split(normalized);
                const auto base = SourceBase(parts.base);
                auto absolute = util::FileSystem::PathFromUtf8(base);
                if (absolute.is_relative()) {
                    if (!projectPath || absolute.has_root_path()) return true;
                    absolute = util::FileSystem::PathFromUtf8(m_projectDirectory) / absolute;
                }
                const auto target = util::FileSystem::PathToUtf8(absolute.lexically_normal());
                const auto key = PathKey(target, m_projectDirectory);
                const auto extension = Lower(util::FileSystem::GetExtension(target));
                if ((!key.starts_with(m_assetsDirectory) && !key.starts_with(m_bakedDirectory))
                    || extension.size() < 2 || extension.size() > 17 || extension == ".meta"
                    || !std::all_of(extension.begin() + 1, extension.end(), [](unsigned char c) { return std::isalnum(c) != 0; }))
                    return true;
                edge.target = FindPathNode(target);
                edge.kind = "path";
                edge.suffix = parts.suffix;
            }
            if (!source) source = FindPathNode(path);
            edge.source = *source;
            edge.field = field;
            edge.line = line;
            m_report.edges.push_back(std::move(edge));
            return true;
        })) m_report.limited = true;
    }
    if (m_report.processed == m_candidates.size()) Complete();
}

void GraphScan::Complete()
{
    m_report.incoming = TraceGraph(m_report, true);
    m_report.outgoing = TraceGraph(m_report, false);
    m_report.finished = true;
}

} /// @note namespace fbzz::editor::assetrefs
