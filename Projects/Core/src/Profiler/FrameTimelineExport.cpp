/// @file    FrameTimelineExport.cpp
/// @brief   タイムラインの完全性判定と CSV・メタデータの保存。
/// @author  Hasegawa Jin
/// @date    2026-11-21

#include "FrameTimelineCapture.hpp"
#include <Core/Util/FileSystem.hpp>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <locale>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#endif

namespace fbzz::profiler::frame_timeline::detail {

namespace {

const char* EventName(Event event)
{
    static constexpr const char* names[]{"main-frame", "time-tick", "input", "window-events", "input-callbacks", "input-actions",
        "asset-pump", "module-update", "module-late-update", "audio-update", "module-render", "frame-tail", "editor-update",
        "simulation", "view", "skinning", "extract", "environment", "graph", "post-cleanup", "worker-frame", "packet",
        "packet-execute", "packet-destroy", "packet-submit", "packet-accept", "wait-slot", "wait-acceptance", "wait-completion",
        "wait-drain", "wait-other", "wait-pump", "execution-mutex", "thread-mutex", "fence-wait", "synchronous-fence-wait", "present", "queue-submit"};
    static_assert(std::size(names) == static_cast<size_t>(Event::COUNT));
    return static_cast<size_t>(event) < std::size(names) ? names[static_cast<size_t>(event)] : "invalid";
}

void WriteContextColumns(std::ostream& output, const Context& context)
{
    output << context.captureFrameOrdinal << ',' << context.applicationFrameSerial << ',' << context.resourceFrameSerial << ','
        << context.submissionSerial << ',' << context.targetSerial << ',' << context.objectId << ',' << context.viewId << ',' << context.detail;
}

const char* EdgeName(Edge edge)
{
    switch (edge) {
    case Edge::BEGIN: return "begin";
    case Edge::END: return "end";
    case Edge::INSTANT: return "instant";
    }
    return "instant";
}

bool IsCaptureComplete(const Capture& capture)
{
    // 指定回数のフレームと終端がそろい、イベントや時刻の記録に欠落がないことを確かめる。
    const bool hasExpectedFrames = capture.closingAnchor && capture.sampleCount == capture.samples;
    bool complete = hasExpectedFrames && !capture.cancelled.load();
    complete = complete && capture.droppedEvents.load() == 0 && capture.droppedCpu.load() == 0;
    complete = complete && capture.invalidBoundaries.load() == 0;

    // 終端は直前フレームの区間を閉じるための行なので、自身の末尾時間は検査しない。
    for (size_t index = 0; index < capture.frameCount; ++index) {
        const auto& record = capture.frames[index];
        if (record.role != detail::FrameRole::CLOSING_ANCHOR) {
            complete &= record.times.TailAvailable();
        }
    }
    return complete;
}

void WriteCsv(std::ostream& output, const Capture& capture, uint64_t eventCount)
{
    output.imbue(std::locale::classic());
    output << std::setprecision(17);
    output << "rowType,captureFrameOrdinal,applicationFrameSerial,resourceFrameSerial,submissionSerial,targetSerial,"
           "objectId,viewId,detail,osThreadId,event,edge,scope,qpc,value,cpuAvailable,kernel100ns,user100ns,"
           "cyclesAvailable,cycles,role,beginQpc,endQpc,nextBeginQpc,profileBeginMs,profileEndMs,"
           "cpuElapsedAvailable,forwardIntervalAvailable,tailAvailable\n";

    for (size_t index = 0; index < capture.frameCount; ++index) {
        const auto& record = capture.frames[index];
        output << "frame,";
        WriteContextColumns(output, record.context);
        output << ',' << record.osThreadId << ",main-frame,anchor,0," << record.times.begin << ",0,0,0,0,0,0,"
            << static_cast<unsigned>(record.role) << ',' << record.times.begin << ',' << record.times.end << ',' << record.times.nextBegin << ','
            << record.profileBeginMs << ',' << record.profileEndMs << ',' << record.times.CpuAvailable() << ','
            << record.times.IntervalAvailable() << ',' << record.times.TailAvailable() << '\n';
    }

    for (uint64_t index = 0; index < eventCount; ++index) {
        const auto& record = capture.events[index];
        const CpuCounters counters = record.cpu != INVALID_INDEX ? capture.cpu[record.cpu] : CpuCounters{};
        output << "event,";
        WriteContextColumns(output, record.context);
        output << ',' << record.osThreadId << ',' << EventName(record.event) << ',' << EdgeName(record.edge) << ','
            << record.scope << ',' << record.qpc << ',' << record.value << ',' << counters.cpuAvailable << ',' << counters.kernel << ',' << counters.user << ','
            << counters.cyclesAvailable << ',' << counters.cycles << ",0,0,0,0,0,0,0,0,0\n";
    }
}

void WriteMetadata(std::ostream& output, const Capture& capture, uint64_t eventCount, bool complete)
{
    uint32_t processId = 0;
#if defined(_WIN32)
    processId = GetCurrentProcessId();
#endif
    output << "{\n  \"schemaVersion\":1,\n  \"instrumentationMode\":\"frame-timeline/qpc-v1\",\n  \"observerSessionId\":" << capture.observerSession
        << ",\n  \"processId\":" << processId << ",\n  \"qpcFrequency\":" << capture.frequency << ",\n  \"requestedWarmupFrames\":" << capture.warmup << ",\n  \"requestedSampleFrames\":" << capture.samples
        << ",\n  \"sampleFrames\":" << capture.sampleCount << ",\n  \"frameAnchors\":" << capture.frameCount << ",\n  \"closingNextBegin\":" << (capture.closingAnchor ? "true" : "false")
        << ",\n  \"eventCapacity\":" << capture.eventCapacity << ",\n  \"eventRowBytes\":" << sizeof(EventRow) << ",\n  \"reservedEventBytes\":" << capture.eventCapacity * sizeof(EventRow)
        << ",\n  \"events\":" << eventCount << ",\n  \"droppedEvents\":" << capture.droppedEvents.load()
        << ",\n  \"droppedCpuCounters\":" << capture.droppedCpu.load() << ",\n  \"invalidBoundaries\":" << capture.invalidBoundaries.load()
        << ",\n  \"wallQueries\":" << capture.wallQueries.load() << ",\n  \"threadCpuQueries\":" << capture.cpuQueries.load()
        << ",\n  \"threadCpuCounterApiCalls\":" << capture.cpuQueries.load() * 2
        << ",\n  \"cancelled\":" << (capture.cancelled.load() ? "true" : "false") << ",\n  \"complete\":" << (complete ? "true" : "false")
        << ",\n  \"fineSpanFilter\":{\"version\":1,\"minimumRetainedMicroseconds\":5,\"minimumRetainedTicks\":" << FineThresholdTicks(capture)
        << ",\"pendingSlotsPerThread\":" << MAX_PENDING_FINE_SPANS << ",\"unfilteredFallbackScopes\":" << capture.fineSpanFallbacks.load()
        << ",\"threadCpuScopesExempt\":true,\"countsAreOverlappingScopesNotWallTime\":true,\"events\":[";
    bool firstFine = true;
    for (size_t index = 0; index < static_cast<size_t>(Event::COUNT); ++index) {
        if (!IsFineSpan(static_cast<Event>(index))) {
            continue;
        }
        if (!firstFine) {
            output << ',';
        }
        firstFine = false;
        output << "{\"event\":\"" << EventName(static_cast<Event>(index)) << "\",\"omittedScopes\":" << capture.filteredFineScopes[index].load()
            << ",\"omittedDurationTicks\":" << capture.filteredFineTicks[index].load() << '}';
    }
    output << "]},\n  \"eventPublicationOrder\":\"Begin precedes its matching End; fine spans may publish after later QPC events\""
        << ",\n  \"frameInterval\":\"frameF.beginQpc to frameF.nextBeginQpc; passive wall row F+1\",\n"
           "  \"applicationFrameSerialDomain\":\"Time::frameCount and BeginCpuFrame producer serial\",\n"
           "  \"resourceFrameSerialDomain\":\"ResourceManager::FrameStamp; joins passive/GPU applicationFrameSerial\",\n"
           "  \"frameRoles\":{\"leading\":1,\"sample\":2,\"closingAnchor\":3},\n"
           "  \"cpuCounterUnits\":\"kernel/user100ns; raw cycles must not be converted to ms\",\n"
           "  \"overlap\":\"wait/pump/worker/present overlap; never add inclusive spans blindly\"\n}\n";
}

} // namespace

bool PublishCaptureFiles(const Capture& capture, const std::string& outputFile)
{
    const bool complete = IsCaptureComplete(capture);
    const auto outputPath = util::FileSystem::PathFromUtf8(outputFile);
    auto pendingPath = outputPath;
    pendingPath += ".pending";
    auto metadataPath = outputPath;
    metadataPath += ".meta.json";

    // 保存先と一時ファイルのどれにも既存の収集結果がないことを確かめる。
    std::error_code error;
    const std::filesystem::path* paths[] = {&outputPath, &metadataPath, &pendingPath};
    for (const auto* path : paths) {
        if (std::filesystem::exists(*path, error) || error) {
            return false;
        }
    }

    // CSV は一時ファイルを閉じてから確定名へ移す。途中まで書いた内容を確定名で公開しない。
    std::ofstream csv(pendingPath, std::ios::binary | std::ios::trunc);
    if (!csv) {
        return false;
    }

    const uint64_t eventCount = (std::min)(capture.eventCount.load(), static_cast<uint64_t>(capture.eventCapacity));
    WriteCsv(csv, capture, eventCount);
    csv.flush();
    const bool csvWritten = csv.good();
    csv.close();
    if (!csvWritten || csv.fail()) {
        return false;
    }

    std::filesystem::rename(pendingPath, outputPath, error);
    if (error) {
        return false;
    }

    // CSV の公開後に、読み手が完全性を判定するためのメタデータを保存する。
    std::ofstream metadata(metadataPath, std::ios::binary | std::ios::trunc);
    WriteMetadata(metadata, capture, eventCount, complete);
    metadata.flush();
    const bool metadataWritten = metadata.good();
    metadata.close();
    if (!metadataWritten || metadata.fail()) {
        return false;
    }

    return true;
}

} // namespace fbzz::profiler::frame_timeline::detail
