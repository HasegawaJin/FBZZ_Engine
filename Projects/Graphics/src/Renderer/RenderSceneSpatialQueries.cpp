/// @file    RenderSceneSpatialQueries.cpp
/// @brief   影の独立view unionをprivate空間indexへ接続するopt-in境界。
/// @author  Hasegawa Jin
/// @date    2026-10-23
#include "RenderSceneSpatialQueries.hpp"
#include <Graphics/Pipeline/RenderPassContext.hpp>
#include <Core/Logger.hpp>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <io.h>
#include <mutex>
#include <share.h>
#include <string>
#include <sys/stat.h>
#include <type_traits>
#include <utility>
#include <vector>

namespace fbzz::renderer::spatial_detail {
namespace {
struct Configuration {
    IndexOptions options;
    bool validate = false;
    std::string tracePath;
};

std::string EnvironmentValue(const char* name)
{
    char* value = nullptr;
    size_t length = 0;
    if (_dupenv_s(&value, &length, name) != 0 || !value) return {};
    const std::string result(value);
    std::free(value);
    return result;
}

const Configuration& Config()
{
    static const Configuration config = [] {
        Configuration value;
        const auto mode = EnvironmentValue("FBZZ_MESH_SPATIAL_MODE");
        if (mode == "bvh" || mode == "validate") value.options.mode = QueryMode::BVH;
        else if (mode == "auto") value.options.mode = QueryMode::AUTO;
        value.validate = mode == "validate";
        value.options.minimumTreeItems = 64;
        const auto dense = EnvironmentValue("FBZZ_MESH_SPATIAL_DENSE_UPDATE");
        if (dense == "refit") value.options.denseUpdate = DenseUpdate::REFIT;
        else if (dense == "linear") value.options.denseUpdate = DenseUpdate::LINEAR;
        if (EnvironmentValue("FBZZ_MESH_SPATIAL_UPDATE_MODE") == "stable") value.options.updateMode = UpdateMode::STABLE;
        std::fprintf(stderr, "SPATIAL_UPDATE_CONFIG mode=%s\n", value.options.updateMode == UpdateMode::STABLE ? "stable" : "legacy");
        value.tracePath = EnvironmentValue("FBZZ_MESH_SPATIAL_TRACE");
        return value;
    }();
    return config;
}

BvhDebugPublication& DebugPublication()
{
    static BvhDebugPublication publication;
    return publication;
}

/// @note Integer observations only; frameStamp is the snapshot clock and contextFrameStamp is the resource context clock.
#define FBZZ_SPATIAL_TRACE_COLUMNS(X) \
    X(snapshotSerial, batch.snapshot->snapshotSerial) X(frameStamp, batch.snapshot->frameStamp) \
    X(contextFrameStamp, context.frameStamp) X(sceneGeneration, batch.snapshot->sceneGeneration) \
    X(objectCount, batch.snapshot->Objects().size()) X(diagnosticsAvailable, !batch.fullScan || batch.validationMismatch) \
    X(cascadeCount, std::clamp(cascades, 0, kMaxShadowCascades)) \
    X(punctualCount, std::clamp(context.punctualShadowViewCount, 0, kMaxPunctualShadows)) \
    X(sources, batch.update.sources) X(staticSources, batch.update.staticSources) \
    X(dynamicSources, batch.update.dynamicSources) X(unboundedSources, batch.update.unboundedSources) \
    X(changedBounds, batch.update.changedBounds) X(changedMembership, batch.update.changedMembership) \
    X(witnessedSources, batch.update.witnessedSources) X(deltaApplied, batch.deltaApplied) X(geometryReused, batch.geometryReused) \
    X(rebuiltTrees, batch.update.rebuiltTrees) X(refittedTrees, batch.update.refittedTrees) \
    X(refitSortedAncestors, batch.update.refitSortedAncestors) X(refitScannedNodes, batch.update.refitScannedNodes) \
    X(refittedLeaves, batch.update.refittedLeaves) X(treeNodes, batch.update.nodes) \
    X(activeSlotRetainedBytes, batch.update.retainedBytes) X(visitedNodes, batch.query.visitedNodes) \
    X(testedLeaves, batch.query.testedLeaves) X(candidates, batch.query.candidates) X(batchCandidates, batch.Size()) \
    X(fullScan, batch.fullScan) X(indexRefreshed, batch.refreshed) X(validated, batch.validated) \
    X(validationMismatch, batch.validationMismatch) X(linearQuery, batch.query.linear) \
    X(numericFallback, batch.query.numericFallback) X(denseLinearFallback, batch.update.denseLinearFallback)

struct SpatialTraceRow {
    uint64_t ordinal = 0;
#define FBZZ_SPATIAL_TRACE_FIELD(name, expression) uint64_t name = 0;
    FBZZ_SPATIAL_TRACE_COLUMNS(FBZZ_SPATIAL_TRACE_FIELD)
#undef FBZZ_SPATIAL_TRACE_FIELD
};
static_assert(std::is_trivially_copyable_v<SpatialTraceRow>);

const char* ModeName(QueryMode mode, bool validate)
{
    if (validate) return "validate";
    return mode == QueryMode::BVH ? "bvh" : (mode == QueryMode::AUTO ? "auto" : "linear");
}
const char* DenseName(DenseUpdate mode)
{
    return mode == DenseUpdate::REFIT ? "refit" : (mode == DenseUpdate::LINEAR ? "linear" : "rebuild");
}
const char* UpdateName(UpdateMode mode)
{
    return mode == UpdateMode::STABLE ? "stable" : "legacy";
}

/// @note Diagnostic runs only: reservation, locking and row copies are observable overhead; timing runs leave TRACE unset.
/// @note Producers must finish before normal Graphics teardown; no Scene/resource owner or pointer is retained by a row.
class SpatialTraceCapture {
public:
    static constexpr size_t MAX_ROWS = 4096;
    explicit SpatialTraceCapture(const Configuration& config)
        : m_path(config.tracePath), m_mode(config.options.mode), m_dense(config.options.denseUpdate),
          m_updateMode(config.options.updateMode), m_validate(config.validate)
    { m_rows.reserve(MAX_ROWS); }

    void Add(SpatialTraceRow row)
    {
        std::lock_guard lock(m_mutex);
        row.ordinal = m_seen++;
        if (m_rows.size() < MAX_ROWS) m_rows.push_back(row);
        else ++m_dropped;
    }

    /// @see https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/sopen-s-wsopen-s _O_CREAT|_O_EXCL refuses an existing output.
    /// @see https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/fdopen-wfdopen The stream owns its descriptor after success.
    ~SpatialTraceCapture()
    {
        int descriptor = -1;
        const int error = _sopen_s(&descriptor, m_path.c_str(), _O_CREAT | _O_EXCL | _O_WRONLY | _O_BINARY | _O_NOINHERIT,
            _SH_DENYRW, _S_IREAD | _S_IWRITE);
        if (error != 0) {
            std::fprintf(stderr, "MeshSpatialTrace: cannot create new output (errno=%d): %s\n", error, m_path.c_str());
            return;
        }
        auto* output = _fdopen(descriptor, "wb");
        if (!output) { _close(descriptor); std::fprintf(stderr, "MeshSpatialTrace: cannot open output stream\n"); return; }
        const auto* mode = ModeName(m_mode, m_validate);
        const auto* dense = DenseName(m_dense);
        const auto* updateMode = UpdateName(m_updateMode);
        std::fprintf(output, "{\"kind\":\"header\",\"schema\":\"fbzz.mesh-spatial.v1\",\"capacity\":%zu,"
            "\"mode\":\"%s\",\"densePolicy\":\"%s\",\"updateMode\":\"%s\",\"timing\":false,\"memoryScope\":\"active-slot-capacity\","
            "\"traceRowBytes\":%zu,\"traceCapacityBytes\":%zu}\n",
            MAX_ROWS, mode, dense, updateMode, sizeof(SpatialTraceRow), m_rows.capacity() * sizeof(SpatialTraceRow));
        for (const auto& row : m_rows) {
            std::fprintf(output, "{\"kind\":\"row\",\"ordinal\":%llu,\"mode\":\"%s\",\"densePolicy\":\"%s\",\"updateMode\":\"%s\"",
                static_cast<unsigned long long>(row.ordinal), mode, dense, updateMode);
#define FBZZ_SPATIAL_TRACE_WRITE(name, expression) \
            std::fprintf(output, ",\"" #name "\":%llu", static_cast<unsigned long long>(row.name));
            FBZZ_SPATIAL_TRACE_COLUMNS(FBZZ_SPATIAL_TRACE_WRITE)
#undef FBZZ_SPATIAL_TRACE_WRITE
            std::fputs("}\n", output);
        }
        std::fprintf(output, "{\"kind\":\"summary\",\"storedRows\":%zu,\"droppedRows\":%llu,\"seenRows\":%llu,\"complete\":true}\n",
            m_rows.size(), static_cast<unsigned long long>(m_dropped), static_cast<unsigned long long>(m_seen));
        const bool failed = std::ferror(output) != 0;
        const int closed = std::fclose(output);
        if (failed || closed != 0) std::fprintf(stderr, "MeshSpatialTrace: output write failed\n");
    }

private:
    std::string m_path;
    QueryMode m_mode;
    DenseUpdate m_dense;
    UpdateMode m_updateMode;
    bool m_validate;
    std::mutex m_mutex;
    std::vector<SpatialTraceRow> m_rows;
    uint64_t m_seen = 0, m_dropped = 0;
};

SpatialTraceCapture& TraceCapture()
{
    static SpatialTraceCapture capture(Config());
    return capture;
}

void RecordTrace(const RenderPassContext& context, int cascades, const CandidateBatch& batch)
{
    if (Config().tracePath.empty()) return;
    SpatialTraceRow row;
#define FBZZ_SPATIAL_TRACE_CAPTURE(name, expression) row.name = static_cast<uint64_t>(expression);
    FBZZ_SPATIAL_TRACE_COLUMNS(FBZZ_SPATIAL_TRACE_CAPTURE)
#undef FBZZ_SPATIAL_TRACE_CAPTURE
    TraceCapture().Add(row);
}
#undef FBZZ_SPATIAL_TRACE_COLUMNS
} /// @note namespace

bool MeshSpatialQueriesEnabled()
{
    const auto& config = Config();
    if (!config.tracePath.empty()) (void)TraceCapture();
    return config.options.mode != QueryMode::LINEAR;
}

CandidateBatch CollectShadowCandidates(const RenderPassContext& context, uint32_t updateMask)
{
    CandidateBatch batch;
    batch.snapshot = context.renderScene;
    if (!MeshSpatialQueriesEnabled() || !batch.snapshot) return batch;
    FBZZ_PROFILE_SCOPE("RenderMeshSpatial::ShadowCandidates");
    std::array<const math::Frustum*, kMaxShadowCascades + kMaxPunctualShadows> volumes{};
    size_t count = 0;
    for (int cascade = 0; cascade < kMaxShadowCascades; ++cascade)
        if ((updateMask & (1u << cascade)) != 0) volumes[count++] = &context.shadowCascades[cascade].frustum;
    const int directionalCount = static_cast<int>(count);
    for (int view = 0; view < std::clamp(context.punctualShadowViewCount, 0, kMaxPunctualShadows); ++view)
        volumes[count++] = &context.punctualShadowViews[view].frustum;
    static thread_local RenderSceneSpatialCache cache;
    const auto ticket = DebugPublication().Begin(batch.snapshot->sceneGeneration, context.outputRT.id, context.outputRT.gen);
    std::shared_ptr<RenderBvhDebugSnapshot> debug;
    if (ticket.revision != 0) {
        debug = std::make_shared<RenderBvhDebugSnapshot>();
        debug->requestRevision = ticket.revision;
        debug->contextFrameStamp = context.frameStamp;
        debug->viewOutputId = context.outputRT.id;
        debug->viewOutputGeneration = context.outputRT.gen;
    }
    batch = cache.Collect(batch.snapshot, context.cullingBoundsPadding,
        std::span<const math::Frustum* const>(volumes.data(), count), Config().options, Config().validate,
        debug ? &ticket.request : nullptr, debug.get());
    if (batch.validationMismatch)
        FBZZ_LOG_ERROR("RenderMeshSpatial BVH union differs from current linear bounds; using full shadow candidate scan");
    RecordTrace(context, directionalCount, batch);
    if (debug) DebugPublication().Publish(ticket, std::move(debug));
    return batch;
}

} /// @note namespace fbzz::renderer::spatial_detail

namespace fbzz::renderer {

void RenderBvhDebug::SetRequest(const RenderBvhDebugRequest& request)
{
    auto& publication = spatial_detail::DebugPublication();
    if (!publication.SetRequest(request)) return;
    const auto ticket = publication.Begin(request.sceneGeneration, request.viewOutputId, request.viewOutputGeneration);
    if (ticket.revision == 0) return;
    auto snapshot = std::make_shared<RenderBvhDebugSnapshot>();
    snapshot->requestRevision = ticket.revision;
    snapshot->sceneGeneration = ticket.request.sceneGeneration;
    snapshot->viewOutputId = ticket.request.viewOutputId;
    snapshot->viewOutputGeneration = ticket.request.viewOutputGeneration;
    snapshot->availability = spatial_detail::Config().options.mode == spatial_detail::QueryMode::LINEAR
        ? RenderBvhDebugAvailability::LINEAR_MODE : RenderBvhDebugAvailability::WAITING;
    publication.Publish(ticket, std::move(snapshot));
}

std::shared_ptr<const RenderBvhDebugSnapshot> RenderBvhDebug::GetSnapshot()
{
    return spatial_detail::DebugPublication().Snapshot();
}

void RenderBvhDebug::Invalidate()
{
    spatial_detail::DebugPublication().Invalidate();
}

} /// @note namespace fbzz::renderer
