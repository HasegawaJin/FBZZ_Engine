/// @file    ScriptProfiler.hpp
/// @brief   スクリプト別計測と世代を所有する診断 snapshot。
/// @author  Hasegawa Jin
/// @date    2026-10-03
#pragma once
#include <Engine/Api.hpp>

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
    FBZZ_ENGINE_API static bool IsRecording();
    FBZZ_ENGINE_API static void RequestRecording(bool recording);
    FBZZ_ENGINE_API static void RequestClear();
    FBZZ_ENGINE_API static void BeginFrame(uint64_t applicationFrameSerial, double frameStartMs);
    FBZZ_ENGINE_API static void EndFrame();
    FBZZ_ENGINE_API static const ScriptProfileSnapshot& GetSnapshot();
    FBZZ_ENGINE_API static ScriptProfileInvocation BeginInvocation(const Script& script, ScriptCallbackKind kind, const char* label);
    /// @note callback 後に対象を参照せず、token だけで区間と障害を確定する。
    FBZZ_ENGINE_API static void EndInvocation(ScriptProfileInvocation invocation, bool succeeded);
    FBZZ_ENGINE_API static const char* GetCallbackName(ScriptCallbackKind kind);
    FBZZ_ENGINE_API static const char* GetModeName(ScriptProfileMode mode);
    /// @note プロジェクト切替と実体再生成を伴う DLL 更新試行で呼ぶ。rollback も旧 epoch へ戻さない。
    FBZZ_ENGINE_API static void AdvanceRuntimeEpoch(bool advanceDllGeneration = false);
    FBZZ_ENGINE_API static uint64_t GetRuntimeEpoch();
    FBZZ_ENGINE_API static uint64_t GetDllGeneration();
    FBZZ_ENGINE_API static void SetExecutionMode(bool inPlayMode);
    FBZZ_ENGINE_API static void ObserveScene(const Scene& scene);
    /// @note 実体の寿命表だけを更新する。無効時に descriptor と名前は解決しない。
    FBZZ_ENGINE_API static void RegisterInstance(const Script& script);
    FBZZ_ENGINE_API static void RebindInstance(const Script& script);
    FBZZ_ENGINE_API static void ForgetInstance(const Script& script);
    /// @return 全世代と UUID/Entity/InspectionId が一致する現在の対象。無ければ nullptr。
    FBZZ_ENGINE_API static Script* ResolveTarget(Scene& scene, const ScriptProfileDescriptor& descriptor, const ScriptProfileContext& context);
};

} /// @note namespace fbzz::scene
