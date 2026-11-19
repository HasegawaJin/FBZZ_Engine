/// @file    AssetReferenceScan.hpp
/// @brief   保存済み参照の根拠と調査範囲を保持する AssetBrowser 内部スナップショット。
/// @author  Hasegawa Jin
/// @date    2026-10-24
#pragma once
#include <Editor/Util/AssetSearch.hpp>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
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

struct ScanCoverage {
    size_t candidates = 0;
    size_t processed = 0;
    size_t parsed = 0;
    size_t unsupported = 0;
    size_t unreadable = 0;
    size_t oversized = 0;
    size_t malformed = 0;
    size_t bytesRead = 0;
    bool limited = false;
    bool finished = false;
};

struct Report : ScanCoverage {
    std::string projectRoot;
    std::string targetPath;
    ImportMetadata metadata;
    uint64_t generation = 0;
    uint64_t searchRevision = 0;
    uint64_t databaseRevision = 0;
    std::vector<Owner> owners;
    size_t unresolvedHints = 0;
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

enum class GraphNodeState { RESOLVED, MISSING_GUID, MISSING_PATH, UNVERIFIED };

struct GraphNode {
    std::string guid;
    std::string path;
    std::string kind;
    GraphNodeState state = GraphNodeState::RESOLVED;
};

struct GraphEdge {
    size_t source = 0;
    size_t target = 0;
    std::string field;
    std::string kind;
    std::string suffix;
    std::string hint;
    uint32_t line = 0;
};

struct GraphHop {
    size_t edge = 0;
    size_t distance = 0;
};

struct GraphDirection {
    std::vector<GraphHop> hops;
    size_t relatedCount = 0;
    size_t brokenCount = 0;
    size_t unverifiedCount = 0;
    bool depthLimited = false;
};

struct GraphRequest {
    std::string projectRoot;
    std::string guid;
    uint64_t searchRevision = 0;
    uint64_t databaseRevision = 0;
    std::vector<AssetSearchEntry> entries;
};

struct GraphReport : ScanCoverage {
    std::string projectRoot;
    uint64_t searchRevision = 0;
    uint64_t databaseRevision = 0;
    size_t root = 0;
    size_t maxDepth = 0;
    std::vector<GraphNode> nodes;
    std::vector<GraphEdge> edges;
    GraphDirection incoming;
    GraphDirection outgoing;
};

/// @return 正規化した 32 桁の GUID。不正な入力なら空。
[[nodiscard]] std::string NormalizeGraphGuid(std::string_view input);

/// @note 保存済み TOML と GUID 索引の一回の調査。完了時に循環を含む両方向の到達範囲を確定する。
class GraphScan final {
public:
    void Begin(GraphRequest request, size_t maxDepth);
    void Advance(size_t maxFiles);
    [[nodiscard]] const GraphReport& Result() const { return m_report; }

private:
    size_t FindGuidNode(const std::string& guid);
    size_t FindPathNode(const std::string& path);
    size_t AddNode(GraphNode node);
    void Complete();

    GraphReport m_report;
    std::vector<AssetSearchEntry> m_candidates;
    std::unordered_map<std::string, size_t> m_guidNodes;
    std::unordered_map<std::string, size_t> m_pathNodes;
    std::string m_projectDirectory;
    std::string m_assetsDirectory;
    std::string m_bakedDirectory;
};

} /// @note namespace fbzz::editor::assetrefs
