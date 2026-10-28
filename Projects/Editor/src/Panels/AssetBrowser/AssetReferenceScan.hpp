/// @file    AssetReferenceScan.hpp
/// @brief   保存済み参照の根拠と調査範囲を保持する AssetBrowser 内部スナップショット。
/// @author  Hasegawa Jin
/// @date    2026-10-24
#pragma once
#include <Editor/Util/AssetSearch.hpp>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace fbzz::editor::assetrefs {

struct ImportMetadata {
    std::string sourceDcc;
    std::string importer;
    int64_t importerVersion = -1;
    bool unreadable = false;
};

struct Evidence {
    std::string field;
    std::string kind;
    uint32_t line = 0;
};

struct Owner {
    std::string path;
    std::string kind;
    ImportMetadata metadata;
    std::vector<Evidence> evidence;
};

struct Request {
    std::string projectRoot;
    std::string targetPath;
    uint64_t searchRevision = 0;
    uint64_t databaseRevision = 0;
    std::vector<AssetSearchEntry> entries;
};

struct Report {
    std::string projectRoot;
    std::string targetPath;
    ImportMetadata metadata;
    uint64_t generation = 0;
    uint64_t searchRevision = 0;
    uint64_t databaseRevision = 0;
    std::vector<Owner> owners;
    size_t candidates = 0;
    size_t processed = 0;
    size_t parsed = 0;
    size_t unsupported = 0;
    size_t unreadable = 0;
    size_t oversized = 0;
    size_t malformed = 0;
    size_t unresolvedHints = 0;
    size_t bytesRead = 0;
    bool limited = false;
    bool finished = false;
};

/// @note UI が所有する一世代の調査。Begin だけが結果を捨て、完了後の Advance は I/O しない。
/// @note AssetSearch の候補を値で所有し、索引の再配置・ルート切替後に pointer を持ち越さない。
/// @see Docs/design/asset-reference-visibility.md
class Scan final {
public:
    void Begin(Request request);
    /// @note 1 呼出しにつき最大 maxFiles 件、各ファイルは最大 2 MiB。要求がある間だけ進める。
    void Advance(size_t maxFiles = 4);
    [[nodiscard]] const Report& Result() const { return m_report; }
    /// @note 内容変更は AssetSearch の世代に含まれないため、true でも保存内容の現在性は保証しない。
    [[nodiscard]] bool MatchesSnapshot(const std::string& projectRoot,
                                       uint64_t searchRevision, uint64_t databaseRevision) const;

private:
    Report m_report;
    std::vector<AssetSearchEntry> m_candidates;
    std::string m_targetBase;
    std::string m_targetSuffix;
    std::string m_targetGuid;
    size_t m_evidenceCount = 0;
};

} /// @note namespace fbzz::editor::assetrefs
