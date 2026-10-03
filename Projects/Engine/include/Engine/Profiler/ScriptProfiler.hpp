/// @file    ScriptProfiler.hpp
/// @brief   スクリプト別計測と世代を所有する診断 snapshot。
/// @author  Hasegawa Jin
/// @date    2026-10-03
#pragma once

#include <Core/Profiler/Profiler.hpp>
#include <Engine/Scene/Entity.hpp>
#include <Engine/Scene/ScriptCallbackKind.hpp>
#include <cstdint>
#include <string>
#include <vector>

namespace fbzz::scene {

class Script;
class Scene;
enum class ScriptProfileMode : uint8_t { EDIT, PLAY };

struct ScriptProfileContext {
    uint64_t executionContextId = 0;
    uint64_t runtimeEpoch = 0;
    uint64_t sceneGeneration = 0;
    uint64_t dllGeneration = 0;
    ScriptProfileMode mode = ScriptProfileMode::EDIT;
};

struct ScriptProfileDescriptor {
    uint64_t sampleKey = 0;
    uint64_t executionContextId = 0;
    uint64_t typeId = 0;
    ScriptCallbackKind callbackKind = ScriptCallbackKind::UNKNOWN;
    EntityID objectEntity;
    std::string inspectionId;
    std::string instanceId;
    std::string objectName;
    std::string typeName;
    std::string callbackLabel;
};

struct ScriptProfileSnapshot : profiler::ProfileSnapshot {
    bool transitionFrame = false;
    std::vector<ScriptProfileContext> contexts;
    std::vector<ScriptProfileDescriptor> descriptors;
};

/// @note 両 checkpoint は自身の Script token を含む境界。Recover は子だけを中断する。
struct ScriptProfileInvocation {
    profiler::ProfileToken token;
    profiler::ProfileCheckpoint scriptCheckpoint;
    profiler::ProfilerCheckpoint performanceCheckpoint;
};

/// @pre 全操作は main thread。Script と Scene のレイアウトには profiler 状態を追加しない。
class ScriptProfiler {
public:
    static bool IsRecording();
    static void RequestRecording(bool recording);
    static void RequestClear();
    static void BeginFrame(uint64_t applicationFrameSerial, double frameStartMs);
    static void EndFrame();
    static const ScriptProfileSnapshot& GetSnapshot();
    static ScriptProfileInvocation BeginInvocation(const Script& script, ScriptCallbackKind kind, const char* label);
    /// @note callback 後に対象を参照せず、token だけで区間と障害を確定する。
    static void EndInvocation(ScriptProfileInvocation invocation, bool succeeded);
    static const char* GetCallbackName(ScriptCallbackKind kind);
    static const char* GetModeName(ScriptProfileMode mode);
    /// @note プロジェクト切替と実体再生成を伴う DLL 更新試行で呼ぶ。rollback も旧 epoch へ戻さない。
    static void AdvanceRuntimeEpoch(bool advanceDllGeneration = false);
    static uint64_t GetRuntimeEpoch();
    static uint64_t GetDllGeneration();
    static void SetExecutionMode(bool inPlayMode);
    static void ObserveScene(const Scene& scene);
    /// @note 実体の寿命表だけを更新する。無効時に descriptor と名前は解決しない。
    static void RegisterInstance(const Script& script);
    static void RebindInstance(const Script& script);
    static void ForgetInstance(const Script& script);
    /// @return 全世代と UUID/Entity/InspectionId が一致する現在の対象。無ければ nullptr。
    static Script* ResolveTarget(Scene& scene, const ScriptProfileDescriptor& descriptor, const ScriptProfileContext& context);
};

} /// @note namespace fbzz::scene
