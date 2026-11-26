/// @file    RenderSceneMaterialDemandTrace.cpp
/// @brief   ビュー抽出のマテリアル需要を記録し、CSV と集計状態を出力する。
/// @author  Hasegawa Jin
/// @date    2026-11-21
#include "RenderSceneMaterialDemand.hpp"

#include <Engine/Scene/Components/Lighting/LightProbeVolumeComponent.hpp>
#include <Engine/Scene/Components/Geometry/MaterialComponent.hpp>
#include <Engine/Scene/Components/Geometry/MeshRenderer.hpp>
#include <Engine/Scene/Components/Lighting/ReflectionProbeComponent.hpp>
#include <Engine/Scene/Components/Geometry/SkinnedMeshRenderer.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Rendering/Passes/RenderPassContext.hpp>
#include <Graphics/Renderer/Resources/ResourceManager.hpp>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <string>

namespace fbzz::scene::render_extraction_detail {

namespace {

/// @note 出力先が未設定なら行バッファを初期化せず、診断用のシーン走査も行わない。
struct MaterialDemandTraceConfig {
    std::string path;
    uint32_t warmup = 120;
    uint32_t samples = 240;
    uint64_t requestedSamples = 240;
    MaterialDemandTraceConfig()
    {
        if (const char* value = std::getenv("FBZZ_MATERIAL_DEMAND_TRACE_OUTPUT")) {
            path = value;
        }
        if (const char* value = std::getenv("FBZZ_MATERIAL_DEMAND_TRACE_WARMUP")) {
            warmup = static_cast<uint32_t>((std::min)(std::strtoul(value, nullptr, 10), 100000ul));
        }
        if (const char* value = std::getenv("FBZZ_MATERIAL_DEMAND_TRACE_SAMPLES")) {
            const auto requested = std::strtoul(value, nullptr, 10);
            requestedSamples = requested > 0 ? requested : 240;
            samples = static_cast<uint32_t>((std::min)(requestedSamples, uint64_t{512}));
        }
    }
};

const MaterialDemandTraceConfig& MaterialDemandConfig()
{
    static const MaterialDemandTraceConfig config;
    return config;
}

struct MaterialDemandCaptureData;
/// @pre 受付を締め切り、すべての計測スコープが終了している。出力中に行は更新されない。
void WriteMaterialDemandCapture(const MaterialDemandCaptureData& capture);

struct MaterialDemandCaptureData {
    static constexpr uint32_t CAPACITY = 512;
    struct Ordinal {
        uint64_t owner = 0, epoch = 0, frame = 0, value = 0;
    };
    std::mutex mutex;
    std::array<material_demand_detail::Row, CAPACITY> rows{};
    std::array<Ordinal, 64> ordinals{};
    uint32_t warmed = 0, reserved = 0, finished = 0, activeScopes = 0;
    bool sealed = false, writeStarted = false;
    ~MaterialDemandCaptureData()
    {
        // 未完了の計測を終了時に出力するのは、行を書き込む抽出スコープが存在しない場合に限る。
        bool exportReady = false;
        {
            std::lock_guard lock(mutex);
            if (writeStarted) {
                return;
            }
            sealed = true;
            writeStarted = true;
            exportReady = activeScopes == 0;
        }
        if (!exportReady) {
            std::fprintf(stderr, "MaterialDemandTrace: incomplete capture has active extraction scopes; export withheld.\n");
            return;
        }
        WriteMaterialDemandCapture(*this);
    }
};

MaterialDemandCaptureData& MaterialDemandCapture()
{
    static MaterialDemandCaptureData capture;
    return capture;
}

void WriteMaterialDemandCapture(const MaterialDemandCaptureData& capture)
{
    const auto& config = MaterialDemandConfig();
    std::ofstream csv(config.path + ".material-demand.csv", std::ios::binary);
    bool first = true;
#define FBZZ_MATERIAL_DEMAND_HEADER(name) if (!first) csv << ','; first = false; csv << #name;
    FBZZ_MATERIAL_DEMAND_COLUMNS(FBZZ_MATERIAL_DEMAND_HEADER)
#undef FBZZ_MATERIAL_DEMAND_HEADER
    csv << '\n';
    uint32_t incomplete = 0, unknownOrdinals = 0, changedIdentities = 0;
    for (uint32_t index = 0; index < capture.reserved; ++index) {
        const auto& row = capture.rows[index];
        if (row.complete == 0) {
            ++incomplete;
        }
        if (row.ordinalKnown == 0) {
            ++unknownOrdinals;
        }
        if (row.identityChangedDuringInvocation != 0) {
            ++changedIdentities;
        }
        first = true;
#define FBZZ_MATERIAL_DEMAND_VALUE(name) if (!first) csv << ','; first = false; csv << row.name;
        FBZZ_MATERIAL_DEMAND_COLUMNS(FBZZ_MATERIAL_DEMAND_VALUE)
#undef FBZZ_MATERIAL_DEMAND_VALUE
        csv << '\n';
    }
    csv.flush();
    csv.close();
    const bool csvWritten = csv.good();
    const bool capacityLimited = config.requestedSamples > MaterialDemandCaptureData::CAPACITY;
    const bool completed = csvWritten && capture.reserved == config.samples && incomplete == 0
        && unknownOrdinals == 0 && changedIdentities == 0 && !capacityLimited;
    std::ofstream status(config.path + ".material-demand.status.json", std::ios::binary);
    status << "{\n  \"complete\":" << (completed ? "true" : "false")
        << ",\n  \"csvWritten\":" << (csvWritten ? "true" : "false")
        << ",\n  \"rowCapacity\":" << MaterialDemandCaptureData::CAPACITY
        << ",\n  \"requestedInvocations\":" << config.requestedSamples
        << ",\n  \"sampleInvocations\":" << config.samples
        << ",\n  \"warmupInvocations\":" << config.warmup
        << ",\n  \"capturedRows\":" << capture.reserved
        << ",\n  \"finishedRows\":" << capture.finished
        << ",\n  \"incompleteRows\":" << incomplete
        << ",\n  \"droppedRows\":0"
        << ",\n  \"capacityLimited\":" << (capacityLimited ? "true" : "false")
        << ",\n  \"unknownOrdinalRows\":" << unknownOrdinals
        << ",\n  \"identityChangedRows\":" << changedIdentities
        << ",\n  \"activeScopesAtExport\":0,\n  \"sealed\":true\n}\n";
    status.flush();
    status.close();
    if (!csvWritten || !status.good()) {
        std::fprintf(stderr, "MaterialDemandTrace: CSV/status write failed for configured output.\n");
    }
}

thread_local uint64_t s_materialDemandDepth = 0;

}

MaterialDemandTraceScope::MaterialDemandTraceScope(const RenderPassContext& context)
{
    const auto& config = MaterialDemandConfig();
    if (config.path.empty()) {
        return;
    }
    auto& capture = MaterialDemandCapture();
    const auto owner = context.resources.GetInstanceId();
    const auto epoch = context.resources.GetResetVersion();
    const auto frame = context.frameStamp;
    {
        std::lock_guard lock(capture.mutex);
        if (capture.writeStarted) {
            return;
        }
        ++capture.activeScopes;
        m_tracked = true;
        ++s_materialDemandDepth;
        uint64_t passOrdinal = 0;
        for (auto& ordinal : capture.ordinals) {
            if (ordinal.owner != 0 && ordinal.owner != owner) {
                continue;
            }
            if (ordinal.owner == 0 || ordinal.epoch != epoch || ordinal.frame != frame) {
                ordinal = {owner, epoch, frame, 0};
            }
            passOrdinal = ++ordinal.value;
            break;
        }
        if (capture.warmed < config.warmup) {
            ++capture.warmed;
        }
        else if (!capture.sealed) {
            m_index = capture.reserved++;
            m_row.emplace();
            auto& row = *m_row;
            row.resourceInstanceId = owner;
            row.resourceEpoch = epoch;
            row.resourceFrame = frame;
            row.sceneGeneration = context.scene.GetRenderSceneGeneration();
            row.reentryDepth = s_materialDemandDepth - 1;
            row.passOrdinalWithinResourceFrame = passOrdinal;
            row.ordinalKnown = passOrdinal != 0;
            if (capture.reserved == config.samples) {
                capture.sealed = true;
            }
        }
    }
    if (!m_row) {
        return;
    }
    auto& row = *m_row;
    row.sceneObjects = context.scene.GameObjectCount();
    row.sceneMeshRenderers = context.scene.GetEntities<MeshRenderer>().size();
    row.sceneSkinnedRenderers = context.scene.GetEntities<SkinnedMeshRenderer>().size();
    row.sceneMaterialComponents = context.scene.GetEntities<MaterialComponent>().size();
    row.userPassesAtExtraction = context.scene.GetUserRenderPasses().size();
    const bool hasProbes = !context.scene.GetEntities<ReflectionProbeComponent>().empty()
        || !context.scene.GetEntities<LightProbeVolumeComponent>().empty();
    if (hasProbes) {
        for (auto& object : context.scene.GameObjects()) {
            ++row.probeCensusObjects;
            if (!object.activeInHierarchy()) {
                continue;
            }
            if (const auto* probe = object.GetComponent<ReflectionProbeComponent>(); probe && probe->enabled
                && probe->captureMode == ReflectionProbeCaptureMode::DynamicScene) {
                ++row.activeReflectionProbes;
            }
            if (const auto* volume = object.GetComponent<LightProbeVolumeComponent>(); volume && volume->enabled) {
                ++row.activeLightProbeVolumes;
            }
        }
    }
}

MaterialDemandTraceScope::~MaterialDemandTraceScope()
{
    if (!m_tracked) {
        return;
    }
    auto& capture = MaterialDemandCapture();
    bool exportCapture = false;
    {
        std::lock_guard lock(capture.mutex);
        if (m_row) {
            capture.rows[m_index] = *m_row;
            ++capture.finished;
        }
        --capture.activeScopes;
        if (capture.sealed && capture.activeScopes == 0 && !capture.writeStarted) {
            capture.writeStarted = true;
            exportCapture = true;
        }
    }
    --s_materialDemandDepth;
    if (exportCapture) {
        WriteMaterialDemandCapture(capture);
    }
}

void MaterialDemandTraceScope::Complete(const RenderPassContext& context)
{
    if (!m_row) {
        return;
    }
    m_row->identityChangedDuringInvocation = m_row->resourceEpoch != context.resources.GetResetVersion()
        || m_row->sceneGeneration != context.scene.GetRenderSceneGeneration() || m_row->resourceFrame != context.resources.FrameStamp();
    m_row->complete = 1;
}

void MaterialDemandTraceScope::CountPotential(const material_demand_detail::View& view, const renderer::RenderMeshItem& item,
    const material_demand_detail::Result& result)
{
    if (m_row->syncEligibleItems == 1) {
        m_row->consumerScopeCertified = view.consumerScopeCertified;
    }
    else {
        m_row->consumerScopeCertified = m_row->consumerScopeCertified != 0 && view.consumerScopeCertified;
    }
    if (result.KnownConsumerPotential()) {
        m_row->knownPotentialDeclaredVertices += item.vertexCount;
    }
    material_demand_detail::CountPotential(*m_row, result);
}

}
