/// @file    LightProbeBakePass.cpp
/// @brief   Light Probe Volume の各プローブ位置で 6 面を描き、L2 球面調和へ射影して SH ボリュームへ書く。
/// @author  Hasegawa Jin
/// @date    2026-09-19
/// @note 1 プローブ = 6 面ぶんのシーン描画なので、probesPerFrame 個ずつ複数フレームへ分けて焼く。
/// @note 面は深度付きの 2D RT。キューブ RT は深度を持たず、壁の向こうの物体が描画順しだいで手前に出て空の遮蔽が崩れるため使わない。
/// @note 射影 CS は生のボリュームへ書き、膨張 CS が壁に埋まったプローブを周りで埋めて描画用ボリュームを作る。
/// @see Docs/design/light-probe-gi.md
#include "RenderScenePassHelpers.hpp"
#include <Graphics/Effects/RenderProbeInput.hpp>
#include <Engine/Renderer/ComputeCall.hpp>
#include <Engine/Renderer/DrawCall.hpp>
#include <Engine/Scene/Components/LightProbeVolumeComponent.hpp>
#include <Engine/Scene/Components/MaterialComponent.hpp>
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Math/Frustum.hpp>
#include <Math/MathUtils.hpp>
#include <Math/Matrix4.hpp>
#include <algorithm>
#include <cmath>
#include <vector>

namespace fbzz::scene {
namespace {

constexpr int kSHSlices = 7;
void RestartBake(LightProbeVolumeComponent& volume)
{
    volume.runtimeCursor = 0;
    volume.runtimePass   = 0;
    volume.runtimeBaking = true;
}

/// @brief 焼き結果を左右する設定の要約。箱と格子は別に見ている。
uint64_t SettingsKey(const LightProbeVolumeComponent& volume)
{
    const auto quantize = [](float v) {
        return static_cast<uint64_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 1000.0f));
    };
    uint64_t key = static_cast<uint64_t>(std::clamp(volume.captureResolution, 8, 128));
    key = key * 16u + static_cast<uint64_t>(std::clamp(volume.bounces, 1, 8));
    key = key * 1024u + quantize(volume.deringing);
    key = key * 2u + (volume.rejectInsideGeometry ? 1u : 0u);
    return key + 1u;
}

/// @brief 格子と面の解像度に合わせて GPU 資源を用意する。
/// @return 焼けない (資源を作れない) なら false。
bool EnsureRuntime(renderer::ResourceManager& resources, LightProbeVolumeComponent& volume)
{
    const auto grid = volume.ClampedGrid();
    if (!volume.runtimeVolume.IsValid() || !volume.runtimeRawVolume.IsValid() || volume.runtimeGrid != grid) {
        for (auto* texture : { &volume.runtimeVolume, &volume.runtimeRawVolume }) {
            if (texture->IsValid()) resources.Release(*texture);
            *texture = resources.CreateComputeTexture3D(
                static_cast<uint32_t>(grid[0]), static_cast<uint32_t>(grid[1]),
                static_cast<uint32_t>(grid[2] * kSHSlices));
        }
        volume.runtimeGrid  = grid;
        /// @note 作り直した直後の中身は未定義。1 回目を焼き終えるまで描画には使わない。
        volume.runtimeReady = false;
        RestartBake(volume);
    }
    const int faceSize = std::clamp(volume.captureResolution, 8, 128);
    const auto missing = [](const auto& faces) {
        return std::any_of(faces.begin(), faces.end(), [](const auto& face) { return !face.IsValid(); });
    };
    if (missing(volume.runtimeFaces) || missing(volume.runtimeFacing) || volume.runtimeFaceSize != faceSize) {
        for (auto* faces : { &volume.runtimeFaces, &volume.runtimeFacing }) {
            for (auto& face : *faces) {
                if (face.IsValid()) resources.Release(face);
                face = resources.CreateRenderTarget(static_cast<uint32_t>(faceSize), static_cast<uint32_t>(faceSize), 1);
            }
        }
        volume.runtimeFaceSize = faceSize;
    }
    return volume.runtimeVolume.IsValid() && volume.runtimeRawVolume.IsValid()
        && !missing(volume.runtimeFaces) && !missing(volume.runtimeFacing);
}

math::Vector3 BoxMin(const GameObject& owner, const LightProbeVolumeComponent& volume)
{
    const math::Vector3 extents{ std::max(volume.boxExtents.x, 0.01f), std::max(volume.boxExtents.y, 0.01f),
                                 std::max(volume.boxExtents.z, 0.01f) };
    return owner.transform.worldPosition - extents;
}

math::Vector3 BoxSize(const LightProbeVolumeComponent& volume)
{
    return { std::max(volume.boxExtents.x, 0.01f) * 2.0f, std::max(volume.boxExtents.y, 0.01f) * 2.0f,
             std::max(volume.boxExtents.z, 0.01f) * 2.0f };
}

/// @brief 箱までの距離の二乗。中なら 0。
float DistanceSqToBox(const math::Vector3& point, const math::Vector3& boxMin, const math::Vector3& boxSize)
{
    const float dx = std::max({ boxMin.x - point.x, 0.0f, point.x - (boxMin.x + boxSize.x) });
    const float dy = std::max({ boxMin.y - point.y, 0.0f, point.y - (boxMin.y + boxSize.y) });
    const float dz = std::max({ boxMin.z - point.z, 0.0f, point.z - (boxMin.z + boxSize.z) });
    return dx * dx + dy * dy + dz * dz;
}

} /// @note namespace

void FillLightProbeVolumeConstants(const GameObject& owner, const LightProbeVolumeComponent& volume,
                                   ProbeVolumeParamsCB& data)
{
    const math::Vector3 boxSize = BoxSize(volume);
    data.boxMin     = BoxMin(owner, volume);
    data.intensity  = std::max(volume.intensity, 0.0f);
    data.invSize    = { 1.0f / boxSize.x, 1.0f / boxSize.y, 1.0f / boxSize.z };
    data.fade       = std::max(volume.edgeFade, 0.0f);
    for (int axis = 0; axis < 3; ++axis)
        data.grid[axis] = static_cast<uint32_t>(volume.runtimeGrid[axis]);
    data.normalBias = std::max(volume.normalBias, 0.0f);
}

LightProbeVolumeSelection ExecuteLightProbeBakePass(RenderPassContext& ctx, const AdvancedGraphicsCB& mainData)
{
    auto& h = ctx.handles;
    auto& resources = ctx.resources;
    const bool canBake = h.lightProbeProjectCS.IsValid() && h.lightProbeProjectCB.IsValid()
        && h.lightProbeCaptureFrameCB.IsValid() && h.lightProbeCaptureAdvancedCB.IsValid();

    struct Candidate { LightProbeVolumeSelection::Entry entry; float distanceSq; float volumeSize; };
    std::vector<Candidate> ready;

    for (auto& go : ctx.scene.GameObjects()) {
        auto* volume = go.GetComponent<LightProbeVolumeComponent>();
        if (!volume || !volume->enabled || !go.activeInHierarchy() || !canBake) continue;
        if (!EnsureRuntime(resources, *volume)) continue;
        if (volume->bakeRequested) {
            volume->bakeRequested = false;
            RestartBake(*volume);
        }
        /// @note 箱が動いた・大きさが変わった・焼きの設定が変わったら焼き直す。古い結果は焼き上がるまで使う。
        const math::Vector3 boxMin = BoxMin(go, *volume);
        const math::Vector3 boxSize = BoxSize(*volume);
        if ((boxMin - volume->runtimeBoxMin).LengthSq() > 1.0e-6f
            || (boxSize - volume->runtimeBoxSize).LengthSq() > 1.0e-6f) {
            volume->runtimeBoxMin = boxMin;
            volume->runtimeBoxSize = boxSize;
            RestartBake(*volume);
        }
        if (const uint64_t key = SettingsKey(*volume); key != volume->runtimeSettingsKey) {
            volume->runtimeSettingsKey = key;
            RestartBake(*volume);
        }

        if (volume->runtimeBaking) {
            /// @note 捕捉で描く面は、メインビューと同じ IBL・天候を受けつつ «メインカメラの画面» に依存する項を切る。
            /// @note 画面空間 AO / 接触影は別視点の画素を読むことになり、面に無関係な黒が焼き込まれる。
            AdvancedGraphicsCB capture = mainData;
            capture.screenAoStrength = 0.0f;
            capture.screenContactShadowStrength = 0.0f;
            capture.probeVolumes[0] = {};
            capture.probeVolumes[1] = {};
            capture.probeSpecularOcclusion = std::clamp(volume->specularOcclusion, 0.0f, 1.0f);
            /// @note 2 回目以降は前回の結果を受けた面を描くので、反射が 1 回ずつ増える (同じボリュームを読み書きしながら進めるので、途中は新旧が混ざる)。
            const bool multiBounce = volume->bounces > 1 && volume->runtimeReady && volume->runtimePass > 0;
            if (multiBounce) FillLightProbeVolumeConstants(go, *volume, capture.probeVolumes[0]);
            resources.Update(h.lightProbeCaptureAdvancedCB, &capture, sizeof(capture));
            resources.Update(h.lightCB, &ctx.lightData, sizeof(ctx.lightData));

            const auto probeInput = multiBounce ? volume->runtimeVolume
                                                : renderer::ResourceHandle<renderer::TextureTag>{};
            renderer::RenderLightProbeInput input;
            input.sourceIndex = go.GetID().index;
            input.sourceGeneration = go.GetID().generation;
            input.boxMin = boxMin; input.boxSize = boxSize;
            input.runtimeGrid = volume->runtimeGrid;
            input.runtimeFaces = volume->runtimeFaces; input.runtimeFacing = volume->runtimeFacing;
            input.runtimeRawVolume = volume->runtimeRawVolume; input.runtimeVolume = volume->runtimeVolume;
            input.runtimeFaceSize = volume->runtimeFaceSize;
            input.rejectInsideGeometry = volume->rejectInsideGeometry; input.deringing = volume->deringing;
            const int probeCount = volume->ProbeCount();
            const int budget = std::clamp(volume->probesPerFrame, 1, 256);
            for (int n = 0; n < budget && volume->runtimeCursor < probeCount; ++n)
                renderer::BakeLightProbe(ctx, input, volume->runtimeCursor++, probeInput);
            renderer::DilateLightProbeVolume(ctx, input);

            if (volume->runtimeCursor >= probeCount) {
                volume->runtimeCursor = 0;
                volume->runtimeReady = true;
                ++volume->runtimePass;
                if (volume->runtimePass >= std::clamp(volume->bounces, 1, 8)) {
                    /// @note 常時更新は «前回の結果を受けて焼く» を回し続ける。反射の回数は収束した値に落ち着く。
                    volume->runtimeBaking = volume->realtimeUpdate;
                    volume->runtimePass = std::clamp(volume->bounces, 1, 8);
                }
            }
        }

        if (!volume->runtimeReady) continue;
        ready.push_back({ { &go, volume }, DistanceSqToBox(ctx.camera.m_position, boxMin, boxSize),
                          boxSize.x * boxSize.y * boxSize.z });
    }

    /// @note 画素ごとに引けるのは 2 つまで。カメラに近い 2 つを選び、小さい箱を内側 (上に重ねる側) にする。
    std::sort(ready.begin(), ready.end(), [](const Candidate& a, const Candidate& b) {
        return a.distanceSq != b.distanceSq ? a.distanceSq < b.distanceSq : a.volumeSize < b.volumeSize;
    });
    LightProbeVolumeSelection selected;
    if (ready.empty()) return selected;
    if (ready.size() == 1) {
        selected.inner = ready[0].entry;
        return selected;
    }
    const bool firstIsInner = ready[0].volumeSize <= ready[1].volumeSize;
    selected.inner = (firstIsInner ? ready[0] : ready[1]).entry;
    selected.outer = (firstIsInner ? ready[1] : ready[0]).entry;
    return selected;
}

} /// @note namespace fbzz::scene
