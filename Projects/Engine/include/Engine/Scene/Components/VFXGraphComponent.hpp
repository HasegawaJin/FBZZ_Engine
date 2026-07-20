// FBZZ Engine
// VFXGraphComponent.hpp | fbzz::scene
// .vfxグラフの再生設定と生成済みノードのランタイム状態を保持する
#pragma once

#include <Engine/Asset/VFXParameter.hpp>
#include <Engine/Scene/Entity.hpp>
#include <Engine/Scene/Script.hpp>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace fbzz::asset { struct VFXGraphAsset; }

namespace fbzz::scene {

struct VFXRuntimeNodeState {
    int nodeId = 0;
    EntityID entity = EntityID::INVALID;
    float startTime = 0.0f;
    float endTime = 0.0f;
    float scheduledStartTime = 0.0f;
    float scheduledEndTime = 0.0f;
    bool active = false;
};

struct VFXRuntimeLinkState {
    int fromNode = 0;
    int toNode = 0;
    std::uint8_t trigger = 0;
    float delay = 0.0f;
    bool fired = false;
};

struct VFXGraphComponent {
    bool enabled = true;
    std::string graphPath;
    bool playOnAwake = true;
    bool loop = false;
    float speed = 1.0f;
    std::string variant;
    std::vector<asset::VFXParamOverride> parameterOverrides;
    std::string serializedParameterOverrides;

    // ランタイム状態はReflectしないため、シーン保存やInspectorのUndo対象に混入しない。
    bool playing = false;
    bool stopped = false;
    bool initialized = false;
    bool reloadRequested = false;
    bool restartRequested = false;
    float playTime = 0.0f;
    float graphDuration = 0.0f;
    std::string loadedGraphPath;
    std::vector<VFXRuntimeNodeState> runtimeNodes;
    std::vector<VFXRuntimeLinkState> runtimeLinks;
    std::uint64_t editorPreviewFrame = 0;
    float editorScrubTime = -1.0f;
    int nestingDepth = 0;
    bool hasEventLinks = false;
    std::vector<asset::VFXParamOverride> resolvedParameters;
    // ロード済みauthoring graphを実行中だけ共有し、動的値ソースを毎フレーム再評価する。
    // Scene/Prefabへは保存せず、reload/stop時に破棄するランタイムキャッシュ。
    std::shared_ptr<asset::VFXGraphAsset> runtimeGraph;

    const char* GetTypeName() const { return "VFX Graph"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        r.Field("graphPath", graphPath);
        r.Field("playOnAwake", playOnAwake);
        r.Field("loop", loop);
        r.FloatRange("speed", speed, 0.0f, 8.0f);
        r.Field("variant", variant);
        serializedParameterOverrides = asset::SerializeVFXOverrides(parameterOverrides);
        r.Field("parameterOverrides", serializedParameterOverrides);
        asset::DeserializeVFXOverrides(serializedParameterOverrides, parameterOverrides);
    }

    void Restart()
    {
        playing = true;
        stopped = false;
        playTime = 0.0f;
        restartRequested = true;
    }

    void Pause() { playing = false; stopped = false; }
    void Resume() { playing = true; stopped = false; }
    void Stop() { playing = false; stopped = true; }
};

} // namespace fbzz::scene
