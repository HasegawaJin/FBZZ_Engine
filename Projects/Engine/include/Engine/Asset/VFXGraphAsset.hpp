// FBZZ Engine
// VFXGraphAsset.hpp | fbzz::asset
// 複数の描画・音響エフェクトを時間依存DAGとして束ねる.vfxアセット定義
#pragma once

#include <Engine/Asset/VFXParameter.hpp>
#include <Engine/Scene/Components/ParticleEmitter.hpp>
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>
#include <cstdint>
#include <string>
#include <vector>

namespace fbzz::asset {

enum class VFXNodeType : std::uint8_t {
    Entry = 0,
    Delay,
    Particle,
    Trail,
    MeshTrail,
    Light,
    Audio,
    Decal,
    SubGraph,
};

enum class VFXLinkTrigger : std::uint8_t {
    OnComplete = 0,
    OnStart,
    OnCollision,
    OnDeath,
};

struct VFXTrailSettings {
    // MeshTrailノードでは残像元の静的Meshとして使う。Trailノードでは未使用。
    std::string meshPath;
    std::string materialPath;
    std::string texturePath;
    math::Vector4 colorStart = { 1.0f, 1.0f, 1.0f, 1.0f };
    math::Vector4 colorEnd = { 1.0f, 1.0f, 1.0f, 0.0f };
    float lifetime = 1.0f;
    float widthStart = 0.2f;
    float widthEnd = 0.02f;
    bool beamMode = false;
    math::Vector3 beamStart = math::Vector3::ZERO;
    math::Vector3 beamEnd = { 0.0f, 0.0f, 5.0f };
};

struct VFXLightSettings {
    math::Vector3 color = { 1.0f, 0.7f, 0.3f };
    float intensity = 4.0f;
    float range = 8.0f;
};

struct VFXAudioSettings {
    std::string clipPath;
    float volume = 1.0f;
    float pitch = 1.0f;
    float spatialBlend = 1.0f;
    bool loop = false;
};

struct VFXDecalSettings {
    std::string albedoPath;
    std::string normalPath;
    std::string emissivePath;
    math::Vector4 color = { 1.0f, 1.0f, 1.0f, 1.0f };
    float normalStrength = 1.0f;
    float emissiveScale = 0.0f;
    float fadeTime = 0.25f;
};

struct VFXSubGraphSettings {
    std::string graphPath;
};

struct VFXGraphNode {
    int id = 0;
    VFXNodeType type = VFXNodeType::Particle;
    std::string name = "Effect";
    float editorX = 0.0f;
    float editorY = 0.0f;
    float startOffset = 0.0f;
    float duration = 1.0f;
    math::Vector3 localPosition = math::Vector3::ZERO;
    math::Vector3 localRotationDegrees = math::Vector3::ZERO;
    math::Vector3 localScale = math::Vector3::ONE;
    // 空ならowner追従。指定時はowner配下のBoneComponent名をsocketとして明示追従する。
    std::string attachBone;
    // WHY: シーン用ParticleEmitterと同じauthoring型を使い、VFX Graphだけ機能差や保存漏れが
    //      生じる二重実装を避ける。GPUハンドル等のランタイム値はアセットcodecが除外する。
    scene::ParticleEmitter particle;
    VFXTrailSettings trail;
    VFXLightSettings light;
    VFXAudioSettings audio;
    VFXDecalSettings decal;
    VFXSubGraphSettings subGraph;
};

struct VFXGraphLink {
    int fromNode = 0;
    int toNode = 0;
    VFXLinkTrigger trigger = VFXLinkTrigger::OnComplete;
    float delay = 0.0f;
};

struct VFXGraphAsset {
    int version = 3;
    std::string name = "VFX Graph";
    // 最悪時のリソース膨張を制作段階で可視化するアセット単位のbudget。
    int maxParticles = 100000;
    int maxLights = 8;
    int maxAudioVoices = 16;
    std::vector<VFXGraphNode> nodes;
    std::vector<VFXGraphLink> links;
    std::vector<VFXParamDefinition> parameters;
    std::vector<VFXParamBinding> bindings;
    std::vector<VFXVariantSet> variants;
    std::vector<VFXSubGraphForward> subGraphForwards;
    std::vector<VFXSignalNode> signalNodes;
    std::vector<VFXSignalOutput> signalOutputs;
};

struct VFXGraphBudgetStats {
    int particles = 0;
    int lights = 0;
    int audioVoices = 0;
};

[[nodiscard]] bool LoadVFXGraphAsset(const std::string& path,
                                     VFXGraphAsset& outAsset,
                                     std::string* outError = nullptr);
// Editor/AIの修復用途。TOMLとして読めればDAGが不正でも構造を返す。
[[nodiscard]] bool ParseVFXGraphAsset(const std::string& path,
                                      VFXGraphAsset& outAsset,
                                      std::string* outError = nullptr);
[[nodiscard]] bool SaveVFXGraphAsset(const std::string& path,
                                     const VFXGraphAsset& asset,
                                     std::string* outError = nullptr);
[[nodiscard]] bool ValidateVFXGraphAsset(const VFXGraphAsset& asset,
                                         std::string* outError = nullptr);
// DAGをトポロジカル評価し、nodesと同じ順序の開始時刻とグラフ全長を返す。
[[nodiscard]] bool BuildVFXGraphSchedule(const VFXGraphAsset& asset,
                                         std::vector<float>& outStartTimes,
                                         float& outDuration,
                                         std::string* outError = nullptr);
[[nodiscard]] const char* VFXNodeTypeName(VFXNodeType type);
[[nodiscard]] const char* VFXLinkTriggerName(VFXLinkTrigger trigger);
[[nodiscard]] VFXGraphBudgetStats CalculateVFXGraphBudget(const VFXGraphAsset& asset);

} // namespace fbzz::asset
