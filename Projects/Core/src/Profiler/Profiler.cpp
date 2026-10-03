/// @file    Profiler.cpp
/// @brief   所有 descriptor を用いた Performance 収集の互換 facade。
/// @author  Hasegawa Jin
/// @date    2026-06-02
#include <Core/Profiler/Profiler.hpp>
#include <algorithm>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace fbzz::profiler {
namespace {
constexpr size_t MAX_DESCRIPTORS = 8192;
/// @note 表を 1 MiB に制限し、最新 snapshot と初回登録の一時文字列を含めて 4 MiB 以内に保つ。
constexpr size_t MAX_STRING_BYTES = 1024 * 1024;
/// @note MSVC の新規 char 文字列の丸めと終端へ 32 byte の余裕を取り、確保前に過大な入力を拒否する。
/// @see https://github.com/microsoft/STL/blob/main/stl/inc/xstring basic_string の _Alloc_mask と _Calculate_growth。
size_t FreshStringBytes(std::string_view value)
{
    return value.size() > MAX_STRING_BYTES - 32 ? MAX_STRING_BYTES + 1 : value.size() + 32;
}
size_t StringBytes(const PerformanceDescriptor& descriptor)
{
    return descriptor.name.capacity() + 1 + descriptor.category.capacity() + 1;
}
struct DescriptorKey {
    std::string_view name;
    std::string_view category;
    uint32_t color;
    bool operator==(const DescriptorKey&) const = default;
};
struct DescriptorHash {
    size_t operator()(const DescriptorKey& key) const {
        return std::hash<std::string_view>{}(key.name) ^ (std::hash<std::string_view>{}(key.category) << 1) ^ key.color;
    }
};
ProfileRecorder s_recorder;
PerformanceSnapshot s_snapshot;
std::vector<PerformanceDescriptor> s_descriptors;
std::unordered_map<DescriptorKey, uint64_t, DescriptorHash> s_descriptorKeys;
std::vector<ProfileToken> s_compatStack;
std::vector<ProfileRecord> s_records;
size_t s_stringBytes = 0;
uint64_t s_nextDescriptorId = 0;
uint64_t s_hostSerial = 0;
uint64_t s_lastFrameIndex = 0;
uint32_t s_compatSuppressedDepth = 0;
ProfileCheckpoint s_compatOverflowCheckpoint;
uint64_t s_compatOverflowScopeId = 0;

uint64_t Resolve(const ProfilerMarker& marker)
{
    if (!s_recorder.IsRecording() || !s_recorder.IsFrameOpen()) return 0;
    const DescriptorKey key{marker.name ? marker.name : "Unnamed", marker.category ? marker.category : "General", marker.color};
    const auto found = s_descriptorKeys.find(key);
    if (found != s_descriptorKeys.end()) return found->second;
    const size_t estimatedBytes = FreshStringBytes(key.name) + FreshStringBytes(key.category);
    if (s_descriptors.size() == MAX_DESCRIPTORS || estimatedBytes > MAX_STRING_BYTES - s_stringBytes) return 0;
    PerformanceDescriptor candidate;
    candidate.name = key.name;
    candidate.category = key.category;
    candidate.color = key.color;
    const size_t bytes = StringBytes(candidate);
    if (bytes > MAX_STRING_BYTES - s_stringBytes) return 0;
    if (s_descriptors.capacity() < MAX_DESCRIPTORS) s_descriptors.reserve(MAX_DESCRIPTORS);
    candidate.sampleKey = ++s_nextDescriptorId;
    s_descriptors.push_back(std::move(candidate));
    auto& descriptor = s_descriptors.back();
    s_stringBytes += bytes;
    s_descriptorKeys.emplace(DescriptorKey{descriptor.name, descriptor.category, descriptor.color}, descriptor.sampleKey);
    return descriptor.sampleKey;
}

void RetireDescriptors()
{
    /// @note 最新 snapshot は文字列を所有するため、フレーム間で未使用 descriptor を退役できる。
    std::unordered_set<uint64_t> used;
    for (const auto& descriptor : s_snapshot.descriptors) used.insert(descriptor.sampleKey);
    std::vector<PerformanceDescriptor> retained;
    retained.reserve(MAX_DESCRIPTORS);
    s_descriptorKeys.clear();
    for (auto& descriptor : s_descriptors)
        if (used.contains(descriptor.sampleKey)) retained.push_back(std::move(descriptor));
    s_descriptors.swap(retained);
    s_stringBytes = 0;
    for (const auto& descriptor : s_descriptors) {
        s_stringBytes += StringBytes(descriptor);
        s_descriptorKeys.emplace(DescriptorKey{descriptor.name, descriptor.category, descriptor.color}, descriptor.sampleKey);
    }
}
} /// @note namespace

void Profiler::SetEnabled(bool enabled)
{
    if (enabled && s_recorder.IsRecording()) return;
    s_recorder.SetRecordingImmediate(enabled);
    s_compatStack.clear();
    s_compatSuppressedDepth = 0;
    s_compatOverflowCheckpoint = {};
    s_compatOverflowScopeId = 0;
    s_records.clear();
    s_snapshot = {};
    s_snapshot.recording = enabled;
    s_snapshot.captureSessionId = s_recorder.GetSnapshot().captureSessionId;
    if (!enabled) { s_descriptorKeys.clear(); s_descriptors.clear(); s_stringBytes = 0; }
}
bool Profiler::IsEnabled() { return s_recorder.IsRecording(); }
void Profiler::RequestRecording(bool recording) { s_recorder.RequestRecording(recording); }
void Profiler::RequestClear() { s_recorder.RequestClear(); }
void Profiler::BeginFrame() { BeginFrame(++s_hostSerial, ProfileRecorder::NowMs()); }
void Profiler::BeginFrame(uint64_t applicationFrameSerial, double frameStartMs)
{
    s_compatStack.clear();
    s_compatSuppressedDepth = 0;
    s_compatOverflowCheckpoint = {};
    s_compatOverflowScopeId = 0;
    if (s_descriptors.size() > MAX_DESCRIPTORS / 2 || s_stringBytes > MAX_STRING_BYTES / 2) RetireDescriptors();
    s_recorder.BeginFrame(applicationFrameSerial, frameStartMs);
    if (s_snapshot.captureSessionId != s_recorder.GetSnapshot().captureSessionId) {
        s_records.clear();
        s_snapshot = {};
    }
    s_snapshot.recording = IsEnabled();
    s_snapshot.captureSessionId = s_recorder.GetSnapshot().captureSessionId;
}
void Profiler::EndFrame()
{
    if (!s_recorder.IsFrameOpen()) return;
    s_recorder.EndFrame();
    static_cast<ProfileSnapshot&>(s_snapshot) = s_recorder.GetSnapshot();
    s_lastFrameIndex = s_snapshot.frameIndex;
    s_snapshot.descriptors.clear();
    std::unordered_set<uint64_t> used;
    for (const auto& sample : s_snapshot.samples) used.insert(sample.sampleKey);
    for (const auto& descriptor : s_descriptors)
        if (used.contains(descriptor.sampleKey)) s_snapshot.descriptors.push_back(descriptor);
    s_records.clear();
    std::unordered_map<uint64_t, const PerformanceDescriptor*> lookup;
    for (const auto& descriptor : s_snapshot.descriptors) lookup.emplace(descriptor.sampleKey, &descriptor);
    auto completion = s_snapshot.samples;
    std::sort(completion.begin(), completion.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.completionOrder < rhs.completionOrder;
    });
    for (const auto& sample : completion) {
        if (sample.status != ProfileSampleStatus::COMPLETE) continue;
        const auto descriptor = lookup.find(sample.sampleKey);
        if (descriptor != lookup.end())
            s_records.push_back({descriptor->second->name.c_str(), descriptor->second->category.c_str(), sample.inclusiveMs,
                                 s_lastFrameIndex, sample.depth, descriptor->second->color});
    }
    s_compatStack.clear();
    s_compatSuppressedDepth = 0;
    s_compatOverflowCheckpoint = {};
    s_compatOverflowScopeId = 0;
}
ProfileToken Profiler::BeginScope(const ProfilerMarker& marker) { return s_recorder.Begin(Resolve(marker)); }
void Profiler::EndScope(ProfileToken token) { s_recorder.End(token); }
void Profiler::BeginSample(const ProfilerMarker& marker)
{
    const bool entersOverflow = s_compatStack.size() == ProfileRecorder::MAX_DEPTH && !s_compatSuppressedDepth;
    if (entersOverflow)
        s_compatOverflowCheckpoint = s_recorder.Checkpoint();
    const auto token = BeginScope(marker);
    if (!token.recorderId) return;
    if (entersOverflow) s_compatOverflowScopeId = token.scopeId;
    if (s_compatStack.size() == ProfileRecorder::MAX_DEPTH || s_compatSuppressedDepth) { ++s_compatSuppressedDepth; return; }
    s_compatStack.push_back(token);
}
void Profiler::EndSample()
{
    if (s_compatSuppressedDepth) {
        if (!--s_compatSuppressedDepth) {
            s_recorder.Recover(s_compatOverflowCheckpoint);
            s_compatOverflowCheckpoint = {};
            s_compatOverflowScopeId = 0;
        }
        return;
    }
    if (s_compatStack.empty()) return;
    const auto token = s_compatStack.back();
    s_compatStack.pop_back();
    EndScope(token);
}
void Profiler::PushMarker(const ProfilerMarker& marker) { s_recorder.Push(Resolve(marker), 0.0, ProfileSampleKind::INSTANT); }
void Profiler::PushSample(const ProfilerMarker& marker, double elapsedMs) { s_recorder.Push(Resolve(marker), elapsedMs, ProfileSampleKind::EXTERNAL_DURATION); }
ProfilerCheckpoint Profiler::Checkpoint()
{
    return {s_recorder.Checkpoint(), s_compatOverflowCheckpoint,
            s_compatStack.empty() ? 0 : s_compatStack.back().scopeId, s_compatOverflowScopeId,
            static_cast<uint32_t>(s_compatStack.size()), s_compatSuppressedDepth};
}
void Profiler::Recover(ProfilerCheckpoint checkpoint)
{
    if (checkpoint.compatStackSize > s_compatStack.size() || checkpoint.compatSuppressedDepth > s_compatSuppressedDepth) return;
    if (checkpoint.compatStackSize && s_compatStack[checkpoint.compatStackSize - 1].scopeId != checkpoint.compatTopScopeId) return;
    /// @note 元の抑制分岐を閉じて同じ深さへ再進入した checkpoint は、現在の対応を巻き戻さない。
    if (checkpoint.compatSuppressedDepth && checkpoint.compatOverflowScopeId != s_compatOverflowScopeId) return;
    if (!s_recorder.Recover(checkpoint.recorder)) return;
    s_compatStack.resize(checkpoint.compatStackSize);
    s_compatSuppressedDepth = checkpoint.compatSuppressedDepth;
    s_compatOverflowCheckpoint = checkpoint.compatOverflow;
    s_compatOverflowScopeId = checkpoint.compatOverflowScopeId;
}
const std::vector<ProfileRecord>& Profiler::GetLastFrameRecords() { return s_records; }
uint64_t Profiler::GetLastFrameIndex() { return s_lastFrameIndex; }
const PerformanceSnapshot& Profiler::GetSnapshot() { return s_snapshot; }

} /// @note namespace fbzz::profiler
