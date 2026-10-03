/// @file    ScriptProfiler.cpp
/// @brief   Script 識別、所有 snapshot、障害復旧と世代検証。
/// @author  Hasegawa Jin
/// @date    2026-10-03
#include <Engine/Profiler/ScriptProfiler.hpp>
#include <Core/Profiler/Profiler.hpp>
#include <Engine/Scene/Script.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <algorithm>
#include <map>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace fbzz::scene {
namespace {
constexpr size_t MAX_DESCRIPTORS = 8192;
/// @note 型表を含む表を 1 MiB に制限し、snapshot と初回登録の一時文字列を含めて 4 MiB 以内に保つ。
constexpr size_t MAX_STRING_BYTES = 1024 * 1024;
/// @note MSVC の新規 char 文字列の丸めと終端へ 32 byte の余裕を取り、確保前に過大な入力を拒否する。
/// @see https://github.com/microsoft/STL/blob/main/stl/inc/xstring basic_string の _Alloc_mask と _Calculate_growth。
size_t FreshStringBytes(std::string_view value)
{
    return value.size() > MAX_STRING_BYTES - 32 ? MAX_STRING_BYTES + 1 : value.size() + 32;
}
size_t StringBytes(const ScriptProfileDescriptor& descriptor)
{
    return descriptor.inspectionId.capacity() + 1 + descriptor.instanceId.capacity() + 1 +
        descriptor.objectName.capacity() + 1 + descriptor.typeName.capacity() + 1 + descriptor.callbackLabel.capacity() + 1;
}
struct TypeLess {
    using is_transparent = void;
    template<class Left, class Right> bool operator()(const Left& left, const Right& right) const
    {
        return left.first != right.first ? left.first < right.first : std::string_view(left.second) < std::string_view(right.second);
    }
};
struct InstanceState {
    uint64_t runtimeEpoch = 1;
    uint64_t dllGeneration = 1;
    ScriptProfileMode mode = ScriptProfileMode::EDIT;
};
struct DescriptorKey {
    const Script* script = nullptr;
    uint64_t context = 0;
    ScriptCallbackKind kind = ScriptCallbackKind::UNKNOWN;
    std::string_view label;
    bool operator==(const DescriptorKey&) const = default;
};
struct DescriptorHash {
    size_t operator()(const DescriptorKey& key) const {
        return std::hash<const Script*>{}(key.script) ^ std::hash<uint64_t>{}(key.context)
             ^ std::hash<std::string_view>{}(key.label) ^ static_cast<size_t>(key.kind);
    }
};
profiler::ProfileRecorder s_recorder(false);
ScriptProfileSnapshot s_snapshot;
std::unordered_map<const Script*, InstanceState> s_instances;
std::vector<ScriptProfileContext> s_contexts;
std::vector<uint64_t> s_frameContexts;
std::vector<ScriptProfileDescriptor> s_descriptors;
std::unordered_map<DescriptorKey, uint64_t, DescriptorHash> s_descriptorKeys;
std::map<std::pair<uint64_t, std::string>, uint64_t, TypeLess> s_typeIds;
uint64_t s_runtimeEpoch = 1;
uint64_t s_dllGeneration = 1;
uint64_t s_nextContextId = 0;
uint64_t s_nextDescriptorId = 0;
uint64_t s_nextTypeId = 0;
ScriptProfileMode s_mode = ScriptProfileMode::EDIT;
bool s_transitionFrame = false;
size_t s_stringBytes = 0;

uint64_t Context(uint64_t sceneGeneration, const InstanceState& state)
{
    auto found = std::find_if(s_contexts.begin(), s_contexts.end(), [&](const auto& context) {
        return context.runtimeEpoch == state.runtimeEpoch && context.dllGeneration == state.dllGeneration
            && context.sceneGeneration == sceneGeneration && context.mode == state.mode;
    });
    uint64_t id = 0;
    if (found != s_contexts.end()) id = found->executionContextId;
    else {
        if (s_contexts.size() == MAX_DESCRIPTORS) return 0;
        id = ++s_nextContextId;
        s_contexts.push_back({id, state.runtimeEpoch, sceneGeneration, state.dllGeneration, state.mode});
    }
    if (std::find(s_frameContexts.begin(), s_frameContexts.end(), id) == s_frameContexts.end()) s_frameContexts.push_back(id);
    return id;
}

void Retire()
{
    std::unordered_set<uint64_t> keep;
    for (const auto& item : s_snapshot.descriptors) keep.insert(item.sampleKey);
    std::vector<ScriptProfileDescriptor> retained;
    retained.reserve(MAX_DESCRIPTORS);
    for (auto& item : s_descriptors)
        if (keep.contains(item.sampleKey)) retained.push_back(std::move(item));
    std::unordered_map<uint64_t, const ScriptProfileDescriptor*> retainedByKey;
    for (const auto& item : retained) retainedByKey.emplace(item.sampleKey, &item);
    std::vector<std::pair<DescriptorKey, uint64_t>> keys;
    keys.reserve(s_descriptorKeys.size());
    for (const auto& [key, value] : s_descriptorKeys) {
        const auto item = retainedByKey.find(value);
        if (item != retainedByKey.end())
            keys.push_back({{key.script, key.context, key.kind, item->second->callbackLabel}, value});
    }
    s_descriptorKeys.clear();
    s_descriptors.swap(retained);
    for (const auto& key : keys) s_descriptorKeys.emplace(key);
    s_stringBytes = 0;
    std::unordered_set<uint64_t> usedContexts;
    for (const auto& item : s_descriptors) {
        usedContexts.insert(item.executionContextId);
        s_stringBytes += StringBytes(item);
    }
    for (const auto& context : s_snapshot.contexts) usedContexts.insert(context.executionContextId);
    s_contexts.erase(std::remove_if(s_contexts.begin(), s_contexts.end(), [&](const auto& item) {
        return !usedContexts.contains(item.executionContextId);
    }), s_contexts.end());
    /// @note 型 ID を DLL 世代内で再利用せず、退役した世代の型名だけを解放する。
    for (auto it = s_typeIds.begin(); it != s_typeIds.end();) {
        if (it->first.first != s_dllGeneration
            && std::none_of(s_contexts.begin(), s_contexts.end(), [&](const auto& item) { return item.dllGeneration == it->first.first; }))
            it = s_typeIds.erase(it);
        else ++it;
    }
    for (const auto& [key, id] : s_typeIds) { (void)id; s_stringBytes += key.second.capacity() + 1; }
}
} /// @note namespace

bool ScriptProfiler::IsRecording() { return s_recorder.IsRecording(); }
void ScriptProfiler::RequestRecording(bool recording) { s_recorder.RequestRecording(recording); }
void ScriptProfiler::RequestClear() { s_recorder.RequestClear(); }
const ScriptProfileSnapshot& ScriptProfiler::GetSnapshot() { return s_snapshot; }
uint64_t ScriptProfiler::GetRuntimeEpoch() { return s_runtimeEpoch; }
uint64_t ScriptProfiler::GetDllGeneration() { return s_dllGeneration; }
const char* ScriptProfiler::GetModeName(ScriptProfileMode mode) { return mode == ScriptProfileMode::PLAY ? "play" : "edit"; }

const char* ScriptProfiler::GetCallbackName(ScriptCallbackKind kind)
{
    static constexpr const char* names[] = {
        "Unknown", "OnAwake", "OnStart", "OnEnable", "OnDisable", "OnUpdate", "OnFixedUpdate", "OnLateUpdate", "OnDestroy",
        "OnDrawGizmos", "OnDrawGizmosSelected", "OnPreRender", "OnPostRender", "OnSetupRenderPasses",
        "OnCollisionEnter", "OnCollisionStay", "OnCollisionExit", "OnTriggerEnter", "OnTriggerStay", "OnTriggerExit",
        "OnNavMeshDestinationReached", "OnNavMeshPathFailed", "OnNavMeshTargetSpotted", "OnNavMeshTargetLost",
        "OnAnimationEvent", "OnAnimatorMove", "OnSequenceEvent", "OnSequenceFinished",
        "OnValidate", "OnBeforeSerialize", "OnAfterDeserialize", "OnSpawn", "OnDespawn",
        "Deferred", "FrameDelay", "EventHandler", "CoroutineStep"
    };
    const size_t index = static_cast<size_t>(kind);
    return index < sizeof(names) / sizeof(names[0]) ? names[index] : "Unknown";
}

void ScriptProfiler::AdvanceRuntimeEpoch(bool advanceDllGeneration)
{
    ++s_runtimeEpoch;
    if (advanceDllGeneration) ++s_dllGeneration;
    if (s_recorder.IsFrameOpen()) s_transitionFrame = true;
}
void ScriptProfiler::SetExecutionMode(bool inPlayMode)
{
    const auto mode = inPlayMode ? ScriptProfileMode::PLAY : ScriptProfileMode::EDIT;
    if (s_mode == mode) return;
    AdvanceRuntimeEpoch();
    s_mode = mode;
}
void ScriptProfiler::RegisterInstance(const Script& script)
{
    if (s_instances.contains(&script) || s_instances.size() == MAX_DESCRIPTORS) return;
    s_instances.emplace(&script, InstanceState{s_runtimeEpoch, s_dllGeneration, s_mode});
}
void ScriptProfiler::RebindInstance(const Script& script)
{
    const auto state = s_instances.find(&script);
    if (state != s_instances.end() && state->second.runtimeEpoch == s_runtimeEpoch
        && state->second.dllGeneration == s_dllGeneration && state->second.mode == s_mode) return;
    ForgetInstance(script);
    RegisterInstance(script);
}
void ScriptProfiler::ForgetInstance(const Script& script)
{
    s_instances.erase(&script);
    for (auto it = s_descriptorKeys.begin(); it != s_descriptorKeys.end();) {
        if (it->first.script == &script) it = s_descriptorKeys.erase(it);
        else ++it;
    }
}
void ScriptProfiler::ObserveScene(const Scene& scene)
{
    if (!IsRecording() || !s_recorder.IsFrameOpen()) return;
    if (!Context(scene.GetRenderSceneGeneration(), {s_runtimeEpoch, s_dllGeneration, s_mode}))
        s_recorder.Push(0, 0.0, profiler::ProfileSampleKind::INSTANT);
}

void ScriptProfiler::BeginFrame(uint64_t applicationFrameSerial, double frameStartMs)
{
    s_frameContexts.clear();
    s_transitionFrame = false;
    if (s_descriptors.size() > MAX_DESCRIPTORS / 2 || s_stringBytes > MAX_STRING_BYTES / 2
        || s_contexts.size() > MAX_DESCRIPTORS / 2) Retire();
    s_recorder.BeginFrame(applicationFrameSerial, frameStartMs);
    if (s_snapshot.captureSessionId != s_recorder.GetSnapshot().captureSessionId) s_snapshot = {};
    s_snapshot.recording = IsRecording();
    s_snapshot.captureSessionId = s_recorder.GetSnapshot().captureSessionId;
}

ScriptProfileInvocation ScriptProfiler::BeginInvocation(const Script& script, ScriptCallbackKind kind, const char* label)
{
    ScriptProfileInvocation invocation;
    if (IsRecording()) {
        uint64_t keyId = 0;
        if (s_recorder.IsFrameOpen()) {
            RegisterInstance(script);
            const auto state = s_instances.find(&script);
            const uint64_t contextId = state == s_instances.end() ? 0
                : Context(script.m_scene ? script.m_scene->GetRenderSceneGeneration() : 0, state->second);
            const char* name = label ? label : GetCallbackName(kind);
            const DescriptorKey key{&script, contextId, kind, name};
            const auto existing = s_descriptorKeys.find(key);
            if (existing != s_descriptorKeys.end()) keyId = existing->second;
            else if (contextId && s_descriptors.size() < MAX_DESCRIPTORS) {
                const auto* go = script.m_gameObject;
                const char* typeName = script.GetTypeName();
                if (!typeName) typeName = "Script";
                const std::string_view instanceId = go ? std::string_view(go->instanceId) : std::string_view{};
                const std::string_view objectName = go ? std::string_view(go->name) : std::string_view{};
                const size_t estimatedBytes = FreshStringBytes(script.InspectionId()) + FreshStringBytes(instanceId) +
                    FreshStringBytes(objectName) + FreshStringBytes(typeName) + FreshStringBytes(name);
                const auto typeKey = std::make_pair(state->second.dllGeneration, std::string_view(typeName));
                auto type = s_typeIds.find(typeKey);
                const size_t estimatedTypeBytes = type == s_typeIds.end() ? FreshStringBytes(typeKey.second) : 0;
                if (estimatedBytes + estimatedTypeBytes <= MAX_STRING_BYTES - s_stringBytes
                    && (type != s_typeIds.end() || s_typeIds.size() < MAX_DESCRIPTORS)) {
                    ScriptProfileDescriptor candidate;
                    candidate.executionContextId = contextId;
                    candidate.callbackKind = kind;
                    candidate.inspectionId = script.InspectionId();
                    candidate.typeName = typeName;
                    candidate.callbackLabel = name;
                    if (go) {
                        candidate.objectEntity = go->GetID();
                        candidate.instanceId = instanceId;
                        candidate.objectName = objectName;
                    }
                    std::pair<uint64_t, std::string> ownedType;
                    if (type == s_typeIds.end()) ownedType = {typeKey.first, std::string(typeKey.second)};
                    const size_t bytes = StringBytes(candidate);
                    const size_t typeBytes = type == s_typeIds.end() ? ownedType.second.capacity() + 1 : 0;
                    if (bytes + typeBytes <= MAX_STRING_BYTES - s_stringBytes) {
                        if (s_descriptors.capacity() < MAX_DESCRIPTORS) s_descriptors.reserve(MAX_DESCRIPTORS);
                        candidate.sampleKey = keyId = ++s_nextDescriptorId;
                        if (type == s_typeIds.end()) type = s_typeIds.emplace(std::move(ownedType), ++s_nextTypeId).first;
                        candidate.typeId = type->second;
                        s_descriptors.push_back(std::move(candidate));
                        const auto& descriptor = s_descriptors.back();
                        s_descriptorKeys.emplace(DescriptorKey{&script, contextId, kind, descriptor.callbackLabel}, keyId);
                        s_stringBytes += bytes + typeBytes;
                    }
                }
            }
        }
        invocation.token = s_recorder.Begin(keyId);
        invocation.scriptCheckpoint = s_recorder.Checkpoint();
    }
    invocation.performanceCheckpoint = profiler::Profiler::Checkpoint();
    return invocation;
}

void ScriptProfiler::EndInvocation(ScriptProfileInvocation invocation, bool succeeded)
{
    if (!succeeded) {
        profiler::Profiler::Recover(invocation.performanceCheckpoint);
        s_recorder.Recover(invocation.scriptCheckpoint);
    }
    s_recorder.End(invocation.token, succeeded ? profiler::ProfileSampleStatus::COMPLETE : profiler::ProfileSampleStatus::FAULTED);
}

void ScriptProfiler::EndFrame()
{
    if (!s_recorder.IsFrameOpen()) return;
    s_recorder.EndFrame();
    static_cast<profiler::ProfileSnapshot&>(s_snapshot) = s_recorder.GetSnapshot();
    s_snapshot.transitionFrame = s_transitionFrame;
    s_snapshot.contexts.clear();
    s_snapshot.descriptors.clear();
    std::unordered_set<uint64_t> used;
    for (const auto& sample : s_snapshot.samples) used.insert(sample.sampleKey);
    for (const auto& item : s_descriptors)
        if (used.contains(item.sampleKey)) s_snapshot.descriptors.push_back(item);
    for (const auto& item : s_contexts)
        if (std::find(s_frameContexts.begin(), s_frameContexts.end(), item.executionContextId) != s_frameContexts.end())
            s_snapshot.contexts.push_back(item);
}

Script* ScriptProfiler::ResolveTarget(Scene& scene, const ScriptProfileDescriptor& descriptor, const ScriptProfileContext& context)
{
    if (context.runtimeEpoch != s_runtimeEpoch || context.dllGeneration != s_dllGeneration || context.mode != s_mode
        || context.sceneGeneration != scene.GetRenderSceneGeneration()) return nullptr;
    auto* go = scene.GetGameObject(descriptor.objectEntity);
    if (!go || go->instanceId != descriptor.instanceId) return nullptr;
    for (const auto& [instance, state] : s_instances) {
        if (state.runtimeEpoch == context.runtimeEpoch && state.dllGeneration == context.dllGeneration && state.mode == context.mode
            && instance->m_scene == &scene && instance->m_gameObject == go && instance->InspectionId() == descriptor.inspectionId)
            return const_cast<Script*>(instance);
    }
    return nullptr;
}

} /// @note namespace fbzz::scene
