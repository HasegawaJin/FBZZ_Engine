/// @file    AssetReferenceScan.cpp
/// @brief   TOML の完全な文字列値から参照根拠を段階的に収集する。
/// @author  Hasegawa Jin
/// @date    2026-10-24
#include "AssetReferenceScan.hpp"
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Asset/TexDescSerializer.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <toml++/toml.hpp>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <functional>
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

} /// @note namespace

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
        std::string text;
        const auto read = ReadBounded(entry.absolutePath,
            (std::min)(MAX_FILE_BYTES, MAX_TOTAL_BYTES - m_report.bytesRead), text);
        if (read == ReadResult::OVERSIZED) {
            ++m_report.oversized;
            m_report.limited = true;
            continue;
        }
        if (read != ReadResult::OK) { ++m_report.unreadable; continue; }
        m_report.bytesRead += text.size();
        const auto parsed = toml::parse(text);
        if (!parsed) { ++m_report.malformed; continue; }
        ++m_report.parsed;
        Owner owner{entry.absolutePath, Kind(entry.extension), {}, {}};
        const auto match = [&](const std::string& value) -> const char* {
            if (value.starts_with(asset::AssetDatabase::kGuidPrefix)) {
                const size_t separator = value.find('|');
                const std::string authority = value.substr(0, separator);
                constexpr size_t BODY_LENGTH = 5 + 32;
                if (authority.size() < BODY_LENGTH) return nullptr;
                const std::string guid = authority.substr(5, 32);
                if (!std::all_of(guid.begin(), guid.end(), [](unsigned char c) { return std::isxdigit(c) != 0; }))
                    return nullptr;
                const std::string suffix = authority.substr(BODY_LENGTH);
                if (!ValidSuffix(suffix) || (!m_targetSuffix.empty() && m_targetSuffix != suffix)) return nullptr;
                if (!m_targetGuid.empty() && Lower(guid) == m_targetGuid) return "GUID value";
                /// @note 別 GUID の古いパスヒントを、現在の参照先だと誤認させない。
                if (!asset::AssetDatabase::PathFromGuid(Lower(guid)).empty() || separator == std::string::npos)
                    return nullptr;
                const Parts hint = Split(value.substr(separator + 1));
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
        size_t nodes = 0;
        std::function<void(const toml::node&, const std::string&, size_t)> visit;
        visit = [&](const toml::node& node, const std::string& field, size_t depth) {
            if (++nodes > MAX_NODES || depth > 64 || m_evidenceCount >= MAX_EVIDENCE) {
                m_report.limited = true;
                return;
            }
            if (const auto* string = node.as_string()) {
                if (const char* kind = match(string->get())) {
                    owner.evidence.push_back({field, kind, static_cast<uint32_t>(node.source().begin.line)});
                    ++m_evidenceCount;
                    if (std::string_view(kind).starts_with("Unresolved")) ++m_report.unresolvedHints;
                }
            } else if (const auto* table = node.as_table()) {
                for (const auto& [key, child] : *table) {
                    if (nodes >= MAX_NODES || m_evidenceCount >= MAX_EVIDENCE) { m_report.limited = true; break; }
                    const std::string childField = field.empty() ? std::string(key.str()) : field + "." + std::string(key.str());
                    visit(child, childField.substr(0, 512), depth + 1);
                }
            } else if (const auto* array = node.as_array()) {
                for (size_t index = 0; index < array->size(); ++index) {
                    if (nodes >= MAX_NODES || m_evidenceCount >= MAX_EVIDENCE) { m_report.limited = true; break; }
                    visit((*array)[index], field + "[" + std::to_string(index) + "]", depth + 1);
                }
            }
        };
        visit(parsed.table(), {}, 0);
        if (!owner.evidence.empty()) {
            owner.metadata = ReadMetadata(entry.absolutePath);
            m_report.owners.push_back(std::move(owner));
        }
    }
    m_report.finished = m_report.processed == m_candidates.size();
}

} /// @note namespace fbzz::editor::assetrefs
