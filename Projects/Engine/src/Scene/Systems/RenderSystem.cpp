// FBZZ Engine
// RenderSystem.cpp | fbzz::scene
// Scene から DrawCall を生成するオーケストレーター
// 各描画パスの実装は RenderPasses/ 以下の Execute*Pass 関数に委譲する。
#include "Engine/Scene/Systems/RenderSystem.hpp"
// ResolveGameCullingSettings — 呼び出し側が明示しなかったときのフォールバック解決に使う。
#include "Engine/Scene/SceneUtils.hpp"
#include "Engine/Scene/Systems/RenderPasses/Geometry/TerrainRenderPass.hpp"
#include "Engine/Scene/Systems/RenderPasses/Geometry/WaterRenderPass.hpp"
#include "Engine/Scene/Systems/RenderPasses/Geometry/DetailRenderPass.hpp"
#include "Engine/Scene/Systems/RenderPasses/Geometry/FoliageRenderPass.hpp"
#include "Engine/Scene/Systems/RenderPasses/Geometry/MeshTrailRenderPass.hpp"
#include "Engine/Scene/Systems/RenderPasses/Geometry/TrailRenderPass.hpp"
#include "Engine/Renderer/RenderSettings.hpp"
#include "Engine/Renderer/RenderDebugOverlay.hpp"
#include "Engine/Renderer/DebugDraw.hpp"
#include "RenderPasses/Debug/DebugPasses.hpp"
#include <Physics/World.hpp>
#include "RenderPasses/Geometry/GeometryPasses.hpp"
#include "RenderPasses/PostProcess/PostProcessPasses.hpp"
#include "RenderPasses/PostProcess/CloudNoiseBake.hpp"
#include <Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp>
#include "RenderPasses/Debug/SelectionPasses.hpp"
#include "Engine/Core/Time.hpp"
#include "Engine/Core/Logger.hpp"
#include "Engine/Scene/Scene.hpp"
#include "Engine/Scene/ScriptComponent.hpp"
#include "Engine/Scene/Transform.hpp"
#include "Engine/Scene/Components/LightComponent.hpp"
#include "Engine/Scene/Components/EnvironmentLightComponent.hpp"
#include "Engine/Scene/Components/AtmosphericScatteringComponent.hpp"
#include "Engine/Scene/Components/SkyRenderer.hpp"
#include "Engine/Scene/Components/PostProcessVolumeComponent.hpp"
#include "Engine/Renderer/PostProcessBlend.hpp"
#include "Engine/Scene/Components/VFXScreenEffect.hpp"
#include "Engine/Scene/Components/ReflectionProbeComponent.hpp"
#include "Engine/Scene/Components/MeshRenderer.hpp"
#include "Engine/Scene/Components/SkinnedMeshRenderer.hpp"
#include "Engine/Scene/Components/TerrainComponent.hpp"
#include "Engine/Asset/AssetManager.hpp"
#include "Engine/Renderer/IRenderer.hpp"
#include "Engine/Renderer/Camera.hpp"
#include "Engine/Renderer/LightSystem.hpp"
#include "Engine/Renderer/Mesh.hpp"
#include "Engine/Renderer/PrimitiveMesh.hpp"
#include "Engine/Scene/Systems/RenderPasses/RenderPipeline.hpp"
#include "Engine/Renderer/DrawCall.hpp"
#include "Engine/Renderer/RenderState.hpp"
#include "Engine/Renderer/ResourceManager.hpp"
#include "Engine/Asset/Skeleton.hpp"
#include <Engine/Profiler/ProfileScope.hpp>
#include <Math/Frustum.hpp>
#include <Math/Matrix4.hpp>
#include <Math/Quaternion.hpp>
#include <Math/Vector3.hpp>
#include <algorithm>
#include <bit>
#include <cassert>
#include <cmath>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fbzz::scene {

namespace {

struct SceneShadowBounds {
    math::Vector3 center = math::Vector3::ZERO;
    float radius = 0.0f;
    bool valid = false;
};

constexpr uint32_t PROCEDURAL_LUT_SIZE = 32u;

uint64_t HashLutSettings(const renderer::LUTColorGradingSettings& settings)
{
    uint64_t hash = 1469598103934665603ull;
    const auto append = [&](float value) {
        hash ^= std::bit_cast<uint32_t>(value);
        hash *= 1099511628211ull;
    };
    append(settings.contrast);
    append(settings.saturation);
    append(settings.hueShift);
    append(settings.temperature);
    append(settings.tint);
    return hash;
}

// RendererがサンプルするLDR RGB座標と同じ順序で32^3 RGBA8 LUTを生成する。
// WHAT: x=R, y=G, z=B、xが最速で並ぶD3D11 Texture3Dのメモリ配置にする。
std::vector<uint8_t> GenerateProceduralColorLut(const renderer::LUTColorGradingSettings& settings)
{
    constexpr float PI = 3.14159265358979323846f;
    const float angle = settings.hueShift * (PI / 180.0f);
    const float s = std::sin(angle);
    const float c = std::cos(angle);
    const float hue[3][3] = {
        { 0.299f + 0.701f*c + 0.168f*s, 0.587f - 0.587f*c + 0.330f*s, 0.114f - 0.114f*c - 0.497f*s },
        { 0.299f - 0.299f*c - 0.328f*s, 0.587f + 0.413f*c + 0.035f*s, 0.114f - 0.114f*c + 0.292f*s },
        { 0.299f - 0.300f*c + 1.250f*s, 0.587f - 0.588f*c - 1.050f*s, 0.114f + 0.886f*c - 0.203f*s },
    };
    const float balance[3] = {
        (std::max)(1.0f + settings.temperature * 0.08f - settings.tint * 0.03f, 0.0f),
        (std::max)(1.0f + settings.tint * 0.06f, 0.0f),
        (std::max)(1.0f - settings.temperature * 0.08f - settings.tint * 0.03f, 0.0f),
    };

    std::vector<uint8_t> pixels(PROCEDURAL_LUT_SIZE * PROCEDURAL_LUT_SIZE * PROCEDURAL_LUT_SIZE * 4u);
    const auto toUnorm = [](float value) {
        return static_cast<uint8_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
    };
    const float denominator = static_cast<float>(PROCEDURAL_LUT_SIZE - 1u);
    for (uint32_t b = 0; b < PROCEDURAL_LUT_SIZE; ++b) {
        for (uint32_t g = 0; g < PROCEDURAL_LUT_SIZE; ++g) {
            for (uint32_t r = 0; r < PROCEDURAL_LUT_SIZE; ++r) {
                const float input[3] = { r / denominator, g / denominator, b / denominator };
                const float balanced[3] = {
                    input[0] * balance[0], input[1] * balance[1], input[2] * balance[2]
                };
                float color[3] = {
                    hue[0][0]*balanced[0] + hue[0][1]*balanced[1] + hue[0][2]*balanced[2],
                    hue[1][0]*balanced[0] + hue[1][1]*balanced[1] + hue[1][2]*balanced[2],
                    hue[2][0]*balanced[0] + hue[2][1]*balanced[1] + hue[2][2]*balanced[2],
                };
                for (float& channel : color)
                    channel = (channel - 0.5f) * (1.0f + settings.contrast) + 0.5f;
                const float luma = color[0] * 0.2126f + color[1] * 0.7152f + color[2] * 0.0722f;
                for (float& channel : color)
                    channel = luma + (channel - luma) * settings.saturation;

                const size_t index = ((static_cast<size_t>(b) * PROCEDURAL_LUT_SIZE + g) *
                                      PROCEDURAL_LUT_SIZE + r) * 4u;
                pixels[index + 0] = toUnorm(color[0]);
                pixels[index + 1] = toUnorm(color[1]);
                pixels[index + 2] = toUnorm(color[2]);
                pixels[index + 3] = 255u;
            }
        }
    }
    return pixels;
}

// Viewport ごとに解像度依存の中間リソースを保持する。
// WHY: Scene View と Game View は解像度が異なるため、単一の static RT 群を共有すると
//      1 フレーム内で互いのサイズへリサイズし続け、D3D11 リソース生成待ちが発生する。
struct ViewRenderTargets {
    // View ごとの RenderGraph 計画と transient RT をフレーム間で保持する。
    // WHY: RenderPipeline をスタック生成すると DX12 の descriptor heap と committed resource を
    //      毎フレーム再生成するため、GPU が空いていても CPU がボトルネックになる。
    RenderPipeline pipeline;
    renderer::ResourceHandle<renderer::RenderTargetTag> hdr;
    renderer::ResourceHandle<renderer::RenderTargetTag> ldr;
    renderer::ResourceHandle<renderer::RenderTargetTag> selectionMask;
    renderer::ResourceHandle<renderer::RenderTargetTag> outline;
    renderer::ResourceHandle<renderer::RenderTargetTag> customPostProcess[2];
    renderer::ResourceHandle<renderer::RenderTargetTag> gbuffer;
    renderer::ResourceHandle<renderer::RenderTargetTag> decalDepth;
    renderer::ResourceHandle<renderer::RenderTargetTag> decalMask;
    renderer::ResourceHandle<renderer::TextureTag> bloomHalf;
    renderer::ResourceHandle<renderer::TextureTag> bloomFull;
    renderer::ResourceHandle<renderer::TextureTag> ssaoRaw;
    renderer::ResourceHandle<renderer::TextureTag> ssaoBlur;
    // ---- Advanced Graphics (解像度依存・ビュー単位) ----
    // WHY: これらは解像度変更時に再生成が必要なため ViewRenderTargets に含める。
    //      static なリソース (BRDF LUT 等) は別途 static 変数で管理する。
    renderer::ResourceHandle<renderer::TextureTag>        ssrResult;           // SSR CS 出力
    renderer::ResourceHandle<renderer::TextureTag>        volumetricResult;    // Volumetric CS 出力
    renderer::ResourceHandle<renderer::RenderTargetTag>   taaHistoryA;         // TAA ping-pong A
    renderer::ResourceHandle<renderer::RenderTargetTag>   taaHistoryB;         // TAA ping-pong B
    renderer::ResourceHandle<renderer::TextureTag>        motionBlurResult;    // Motion Blur CS 出力
    renderer::ResourceHandle<renderer::TextureTag>        gtaoRaw;             // GTAO RAW CS 出力
    renderer::ResourceHandle<renderer::TextureTag>        gtaoBlur;            // GTAO Blur CS 出力
    renderer::ResourceHandle<renderer::TextureTag>        contactShadowResult; // Contact Shadow CS 出力
    // ---- ビュー別定数バッファ / 再投影行列 ----
    // WHY: sPrevVP を static で共有すると複数ビュー (SceneView + GameView) で
    //      フレームをまたいで互いのカメラ行列を誤って参照し、MotionBlur / TAA が
    //      常に壊れた再投影を行う。viewKey で分離した ViewRenderTargets に持つことで正しく分離する。
    renderer::ResourceHandle<renderer::ConstantBufferTag> advancedGraphicsCB;
    math::Matrix4 prevViewProjection    = math::Matrix4::Identity();
    math::Matrix4 invPrevViewProjection = math::Matrix4::Identity();
    // TAA ジッター列の現在位置。ビュー別に持たないと SceneView と GameView が
    // 同じ番号を取り合って、どちらもサンプル点が飛び飛びになる。
    uint32_t taaFrameIndex = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

// Halton 列 (基数 base) の index 番目。
// WHY: TAA のサンプル点は「少ない枚数でもピクセル内に偏りなく散る」必要がある。
//      乱数だと数フレームでは固まりが出るが、Halton は低食い違い量列なので均等に埋まる。
float HaltonRadicalInverse(uint32_t index, uint32_t base)
{
    const float invBase = 1.0f / static_cast<float>(base);
    float result   = 0.0f;
    float fraction = invBase;
    while (index > 0u) {
        result   += static_cast<float>(index % base) * fraction;
        index    /= base;
        fraction *= invBase;
    }
    return result;
}

// Resize 前のネイティブリソースを ResourceManager から確実に解放する。
void ReleaseViewRenderTargets(ViewRenderTargets& targets, renderer::ResourceManager& resources)
{
    // transient RT も同じ Viewport 寿命に属するため、固定 RT より先に明示解放する。
    targets.pipeline.ReleaseTransientPool(resources);
    if (targets.hdr.IsValid())                  resources.Release(targets.hdr);
    if (targets.ldr.IsValid())                  resources.Release(targets.ldr);
    if (targets.selectionMask.IsValid())        resources.Release(targets.selectionMask);
    if (targets.outline.IsValid())              resources.Release(targets.outline);
    if (targets.customPostProcess[0].IsValid()) resources.Release(targets.customPostProcess[0]);
    if (targets.customPostProcess[1].IsValid()) resources.Release(targets.customPostProcess[1]);
    if (targets.gbuffer.IsValid())              resources.Release(targets.gbuffer);
    if (targets.decalDepth.IsValid())           resources.Release(targets.decalDepth);
    if (targets.decalMask.IsValid())            resources.Release(targets.decalMask);
    if (targets.bloomHalf.IsValid())            resources.Release(targets.bloomHalf);
    if (targets.bloomFull.IsValid())            resources.Release(targets.bloomFull);
    if (targets.ssaoRaw.IsValid())               resources.Release(targets.ssaoRaw);
    if (targets.ssaoBlur.IsValid())              resources.Release(targets.ssaoBlur);
    if (targets.ssrResult.IsValid())             resources.Release(targets.ssrResult);
    if (targets.volumetricResult.IsValid())      resources.Release(targets.volumetricResult);
    if (targets.taaHistoryA.IsValid())           resources.Release(targets.taaHistoryA);
    if (targets.taaHistoryB.IsValid())           resources.Release(targets.taaHistoryB);
    if (targets.motionBlurResult.IsValid())      resources.Release(targets.motionBlurResult);
    if (targets.gtaoRaw.IsValid())               resources.Release(targets.gtaoRaw);
    if (targets.gtaoBlur.IsValid())              resources.Release(targets.gtaoBlur);
    if (targets.contactShadowResult.IsValid())   resources.Release(targets.contactShadowResult);
    // WHY: advancedGraphicsCB / prevViewProjection は解像度非依存。
    //      リサイズのたびに破棄・再生成するとコストが発生し、prevVP が Identity にリセットされて
    //      MotionBlur / TAA が 1 フレーム乱れる。保存して復元することで連続性を維持する。
    auto savedCB         = targets.advancedGraphicsCB;
    auto savedPrevVP     = targets.prevViewProjection;
    auto savedPrevInvVP  = targets.invPrevViewProjection;
    auto savedTaaIndex   = targets.taaFrameIndex;
    targets = {};
    targets.advancedGraphicsCB  = savedCB;
    targets.prevViewProjection    = savedPrevVP;
    targets.invPrevViewProjection = savedPrevInvVP;
    targets.taaFrameIndex         = savedTaaIndex;
}

void AccumulateBounds(SceneShadowBounds& aggregate, const WorldBounds& bounds)
{
    if (bounds.radius <= 0.0f) return;
    if (!aggregate.valid) {
        aggregate.center = bounds.center;
        aggregate.radius = bounds.radius;
        aggregate.valid = true;
        return;
    }

    const math::Vector3 delta = bounds.center - aggregate.center;
    const float distance = delta.Length();
    if (distance + bounds.radius <= aggregate.radius) return;
    if (distance + aggregate.radius <= bounds.radius) {
        aggregate.center = bounds.center;
        aggregate.radius = bounds.radius;
        return;
    }

    const float newRadius = (aggregate.radius + distance + bounds.radius) * 0.5f;
    if (distance > 0.0001f) {
        aggregate.center = aggregate.center + delta * ((newRadius - aggregate.radius) / distance);
    }
    aggregate.radius = newRadius;
}

bool ComputeTerrainWorldBounds(const Transform& transform, const TerrainComponent& terrain, WorldBounds& outBounds)
{
    if (!terrain.enabled || terrain.heightData.empty() || terrain.columns < 2 || terrain.rows < 2)
        return false;

    auto [minIt, maxIt] = std::minmax_element(terrain.heightData.begin(), terrain.heightData.end());
    const float minY = (*minIt) * terrain.maxHeight;
    const float maxY = (*maxIt) * terrain.maxHeight;
    const math::Vector3 localCenter = {
        static_cast<float>(terrain.columns - 1) * terrain.cellSize * 0.5f,
        (minY + maxY) * 0.5f,
        static_cast<float>(terrain.rows - 1) * terrain.cellSize * 0.5f
    };
    const math::Vector3 localExtents = {
        static_cast<float>(terrain.columns - 1) * terrain.cellSize * 0.5f,
        (maxY - minY) * 0.5f,
        static_cast<float>(terrain.rows - 1) * terrain.cellSize * 0.5f
    };

    const math::Vector4 worldCenter = transform.GetWorldMatrix()
        * math::Vector4{ localCenter.x, localCenter.y, localCenter.z, 1.0f };
    const float maxScale = (std::max)(
        (std::max)(std::abs(transform.worldScale.x), std::abs(transform.worldScale.y)),
        std::abs(transform.worldScale.z));

    outBounds.center = { worldCenter.x, worldCenter.y, worldCenter.z };
    outBounds.radius = localExtents.Length() * (std::max)(maxScale, 0.0001f);
    return outBounds.radius > 0.0f;
}

// 視錐台スライス [nearZ, farZ] の外接球。centerDistance はカメラ前方への距離。
// WHY 外接球で包むか: カスケードの担当範囲をスライスの 8 頂点へぴったり合わせると、
//     カメラを回すたびに箱の大きさが変わり、影の精細度が回転で脈動する。
//     外接球はカメラの向きに依存しない半径を返すので、その脈動が起きない。
struct FrustumSliceSphere {
    float centerDistance = 0.0f;
    float radius         = 0.0f;
};

FrustumSliceSphere ComputeFrustumSliceSphere(const renderer::Camera& camera,
                                             float nearZ, float farZ)
{
    constexpr float DEG_TO_RAD = 0.01745329251994329577f;
    nearZ = (std::max)(nearZ, 0.01f);
    farZ  = (std::max)(farZ, nearZ + 0.01f);

    // 視錐台の対角方向の傾き。k = |(±aspect*t, ±t, 1)| の xy 成分の長さ。
    const float tanHalfFov = std::tan(camera.m_fovY * 0.5f * DEG_TO_RAD);
    const float k  = tanHalfFov * std::sqrt(1.0f + camera.m_aspect * camera.m_aspect);
    const float k2 = k * k;

    FrustumSliceSphere sphere;
    // near 面が far 面より広いほど中心は手前へ寄る。k² が十分大きいときは
    // far 面の外接円がスライス全体を包むので、中心は far 面上に載る。
    if (k2 >= (farZ - nearZ) / (farZ + nearZ)) {
        sphere.centerDistance = farZ;
        sphere.radius         = farZ * k;
        return sphere;
    }

    const float sum  = farZ + nearZ;
    const float diff = farZ - nearZ;
    sphere.centerDistance = 0.5f * sum * (1.0f + k2);
    sphere.radius = 0.5f * std::sqrt(diff * diff
                                   + 2.0f * (farZ * farZ + nearZ * nearZ) * k2
                                   + sum * sum * k2 * k2);
    return sphere;
}

// ComputeCascadeSplits — practical split scheme でカスケード境界距離を決める。
// 対数分割 (手前を細かく) と等分割 (遠方を細かく) を lambda で線形補間する。
// WHY: 対数分割だけだと最遠カスケードが担当する帯が広すぎて遠景の影が溶け、
//      等分割だけだと足元のカスケードが広すぎて肝心の近距離が粗くなる。
//      両者の中間を係数 1 本で選べるのが実務上いちばん扱いやすい。
// outSplits[i] は「カスケード i が担当する far 距離」。outSplits[count-1] == shadowDistance。
void ComputeCascadeSplits(float nearZ, float shadowDistance, int cascadeCount,
                          float lambda, float* outSplits)
{
    const float clampedLambda = std::clamp(lambda, 0.0f, 1.0f);
    const float range         = shadowDistance - nearZ;

    for (int i = 0; i < cascadeCount; ++i) {
        const float ratio = static_cast<float>(i + 1) / static_cast<float>(cascadeCount);
        const float logSplit     = nearZ * std::pow(shadowDistance / nearZ, ratio);
        const float uniformSplit = nearZ + range * ratio;
        outSplits[i] = clampedLambda * logSplit + (1.0f - clampedLambda) * uniformSplit;
    }
    // 丸め誤差で最遠が shadowDistance を下回ると、影の到達距離が設定より短くなる。
    outSplits[cascadeCount - 1] = shadowDistance;
}

SceneShadowBounds ComputeSceneShadowBounds(Scene& scene, fbzz::LayerMask cullingMask)
{
    SceneShadowBounds result{};
    for (auto& go : scene.GameObjects()) {
        if (!ShouldRenderGameObject(go, cullingMask)) continue;

        if (auto* mr = go.GetComponent<MeshRenderer>()) {
            if (mr->enabled && mr->mesh && !mr->mesh->isSkinned && mr->mesh->boundsRadius > 0.0f)
                AccumulateBounds(result, ComputeWorldBounds(go.transform, *mr->mesh));
        }

        if (auto* smr = go.GetComponent<SkinnedMeshRenderer>()) {
            if (smr->enabled && smr->model) {
                WorldBounds bounds{};
                if (ComputeSkinnedWorldBounds(go.transform, *smr, bounds))
                    AccumulateBounds(result, bounds);
            }
        }

        if (auto* terrain = go.GetComponent<TerrainComponent>()) {
            WorldBounds bounds{};
            if (ComputeTerrainWorldBounds(go.transform, *terrain, bounds))
                AccumulateBounds(result, bounds);
        }
    }
    return result;
}

} // namespace

void RenderSystem(Scene& scene,
                  renderer::IRenderer& renderer,
                  renderer::ResourceManager& resources,
                  const renderer::Camera& inputCamera,
                  renderer::ResourceHandle<renderer::RenderTargetTag> outputRT,
                  const renderer::RenderSettings* settings,
                  fbzz::LayerMask cullingMask,
                  const RenderSystemUIOptions* uiOptions,
                  const physics::World* physicsWorld,
                  const CameraCullingSettings* cullingSettings)
{
    FBZZ_PROFILE_SCOPE("RenderSystem");

    // カリング挙動は「明示指定 > シーンのメインカメラ > 既定値」の順で解決する。
    // WHY シーンから引くフォールバックを持つか: Standalone / GameHub テンプレートは
    //     RenderSystem をそのまま呼ぶだけなので、呼び出し側を書き換えなくても
    //     CameraComponent のカリング設定が効くようにしておきたい。
    const CameraCullingSettings resolvedCulling =
        cullingSettings ? *cullingSettings : ResolveGameCullingSettings(scene);

    // VFXCameraShake — VFX グラフの Camera Shake ノードによる揺れ。
    // WHY: カメラ本体の position/rotation を書き換えると DebugCamera が持つ yaw/pitch と
    //      乖離して操作が壊れる (既知の落とし穴)。描画に使うカメラのコピーだけをずらし、
    //      呼び出し側のカメラ状態には一切触れない。カリングも揺れた視点で行われるため、
    //      画面端で物が消える不整合も起きない。
    renderer::Camera shakenCamera = inputCamera;
    {
        math::Vector3 offset = math::Vector3::ZERO;
        float rollDegrees = 0.0f;
        for (auto [tf, shake] : scene.View<Transform, VFXCameraShake>()) {
            const float weight = shake.enabled ? std::clamp(shake.weight, 0.0f, 1.0f) : 0.0f;
            if (weight <= 0.0f) continue;
            // 発生源から遠いほど弱める。radius <= 0 は距離減衰なし。
            float distanceScale = 1.0f;
            if (shake.radius > 0.0f) {
                const float distance = (tf.worldPosition - inputCamera.m_position).Length();
                distanceScale = std::clamp(1.0f - distance / shake.radius, 0.0f, 1.0f);
            }
            const float amount = weight * distanceScale;
            if (amount <= 0.0f) continue;
            // 軸ごとに位相をずらした正弦の合成。決定論的で、フレームレートに依存しない。
            const float phase = shake.elapsed * shake.frequency;
            offset.x += std::sin(phase * 1.00f) * shake.amplitude * amount;
            offset.y += std::sin(phase * 1.37f + 1.7f) * shake.amplitude * amount;
            offset.z += std::sin(phase * 0.83f + 3.1f) * shake.amplitude * amount * 0.5f;
            rollDegrees += std::sin(phase * 1.11f + 0.6f) * shake.rotationAmplitude * amount;
        }
        if (offset.LengthSq() > 0.0f || rollDegrees != 0.0f) {
            constexpr float DEG_TO_RAD = 0.01745329251994329577f;
            // オフセットはカメラのローカル軸で与え、向きに依らず自然に揺れるようにする。
            shakenCamera.m_position = inputCamera.m_position
                + inputCamera.GetRight() * offset.x
                + inputCamera.GetUp() * offset.y
                + inputCamera.GetForward() * offset.z;
            shakenCamera.m_rotation = inputCamera.m_rotation
                * math::Quaternion::FromEuler({ 0.0f, 0.0f, rollDegrees * DEG_TO_RAD });
        }
    }
    const renderer::Camera& camera = shakenCamera;

    static renderer::RenderSettings sDefaultSettings;
    renderer::RenderSettings effectiveSettings = settings ? *settings : sDefaultSettings;

    // ── ルック設定の解決 ───────────────────────────────────────────
    // ベースは VolumeSettings の既定値。ProjectSettings はもうポストプロセスも
    // 高度グラフィクスも持たない (公開を撤去した) ため、ボリュームが唯一の供給源。
    // WHY 既定値から始めるか: 「ボリュームを 1 つも置いていないシーンは素の絵になる」
    //      という規則が一番説明しやすい。プロジェクト全体の隠れたベースがあると、
    //      同じプロファイルを別プロジェクトへ持ち込んだときに絵が変わる。
    renderer::VolumeSettings volumeSettings;
    if (const auto* runtimePostProcess = scene.TryGetRuntimePostProcessSettings())
        volumeSettings.post = *runtimePostProcess;

    // ── コンポーネントによる設定上書き (ProjectSettings < runtimePostProcess < Component) ───
    // EnvironmentLightComponent — シーン Inspector から IBL を上書きする。
    // 最初のアクティブなコンポーネントのみ採用する。複数置かれた場合は先着優先。
    IblSource activeIblSource = IblSource::StaticDDS; // 空連動 IBL: 採用された EnvironmentLight の source
    for (auto [tf, elc] : scene.View<Transform, EnvironmentLightComponent>()) {
        if (!elc.enabled) continue;
        activeIblSource = elc.source;
        effectiveSettings.ibl.enabled       = elc.source == IblSource::DynamicSky
            || (!elc.irradiancePath.empty() && !elc.prefilterPath.empty());
        effectiveSettings.ibl.irradiancePath = elc.irradiancePath;
        effectiveSettings.ibl.prefilterPath  = elc.prefilterPath;
        effectiveSettings.ibl.intensity      = elc.intensity;
        effectiveSettings.ibl.diffuseScale   = elc.diffuseScale;
        effectiveSettings.ibl.specularScale  = elc.specularScale;
        effectiveSettings.ibl.maxMipLevel    = elc.maxMipLevel;
        break;
    }
    // AtmosphericScatteringComponent — シーン Inspector から霧設定を上書きする。
    for (auto [tf, atm] : scene.View<Transform, AtmosphericScatteringComponent>()) {
        if (!atm.enabled) continue;
        auto& fog      = volumeSettings.post.fog;
        fog.enabled    = atm.fogEnabled;
        fog.source     = static_cast<int>(atm.fogSource);
        fog.density    = atm.fogDensity;
        fog.farDistance = atm.fogFar;
        fog.color[0]   = atm.fogColor.x;
        fog.color[1]   = atm.fogColor.y;
        fog.color[2]   = atm.fogColor.z;
        break;
    }
    // ── PostProcessVolumeComponent の合成 ────────────────────────────────────
    // ベース (既定値 / runtime 上書き) の上に、有効なボリュームを
    // priority 昇順で重み付きにブレンドしていく。
    //
    // WHY priority で明示的に並べるか: View の走査順に依存させると、
    //      GameObject を作り直しただけで重なり順が変わり、見た目が非決定的になる。
    {
        struct VolumeEntry {
            const PostProcessVolumeComponent* volume  = nullptr;
            const asset::PostProcessProfile*  profile = nullptr;
            float weight = 0.0f;
        };
        std::vector<VolumeEntry> entries;

        // 距離判定はシェイク適用後のカメラ位置で行う。
        // WHY: シェイク量は数十 cm 程度で、influenceRadius に対して無視できる。
        //      別の位置を使い分けるより、実際に描画している視点で統一する方が単純。
        const math::Vector3 viewPosition = camera.m_position;

        for (auto [tf, ppv] : scene.View<Transform, PostProcessVolumeComponent>()) {
            if (!ppv.enabled) continue;

            // プロファイル未アサイン / 参照切れのボリュームは何も適用しない。
            // WHY 既定値へフォールバックしないか: 参照が切れたボリュームが
            //      「素のルック」を主張すると、priority 次第で他のボリュームを
            //      打ち消してしまう。壊れている事実は Inspector の警告に留める。
            const asset::PostProcessProfile* resolved = ppv.Resolve();
            if (!resolved) continue;

            float weight = std::clamp(ppv.blendWeight, 0.0f, 1.0f);
            if (!ppv.isGlobal) {
                const float distance = (viewPosition - tf.worldPosition).Length();
                weight *= renderer::PostProcessVolumeDistanceWeight(
                    distance, ppv.influenceRadius, ppv.blendDistance);
            }
            if (weight <= 0.0f) continue;

            entries.push_back({ &ppv, resolved, weight });
        }

        // priority 昇順。同値は安定ソートで走査順を保つ (再現性のため)。
        std::stable_sort(entries.begin(), entries.end(),
            [](const VolumeEntry& lhs, const VolumeEntry& rhs) {
                return lhs.volume->priority < rhs.volume->priority;
            });

        for (const VolumeEntry& entry : entries) {
            // プロファイルが持つオーバーライドだけが現在の合成結果へ混ざる。
            // 載っていない効果は素通しされるため、「洞窟プロファイルは Fog と
            // Color Grading の 2 つだけ持つ」という差分オーサリングが成立する。
            entry.profile->ApplyTo(volumeSettings, entry.weight);
        }

        // 解決したルックを描画用の RenderSettings へ流し込む。
        // 以降のコード (VFXScreenEffect / 各 RenderPass) は今までどおり
        // effectiveSettings.postProcess や .ssr をフラットに読む。
        renderer::ApplyVolumeSettings(volumeSettings, effectiveSettings);
    }
    // VFXScreenEffect — VFX グラフの ScreenEffect ノードが出す一時的な画面演出。
    // WHY: PostProcessVolume は「設定の差し替え」なので、爆発フラッシュのような
    //      一瞬の上乗せや、複数エフェクトの同時発生を表現できない。
    //      解決済み設定へ後段で加算することで、シーンのグレーディングを壊さずに重ねる。
    //      フラッシュだけは加算し合うと即飽和するため、最も強いものを採用する。
    {
        renderer::PostProcessSettings& pp = effectiveSettings.postProcess;
        float strongestFlash = 0.0f;
        for (auto [tf, effect] : scene.View<Transform, VFXScreenEffect>()) {
            const float weight = effect.enabled ? std::clamp(effect.weight, 0.0f, 1.0f) : 0.0f;
            if (weight <= 0.0f) continue;
            if (effect.bloomBoost > 0.0f) {
                pp.bloom.enabled = true;
                pp.bloom.intensity += effect.bloomBoost * weight;
            }
            if (effect.chromaticAberration > 0.0f) {
                pp.lens.chromaticAberrationEnabled = true;
                pp.lens.chromaticAberration += effect.chromaticAberration * weight;
            }
            if (effect.lensDistortion != 0.0f) {
                pp.lens.distortionEnabled = true;
                pp.lens.distortion += effect.lensDistortion * weight;
            }
            if (effect.vignette > 0.0f) {
                pp.vignette.enabled = true;
                pp.vignette.intensity += effect.vignette * weight;
            }
            const float flash = effect.flashIntensity * weight;
            if (flash > strongestFlash) {
                strongestFlash = flash;
                pp.screenFadeColor[0] = effect.flashColor.x;
                pp.screenFadeColor[1] = effect.flashColor.y;
                pp.screenFadeColor[2] = effect.flashColor.z;
            }
        }
        if (strongestFlash > 0.0f)
            pp.screenFadeAlpha = std::clamp(pp.screenFadeAlpha + strongestFlash, 0.0f, 1.0f);
    }
    // NOTE: ReflectionProbeComponent は将来の局所反射ブレンド実装で使用予定。
    //       現時点は Inspector / Serializer のみ対応し、RenderSystem での適用は未実装。

    const renderer::RenderSettings& rs = effectiveSettings;

    // 静的ハンドルの検証・デバイスリセット復旧・初回バッファ生成をまとめて計測する。
    profiler::Profiler::BeginSample(
        profiler::ProfilerMarker("RenderSystem::StaticResourceSetup", "Rendering"));

    // =========================================================================
    // 静的リソースの遅延初期化
    // =========================================================================
    static uint64_t sResourceResetVersion = resources.GetResetVersion();
    static uint32_t sShadowMapResolution  = 0u;
    static renderer::ResourceHandle<renderer::RenderTargetTag> shadowMapRT;
    static auto shadowShader         = resources.LoadShader("Assets/Shaders/Pipeline/Shadow/ShadowMap.hlsl");
    static auto skinnedShadowShader  = resources.LoadShader("Assets/Shaders/Pipeline/Shadow/SkinnedShadowMap.hlsl");
    // コンピュートスキニング。無効ならスキンド描画は従来の VS スキニング経路へ落ちる。
    static auto skinningComputeCS    = resources.LoadShader("Assets/Shaders/Pipeline/Skinning/SkinningCompute.cs.hlsl");

    // スキンドメッシュに AnimatorComponent がない場合のアイデンティティボーンパレット
    static renderer::ResourceHandle<renderer::ConstantBufferTag> bindPoseSkinningCB;
    if (!bindPoseSkinningCB.IsValid()) {
        struct BindPoseData { math::Matrix4 bones[asset::MAX_SKINNING_BONES]; };
        BindPoseData bp{};
        for (auto& m : bp.bones) m = math::Matrix4::Identity();
        bindPoseSkinningCB = resources.CreateConstantBuffer(sizeof(BindPoseData));
        resources.Update(bindPoseSkinningCB, &bp, sizeof(BindPoseData));
    }

    static auto compositeShader         = resources.LoadShader("Assets/Shaders/PostProcess/Color/Composite.hlsl");
    static auto causticsShader          = resources.LoadShader("Assets/Shaders/PostProcess/Water/Caustics.hlsl");
    static auto volumetricCloudShader   = resources.LoadShader("Assets/Shaders/PostProcess/Cloud/VolumetricCloud.hlsl");
    static auto cloudUpscaleShader      = resources.LoadShader("Assets/Shaders/PostProcess/Cloud/CloudUpscale.hlsl");
    static auto ssaoShader              = resources.LoadShader("Assets/Shaders/PostProcess/AmbientOcclusion/SSAO.cs.hlsl");
    static auto ssaoBlurShader          = resources.LoadShader("Assets/Shaders/PostProcess/AmbientOcclusion/SSAOBlur.cs.hlsl");
    static auto bloomDownShader         = resources.LoadShader("Assets/Shaders/PostProcess/Bloom/BloomDownsample.cs.hlsl");
    static auto bloomUpShader           = resources.LoadShader("Assets/Shaders/PostProcess/Bloom/BloomUpsample.cs.hlsl");
    static auto selectionMaskShader     = resources.LoadShader("Assets/Shaders/Debug/SelectionMask.hlsl");
    static auto selectionMaskSkinnedShader = resources.LoadShader("Assets/Shaders/Debug/SelectionMaskSkinnedMesh.hlsl");
    static auto selectionMaskParticleShader = resources.LoadShader("Assets/Shaders/Debug/SelectionMaskParticle.hlsl");
    static auto selectionMaskParticleGpuShader = resources.LoadShader("Assets/Shaders/Debug/SelectionMaskParticleGPU.hlsl");
    static auto selectionOutlineShader  = resources.LoadShader("Assets/Shaders/PostProcess/Outline/SelectionOutline.hlsl");
    static auto fxaaShader              = resources.LoadShader("Assets/Shaders/PostProcess/AntiAliasing/FXAA.hlsl");

    // ---- Advanced Graphics シェーダー (static で初回ロード、Reset 後に再ロード) ----
    static auto iblBrdfBakeShader   = resources.LoadShader("Assets/Shaders/PostProcess/AmbientOcclusion/BRDFIntegration.cs.hlsl");
    static auto gtaoShader          = resources.LoadShader("Assets/Shaders/PostProcess/AmbientOcclusion/GTAO.cs.hlsl");
    static auto gtaoBlurShader      = resources.LoadShader("Assets/Shaders/PostProcess/AmbientOcclusion/GTAOBlur.cs.hlsl");
    static auto ssrShader           = resources.LoadShader("Assets/Shaders/PostProcess/Reflections/SSR.cs.hlsl");
    static auto volumetricShader    = resources.LoadShader("Assets/Shaders/PostProcess/Lighting/VolumetricLight.cs.hlsl");
    static auto contactShadowShader = resources.LoadShader("Assets/Shaders/PostProcess/Shadow/ContactShadows.cs.hlsl");
    static auto taaShader           = resources.LoadShader("Assets/Shaders/PostProcess/AntiAliasing/TAA.hlsl");
    static auto motionBlurShader    = resources.LoadShader("Assets/Shaders/PostProcess/Motion/MotionBlur.cs.hlsl");
    static auto lensFlareShader     = resources.LoadShader("Assets/Shaders/PostProcess/Flare/LensFlare.hlsl");
    // WHY: BRDF LUT は 512×512 の定数テーブルで、解像度・シーンが変わっても内容は変わらない。
    //      毎フレーム再生成するコストを避けるため static で一度だけ生成し、IBLBakePass でのみ書き込む。
    static auto iblBrdfLut          = resources.CreateComputeTexture(512, 512);
    static renderer::ResourceHandle<renderer::TextureTag> proceduralColorLut;
    static uint64_t proceduralColorLutHash = 0u;

    static auto skydomeShader = resources.LoadShader("Assets/Shaders/Material/Sky/Skydome.hlsl");
    static auto sunMoonShader = resources.LoadShader("Assets/Shaders/Material/Sky/SunMoon.hlsl");
    static auto skydomeMesh   = renderer::PrimitiveMesh::Sphere(resources, 32);

    // 空連動 IBL (環境システム Phase A): 空を焼くキューブマップ RT と、面ごとの view/proj 用 CB。
    // EnvironmentResources はフレームをまたいで保持し、SkyRenderer が変化した時だけ再キャプチャする。
    static constexpr uint32_t kSkyEnvCubeSize = 128;
    static EnvironmentResources sEnvironmentResources;
    static uint64_t sEnvResetVersion = resources.GetResetVersion();
    static renderer::ResourceHandle<renderer::RenderTargetTag>   skyEnvCubeRT;
    static renderer::ResourceHandle<renderer::ConstantBufferTag> skyCaptureFrameCB;
    if (!skyEnvCubeRT.IsValid() || sEnvResetVersion != resources.GetResetVersion()) {
        sEnvResetVersion  = resources.GetResetVersion();
        skyEnvCubeRT      = resources.CreateCubemapRenderTarget(kSkyEnvCubeSize, 1);
        skyCaptureFrameCB = resources.CreateConstantBuffer(sizeof(PerFrameCB));
        // リソース再生成後 (デバイスリセット等) は古い動的 IBL ハンドルが無効。クリアして焼き直す。
        sEnvironmentResources.skyEnvCube    = {};
        sEnvironmentResources.skyIrradiance = {};
        sEnvironmentResources.skyPrefilter  = {};
        sEnvironmentResources.needsConvolution = false;
        sEnvironmentResources.MarkDirty();
    }

    // ボリューメトリック雲の 3D ノイズ (Shape 128³ + Detail 32³) を起動時に 1 回だけ CPU 焼きする。
    // タイラブルなので WRAP サンプルで無限に並べられる。デバイスリセット後は SRV が無効になるため焼き直す。
    static renderer::ResourceHandle<renderer::TextureTag> cloudShapeTex;
    static renderer::ResourceHandle<renderer::TextureTag> cloudDetailTex;
    if (resources.Get(cloudShapeTex) == nullptr || resources.Get(cloudDetailTex) == nullptr) {
        const std::vector<uint8_t> shape  = cloudnoise::BakeShape(128);
        const std::vector<uint8_t> detail = cloudnoise::BakeDetail(32);
        cloudShapeTex  = resources.CreateTexture3D(shape.data(),  128, 128, 128);
        cloudDetailTex = resources.CreateTexture3D(detail.data(),  32,  32,  32);
        FBZZ_LOG_DEBUG("VolumetricCloud: baked tileable 3D noise (shape 128^3, detail 32^3)");
    }

    static auto gbufferShader          = resources.LoadShader("Assets/Shaders/Pipeline/Deferred/GBuffer.hlsl");
    static auto deferredLightingShader = resources.LoadShader("Assets/Shaders/Pipeline/Deferred/DeferredLighting.hlsl");
    static auto depthCopyShader        = resources.LoadShader("Assets/Shaders/Pipeline/Deferred/DepthCopy.hlsl");

    // クラスタライトカリング (Forward+ / Deferred+)。
    static auto clusterCullCS = resources.LoadShader("Assets/Shaders/Pipeline/Clustered/ClusterLightCull.cs.hlsl");
    // ライト配列とクラスタリストは解像度非依存の固定長なので、確保は初回の 1 回だけ。
    // WHY RW にするか: clusterIndexBuffer は CS が u2 へ書き、PS が t30 から読む。
    //      punctualLightBuffer は CPU が毎フレーム更新して CS/PS が読むだけなので読み取り専用。
    static auto punctualLightBuffer = resources.CreateStructuredBuffer(
        nullptr, kMaxPunctualLights, static_cast<uint32_t>(sizeof(PunctualLightGPU)));
    static auto clusterIndexBuffer = resources.CreateRWStructuredBuffer(
        nullptr, kClusterCount * kClusterStride, static_cast<uint32_t>(sizeof(uint32_t)));
    static auto clusterCB = resources.CreateConstantBuffer(sizeof(ClusterConstantsCB));

    static auto decalShader     = resources.LoadShader("Assets/Shaders/Material/Decal/Decal.hlsl");
    static auto decalMaskShader = resources.LoadShader("Assets/Shaders/Material/Decal/DecalMask.hlsl");
    static auto decalMaskSkinnedShader = resources.LoadShader("Assets/Shaders/Material/Decal/DecalMaskSkinned.hlsl");

    static auto particleShader      = resources.LoadShader("Assets/Shaders/Material/Effects/Particle.hlsl");
    static auto particleGpuSimCS   = resources.LoadShader("Assets/Shaders/Material/Effects/ParticleGpuSim.cs.hlsl");
    static auto particleGpuShader  = resources.LoadShader("Assets/Shaders/Material/Effects/ParticleGPU.hlsl");
    // GPU ソート 3 段 (キー生成 / グローバル段 / LDS 段)。sortMode != None のときだけ走る。
    static auto particleGpuSortKeysCS  = resources.LoadShader("Assets/Shaders/Material/Effects/ParticleGpuSortKeys.cs.hlsl");
    static auto particleGpuSortStepCS  = resources.LoadShader("Assets/Shaders/Material/Effects/ParticleGpuSortStep.cs.hlsl");
    static auto particleGpuSortLocalCS = resources.LoadShader("Assets/Shaders/Material/Effects/ParticleGpuSortLocal.cs.hlsl");
    static auto particleGpuMeshShader  = resources.LoadShader("Assets/Shaders/Material/Effects/ParticleGpuMesh.hlsl");
    // 自己影: 光源から見た密度を積む。selfShadowStrength > 0 のエミッターがあるときだけ走る。
    static auto particleSelfShadowShader = resources.LoadShader("Assets/Shaders/Material/Effects/ParticleSelfShadowDensity.hlsl");
    static auto trailShader    = resources.LoadShader("Assets/Shaders/Material/Effects/Trail.hlsl");
    static auto meshTrailShader = resources.LoadShader("Assets/Shaders/Material/Effects/MeshTrail.hlsl");
    static auto skinnedMeshTrailShader = resources.LoadShader("Assets/Shaders/Material/Effects/SkinnedMeshTrail.hlsl");
    static auto detailMeshShader      = resources.LoadShader("Assets/Shaders/Detail/Detail.hlsl");
    static auto detailBillboardShader = resources.LoadShader("Assets/Shaders/Detail/Detail.hlsl"); // 同一ソース、isBillboard フラグで切り替え
    static auto detailGrassShader     = resources.LoadShader("Assets/Shaders/Detail/DetailGrass.hlsl");
    // Deferred 用 GBuffer 書き込み変種。
    static auto detailGBufferShader      = resources.LoadShader("Assets/Shaders/Detail/DetailGBuffer.hlsl");
    static auto detailGrassGBufferShader = resources.LoadShader("Assets/Shaders/Detail/DetailGrassGBuffer.hlsl");
    static auto detailGrassCB         = resources.CreateConstantBuffer(sizeof(DetailGrassCB));
    static auto foliageShader         = resources.LoadShader("Assets/Shaders/Foliage/Foliage.hlsl");
    static auto foliageGBufferShader  = resources.LoadShader("Assets/Shaders/Foliage/FoliageGBuffer.hlsl");
    static auto detailMeshPSO   = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID,
        renderer::BlendMode::OPAQUE_BLEND,
        renderer::DepthMode::DEPTH_ON
    });
    static auto detailNoCullPSO = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID_NOCULL,
        renderer::BlendMode::OPAQUE_BLEND,
        renderer::DepthMode::DEPTH_ON
    });
    static auto foliagePSO = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID,
        renderer::BlendMode::OPAQUE_BLEND,
        renderer::DepthMode::DEPTH_ON
    });
    static auto foliageNoCullPSO = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID_NOCULL,
        renderer::BlendMode::OPAQUE_BLEND,
        renderer::DepthMode::DEPTH_ON
    });

    static auto frameCB    = resources.CreateConstantBuffer(sizeof(PerFrameCB));
    static auto objectCB   = resources.CreateConstantBuffer(sizeof(PerObjectCB));
    static auto lightCB    = resources.CreateConstantBuffer(sizeof(renderer::LightConstantsCB));
    static auto shadowCB   = resources.CreateConstantBuffer(sizeof(ShadowConstantsCB));
    // コンピュートスキニングの b0 (頂点数のみ)。16 バイト境界へ切り上げられる。
    static auto skinningCB = resources.CreateConstantBuffer(16);
    static auto postprocCB = resources.CreateConstantBuffer(sizeof(PostProcCB));
    static auto outlineCB  = resources.CreateConstantBuffer(sizeof(OutlineCB));
    static auto atmCB      = resources.CreateConstantBuffer(sizeof(AtmosphereCB));
    static auto decalCB    = resources.CreateConstantBuffer(sizeof(DecalCB));
    static auto decalMaterialCB = resources.CreateConstantBuffer(sizeof(DecalMaterialCB));
    static auto decalReceiverCB = resources.CreateConstantBuffer(sizeof(DecalReceiverCB));
    static auto volumetricCloudCB = resources.CreateConstantBuffer(176);
    // パーティクル自己影: 光源側の密度 RT と、光源行列を入れる専用 frame CB。
    // WHY: RenderPassHandles はフレームごとに作り直される値型なので、
    //      パス側で遅延生成すると毎フレーム新しい RT を作って漏らす。ここで静的に持つ。
    // NOTE: 解像度は固定。自己影が拾うのは「煙の内部で光がどれだけ減るか」という
    //       低周波の情報で、輪郭の鮮鋭さは要らない。
    static auto particleSelfShadowRT =
        resources.CreateRenderTarget(RenderPassHandles::kSelfShadowResolution, RenderPassHandles::kSelfShadowResolution, 1);
    static auto particleSelfShadowFrameCB = resources.CreateConstantBuffer(sizeof(PerFrameCB));

    // WHY: static handle は通常フレームでは再利用し、ResourceManager::Reset() 後だけ世代差分で再生成する。
    //      これによりデバイスロスト復帰時も旧ネイティブリソースへ触らない。
    static auto defaultPSO = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID,
        renderer::BlendMode::OPAQUE_BLEND,
        renderer::DepthMode::DEPTH_ON
    });
    static auto wireframePSO = resources.CreatePipelineState({
        renderer::RasterizerMode::WIREFRAME,
        renderer::BlendMode::OPAQUE_BLEND,
        renderer::DepthMode::DEPTH_ON
    });
    static auto selectionMaskPso = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID,
        renderer::BlendMode::OPAQUE_BLEND,
        renderer::DepthMode::DEPTH_ON
    });
    static auto skydomePSO = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID_NOCULL,
        renderer::BlendMode::OPAQUE_BLEND,
        renderer::DepthMode::DEPTH_SKY
    });
    static auto sunMoonPSO = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID_NOCULL,
        renderer::BlendMode::ADDITIVE,
        renderer::DepthMode::DEPTH_SKY
    });
    static auto particlePSO = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID_NOCULL,
        renderer::BlendMode::ADDITIVE,
        renderer::DepthMode::DEPTH_READ
    });
    static auto particleAlphaPSO = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID_NOCULL,
        renderer::BlendMode::ALPHA_BLEND,
        renderer::DepthMode::DEPTH_READ
    });
    static auto particlePremultipliedPSO = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID_NOCULL,
        renderer::BlendMode::PREMULTIPLIED,
        renderer::DepthMode::DEPTH_READ
    });
    static auto particleGpuPSO = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID_NOCULL,
        renderer::BlendMode::ADDITIVE,
        renderer::DepthMode::DEPTH_READ
    });
    static auto particleGpuAlphaPSO = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID_NOCULL,
        renderer::BlendMode::ALPHA_BLEND,
        renderer::DepthMode::DEPTH_READ
    });
    static auto particleGpuPremultipliedPSO = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID_NOCULL,
        renderer::BlendMode::PREMULTIPLIED,
        renderer::DepthMode::DEPTH_READ
    });
    static auto trailPSO = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID_NOCULL,
        renderer::BlendMode::ALPHA_BLEND,
        renderer::DepthMode::DEPTH_READ
    });
    static auto meshTrailPSO = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID,
        renderer::BlendMode::ALPHA_BLEND,
        renderer::DepthMode::DEPTH_READ
    });
    static auto meshTrailDoubleSidedPSO = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID_NOCULL,
        renderer::BlendMode::ALPHA_BLEND,
        renderer::DepthMode::DEPTH_READ
    });
    static auto postprocPSO = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID,
        renderer::BlendMode::OPAQUE_BLEND,
        renderer::DepthMode::DEPTH_OFF
    });
    static auto causticsPSO = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID,
        renderer::BlendMode::ADDITIVE,
        renderer::DepthMode::DEPTH_OFF
    });
    static auto volumetricCloudPSO = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID,
        renderer::BlendMode::ALPHA_BLEND,
        renderer::DepthMode::DEPTH_OFF
    });
    // VolumetricCloud.hlsl は scatter.rgb に既に透過率を積分した premultiplied 値を返す。
    // Overdraw 可視化も volumetricCloudPSO を共有するため、雲の合成だけ専用 PSO に分離する。
    static auto volumetricCloudPremultipliedPSO = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID,
        renderer::BlendMode::PREMULTIPLIED,
        renderer::DepthMode::DEPTH_OFF
    });
    // ---- Advanced Graphics PSO / 定数バッファ ----
    // taaPSO: OPAQUE — TAA は ping-pong バッファへ上書きするため α ブレンドは不要
    static auto taaPSO = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID,
        renderer::BlendMode::OPAQUE_BLEND,
        renderer::DepthMode::DEPTH_OFF
    });
    // lensFlarePSO: ADDITIVE — ゴーストはフレアを HDR バッファに加算合成する
    static auto lensFlarePSO = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID,
        renderer::BlendMode::ADDITIVE,
        renderer::DepthMode::DEPTH_OFF
    });
    static auto decalPSO = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID,
        renderer::BlendMode::ALPHA_BLEND,
        renderer::DepthMode::DEPTH_OFF
    });
    static auto decalMaskPso = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID,
        renderer::BlendMode::OPAQUE_BLEND,
        renderer::DepthMode::DEPTH_OFF
    });

    const uint32_t shadowRes = rs.shadow.mapResolution;
    if (sResourceResetVersion != resources.GetResetVersion() || sShadowMapResolution != shadowRes || !shadowMapRT.IsValid()) {
        sResourceResetVersion = resources.GetResetVersion();
        sShadowMapResolution  = shadowRes;

        shadowMapRT = resources.CreateRenderTarget(shadowRes, shadowRes, 0);
        shadowShader        = resources.LoadShader("Assets/Shaders/Pipeline/Shadow/ShadowMap.hlsl");
        skinnedShadowShader = resources.LoadShader("Assets/Shaders/Pipeline/Shadow/SkinnedShadowMap.hlsl");
        skinningComputeCS   = resources.LoadShader("Assets/Shaders/Pipeline/Skinning/SkinningCompute.cs.hlsl");
        // Mesh* / AnimatorComponent* をキーにしたキャッシュはリソースリセットで無効になる。
        ReleaseSkinningComputeCaches();
        compositeShader     = resources.LoadShader("Assets/Shaders/PostProcess/Color/Composite.hlsl");
        causticsShader      = resources.LoadShader("Assets/Shaders/PostProcess/Water/Caustics.hlsl");
        volumetricCloudShader = resources.LoadShader("Assets/Shaders/PostProcess/Cloud/VolumetricCloud.hlsl");
        cloudUpscaleShader    = resources.LoadShader("Assets/Shaders/PostProcess/Cloud/CloudUpscale.hlsl");
        ssaoShader          = resources.LoadShader("Assets/Shaders/PostProcess/AmbientOcclusion/SSAO.cs.hlsl");
        ssaoBlurShader      = resources.LoadShader("Assets/Shaders/PostProcess/AmbientOcclusion/SSAOBlur.cs.hlsl");
        bloomDownShader     = resources.LoadShader("Assets/Shaders/PostProcess/Bloom/BloomDownsample.cs.hlsl");
        bloomUpShader       = resources.LoadShader("Assets/Shaders/PostProcess/Bloom/BloomUpsample.cs.hlsl");
        selectionMaskShader = resources.LoadShader("Assets/Shaders/Debug/SelectionMask.hlsl");
        selectionMaskSkinnedShader = resources.LoadShader("Assets/Shaders/Debug/SelectionMaskSkinnedMesh.hlsl");
        selectionMaskParticleShader = resources.LoadShader("Assets/Shaders/Debug/SelectionMaskParticle.hlsl");
        selectionMaskParticleGpuShader = resources.LoadShader("Assets/Shaders/Debug/SelectionMaskParticleGPU.hlsl");
        selectionOutlineShader = resources.LoadShader("Assets/Shaders/PostProcess/Outline/SelectionOutline.hlsl");
        fxaaShader = resources.LoadShader("Assets/Shaders/PostProcess/AntiAliasing/FXAA.hlsl");
        skydomeShader = resources.LoadShader("Assets/Shaders/Material/Sky/Skydome.hlsl");
        sunMoonShader = resources.LoadShader("Assets/Shaders/Material/Sky/SunMoon.hlsl");
        skydomeMesh   = renderer::PrimitiveMesh::Sphere(resources, 32);
        gbufferShader = resources.LoadShader("Assets/Shaders/Pipeline/Deferred/GBuffer.hlsl");
        deferredLightingShader = resources.LoadShader("Assets/Shaders/Pipeline/Deferred/DeferredLighting.hlsl");
        depthCopyShader = resources.LoadShader("Assets/Shaders/Pipeline/Deferred/DepthCopy.hlsl");
        clusterCullCS = resources.LoadShader("Assets/Shaders/Pipeline/Clustered/ClusterLightCull.cs.hlsl");
        decalShader = resources.LoadShader("Assets/Shaders/Material/Decal/Decal.hlsl");
        decalMaskShader = resources.LoadShader("Assets/Shaders/Material/Decal/DecalMask.hlsl");
        decalMaskSkinnedShader = resources.LoadShader("Assets/Shaders/Material/Decal/DecalMaskSkinned.hlsl");
        // .mat の解決結果はシェーダー・テクスチャ・cbuffer のハンドルを持つ。
        ReleaseDecalMaterialCache();
        particleShader     = resources.LoadShader("Assets/Shaders/Material/Effects/Particle.hlsl");
        particleGpuSimCS  = resources.LoadShader("Assets/Shaders/Material/Effects/ParticleGpuSim.cs.hlsl");
        particleGpuShader = resources.LoadShader("Assets/Shaders/Material/Effects/ParticleGPU.hlsl");
        particleGpuSortKeysCS  = resources.LoadShader("Assets/Shaders/Material/Effects/ParticleGpuSortKeys.cs.hlsl");
        particleGpuSortStepCS  = resources.LoadShader("Assets/Shaders/Material/Effects/ParticleGpuSortStep.cs.hlsl");
        particleGpuSortLocalCS = resources.LoadShader("Assets/Shaders/Material/Effects/ParticleGpuSortLocal.cs.hlsl");
        particleGpuMeshShader  = resources.LoadShader("Assets/Shaders/Material/Effects/ParticleGpuMesh.hlsl");
        particleSelfShadowShader = resources.LoadShader("Assets/Shaders/Material/Effects/ParticleSelfShadowDensity.hlsl");
        trailShader = resources.LoadShader("Assets/Shaders/Material/Effects/Trail.hlsl");
        meshTrailShader = resources.LoadShader("Assets/Shaders/Material/Effects/MeshTrail.hlsl");
        skinnedMeshTrailShader = resources.LoadShader("Assets/Shaders/Material/Effects/SkinnedMeshTrail.hlsl");
        detailMeshShader      = resources.LoadShader("Assets/Shaders/Detail/Detail.hlsl");
        detailBillboardShader = resources.LoadShader("Assets/Shaders/Detail/Detail.hlsl");
        detailGrassShader     = resources.LoadShader("Assets/Shaders/Detail/DetailGrass.hlsl");
        detailGBufferShader      = resources.LoadShader("Assets/Shaders/Detail/DetailGBuffer.hlsl");
        detailGrassGBufferShader = resources.LoadShader("Assets/Shaders/Detail/DetailGrassGBuffer.hlsl");
        detailGrassCB         = resources.CreateConstantBuffer(sizeof(DetailGrassCB));
        foliageShader         = resources.LoadShader("Assets/Shaders/Foliage/Foliage.hlsl");
        foliageGBufferShader  = resources.LoadShader("Assets/Shaders/Foliage/FoliageGBuffer.hlsl");
        detailMeshPSO   = resources.CreatePipelineState({ renderer::RasterizerMode::SOLID,        renderer::BlendMode::OPAQUE_BLEND, renderer::DepthMode::DEPTH_ON });
        detailNoCullPSO = resources.CreatePipelineState({ renderer::RasterizerMode::SOLID_NOCULL, renderer::BlendMode::OPAQUE_BLEND, renderer::DepthMode::DEPTH_ON });
        foliagePSO       = resources.CreatePipelineState({ renderer::RasterizerMode::SOLID,        renderer::BlendMode::OPAQUE_BLEND, renderer::DepthMode::DEPTH_ON });
        foliageNoCullPSO = resources.CreatePipelineState({ renderer::RasterizerMode::SOLID_NOCULL, renderer::BlendMode::OPAQUE_BLEND, renderer::DepthMode::DEPTH_ON });

        // ---- Advanced Graphics: デバイスリセット後に再ロード ----
        iblBrdfBakeShader   = resources.LoadShader("Assets/Shaders/PostProcess/AmbientOcclusion/BRDFIntegration.cs.hlsl");
        gtaoShader          = resources.LoadShader("Assets/Shaders/PostProcess/AmbientOcclusion/GTAO.cs.hlsl");
        gtaoBlurShader      = resources.LoadShader("Assets/Shaders/PostProcess/AmbientOcclusion/GTAOBlur.cs.hlsl");
        ssrShader           = resources.LoadShader("Assets/Shaders/PostProcess/Reflections/SSR.cs.hlsl");
        volumetricShader    = resources.LoadShader("Assets/Shaders/PostProcess/Lighting/VolumetricLight.cs.hlsl");
        contactShadowShader = resources.LoadShader("Assets/Shaders/PostProcess/Shadow/ContactShadows.cs.hlsl");
        taaShader           = resources.LoadShader("Assets/Shaders/PostProcess/AntiAliasing/TAA.hlsl");
        motionBlurShader    = resources.LoadShader("Assets/Shaders/PostProcess/Motion/MotionBlur.cs.hlsl");
        lensFlareShader     = resources.LoadShader("Assets/Shaders/PostProcess/Flare/LensFlare.hlsl");
        taaPSO              = resources.CreatePipelineState({ renderer::RasterizerMode::SOLID, renderer::BlendMode::OPAQUE_BLEND, renderer::DepthMode::DEPTH_OFF });
        lensFlarePSO        = resources.CreatePipelineState({ renderer::RasterizerMode::SOLID, renderer::BlendMode::ADDITIVE,      renderer::DepthMode::DEPTH_OFF });
        // WHY: デバイスリセット後は iblBrdfLut の内容が失われるため再生成する。
        //      IBLBakePass は焼き済み対象を世代付きハンドルで追跡し、新ハンドルを次フレームで再生成する。
        iblBrdfLut          = resources.CreateComputeTexture(512, 512);

        bindPoseSkinningCB = {};
        {
            struct BindPoseData { math::Matrix4 bones[asset::MAX_SKINNING_BONES]; };
            BindPoseData bp{};
            for (auto& m : bp.bones) m = math::Matrix4::Identity();
            bindPoseSkinningCB = resources.CreateConstantBuffer(sizeof(BindPoseData));
            resources.Update(bindPoseSkinningCB, &bp, sizeof(BindPoseData));
        }
        // モデル側のリファレンスポーズ CB もデバイスリセットで失われる。
        // ハンドルを落としておけば下の遅延生成が次フレームで作り直す。
        for (auto& go : scene.GameObjects()) {
            if (auto* smr = go.GetComponent<SkinnedMeshRenderer>())
                if (smr->model) smr->model->referencePoseCB = {};
        }
        frameCB    = resources.CreateConstantBuffer(sizeof(PerFrameCB));
        objectCB   = resources.CreateConstantBuffer(sizeof(PerObjectCB));
        lightCB    = resources.CreateConstantBuffer(sizeof(renderer::LightConstantsCB));
        shadowCB   = resources.CreateConstantBuffer(sizeof(ShadowConstantsCB));
        postprocCB = resources.CreateConstantBuffer(sizeof(PostProcCB));
        outlineCB  = resources.CreateConstantBuffer(sizeof(OutlineCB));
        atmCB      = resources.CreateConstantBuffer(sizeof(AtmosphereCB));
        decalCB    = resources.CreateConstantBuffer(sizeof(DecalCB));
        decalMaterialCB = resources.CreateConstantBuffer(sizeof(DecalMaterialCB));
        decalReceiverCB = resources.CreateConstantBuffer(sizeof(DecalReceiverCB));
        volumetricCloudCB = resources.CreateConstantBuffer(176);
        particleSelfShadowRT = resources.CreateRenderTarget(
            RenderPassHandles::kSelfShadowResolution, RenderPassHandles::kSelfShadowResolution, 1);
        particleSelfShadowFrameCB = resources.CreateConstantBuffer(sizeof(PerFrameCB));

        defaultPSO = resources.CreatePipelineState({ renderer::RasterizerMode::SOLID, renderer::BlendMode::OPAQUE_BLEND, renderer::DepthMode::DEPTH_ON });
        wireframePSO = resources.CreatePipelineState({ renderer::RasterizerMode::WIREFRAME, renderer::BlendMode::OPAQUE_BLEND, renderer::DepthMode::DEPTH_ON });
        selectionMaskPso = resources.CreatePipelineState({ renderer::RasterizerMode::SOLID, renderer::BlendMode::OPAQUE_BLEND, renderer::DepthMode::DEPTH_ON });
        skydomePSO = resources.CreatePipelineState({ renderer::RasterizerMode::SOLID_NOCULL, renderer::BlendMode::OPAQUE_BLEND, renderer::DepthMode::DEPTH_SKY });
        sunMoonPSO = resources.CreatePipelineState({ renderer::RasterizerMode::SOLID_NOCULL, renderer::BlendMode::ADDITIVE, renderer::DepthMode::DEPTH_SKY });
        particlePSO = resources.CreatePipelineState({ renderer::RasterizerMode::SOLID_NOCULL, renderer::BlendMode::ADDITIVE, renderer::DepthMode::DEPTH_READ });
        particleAlphaPSO = resources.CreatePipelineState({ renderer::RasterizerMode::SOLID_NOCULL, renderer::BlendMode::ALPHA_BLEND, renderer::DepthMode::DEPTH_READ });
        particlePremultipliedPSO = resources.CreatePipelineState({ renderer::RasterizerMode::SOLID_NOCULL, renderer::BlendMode::PREMULTIPLIED, renderer::DepthMode::DEPTH_READ });
        particleGpuPSO = resources.CreatePipelineState({ renderer::RasterizerMode::SOLID_NOCULL, renderer::BlendMode::ADDITIVE, renderer::DepthMode::DEPTH_READ });
        particleGpuAlphaPSO = resources.CreatePipelineState({ renderer::RasterizerMode::SOLID_NOCULL, renderer::BlendMode::ALPHA_BLEND, renderer::DepthMode::DEPTH_READ });
        particleGpuPremultipliedPSO = resources.CreatePipelineState({ renderer::RasterizerMode::SOLID_NOCULL, renderer::BlendMode::PREMULTIPLIED, renderer::DepthMode::DEPTH_READ });
        trailPSO = resources.CreatePipelineState({ renderer::RasterizerMode::SOLID_NOCULL, renderer::BlendMode::ALPHA_BLEND, renderer::DepthMode::DEPTH_READ });
        meshTrailPSO = resources.CreatePipelineState({ renderer::RasterizerMode::SOLID, renderer::BlendMode::ALPHA_BLEND, renderer::DepthMode::DEPTH_READ });
        meshTrailDoubleSidedPSO = resources.CreatePipelineState({ renderer::RasterizerMode::SOLID_NOCULL, renderer::BlendMode::ALPHA_BLEND, renderer::DepthMode::DEPTH_READ });
        postprocPSO = resources.CreatePipelineState({ renderer::RasterizerMode::SOLID, renderer::BlendMode::OPAQUE_BLEND, renderer::DepthMode::DEPTH_OFF });
        causticsPSO = resources.CreatePipelineState({ renderer::RasterizerMode::SOLID, renderer::BlendMode::ADDITIVE, renderer::DepthMode::DEPTH_OFF });
        volumetricCloudPSO = resources.CreatePipelineState({ renderer::RasterizerMode::SOLID, renderer::BlendMode::ALPHA_BLEND, renderer::DepthMode::DEPTH_OFF });
        volumetricCloudPremultipliedPSO = resources.CreatePipelineState({ renderer::RasterizerMode::SOLID, renderer::BlendMode::PREMULTIPLIED, renderer::DepthMode::DEPTH_OFF });
        decalPSO = resources.CreatePipelineState({ renderer::RasterizerMode::SOLID, renderer::BlendMode::ALPHA_BLEND, renderer::DepthMode::DEPTH_OFF });
        decalMaskPso = resources.CreatePipelineState({ renderer::RasterizerMode::SOLID, renderer::BlendMode::OPAQUE_BLEND, renderer::DepthMode::DEPTH_OFF });
    }

    // パーティクルバッファ (最大描画数分を事前確保)
    static renderer::ResourceHandle<renderer::BufferTag> particleVB;
    static renderer::ResourceHandle<renderer::BufferTag> particleIB;
    static uint64_t sParticleResetVersion = 0;
    if (sParticleResetVersion != resources.GetResetVersion()) {
        particleVB = {};
        particleIB = {};
        sParticleResetVersion = resources.GetResetVersion();
    }
    constexpr uint32_t kMaxParticleVertices = static_cast<uint32_t>(kMaxParticleDraw) * 4u;
    if (!particleVB.IsValid())
    {
        particleVB = resources.CreateVertexBuffer(
            nullptr,
            kMaxParticleVertices * static_cast<uint32_t>(sizeof(ParticleVertex)),
            static_cast<uint32_t>(sizeof(ParticleVertex)));
    }
    if (!particleIB.IsValid())
    {
        std::vector<uint32_t> idx;
        idx.reserve(kMaxParticleDraw * 6);
        for (int i = 0; i < kMaxParticleDraw; ++i) {
            uint32_t b = static_cast<uint32_t>(i * 4);
            idx.insert(idx.end(), { b, b+1, b+2, b+1, b+3, b+2 });
        }
        particleIB = resources.CreateIndexBuffer(idx.data(), static_cast<uint32_t>(idx.size()));
    }

    profiler::Profiler::EndSample();

    // UI の描画先種別を安定キーにして、Scene / Game の中間リソースを分離する。
    // uiOptions がない通常ゲーム描画は key=0 の単一コンテキストを使う。
    const uint32_t viewKey = uiOptions
        ? static_cast<uint32_t>(uiOptions->targetView) + 1u
        : 0u;
    static std::unordered_map<uint32_t, ViewRenderTargets> s_viewTargets;
    static uint64_t sRenderTargetResetVersion = 0;
    if (sRenderTargetResetVersion != resources.GetResetVersion()) {
        // ResourceManager::Reset() 後は旧ハンドルが無効なので Release せずキャッシュだけ破棄する。
        s_viewTargets.clear();
        sRenderTargetResetVersion = resources.GetResetVersion();
    }

    ViewRenderTargets& viewTargets = s_viewTargets[viewKey];
    auto& hdrRT                   = viewTargets.hdr;
    auto& ldrRT                   = viewTargets.ldr;
    auto& selectionMaskRT         = viewTargets.selectionMask;
    auto& outlineRT               = viewTargets.outline;
    auto& customPostProcessRT     = viewTargets.customPostProcess;
    auto& gbufferRT               = viewTargets.gbuffer;
    auto& decalDepthRT            = viewTargets.decalDepth;
    auto& decalMaskRT             = viewTargets.decalMask;
    auto& bloomHalf               = viewTargets.bloomHalf;
    auto& bloomFull               = viewTargets.bloomFull;
    auto& ssaoRaw                 = viewTargets.ssaoRaw;
    auto& ssaoBlur                = viewTargets.ssaoBlur;
    auto& ssrResult               = viewTargets.ssrResult;
    auto& volumetricResult        = viewTargets.volumetricResult;
    auto& taaHistoryA             = viewTargets.taaHistoryA;
    auto& taaHistoryB             = viewTargets.taaHistoryB;
    auto& motionBlurResult        = viewTargets.motionBlurResult;
    auto& gtaoRaw                 = viewTargets.gtaoRaw;
    auto& gtaoBlur                = viewTargets.gtaoBlur;
    auto& contactShadowResult     = viewTargets.contactShadowResult;
    uint32_t& sHdrW               = viewTargets.width;
    uint32_t& sHdrH               = viewTargets.height;
    // advancedGraphicsCB はビュー別に生成する。
    // s_viewTargets.clear() によるデバイスリセット後は無効になるため、ここで lazily 再生成する。
    if (!viewTargets.advancedGraphicsCB.IsValid())
        viewTargets.advancedGraphicsCB = resources.CreateConstantBuffer(sizeof(AdvancedGraphicsCB));
    auto& advancedGraphicsCB = viewTargets.advancedGraphicsCB;

    {
        FBZZ_PROFILE_SCOPE("RenderSystem::ResizeRenderTargets");
        const auto* output = resources.Get(outputRT);
        uint32_t curW = output ? output->GetWidth()  : renderer.GetWidth();
        uint32_t curH = output ? output->GetHeight() : renderer.GetHeight();
        if (curW == 0 || curH == 0) return;
        if (!hdrRT.IsValid() || sHdrW != curW || sHdrH != curH)
        {
            ReleaseViewRenderTargets(viewTargets, resources);
            hdrRT           = resources.CreateRenderTarget(curW, curH, 1);
            ldrRT           = resources.CreateRenderTarget(curW, curH, 1);
            selectionMaskRT = resources.CreateRenderTarget(curW, curH, 1);
            outlineRT       = resources.CreateRenderTarget(curW, curH, 1);
            customPostProcessRT[0] = resources.CreateRenderTarget(curW, curH, 1);
            customPostProcessRT[1] = resources.CreateRenderTarget(curW, curH, 1);
            gbufferRT       = resources.CreateRenderTarget(curW, curH, 2);
            decalDepthRT    = resources.CreateRenderTarget(curW, curH, 0);
            decalMaskRT     = resources.CreateRenderTarget(curW, curH, 1);
            bloomHalf       = resources.CreateComputeTexture((std::max)(1u, curW / 2), (std::max)(1u, curH / 2));
            bloomFull       = resources.CreateComputeTexture(curW, curH);
            // AO / 接触影は低周波なので半解像度で焼き、消費側 (DeferredLighting) が正規化 UV の
            // linear サンプルでアップスケールする。フル解像度比でコスト約 1/4。SSR は鏡面が崩れる
            // ためフル解像度のまま維持する。
            ssaoRaw         = resources.CreateComputeTexture((std::max)(1u, curW / 2), (std::max)(1u, curH / 2));
            ssaoBlur        = resources.CreateComputeTexture((std::max)(1u, curW / 2), (std::max)(1u, curH / 2));
            // ---- Advanced Graphics per-view テクスチャ ----
            ssrResult           = resources.CreateComputeTexture(curW, curH);
            volumetricResult    = resources.CreateComputeTexture(curW, curH);
            taaHistoryA         = resources.CreateRenderTarget(curW, curH, 1);
            taaHistoryB         = resources.CreateRenderTarget(curW, curH, 1);
            motionBlurResult    = resources.CreateComputeTexture(curW, curH);
            gtaoRaw             = resources.CreateComputeTexture((std::max)(1u, curW / 2), (std::max)(1u, curH / 2)); // 半解像度 AO
            gtaoBlur            = resources.CreateComputeTexture((std::max)(1u, curW / 2), (std::max)(1u, curH / 2)); // 半解像度 AO
            contactShadowResult = resources.CreateComputeTexture((std::max)(1u, curW / 2), (std::max)(1u, curH / 2)); // 半解像度 接触影
            sHdrW = curW;
            sHdrH = curH;
        }
    }

    // =========================================================================
    // ライト収集とシャドウ範囲計算はシーン全体を走査するため、独立して計測する。
    profiler::Profiler::BeginSample(
        profiler::ProfilerMarker("RenderSystem::LightingSetup", "Rendering"));

    // ライト定数バッファを構築
    // =========================================================================
    renderer::LightConstantsCB lightData{};
    lightData.lightDir       = { 0.0f, -1.0f, 0.5f };
    lightData.lightColor     = { 1.0f,  1.0f, 1.0f };
    lightData.lightIntensity = 1.0f;

    // Directional Light のシャドウ設定 (LightComponent から取得)
    bool  dirCastShadows    = true;
    float dirShadowBias     = 1.0f;
    float dirShadowStrength = 1.0f;
    float dirShadowDistance = 0.0f;

    // クラスタライティング用の統合ライト配列。b3 の固定長配列と並行して構築する。
    // WHY 並行構築か: b3 は点 8 / スポット 4 で打ち切るため、それを超えたライトは
    //      これまで黙って捨てられていた。こちらは 256 本まで拾い、対応済みのパスだけが参照する。
    //      b3 側の詰め方は 1 行も変えないので、未対応パスの見た目は完全に据え置きになる。
    std::vector<PunctualLightGPU> punctualLights;
    punctualLights.reserve(32);

    constexpr float kDegToRad = 3.14159265f / 180.0f;
    for (auto [tf, lc] : scene.View<Transform, LightComponent>()) {
        if (!lc.enabled) continue;
        // 点光源 / スポットは上限に達するまで統合配列へも積む。
        // NOTE: 追加順は b3 と同じ ECS 走査順。ただし b3 が「点を全部→スポットを全部」の
        //       2 配列なのに対しこちらは 1 本なので、混在シーンでは評価順が変わりうる。
        //       不透明ライティングの加算なので結果は変わらない (順序による丸め差のみ)。
        if (lc.type != LightComponent::Type::Directional
            && punctualLights.size() < kMaxPunctualLights) {
            PunctualLightGPU& gpu = punctualLights.emplace_back();
            gpu.position  = tf.position;
            gpu.range     = lc.range;
            gpu.color     = lc.color;
            gpu.intensity = lc.intensity;
            if (lc.type == LightComponent::Type::Spot) {
                gpu.direction = tf.forward.Normalized();
                gpu.innerCos  = std::cos(lc.innerCone * kDegToRad);
                gpu.outerCos  = std::cos(lc.outerCone * kDegToRad);
                gpu.type      = static_cast<uint32_t>(1); // FBZZ_LIGHT_TYPE_SPOT
            } else {
                gpu.direction = { 0.0f, -1.0f, 0.0f };
                gpu.innerCos  = 0.0f;
                gpu.outerCos  = 0.0f;
                gpu.type      = static_cast<uint32_t>(0); // FBZZ_LIGHT_TYPE_POINT
            }
        }

        if (lc.type == LightComponent::Type::Directional) {
            lightData.lightDir       = tf.forward.Normalized();
            lightData.lightColor     = lc.color;
            lightData.lightIntensity = lc.intensity;
            dirCastShadows    = lc.castShadows;
            dirShadowBias     = lc.shadowBias;
            dirShadowStrength = lc.shadowStrength;
            dirShadowDistance = lc.shadowDistance;
        } else if (lc.type == LightComponent::Type::Point
                   && lightData.pointLightCount < 8) {
            auto& pl    = lightData.pointLights[lightData.pointLightCount++];
            pl.position  = tf.position;
            pl.range     = lc.range;
            pl.color     = lc.color;
            pl.intensity = lc.intensity;
        } else if (lc.type == LightComponent::Type::Spot
                   && lightData.spotLightCount < 4) {
            auto& sl    = lightData.spotLights[lightData.spotLightCount++];
            sl.position  = tf.position;
            sl.direction = tf.forward.Normalized();
            sl.range     = lc.range;
            sl.innerCos  = std::cos(lc.innerCone * kDegToRad);
            sl.outerCos  = std::cos(lc.outerCone * kDegToRad);
            sl.color     = lc.color;
            sl.intensity = lc.intensity;
        }
    }

    // ── 昼夜の色・強度カーブ (Phase B) ─────────────────────────────────────────────
    // WHY: 太陽の「向き」は DirectionalLight の transform を唯一のソースとする (ここで lightDir は上書きしない)。
    //      SkyRenderer.dayNightEnabled のときは、その光源の「太陽高度」から色・強度の昼夜遷移だけを駆動する。
    //      → DirectionalLight を回すと 太陽ディスク(SunMoon)・空・月(アンチ太陽)・空連動 IBL・ライティングが一緒に動く。
    //      時刻アニメをしたい場合はスクリプトでライトの向きを回す。
    // 雲シャドウ params (Phase C) も SkyRenderer から読む。passCtx へ後で転送する。
    float skyCloudShadowStrength = 0.0f, skyCloudShadowCoverage = 0.5f,
          skyCloudShadowScale = 0.02f, skyCloudShadowSpeed = 1.0f;
    for (auto [tf, sky] : scene.View<Transform, SkyRenderer>()) {
        (void)tf; // 太陽の向きは DirectionalLight 側で決まるため SkyRenderer の Transform は使わない
        if (!sky.enabled) continue;

        // 雲シャドウは昼夜サイクルとは独立に常に反映する。
        skyCloudShadowStrength = sky.cloudShadowStrength;
        skyCloudShadowCoverage = sky.cloudShadowCoverage;
        // Component は「まだら 1 周期の大きさ [m]」。シェーダーは world→UV スケールを要る。
        skyCloudShadowScale    = 1.0f / (std::max)(sky.cloudShadowSize, 1.0f);
        skyCloudShadowSpeed    = sky.cloudShadowSpeed;

        if (sky.dayNightEnabled) {
            // 太陽方向 (toward sun) = -lightDir。その高度 [度] を軸に 夜 ↔ 夕方 ↔ 昼 を補間する。
            // WHY 高度 0° を夕方のキーに置くか: 「ライトを水平に向ける = 夕方」が直感どおりに
            //     なり、昼側と夜側それぞれ独立した帯幅で抜けられる。旧実装は夕焼けの重みに
            //     昼の重みを掛けていたため、夕焼けが最も濃いはずの地平線上で重みが 0.17 まで
            //     落ち、sunsetColor がどう振っても 3 割以上乗らず夕方を作れなかった。
            const math::Vector3 sunToSun =
                math::Vector3{ -lightData.lightDir.x, -lightData.lightDir.y, -lightData.lightDir.z }.Normalized();

            auto clamp01 = [](float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); };
            auto lerp1   = [](float a, float b, float t) { return a + (b - a) * t; };
            auto lerp3   = [](const math::Vector3& a, const math::Vector3& b, float t) {
                return math::Vector3{ a.x + (b.x - a.x) * t,
                                      a.y + (b.y - a.y) * t,
                                      a.z + (b.z - a.z) * t };
            };

            constexpr float kRadToDeg = 57.29577951f;
            const float sinAlt      = (std::max)(-1.0f, (std::min)(1.0f, sunToSun.y));
            const float altitudeDeg = std::asin(sinAlt) * kRadToDeg;

            const bool  above = altitudeDeg >= 0.0f;
            const float span  = above ? (std::max)(sky.dayAltitude,   0.1f)
                                      : (std::max)(sky.nightAltitude, 0.1f);
            float t = clamp01(std::fabs(altitudeDeg) / span);
            t = t * t * (3.0f - 2.0f * t); // smoothstep: 帯の端で色・明るさが折れないようにする

            lightData.lightColor     = lerp3(sky.sunsetColor,
                                             above ? sky.dayColor : sky.nightColor, t);
            lightData.lightIntensity = lerp1(sky.sunsetIntensity,
                                             above ? sky.dayIntensity : sky.nightIntensity, t);
            // 空の明るさは太陽光の強さとは別軸で補間する (Skydome / SunMoon / 雲 / 光芒 /
            // エアリアルパースが参照)。分離前は lightIntensity を共用していたため、太陽を
            // 強くすると空まで白飛びしていた。詳細は SkyRenderer::skyDayBrightness を参照。
            lightData.skyDimmer      = lerp1(sky.skySunsetBrightness,
                                             above ? sky.skyDayBrightness : sky.skyNightBrightness, t);
        }
        break;
    }

    // ambientColor: Lit モードでは AMBIENT_SCALE 相当値、Unlit 系では白に上書き
    lightData.ambientColor = { 0.08f, 0.08f, 0.08f };
    if (rs.IsUnlit()) {
        lightData.ambientColor    = { 1.0f, 1.0f, 1.0f };
        lightData.lightIntensity  = 0.0f;
        lightData.pointLightCount = 0;
        lightData.spotLightCount  = 0;
        // 空も消灯する。分離前は lightIntensity=0 が空系シェーダーにも効いていたので、
        // Unlit 表示で空が黒く落ちる従来の挙動を skyDimmer 側で維持する。
        lightData.skyDimmer       = 0.0f;
    }

    math::Vector3 lightDir = lightData.lightDir.Normalized();
    SceneShadowBounds shadowBounds;
    {
        FBZZ_PROFILE_SCOPE("RenderSystem::ComputeShadowBounds");
        shadowBounds = ComputeSceneShadowBounds(scene, cullingMask);
    }
    if (!shadowBounds.valid) {
        shadowBounds.center = camera.m_position + camera.GetForward() * 20.0f;
        shadowBounds.radius = 40.0f;
        shadowBounds.valid = true;
    }

    // ── カスケードシャドウ (CSM) のフィッティング ───────────────────────────────
    // WHY: シャドウの精細さを決めるのは解像度そのものではなく「1 テクセルが覆うワールド距離」。
    //      単一シャドウマップは、影の到達距離を伸ばすとその割り算の分母が伸びるだけで、
    //      近距離の精細さと遠距離の到達距離を同時に満たせない。
    //      視錐台を距離で区切り、手前ほど狭い範囲へ 1 タイルを丸ごと割り当てることで、
    //      到達距離を保ったまま足元のテクセル密度だけを上げる。
    // NOTE: shadowBounds (シーン全体) は「これ以上大きくしない」上限としてだけ使う。
    //       小さなシーンでは最遠カスケードがシーン全体をそのまま覆う。
    const int cascadeCount =
        std::clamp(rs.shadow.cascadeCount, 1, fbzz::renderer::kMaxShadowCascades);

    // アトラス配置: 1 分割なら全面、2 分割以上なら 2x2 タイル。
    // WHY: 全カスケードを 1 枚の深度テクスチャへ収めることで、影を読む 20 以上のシェーダーが
    //      バインドもサンプラーも変えずに済む。解像度とメモリは分割数によらず一定で、
    //      増えるのは「同じ面積をどう配分するか」だけ。
    const uint32_t atlasResolution = (std::max)(rs.shadow.mapResolution, 1u);
    const uint32_t tilesPerSide    = (cascadeCount > 1) ? 2u : 1u;
    const uint32_t tileSize        = (std::max)(atlasResolution / tilesPerSide, 1u);

    // 影の最大到達距離。LightComponent::shadowDistance > 0 は従来どおり手動指定を優先する。
    const float shadowDistance = (dirShadowDistance > 0.0f)
        ? (std::max)(dirShadowDistance, 1.0f)
        : (std::max)(rs.shadow.autoFitDistance, 1.0f);

    float cascadeSplits[fbzz::renderer::kMaxShadowCascades] = {};
    ComputeCascadeSplits((std::max)(camera.m_near, 0.01f), shadowDistance,
                         cascadeCount, rs.shadow.cascadeSplitLambda, cascadeSplits);

    const math::Vector3 up = (std::abs(lightDir.y) > 0.99f)
                             ? math::Vector3{ 1.0f, 0.0f, 0.0f }
                             : math::Vector3{ 0.0f, 1.0f, 0.0f };
    // 位置を持たない回転だけのライト空間。テクセルスナップの量子化格子として使う。
    const math::Matrix4 snapView     = math::Matrix4::LookAt(math::Vector3::ZERO, lightDir, up);
    const math::Matrix4 snapViewInv  = math::Matrix4::Inverse(snapView);

    ShadowCascade cascades[fbzz::renderer::kMaxShadowCascades] = {};
    float cascadeSliceNear = (std::max)(camera.m_near, 0.01f);

    for (int i = 0; i < cascadeCount; ++i) {
        const float sliceFar = cascadeSplits[i];

        // このカスケードが担当する視錐台スライスの外接球。
        // 外接球はカメラの向きに依存しない半径を返すので、振り向いてもボリュームの
        // 大きさが変わらず、影の精細度が回転で脈動しない。
        const FrustumSliceSphere slice =
            ComputeFrustumSliceSphere(camera, cascadeSliceNear, sliceFar);

        // シーン全体より大きい影ボリュームを作っても無駄なテクセルが増えるだけ。
        float radius = (std::max)((std::min)(slice.radius, shadowBounds.radius), 1.0f);

        math::Vector3 center = camera.m_position + camera.GetForward() * slice.centerDistance;
        if (radius < shadowBounds.radius) {
            // シーン球からはみ出さないよう、中心をシーン球内へ引き戻す。
            const math::Vector3 offset   = center - shadowBounds.center;
            const float         distance = offset.Length();
            const float         limit    = (std::max)(shadowBounds.radius - radius, 0.0f);
            if (distance > limit && distance > 0.0001f)
                center = shadowBounds.center + offset * (limit / distance);
        } else {
            center = shadowBounds.center;
        }

        // テクセルスナップ。中心がカメラに追従すると、サブテクセルのずれで影の縁が
        // 毎フレーム別のテクセルへ丸められ、静止していても輪郭が波打つ (shadow swimming)。
        // 中心をライト空間で 1 テクセル単位へ量子化すると、カメラが動いてもマップ上の
        // 標本位置が変わらなくなり、この揺れが消える。
        const float texelWorldSize = (radius * 2.0f) / static_cast<float>(tileSize);
        {
            math::Vector4 lightSpace =
                snapView * math::Vector4{ center.x, center.y, center.z, 1.0f };
            lightSpace.x = std::floor(lightSpace.x / texelWorldSize) * texelWorldSize;
            lightSpace.y = std::floor(lightSpace.y / texelWorldSize) * texelWorldSize;
            const math::Vector4 snapped = snapViewInv * lightSpace;
            center = { snapped.x, snapped.y, snapped.z };
        }

        // 深度レンジ。影ボリュームを縮めても、その外側にいる背の高い caster (建物・地形) が
        // 影を落とし込めるよう、ライト方向の引きはシーン全体の広がりから取る。
        // WHY: near/far だけ広げても塗るテクセル数は増えないので、フィルレートは無関係。
        const float pullback   = (std::max)(shadowBounds.radius, radius) + 20.0f;
        const float depthRange = pullback + radius + 20.0f;

        const math::Vector3 eye  = center - lightDir * pullback;
        const math::Matrix4 view = math::Matrix4::LookAt(eye, center, up);
        const math::Matrix4 proj = math::Matrix4::Orthographic(-radius, radius,
                                                               -radius, radius,
                                                               1.0f, depthRange);

        ShadowCascade& cascade = cascades[i];
        cascade.viewProjection = proj * view;
        cascade.view           = view;
        cascade.eyePos         = eye;
        cascade.frustum        = math::Frustum::FromViewProjection(cascade.viewProjection);
        cascade.texelWorldSize = texelWorldSize;
        // ワールド空間で約 5mm 相当の一定バイアスになるよう深度レンジで正規化する。
        // カスケードごとにレンジが違うので、値もカスケードごとに持つ。
        cascade.biasNDC = (0.005f * dirShadowBias) / (std::max)(depthRange - 1.0f, 1.0f);

        // アトラス内のタイル位置 (2x2 を左上から Z 字順に埋める)。
        const uint32_t tileX = static_cast<uint32_t>(i) % tilesPerSide;
        const uint32_t tileY = static_cast<uint32_t>(i) / tilesPerSide;
        cascade.viewportX    = tileX * tileSize;
        cascade.viewportY    = tileY * tileSize;
        cascade.viewportSize = tileSize;

        const float uvScale = static_cast<float>(tileSize) / static_cast<float>(atlasResolution);
        cascade.atlasRect = {
            static_cast<float>(cascade.viewportX) / static_cast<float>(atlasResolution),
            static_cast<float>(cascade.viewportY) / static_cast<float>(atlasResolution),
            uvScale, uvScale
        };

        cascadeSliceNear = sliceFar;
    }

    // 単一のライト行列で足りるパス (パーティクル自己影など) 向けの代表値。
    // 最遠カスケードを渡す: 影の到達範囲全体をいちばん広く覆うのがこれだから。
    // WHY view と viewProjection を対で渡すか: パーティクル自己影はビルボードを光源へ
    //      正対させるために view の内訳を要求する。片方だけ別カスケードにすると破綻する。
    const ShadowCascade& widestCascade = cascades[cascadeCount - 1];
    const math::Matrix4  lightVP   = widestCascade.viewProjection;
    const math::Matrix4  lightView = widestCascade.view;
    const math::Vector3  lightPos  = widestCascade.eyePos;

    // Forward / Deferred の切り替えで見た目が変わらないよう、不透明物は可能な限り共通の
    // GBuffer → AO → DeferredLighting 経路を通す。
    // WHY: Forward 直描き経路では SSAO/GTAO/ContactShadows/SSR/IBL が Terrain/Detail/Foliage に
    //      乗らず、Unity のようなレンダリングモード切り替え時の見た目互換性を保てない。
    //      必須リソースが欠ける場合だけ従来 Forward にフォールバックする。
    const bool wantsDeferredPipeline =
        rs.pipeline == renderer::RenderingPipeline::Deferred
        || rs.pipeline == renderer::RenderingPipeline::DeferredPlus;
    const bool useGBufferOpaquePipeline =
        wantsDeferredPipeline &&
        gbufferRT.IsValid() &&
        gbufferShader.IsValid() &&
        deferredLightingShader.IsValid() &&
        depthCopyShader.IsValid();
    const bool ssaoEnabled =
        useGBufferOpaquePipeline &&
        rs.postProcess.ambientOcclusion.enabled &&
        ssaoShader.IsValid() &&
        ssaoBlurShader.IsValid() &&
        ssaoRaw.IsValid() &&
        ssaoBlur.IsValid();

    const bool selectionOutlineEnabled =
        rs.showSelectionOutline && !rs.selectedObjects.empty() &&
        selectionMaskRT.IsValid() && selectionMaskPso.IsValid() &&
        selectionOutlineShader.IsValid();
    profiler::Profiler::EndSample();

    // =========================================================================
    // RenderPassHandles を組み立て
    // =========================================================================
    RenderPassHandles passHandles{};
    passHandles.shadowMapRT       = shadowMapRT;
    passHandles.hdrRT             = hdrRT;
    passHandles.ldrRT             = ldrRT;
    passHandles.selectionMaskRT   = selectionMaskRT;
    passHandles.outlineRT         = outlineRT;
    passHandles.customPostProcessRT[0] = customPostProcessRT[0];
    passHandles.customPostProcessRT[1] = customPostProcessRT[1];
    passHandles.gbufferRT         = gbufferRT;
    passHandles.bloomHalf         = bloomHalf;
    passHandles.bloomFull         = bloomFull;
    passHandles.ssaoRaw           = ssaoRaw;
    passHandles.ssaoBlur          = ssaoBlur;
    passHandles.ssaoShader        = ssaoShader;
    passHandles.ssaoBlurShader    = ssaoBlurShader;
    passHandles.bloomDownShader   = bloomDownShader;
    passHandles.bloomUpShader     = bloomUpShader;
    passHandles.compositeShader   = compositeShader;
    passHandles.causticsShader    = causticsShader;
    passHandles.volumetricCloudShader = volumetricCloudShader;
    passHandles.cloudUpscaleShader    = cloudUpscaleShader;
    passHandles.selectionMaskShader       = selectionMaskShader;
    passHandles.selectionMaskSkinnedShader = selectionMaskSkinnedShader;
    passHandles.selectionMaskParticleShader = selectionMaskParticleShader;
    passHandles.selectionMaskParticleGpuShader = selectionMaskParticleGpuShader;
    passHandles.selectionOutlineShader    = selectionOutlineShader;
    passHandles.fxaaShader        = fxaaShader;
    passHandles.customPostProcessShaders.resize(rs.postProcess.customEffects.size());
    std::vector<uint32_t> customPostProcessIndices;
    customPostProcessIndices.reserve(rs.postProcess.customEffects.size());
    for (uint32_t i = 0; i < static_cast<uint32_t>(rs.postProcess.customEffects.size()); ++i) {
        const auto& custom = rs.postProcess.customEffects[i];
        if (!custom.enabled || custom.shaderPath.empty()) continue;
        passHandles.customPostProcessShaders[i] = resources.LoadShader(custom.shaderPath);
        if (passHandles.customPostProcessShaders[i].IsValid())
            customPostProcessIndices.push_back(i);
    }
    passHandles.selectionMaskPSO  = selectionMaskPso;
    passHandles.postprocPSO       = postprocPSO;
    passHandles.causticsPSO       = causticsPSO;
    passHandles.volumetricCloudPSO = volumetricCloudPSO;
    passHandles.volumetricCloudPremultipliedPSO = volumetricCloudPremultipliedPSO;
    passHandles.frameCB           = frameCB;
    passHandles.objectCB          = objectCB;
    passHandles.lightCB           = lightCB;
    passHandles.bindPoseSkinningCB = bindPoseSkinningCB;

    // ── スキンドモデルのリファレンスポーズ CB を遅延生成 ─────────────────
    // WHY: AnimatorComponent を持たない SkinnedMeshRenderer の既定パレット。
    //   モデル単位 (= スケルトン単位) に 1 本で、全インスタンスが共有する。
    //   これが無いと単位行列へフォールバックし、ノード階層にバインド変換を持つ
    //   アセットが倒れて描画される。
    {
        struct RefPoseData { math::Matrix4 bones[asset::MAX_SKINNING_BONES]; };
        for (auto& go : scene.GameObjects()) {
            auto* smr = go.GetComponent<SkinnedMeshRenderer>();
            if (!smr || !smr->model || !smr->model->skeleton) continue;
            asset::Model& model = *smr->model;
            if (model.referencePoseCB.IsValid()) continue;

            const std::vector<math::Matrix4>& refPose = model.skeleton->referencePose;
            if (refPose.empty()) continue;

            RefPoseData rp{};
            for (auto& m : rp.bones) m = math::Matrix4::Identity();
            const size_t maxBones = static_cast<size_t>(asset::MAX_SKINNING_BONES);
            const size_t count = refPose.size() < maxBones ? refPose.size() : maxBones;
            for (size_t i = 0; i < count; ++i) rp.bones[i] = refPose[i];

            model.referencePoseCB = resources.CreateConstantBuffer(sizeof(RefPoseData));
            if (model.referencePoseCB.IsValid())
                resources.Update(model.referencePoseCB, &rp, sizeof(RefPoseData));
        }
    }
    passHandles.postprocCB        = postprocCB;
    passHandles.outlineCB         = outlineCB;
    passHandles.volumetricCloudCB = volumetricCloudCB;
    passHandles.cloudShapeTex     = cloudShapeTex;
    passHandles.cloudDetailTex    = cloudDetailTex;
    passHandles.decalDepthRT      = decalDepthRT;
    passHandles.decalMaskRT       = decalMaskRT;
    passHandles.decalShader       = decalShader;
    passHandles.decalMaskShader   = decalMaskShader;
    passHandles.decalMaskSkinnedShader = decalMaskSkinnedShader;
    passHandles.decalPSO          = decalPSO;
    passHandles.decalMaskPSO      = decalMaskPso;
    passHandles.decalCB           = decalCB;
    passHandles.decalMaterialCB   = decalMaterialCB;
    passHandles.decalReceiverCB   = decalReceiverCB;
    // ── ジオメトリ用ハンドル ──────────────────────────────────────────────────
    passHandles.shadowShader         = shadowShader;
    passHandles.shadowSkinnedShader  = skinnedShadowShader;
    passHandles.shadowCB             = shadowCB;
    passHandles.skinningComputeCS    = skinningComputeCS;
    passHandles.skinningCB           = skinningCB;
    passHandles.defaultPSO           = defaultPSO;
    passHandles.wireframePSO         = wireframePSO;
    passHandles.skyShader            = skydomeShader;
    passHandles.sunMoonShader        = sunMoonShader;
    passHandles.skyPSO               = skydomePSO;
    passHandles.sunMoonPSO           = sunMoonPSO;
    if (skydomeMesh) {
        passHandles.skyVB         = skydomeMesh->vertexBuffer;
        passHandles.skyIB         = skydomeMesh->indexBuffer;
        passHandles.skyIndexCount = skydomeMesh->indexCount;
    }
    passHandles.atmosphereCB         = atmCB;
    passHandles.skyEnvCubeRT         = skyEnvCubeRT;
    passHandles.skyCaptureFrameCB    = skyCaptureFrameCB;
    passHandles.particleShader       = particleShader;
    passHandles.particlePSO          = particlePSO;
    passHandles.particleAlphaPSO     = particleAlphaPSO;
    passHandles.particlePremultipliedPSO = particlePremultipliedPSO;
    passHandles.particleVB           = particleVB;
    passHandles.particleIB           = particleIB;
    passHandles.particleGpuSimCS     = particleGpuSimCS;
    passHandles.particleGpuSortKeysCS  = particleGpuSortKeysCS;
    passHandles.particleGpuSortStepCS  = particleGpuSortStepCS;
    passHandles.particleGpuSortLocalCS = particleGpuSortLocalCS;
    passHandles.particleGpuMeshShader  = particleGpuMeshShader;
    passHandles.particleSelfShadowShader = particleSelfShadowShader;
    passHandles.particleSelfShadowRT      = particleSelfShadowRT;
    passHandles.particleSelfShadowFrameCB = particleSelfShadowFrameCB;
    passHandles.particleGpuShader    = particleGpuShader;
    passHandles.particleGpuAlphaShader = particleGpuShader; // 同一シェーダー、PSO で合成モードを切り替える
    passHandles.particleGpuPSO       = particleGpuPSO;
    passHandles.particleGpuAlphaPSO  = particleGpuAlphaPSO;
    passHandles.particleGpuPremultipliedPSO = particleGpuPremultipliedPSO;
    passHandles.trailShader          = trailShader;
    passHandles.trailPSO             = trailPSO;
    passHandles.meshTrailShader      = meshTrailShader;
    passHandles.skinnedMeshTrailShader = skinnedMeshTrailShader;
    passHandles.meshTrailPSO         = meshTrailPSO;
    passHandles.meshTrailDoubleSidedPSO = meshTrailDoubleSidedPSO;
    passHandles.detailMeshShader      = detailMeshShader;
    passHandles.detailBillboardShader = detailBillboardShader;
    passHandles.detailGrassShader     = detailGrassShader;
    passHandles.detailGBufferShader      = detailGBufferShader;
    passHandles.detailGrassGBufferShader = detailGrassGBufferShader;
    passHandles.detailGrassCB         = detailGrassCB;
    passHandles.detailMeshPSO         = detailMeshPSO;
    passHandles.detailNoCullPSO       = detailNoCullPSO;
    passHandles.foliageShader          = foliageShader;
    passHandles.foliageGBufferShader   = foliageGBufferShader;
    passHandles.foliagePSO             = foliagePSO;
    passHandles.foliageNoCullPSO       = foliageNoCullPSO;
    passHandles.gbufferShader        = gbufferShader;
    passHandles.deferredLightingShader = deferredLightingShader;
    passHandles.depthCopyShader      = depthCopyShader;
    passHandles.clusterCullCS        = clusterCullCS;
    passHandles.punctualLightBuffer  = punctualLightBuffer;
    passHandles.clusterIndexBuffer   = clusterIndexBuffer;
    passHandles.clusterCB            = clusterCB;

    // ---- Advanced Graphics ハンドルを passHandles に束縛 ----
    passHandles.advancedGraphicsCB   = advancedGraphicsCB;
    if (rs.lutColorGrading.enabled) {
        const uint64_t lutHash = HashLutSettings(rs.lutColorGrading);
        const bool lutResourceAlive = resources.Get(proceduralColorLut) != nullptr;
        if (!lutResourceAlive || proceduralColorLutHash != lutHash) {
            if (lutResourceAlive)
                resources.Release(proceduralColorLut);
            const std::vector<uint8_t> pixels = GenerateProceduralColorLut(rs.lutColorGrading);
            proceduralColorLut = resources.CreateTexture3D(
                pixels.data(), PROCEDURAL_LUT_SIZE, PROCEDURAL_LUT_SIZE, PROCEDURAL_LUT_SIZE);
            proceduralColorLutHash = lutHash;
        }
    }
    passHandles.proceduralColorLut = proceduralColorLut;
    // IBL BRDF LUT: static ComputeTexture。IBLBakePass が初回フレームで書き込む。
    passHandles.iblBrdfLut           = iblBrdfLut;
    passHandles.iblBrdfBakeShader    = iblBrdfBakeShader;
    // IBL キューブマップ: RenderSettings に指定されたパスを毎フレーム LoadTexture でキャッシュ参照する。
    // WHY: LoadTexture は内部でキャッシュするため、毎フレーム呼んでも I/O は初回のみ。
    if (!rs.ibl.irradiancePath.empty())
        passHandles.iblIrradiance = resources.LoadTexture(rs.ibl.irradiancePath);
    if (!rs.ibl.prefilterPath.empty())
        passHandles.iblPrefilter  = resources.LoadTexture(rs.ibl.prefilterPath);
    // SSR
    passHandles.ssrResult            = ssrResult;
    passHandles.ssrShader            = ssrShader;
    // Volumetric Lighting
    passHandles.volumetricResult     = volumetricResult;
    passHandles.volumetricShader     = volumetricShader;
    // TAA (ping-pong)
    passHandles.taaHistoryA          = taaHistoryA;
    passHandles.taaHistoryB          = taaHistoryB;
    passHandles.taaShader            = taaShader;
    passHandles.taaPSO               = taaPSO;
    // Motion Blur
    passHandles.motionBlurResult     = motionBlurResult;
    passHandles.motionBlurShader     = motionBlurShader;
    // GTAO
    passHandles.gtaoRaw              = gtaoRaw;
    passHandles.gtaoBlur             = gtaoBlur;
    passHandles.gtaoShader           = gtaoShader;
    passHandles.gtaoBlurShader       = gtaoBlurShader;
    // Contact Shadows
    passHandles.contactShadowResult  = contactShadowResult;
    passHandles.contactShadowShader  = contactShadowShader;
    // Lens Flare
    passHandles.lensFlareShader      = lensFlareShader;
    passHandles.lensFlarePSO         = lensFlarePSO;

    // カメラ視錐台とライト視錐台を事前に抽出する。
    // WHY: Gribb-Hartmann 法は VP 行列の各行の和・差から 6 平面を直接導出するため
    //      逆行列を使わず高速に抽出できる。全ジオメトリパスで共有する。
    const math::Frustum cameraFrustum = math::Frustum::FromViewProjection(camera.GetViewProjection());
    const math::Frustum lightFrustum  = math::Frustum::FromViewProjection(lightVP);
    OcclusionCuller occlusionCuller;

    // 参照メンバー (scene / renderer / resources / camera / settings / handles) までは
    // 集成体初期化で埋める必要があるが、それ以降は名前付きで代入する。
    // WHY: 以前は末尾まで位置指定で並べていたため、RenderPassContext へフィールドを 1 つ
    //      挿しただけで以降の値が全て 1 つずつずれた。名前で書けばその事故は起きない。
    RenderPassContext passCtx{
        scene, renderer, resources, camera, rs,
        outputRT, cullingMask, passHandles
    };
    passCtx.frustumCullingEnabled   = resolvedCulling.frustumCulling;
    passCtx.occlusionCullingEnabled = resolvedCulling.occlusionCulling;
    passCtx.cullingBoundsPadding    = resolvedCulling.cullingBoundsPadding;
    passCtx.cullMaxDistance         = resolvedCulling.maxDrawDistance;
    passCtx.cullDistanceSpherical   = resolvedCulling.cullDistanceSpherical;
    passCtx.smallObjectScreenHeight = resolvedCulling.smallObjectScreenHeight;
    for (int i = 0; i < kCullLayerCount; ++i) {
        passCtx.cullLayerDistances[i] = resolvedCulling.layerCullDistances[i];
        if (resolvedCulling.layerCullDistances[i] > 0.0f)
            passCtx.hasLayerCullDistances = true;
    }
    // 極小オブジェクト判定用の射影スケール = 1/tan(fovY/2)。
    // WHY ここで 1 回だけ求めるか: Camera::GetProjectionMatrix() は毎回行列を組み直すため、
    //     オブジェクトごとに呼ぶと判定本体より行列生成の方が高くつく。
    passCtx.cullProjScaleY    = camera.GetProjectionMatrix().m[1][1];
    passCtx.cullCameraForward = camera.GetForward();
    passCtx.width                   = sHdrW;
    passCtx.height                  = sHdrH;
    // TAA サブピクセルジッター。8 フレーム周期の Halton(2,3) をピクセル内 ±0.5 に写す。
    // WHY 8 か: 短いほど収束が速く、長いほど品質が上がる。8 は TAA の標準的な折衷で、
    //      taaFeedback 0.9 (履歴 90%) なら数フレームでほぼ収束する。
    // NOTE: TAA が無効なフレームは 0 のまま。ジッターだけ残すと画面全体が揺れて見える。
    if (rs.IsTaaActive() && sHdrW > 0u && sHdrH > 0u) {
        constexpr uint32_t kTaaJitterPeriod = 8u;
        const uint32_t sampleIndex = viewTargets.taaFrameIndex % kTaaJitterPeriod + 1u;
        const float offsetPxX = HaltonRadicalInverse(sampleIndex, 2u) - 0.5f;
        const float offsetPxY = HaltonRadicalInverse(sampleIndex, 3u) - 0.5f;
        // ピクセル → NDC。NDC の Y は上向きなので符号を反転する。
        passCtx.taaJitterNdcX =  2.0f * offsetPxX / static_cast<float>(sHdrW);
        passCtx.taaJitterNdcY = -2.0f * offsetPxY / static_cast<float>(sHdrH);
        ++viewTargets.taaFrameIndex;
    } else {
        viewTargets.taaFrameIndex = 0u;
    }
    passCtx.selectionOutlineEnabled = selectionOutlineEnabled;
    passCtx.lightData               = lightData;
    passCtx.lightVP                 = lightVP;
    passCtx.lightView               = lightView;
    passCtx.lightEyePos             = lightPos;
    passCtx.isDeferred              = useGBufferOpaquePipeline;
    passCtx.ssaoEnabled             = ssaoEnabled;

    // ── クラスタライトカリングのモード決定と定数の組み立て ──────────────────────
    // WHY useGBufferOpaquePipeline の判定式には手を触れないか: あちらは「GBuffer 経路を
    //     使えるか」であって、ライトの供給方法とは直交する軸。混ぜると Forward/Deferred の
    //     選択そのものが変わり、本件と無関係な見た目変更が全シーンへ波及する。
    const bool clusteredAvailable =
        punctualLightBuffer.IsValid() && clusterIndexBuffer.IsValid() && clusterCB.IsValid();
    const bool clusteredEnabled =
        rs.UsesClusteredLighting() && !rs.IsUnlit() && clusteredAvailable && clusterCullCS.IsValid();

    ClusterLightMode clusterMode = ClusterLightMode::Legacy;
    if (clusteredEnabled)
        clusterMode = rs.clustered.forceAllLights ? ClusterLightMode::Linear
                                                  : ClusterLightMode::Clustered;

    passCtx.punctualLights    = std::move(punctualLights);
    passCtx.clusterLightMode  = clusterMode;
    passCtx.clusterDebugHeatmap =
        clusteredEnabled && rs.clustered.debugHeatmap && clusterMode == ClusterLightMode::Clustered;

    if (clusteredAvailable) {
        // ライト配列は毎フレーム転送する。上限 256 本 × 64B = 16KB で、部分更新の価値はない。
        if (!passCtx.punctualLights.empty()) {
            resources.Update(punctualLightBuffer, passCtx.punctualLights.data(),
                             passCtx.punctualLights.size() * sizeof(PunctualLightGPU));
        }

        // 指数分割の係数。slice = log(viewZ) * scale + bias が [0, GridZ) に収まるよう決める。
        //   slice(nearZ) = 0 / slice(clusterFar) = GridZ
        // WHY 対数か: 点光源のカリングで効くのは深度方向の薄さで、カメラ近傍ほど細かく
        //      切りたい。等間隔だと手前の 1 スライスが広くなりすぎて何も落とせない。
        // Windows.h の min/max マクロ展開を防ぎ、std::max/std::min を確実に呼び出す。
        const float nearZ = (std::max)(camera.m_near, 0.01f);
        const float farZ  = (std::max)((std::min)(camera.m_far, rs.clustered.maxDistance), nearZ * 2.0f);
        const float logRatio = std::log(farZ / nearZ);

        ClusterConstantsCB clusterData{};
        clusterData.clusterTilePx[0] = static_cast<float>(sHdrW) / static_cast<float>(kClusterGridX);
        clusterData.clusterTilePx[1] = static_cast<float>(sHdrH) / static_cast<float>(kClusterGridY);
        clusterData.clusterSliceScale = static_cast<float>(kClusterGridZ) / logRatio;
        clusterData.clusterSliceBias  = -static_cast<float>(kClusterGridZ) * std::log(nearZ) / logRatio;
        clusterData.clusterLightMode  = static_cast<uint32_t>(clusterMode);
        clusterData.punctualLightCount = static_cast<uint32_t>(passCtx.punctualLights.size());
        clusterData.clusterDebugMode  = passCtx.clusterDebugHeatmap ? 1u : 0u;
        resources.Update(clusterCB, &clusterData, sizeof(ClusterConstantsCB));
    }
    passCtx.cameraFrustum           = &cameraFrustum;
    passCtx.lightFrustum            = &lightFrustum;
    passCtx.occlusionCuller         = &occlusionCuller;
    passCtx.physicsWorld  = physicsWorld;
    passCtx.environmentResources = &sEnvironmentResources; // 空連動 IBL の永続状態 (フレームをまたぐ)
    // 雲シャドウ (Phase C): SkyRenderer から読んだ params + 現在時刻を影パスへ渡す。
    passCtx.cloudShadowStrength = skyCloudShadowStrength;
    passCtx.cloudShadowCoverage = skyCloudShadowCoverage;
    passCtx.cloudShadowScale    = skyCloudShadowScale;
    passCtx.cloudShadowSpeed    = skyCloudShadowSpeed;
    passCtx.cloudShadowTime     = Time::time;
    // Shadow トグルを CB まで伝える。
    // WHY: 以前 rs.shadowEnabled は ShadowPass の「描画」しか止めておらず、
    //      ライティング側のシェーダーは影を切っても PCF ループ (既定 7x7 = 49 タップ) を
    //      毎ピクセル回し続けていた。クリア済みの深度を舐めて必ず factor=1 を得るだけの
    //      完全な無駄で、しかも「影を切っても速くならない」ため切り分けの道具としても
    //      機能していなかった。強度 0 を CB へ流し、シェーダー側で早期 return させる。
    passCtx.shadowStrength =
        (rs.shadowEnabled && dirCastShadows) ? dirShadowStrength : 0.0f;
    // 単一カスケード相当のバイアス。カスケードごとの値は ShadowCascade::biasNDC が持つ。
    passCtx.shadowBiasNDC  = cascades[0].biasNDC;
    passCtx.shadowCascadeCount = cascadeCount;
    for (int i = 0; i < cascadeCount; ++i)
        passCtx.shadowCascades[i] = cascades[i];

    // ── 空連動 IBL (環境システム Phase A): source=DynamicSky のとき空→動的 IBL を用意する ──
    // WHY: AdvancedGraphicsCB / 各 Lit パスより前に焼くことで、同フレームで動的 IBL を消費できる。
    //      キャプチャ先・畳み込み出力は RenderGraph 管理外のため、グラフ実行前に直接呼ぶ
    //      (順序が消費パスと厳密化する必要が出た段階で §4-2 のグラフ統合へ移す)。
    //      SkyCapture/SkyLightBake は dirty を内部判定し、不要フレームは即 return する (キャッシュ)。
    bool dynamicIblReady = false;
    float reflectionProbeIntensity = 1.0f;
    int dynamicIblMipCount = 0;
    if (activeIblSource == IblSource::DynamicSky) {
        ExecuteSkyCapturePass(passCtx);
        ExecuteSkyLightBakePass(passCtx);
        if (sEnvironmentResources.HasBakedTextures()) {
            passHandles.iblIrradiance = sEnvironmentResources.skyIrradiance;
            passHandles.iblPrefilter  = sEnvironmentResources.skyPrefilter;
            dynamicIblReady = true;
            dynamicIblMipCount = static_cast<int>(sEnvironmentResources.prefilteredMipCount);
        }
    }

    // 局所 Reflection Probe はカメラが影響範囲内にいるとき、グローバル IBL より優先する。
    // WHY: Lit シェーダーの IBL スロットを既存のまま共有すれば、各マテリアルへ専用分岐や
    //      追加テクスチャを持たせず、空のみ／周辺メッシュ込みの両キャプチャ方式を適用できる。
    if (auto* localProbe = ExecuteReflectionProbeCapturePass(passCtx)) {
        passHandles.iblIrradiance = localProbe->runtimeIrradiance;
        passHandles.iblPrefilter  = localProbe->runtimePrefilter;
        dynamicIblReady = true;
        reflectionProbeIntensity = localProbe->intensity;
        dynamicIblMipCount = static_cast<int>(localProbe->runtimePrefilterMipCount);
    }

    // ---- AdvancedGraphicsCB を毎フレーム更新 ----
    // WHAT: IBL・SSR・TAA・GTAO・ContactShadow 等の詳細設定を AdvancedGraphicsCB(b8) に転送する。
    //       各パスはここで書いたデータを読むだけなので、更新はこの 1 か所に集中させる。
    if (advancedGraphicsCB.IsValid()) {
        AdvancedGraphicsCB agData{};
        // WHY: IBL が無効、または必要なキューブマップが欠けている場合は未バインド SRV を
        //      サンプルさせず、従来の ambient ライティングへ確実にフォールバックする。
        // 動的 IBL (DynamicSky) は .dds アセット (rs.HasValidIblAssets) を持たないため、
        // dynamicIblReady を別経路として許可する。どちらもハンドルが揃っていることを必須にする。
        const bool iblResourcesReady =
            (dynamicIblReady || (rs.ibl.enabled && rs.HasValidIblAssets())) &&
            passHandles.iblIrradiance.IsValid() && passHandles.iblPrefilter.IsValid();
        agData.iblIntensity          = iblResourcesReady ? rs.ibl.intensity * reflectionProbeIntensity : 0.0f;
        agData.iblDiffuseScale       = rs.ibl.diffuseScale;
        agData.iblSpecularScale      = rs.ibl.specularScale;
        // 動的 IBL は SkyLightBake が焼いた prefilter mip 数に合わせる (maxMip = mip 数 - 1)。
        agData.iblMaxMipLevel        = dynamicIblReady
            ? dynamicIblMipCount - 1
            : rs.ibl.maxMipLevel;
        agData.ssrMaxDistance        = rs.ssr.maxDistance;
        agData.ssrThickness          = rs.ssr.thickness;
        agData.ssrSteps              = rs.ssr.steps;
        agData.ssrIntensity          = rs.ssr.enabled ? rs.ssr.intensity : 0.0f;
        agData.volLightIntensity     = rs.volumetricLight.enabled ? rs.volumetricLight.intensity : 0.0f;
        agData.volScattering         = rs.volumetricLight.scattering;
        agData.volSteps              = rs.volumetricLight.steps;
        agData.volMaxDist            = rs.volumetricLight.maxDist;
        agData.volMinDist            = rs.volumetricLight.minDist;
        agData.volDensity            = rs.volumetricLight.density;
        agData.volHeightFalloff      = rs.volumetricLight.heightFalloff;
        agData.volHeightStart        = rs.volumetricLight.heightStart;
        agData.volTintR              = rs.volumetricLight.tint[0];
        agData.volTintG              = rs.volumetricLight.tint[1];
        agData.volTintB              = rs.volumetricLight.tint[2];
        agData.volEdgeFade           = rs.volumetricLight.edgeFade;
        agData.taaFeedback           = rs.taa.feedback;
        agData.taaJitterX            = passCtx.taaJitterNdcX;
        agData.taaJitterY            = passCtx.taaJitterNdcY;
        agData.motionBlurStrength    = rs.motionBlur.enabled ? rs.motionBlur.strength : 0.0f;
        agData.motionBlurSamples     = rs.motionBlur.samples;
        agData.screenWidth           = static_cast<float>(sHdrW);
        agData.screenHeight          = static_cast<float>(sHdrH);
        agData.gtaoIntensity         = rs.IsGtaoActive()   ? rs.gtao.intensity : 0.0f;
        agData.gtaoRadius            = rs.gtao.radius;
        agData.gtaoSlices            = rs.gtao.slices;
        agData.gtaoStepsPerSlice     = rs.gtao.stepsPerSlice;
        agData.contactShadowStrength = rs.contactShadow.enabled ? rs.contactShadow.strength  : 0.0f;
        agData.contactShadowRayLen   = rs.contactShadow.rayLength;
        agData.contactShadowSteps    = rs.contactShadow.steps;
        agData.contactShadowThick    = rs.contactShadow.thickness;
        agData.lensFlareIntensity    = rs.lensFlare.enabled ? rs.lensFlare.intensity : 0.0f;
        agData.lensFlareGhostCount   = rs.lensFlare.ghostCount;
        agData.lensFlareHaloWidth    = rs.lensFlare.haloWidth;
        agData.lensFlareDistort      = rs.lensFlare.distortion;
        agData.pcssLightRadius       = rs.shadow.pcssLightRadius;
        agData.pcssEnabled           = rs.shadow.pcssEnabled ? 1 : 0;
        agData.lutBlend              = (rs.lutColorGrading.enabled && resources.Get(proceduralColorLut) != nullptr)
            ? rs.lutColorGrading.blend : 0.0f;
        // 前フレームの VP 行列 — TAA / Motion Blur が深度再投影で使用する。
        // WHY: viewTargets に持つことでビュー別に分離し、SceneView と GameView が
        //      互いのカメラ行列を参照して壊れる問題を防ぐ。
        agData.prevViewProjection    = viewTargets.prevViewProjection;
        agData.invPrevViewProjection = viewTargets.invPrevViewProjection;
        resources.Update(advancedGraphicsCB, &agData, sizeof(AdvancedGraphicsCB));
        // ここはジッターを載せない (GetViewProjection() のまま) こと。
        // WHY: TAA はジッター込みの invViewProjection でワールド座標を復元し、この行列で
        //      前フレームへ再投影する。両方にジッターを載せると、ジッター差分がそのまま
        //      「動き」として現れて履歴が毎フレームずれ、収束せずに滲む。履歴バッファは
        //      ピクセル中心で収束した絵なので、引く座標もピクセル中心でなければならない。
        viewTargets.prevViewProjection    = camera.GetViewProjection();
        viewTargets.invPrevViewProjection = math::Matrix4::Inverse(camera.GetViewProjection());
    } // end AdvancedGraphicsCB update

    // =========================================================================
    // RenderPipeline にパスを登録
    // =========================================================================
    RenderPipeline& pipeline = viewTargets.pipeline;
    pipeline.BeginBuild();
    profiler::Profiler::BeginSample(
        profiler::ProfilerMarker("RenderSystem::BuildPipeline", "Rendering"));
    pipeline.DeclareResource("Output",     { renderer::RenderGraph::ResourceKind::RenderTarget, sHdrW, sHdrH, 0, true,  false });
    pipeline.DeclareResource("ShadowMap",  { renderer::RenderGraph::ResourceKind::RenderTarget, rs.shadow.mapResolution, rs.shadow.mapResolution, 0, false, false });
    pipeline.DeclareResource("HDR",        { renderer::RenderGraph::ResourceKind::RenderTarget, sHdrW, sHdrH, 0, false, true });
    pipeline.DeclareResource("LDR",        { renderer::RenderGraph::ResourceKind::RenderTarget, sHdrW, sHdrH, 0, false, true });
    pipeline.DeclareResource("SelectionMask", { renderer::RenderGraph::ResourceKind::RenderTarget, sHdrW, sHdrH, 0, false, true });
    pipeline.DeclareResource("Outline",    { renderer::RenderGraph::ResourceKind::RenderTarget, sHdrW, sHdrH, 0, false, true });
    pipeline.DeclareResource("SceneColor", { renderer::RenderGraph::ResourceKind::RenderTarget, sHdrW, sHdrH, 0, false, true });
    pipeline.DeclareResource("CustomPostProcess0", { renderer::RenderGraph::ResourceKind::RenderTarget, sHdrW, sHdrH, 0, false, true });
    pipeline.DeclareResource("CustomPostProcess1", { renderer::RenderGraph::ResourceKind::RenderTarget, sHdrW, sHdrH, 0, false, true });
    pipeline.DeclareResource("Bloom",      { renderer::RenderGraph::ResourceKind::Texture, sHdrW, sHdrH, 0, false, true });
    if (useGBufferOpaquePipeline)
        pipeline.DeclareResource("GBuffer", { renderer::RenderGraph::ResourceKind::RenderTarget, sHdrW, sHdrH, 0, false, false });
    if (ssaoEnabled)
        pipeline.DeclareResource("SSAO",               { renderer::RenderGraph::ResourceKind::Texture, sHdrW, sHdrH, 0, false, true });
    // GTAO / ContactShadows は GBuffer を読んで独自の UAV テクスチャに書く。
    // WHY: "GBuffer"→"GBuffer" で宣言すると GBuffer への偽書き込みとみなされ、
    //      DeferredLighting との依存順が崩れる可能性があるため専用名で宣言する。
    if (useGBufferOpaquePipeline && rs.IsGtaoActive())
        pipeline.DeclareResource("GTAOResult",          { renderer::RenderGraph::ResourceKind::Texture, sHdrW, sHdrH, 0, false, true });
    if (useGBufferOpaquePipeline && rs.contactShadow.enabled)
        pipeline.DeclareResource("ContactShadowResult", { renderer::RenderGraph::ResourceKind::Texture, sHdrW, sHdrH, 0, false, true });
    pipeline.SetOutputs({ "Output" });

    // IBL BRDF LUT 焼き付け — IBLBakePass が対象テクスチャの世代を追跡し、初回のみ実行する。
    // WHY: 512x512 の積分テーブルはシーン・設定に依存しない定数。毎フレーム実行するのは無駄なため
    //      内部フラグでガードし、ここは毎フレーム呼ぶが実処理は初回のみ走る。
    pipeline.AddRawPass("IBLBrdfBake", {}, {}, [&]() {
        ExecuteIBLBakeBrdfLutPass(passCtx);
    }, false); // 外部 ComputeTexture への副作用パスなので RenderGraph カリング禁止

    scene.ClearUserRenderPasses();

    auto appendQueuedUserPasses = [&](UserRenderPassInjectionPoint injectionPoint) {
        for (const auto& desc : scene.GetUserRenderPasses()) {
            if (desc.injectionPoint != injectionPoint)
                continue;
            assert(!desc.name.empty() && "UserRenderPassDesc.name is required");
            auto execute = desc.execute;
            pipeline.AddRawPass(desc.name, desc.accesses, [&, execute]() {
                if (execute)
                    execute(passCtx);
            }, desc.allowCulling);
        }
    };

    // ── Skinning (コンピュート) ───────────────────────────────────────────────
    // WHY Shadow より前: 変形結果をシャドウ・GBuffer・Forward が共有する。
    //     ここで 1 回だけ計算しておかないと、各パスの VS が同じ変形を繰り返す。
    // NOTE: RenderGraph のリソース依存には乗せない (出力は論理リソースではなく
    //       SkinnedMeshRenderer が持つ頂点バッファのため)。カリング禁止で常に実行する。
    pipeline.AddRawPass("SkinningCompute", {}, {}, [&]() {
        ExecuteSkinningComputePass(passCtx);
    }, false);

    // ── クラスタライトカリング ────────────────────────────────────────────────
    // WHY Shadow より前か: Forward / Deferred のどちらの経路でも同じクラスタ結果を読む。
    //     ここで 1 回だけ作っておけば、以降のライティングパスは引くだけで済む。
    // NOTE: 出力は StructuredBuffer で RenderGraph の論理リソースではないため
    //       reads/writes は空。カリング禁止で常に実行する (SkinningCompute と同じ)。
    if (clusteredEnabled) {
        pipeline.AddRawPass("ClusterLightCull", {}, {}, [&]() {
            ExecuteClusterLightCullPass(passCtx);
        }, false);
    }

    // ── Shadow ────────────────────────────────────────────────────────────────
    pipeline.AddRawPass("Shadow", {}, { "ShadowMap" }, [&]() {
        ExecuteShadowPass(passCtx);
    });

    // ── Forward or Deferred ───────────────────────────────────────────────────
    if (!useGBufferOpaquePipeline) {
        pipeline.AddRawPass("ForwardOpaque", { "ShadowMap" }, { "HDR" }, [&]() {
            ExecuteForwardPasses(passCtx);
        });
    }

    if (useGBufferOpaquePipeline) {
        pipeline.AddRawPass("DeferredGBuffer", { "ShadowMap" }, { "GBuffer" }, [&]() {
            ExecuteGBufferPass(passCtx);
        });

        // Deferred Terrain / Detail / Foliage — すべて GBuffer へ書き込む。DepthCopy / AO / Lighting より
        // 前に描くことで、GTAO/SSAO/ContactShadows/SSR/DeferredLighting/IBL が地形・草・樹木へも効く。
        // WHY: forward 描画では GBuffer に入らず、AO/接触影/SSR/PBR ライティングが乗らなかった。
        //      Detail/Foliage はアルファテスト（clip）の不透明として GBuffer へ描く。
        pipeline.AddPass<TerrainRenderPass>();
        pipeline.AddPass<DetailRenderPass>();
        pipeline.AddPass<FoliageRenderPass>();

        pipeline.AddRawPass("DeferredDepthCopy", { "GBuffer" }, { "HDR" }, [&]() {
            ExecuteDeferredDepthCopyPass(passCtx);
        });
    }

    // ── Terrain / Detail / Foliage (Forward パイプライン用) ────────────────────
    // GBuffer 経路が使えないフォールバック Forward では ForwardOpaque / Sky の間に HDR RT (depth 共有) へ描く。
    // WHY: Sky より前に描くことで地形の上に空が被らず、Player 等とも正しく depth test される。
    //      通常は上の GBuffer フェーズで描画済みのためここでは描かない。
    if (!useGBufferOpaquePipeline) {
        pipeline.AddPass<TerrainRenderPass>();
        pipeline.AddPass<DetailRenderPass>();
        pipeline.AddPass<FoliageRenderPass>();
    }

    // Sky / SunMoon — GBuffer フォールバックの Forward ではここ（不透明描画後・雲前）。
    // 通常の GBuffer 経路では DeferredLighting 後に描く（下のブロック）。
    // WHY: GBuffer 経路では Terrain/Detail/Foliage が HDR を書かず GBuffer へ描くため、Sky の HDR 書き込みが
    //      DeferredDepthCopy（HDR をクリアする）との順序保証を失い、グラフが Sky を DepthCopy より前に
    //      並べるとクリアでスカイが消える。Lighting 後に置くと HDR 依存チェーンで DepthCopy より確実に後になる。
    if (!useGBufferOpaquePipeline) {
        pipeline.AddRawPass("Sky", { "HDR" }, { "HDR" }, [&]() {
            ExecuteSkyPass(passCtx);
        });
        pipeline.AddRawPass("SunMoon", { "HDR" }, { "HDR" }, [&]() {
            ExecuteSunMoonPass(passCtx);
        });

        // VolumetricCloud — GBuffer フォールバックの Forward では Sky 後・透明物前に HDR へ合成する。
        // WHY: 空を背景にしつつ、後続の水面・透明エフェクトで上書きできる順序にする。
        pipeline.AddRawPass("VolumetricCloud", { "HDR" }, { "HDR" }, [&]() {
            ExecuteVolumetricCloudPass(passCtx);
        });
    }

    // ── SSAO + Deferred Lighting ──────────────────────────────────────────────
    if (useGBufferOpaquePipeline) {
        // GTAO — DeferredLighting より前に GBuffer から AO を計算する。
        // WHY: DeferredLighting は t23 (TEX_GTAO) を SRV として読む。
        //      "GTAOResult" として宣言することで GBuffer への偽書き込みを除去し、
        //      DeferredLighting が正確な依存でこの出力を待てるようにする。
        if (rs.IsGtaoActive()) {
            pipeline.AddRawPass("GTAO", { "GBuffer" }, { "GTAOResult" }, [&]() {
                ExecuteGTAOPass(passCtx);
            });
        }
        // ContactShadows — DeferredLighting より前に深度から接触影マスクを生成する。
        // WHY: GTAO と同様に ContactShadowResult として宣言し偽依存を除去する。
        if (rs.contactShadow.enabled) {
            pipeline.AddRawPass("ContactShadows", { "GBuffer" }, { "ContactShadowResult" }, [&]() {
                ExecuteContactShadowsPass(passCtx);
            });
        }
        if (ssaoEnabled) {
            pipeline.AddRawPass("SSAO", { "GBuffer" }, { "SSAO" }, [&]() {
                ExecuteSSAOPass(passCtx);
            });
        }
        // DeferredLighting の reads を動的に構築し、GTAO/ContactShadows/SSAO が有効な
        // ときだけその出力への依存を宣言する。
        // WHY: 静的な reads 文字列では有効/無効の組み合わせごとに分岐が必要になり、
        //      将来の AO 種類追加時にも変更が局所化されない。
        {
            using RA = renderer::RenderGraph::ResourceAccess;
            using RU = renderer::RenderGraph::ResourceUsage;
            std::vector<RA> deferredAccesses = {
                { "GBuffer", RU::Read     },
                { "HDR",     RU::ReadWrite }, // 深度を読み、ライティング結果を書く
            };
            if (ssaoEnabled)              deferredAccesses.push_back({ "SSAO",               RU::Read });
            if (rs.IsGtaoActive())        deferredAccesses.push_back({ "GTAOResult",          RU::Read });
            if (rs.contactShadow.enabled) deferredAccesses.push_back({ "ContactShadowResult", RU::Read });
            pipeline.AddRawPass("DeferredLighting", std::move(deferredAccesses), [&]() {
                ExecuteDeferredLightingPass(passCtx);
            });
        }

        // Sky / SunMoon — GBuffer ライティング後に HDR へ描く。
        // WHY: スカイドームは深度==1.0（最遠面）のピクセルにだけ描かれる。Lighting 後に描くことで
        //      ジオメトリ確定後の背景を埋め、かつ HDR 依存チェーンで DeferredDepthCopy の HDR クリアより
        //      確実に後段になり、クリアでスカイが消える問題を避ける。
        pipeline.AddRawPass("Sky", { "HDR" }, { "HDR" }, [&]() {
            ExecuteSkyPass(passCtx);
        });
        pipeline.AddRawPass("SunMoon", { "HDR" }, { "HDR" }, [&]() {
            ExecuteSunMoonPass(passCtx);
        });

        // VolumetricCloud — GBuffer Lighting / Sky 後・透明物前に HDR へ合成する。
        // WHY: Lighting・空に上書きされず、透明物や水面を雲の手前に描ける順序にする。
        pipeline.AddRawPass("VolumetricCloud", { "GBuffer", "HDR" }, { "HDR" }, [&]() {
            ExecuteVolumetricCloudPass(passCtx);
        });

        pipeline.AddRawPass("DeferredSkinnedForward", { "HDR" }, { "HDR" }, [&]() {
            ExecuteDeferredSkinnedForwardPass(passCtx);
        });

        pipeline.AddRawPass("DeferredForwardTransparent", { "HDR" }, { "HDR" }, [&]() {
            ExecuteDeferredForwardTransparentPass(passCtx);
        });

        // SSR — 全透明オブジェクト描画後に GBuffer の法線・深度・金属度を使って反射を計算する。
        // WHY: 選択中の Forward/Deferred ではなく GBuffer の有無が実行条件。
        //      透明オブジェクト通過後の深度を使うため、DeferredForwardTransparent の後に配置する。
        if (rs.ssr.enabled) {
            pipeline.AddRawPass("SSR", { "GBuffer", "HDR" }, { "HDR" }, [&]() {
                ExecuteSSRPass(passCtx);
            });
        }
    }

    // WaterCaustics — 水面下の不透明ジオメトリへコースティクスを投影してから、水面本体を透明描画する。
    // WHY: Water の後に加算すると水面そのものへ模様が乗りやすいため、深度が不透明物だけを指す段階で実行する。
    pipeline.AddRawPass("WaterCaustics", { "HDR" }, { "HDR" }, [&]() {
        ExecuteCausticsPass(passCtx);
    });

    pipeline.AddPass<WaterRenderPass>();

    for (EntityID id : scene.GetEntities<ScriptComponent>()) {
        auto* sc = scene.GetComponent<ScriptComponent>(id);
        auto* go = scene.GetGameObject(id);
        if (!sc || !go)
            continue;
        for (auto& entry : sc->scripts) {
            if (!entry.script || !entry.script->enabled)
                continue;
            entry.script->SetContext(&scene, go);
            entry.script->ExecuteCallback(&Script::OnSetupRenderPasses, pipeline, passCtx);
        }
    }

    appendQueuedUserPasses(UserRenderPassInjectionPoint::AfterOpaque);

    // ── デカール用深度スナップショット ────────────────────────────────────────
    pipeline.DeclareResource("DecalDepth", { renderer::RenderGraph::ResourceKind::RenderTarget, sHdrW, sHdrH, 0, false, true });
    pipeline.AddRawPass("DecalDepthCopy", { useGBufferOpaquePipeline ? "GBuffer" : "HDR" }, { "DecalDepth" }, [&]() {
        renderer.SetRenderTarget(decalDepthRT, resources);
        renderer.ClearDepth();
        if (depthCopyShader.IsValid()) {
            renderer::DrawCall dc;
            dc.shader        = depthCopyShader;
            dc.pipelineState = defaultPSO;
            dc.vertexCount   = 3;
            dc.textures[7]   = useGBufferOpaquePipeline
                ? resources.GetDepthTexture(gbufferRT)
                : resources.GetDepthTexture(hdrRT);
            renderer.Submit(dc, resources);
        }
    });

    // ── Decal + Trail + Particle ──────────────────────────────────────────────
    pipeline.AddRawPass("Decal", { "HDR", "DecalDepth" }, { "HDR" }, [&]() {
        ExecuteDecalPass(passCtx);
    });

    pipeline.AddPass<MeshTrailRenderPass>();
    pipeline.AddPass<TrailRenderPass>();

    pipeline.AddRawPass("Particle", { "HDR", "DecalDepth" }, { "HDR" }, [&]() {
        ExecuteParticlePass(passCtx);
    });

    // Overdraw 可視化は診断表示。有効なときだけ Particle の直後に HDR を上書きする。
    // GPU 時間を Particle パスの実測値と混ぜないよう、別パスとして計測させる。
    if (rs.particleOverdrawView) {
        pipeline.AddRawPass("ParticleOverdraw", { "HDR" }, { "HDR" }, [&]() {
            ExecuteParticleOverdrawPass(passCtx);
        });
    }

    appendQueuedUserPasses(UserRenderPassInjectionPoint::AfterTransparent);

    // ── Selection / Debug ─────────────────────────────────────────────────────
    if (selectionOutlineEnabled) {
        pipeline.AddRawPass("SelectionMask", { "HDR" }, { "SelectionMask" }, [&]() {
            ExecuteSelectionMaskPass(passCtx);
        });
    }

    pipeline.AddPass<ConstraintDebugPass>();
    pipeline.AddPass<AnimatorDebugPass>();
    pipeline.AddPass<GridDebugPass>();
    pipeline.AddPass<LightRangeDebugPass>();
    pipeline.AddPass<VFXGizmoDebugPass>();
    pipeline.AddPass<TerrainCollisionDebugPass>();

    pipeline.AddRawPass("ScriptDebugDraw", { "HDR" }, { "HDR" }, [&]() {
        scene.TickScriptDebugDrawCommands(Time::deltaTime);
        renderer::DebugDraw::BeginFrame(passCtx.renderer, passCtx.resources, passCtx.camera.GetViewProjection());

        // OnDrawGizmos: DebugDraw::BeginFrame/Flush の区間内で Script が gizmo プロキシを使って
        // 視野錐・ウェイポイント・検知範囲などを直接描画する。
        // WHY: コマンドキュー経由の ScriptDebugProxy と異なり、Gizmo の複合プリミティブは
        //      レンダリング区間内で直接呼ぶ必要があるため、専用コールバックを設ける。
        for (EntityID id : scene.GetEntities<ScriptComponent>()) {
            auto* sc = scene.GetComponent<ScriptComponent>(id);
            auto* go = scene.GetGameObject(id);
            if (!sc || !go) continue;
            for (auto& entry : sc->scripts) {
                if (!entry.script || !entry.script->enabled) continue;
                entry.script->SetContext(&scene, go);
                entry.script->gizmo.renderer = &passCtx.renderer;
                entry.script->ExecuteCallback(&Script::OnDrawGizmos, "OnDrawGizmos");
                entry.script->gizmo.renderer = nullptr;
            }
        }

        for (const auto& command : scene.GetScriptDebugDrawCommands()) {
            switch (command.type) {
            case ScriptDebugDrawType::Line:
                renderer::DebugDraw::Line(passCtx.renderer, command.a, command.b, command.color);
                break;
            case ScriptDebugDrawType::Sphere:
                renderer::DebugDraw::Sphere(passCtx.renderer, command.a, command.radius, command.color);
                break;
            case ScriptDebugDrawType::Box:
                renderer::DebugDraw::Box(passCtx.renderer, command.a, command.halfExtents, command.color);
                break;
            case ScriptDebugDrawType::Ray:
                renderer::DebugDraw::Line(passCtx.renderer, command.a, command.b, command.color);
                break;
            case ScriptDebugDrawType::Arrow:
                // headLength = radius, headRadius = halfExtents.x
                renderer::DebugDraw::Arrow(passCtx.renderer, command.a, command.b,
                                           command.radius, command.halfExtents.x, command.color);
                break;
            case ScriptDebugDrawType::Cone:
                // direction = b, height = halfExtents.x, baseRadius = radius
                renderer::DebugDraw::Cone(passCtx.renderer, command.a, command.b,
                                          command.halfExtents.x, command.radius, command.color);
                break;
            }
        }
        renderer::DebugDraw::Flush();
    });

    // コライダーは Script の Gizmo より後に描く。
    // WHY: デバッグ線はすべて深度オフの 1px ラインなので、Gizmo とコライダーが同じ形を
    //      同じ位置に出すと、どちらのピクセルが残るかがサブピクセルの被り方で決まり、
    //      カメラが動くたびにちらついていた。コライダー側を破線にして最後に描けば、
    //      隙間から Gizmo が見えて両方とも読める (DebugCollidersPass の DASH_* を参照)。
    pipeline.AddPass<DebugCollidersPass>();

    pipeline.AddPass<NavMeshDebugPass>();
    pipeline.AddPass<DecalDebugPass>();

    appendQueuedUserPasses(UserRenderPassInjectionPoint::BeforePostProcess);

    // ── PostProcess チェーン ──────────────────────────────────────────────────
    // MotionBlur CS — HDR 空間でカメラモーションブラーを計算し motionBlurResult に書く。
    // WHY: Composite パスが motionBlurResult を hdrRT の代わりに読む。
    //      Bloom の前に走らせることで blur 後の輝度が Bloom に乗る。
    if (rs.motionBlur.enabled) {
        pipeline.AddRawPass("MotionBlur", { "HDR" }, { "HDR" }, [&]() {
            ExecuteMotionBlurPass(passCtx);
        });
    }
    // VolumetricLight CS — ゴッドレイ・光柱を HDR バッファに加算合成する。
    // WHY: Bloom の前に配置することで体積光が Bloom に乗り、より明るい光の広がりが出る。
    if (rs.volumetricLight.enabled) {
        pipeline.AddRawPass("VolumetricLight", { "HDR", "ShadowMap" }, { "HDR" }, [&]() {
            ExecuteVolumetricLightPass(passCtx);
        });
    }
    // LensFlare PS — 輝度抽出した光源を ADDITIVE に HDR に合成する。
    // WHY: Bloom の前に置くことでフレアも Bloom に乗る。ただしその順序では bloomHalf に
    //      今フレームの輝点がまだ無いため、パス自身が bloomHalf へ輝度抽出を焼いてから読む
    //      (Bloom として宣言しているのはこの書き込み)。hdrRT とは別リソースなので競合しない。
    if (rs.lensFlare.enabled) {
        pipeline.AddRawPass("LensFlare", { "HDR" }, { "HDR", "Bloom" }, [&]() {
            ExecuteLensFlarePass(passCtx);
        });
    }
    if (rs.postProcess.bloom.enabled) {
        pipeline.AddRawPass("Bloom", { "HDR" }, { "Bloom" }, [&]() {
            ExecuteBloomPass(passCtx);
        });
    }

    const bool customPostProcessEnabled =
        !customPostProcessIndices.empty() &&
        customPostProcessRT[0].IsValid() &&
        customPostProcessRT[1].IsValid();
    // hasPostCompositeEffects: Composite の出力先が "LDR" か "Output" かを決める。
    // WHY: このフラグが true なら Composite は ldrRT に書き、後続エフェクトがチェーンを形成する。
    const bool hasPostCompositeEffects =
        rs.IsTaaActive() || customPostProcessEnabled || selectionOutlineEnabled || rs.postProcess.fxaaEnabled;

    if (rs.postProcess.bloom.enabled) {
        pipeline.AddRawPass("Composite", { "HDR", "Bloom" }, { hasPostCompositeEffects ? "LDR" : "Output" }, [&]() {
            ExecuteCompositePass(passCtx);
        });
    } else {
        pipeline.AddRawPass("Composite", { "HDR" }, { hasPostCompositeEffects ? "LDR" : "Output" }, [&]() {
            ExecuteCompositePass(passCtx);
        });
    }

    // ---- Post-composite チェーン ----
    // ppCurrent: 「LDR 空間の最新フレームを持つ RenderGraph リソース名」を追跡する。
    // WHY: エフェクトごとに条件分岐でリソース名を手動管理する代わりに ppCurrent を
    //      進めることで、TAA/CustomPP/SelectionOutline/FXAA の組み合わせを
    //      単一の直列チェーンとして表現できる。
    std::string ppCurrent  = hasPostCompositeEffects ? "LDR" : "Output";
    int         ppPingPong = 0; // customPostProcessRT の ping-pong インデックス

    // TAA — 最初に適用することで後続の CustomPP/SelectionOutline が TAA 済み映像に乗る。
    // WHY: CustomPP より前に登録することで RenderGraph が TAA → CustomPP の
    //      依存順を正しく解決する (Kahn's algorithm は登録順をタイブレークに使う)。
    if (rs.IsTaaActive()) {
        pipeline.AddRawPass("TAA", { ppCurrent }, { ppCurrent }, [&]() {
            ExecuteTAAPass(passCtx);
            // taaFlip は ExecuteTAAPass 内で反転済み — 反転後のフラグで「書いた方」を特定する。
            auto& taaOut = passHandles.taaFlip ? passHandles.taaHistoryB : passHandles.taaHistoryA;
            passHandles.fxaaInput = resources.GetColorTexture(taaOut, 0);
            // WHY: CustomPP / SelectionOutline は postProcessInput を参照する。
            //      TAA 後は履歴バッファが最新フレームなので postProcessInput も更新する。
            //      更新しないと後続エフェクトが TAA 適用前の ldrRT を誤読する。
            passHandles.postProcessInput = passHandles.fxaaInput;
        });
    }

    // Custom PostProcess チェーン
    for (uint32_t i = 0; i < static_cast<uint32_t>(customPostProcessIndices.size()); ++i) {
        const uint32_t customIndex  = customPostProcessIndices[i];
        const bool     isLastEffect = (i + 1 == static_cast<uint32_t>(customPostProcessIndices.size()))
                                       && !selectionOutlineEnabled
                                       && !rs.postProcess.fxaaEnabled;
        // outputIndex == 2 → ExecuteCustomPostProcessPass が ctx.outputRT に直書きする規約
        const uint32_t    outputIndex = isLastEffect ? 2u : static_cast<uint32_t>(ppPingPong % 2);
        const std::string outRes      = isLastEffect
            ? "Output"
            : ("CustomPostProcess" + std::to_string(outputIndex));
        pipeline.AddRawPass(
            "CustomPostProcess" + std::to_string(i),
            { ppCurrent },
            { outRes },
            [&, customIndex, outputIndex]() {
                ExecuteCustomPostProcessPass(passCtx, customIndex, outputIndex);
            });
        ppCurrent = outRes;
        ++ppPingPong;
    }

    // SelectionOutline
    if (selectionOutlineEnabled) {
        const bool        isLastEffect = !rs.postProcess.fxaaEnabled;
        const std::string outRes       = isLastEffect ? "Output" : "Outline";
        pipeline.AddRawPass("SelectionOutline",
            { ppCurrent, "SelectionMask" },
            { outRes },
            [&]() { ExecuteSelectionOutlinePass(passCtx); });
        ppCurrent = outRes;
    }

    // FXAA
    if (rs.postProcess.fxaaEnabled) {
        pipeline.AddRawPass("FXAA", { ppCurrent }, { "Output" }, [&]() { ExecuteFxaaPass(passCtx); });
        ppCurrent = "Output";
    }

    // TAA_Blit — TAA が有効で後続エフェクトが何もない場合のみ OutputRT への転送が必要。
    // WHY: TAA は ping-pong 履歴バッファにのみ書き OutputRT には書かない。
    //      FXAA/SelectionOutline/CustomPP がすべて無効のとき ppCurrent は "LDR" のままなので
    //      ここで Output に届ける。ppCurrent が "Output" なら既に書かれているためスキップ。
    if (ppCurrent != "Output") {
        pipeline.AddRawPass("TAA_Blit", { ppCurrent }, { "Output" }, [&]() {
            ExecuteTAABlitPass(passCtx);
        });
    }

    if (uiOptions && uiOptions->enabled && uiOptions->context) {
        pipeline.AddRawPass(
            "UIPass",
            { { "Output", renderer::RenderGraph::ResourceUsage::ReadWrite } },
            [&]() {
                // WHY: UI は最終フレームへの合成であり、Composite / FXAA / CustomPostProcess の
                //      どの分岐が最後に Output を書いたかに依存してはいけない。
                //      Output を ReadWrite する RenderGraph pass として登録し、この pass 内で
                //      明示的に outputRT をバインドすることで、Game / Scene / UI Viewport の
                //      いずれでも同じ順序と同じ RT に描画できる。
                renderer.SetRenderTarget(outputRT, resources);
                const float uiWidth = uiOptions->viewportWidth > 0.0f
                    ? uiOptions->viewportWidth
                    : static_cast<float>(sHdrW);
                const float uiHeight = uiOptions->viewportHeight > 0.0f
                    ? uiOptions->viewportHeight
                    : static_cast<float>(sHdrH);
                // デバッグ表示の可否は RenderSettings が持つ。UISystem は設定の
                // 所有者を知らない自由関数なので、知っている側が毎フレーム入れる。
                uiOptions->context->showRects = passCtx.settings.showUIRects;
                UISystem(scene,
                         renderer,
                         resources,
                         *uiOptions->context,
                         uiWidth,
                         uiHeight,
                         uiOptions->mouseInCanvasSpace,
                         uiOptions->mousePressed,
                         camera.m_position,
                         camera.m_rotation,
                         camera.GetViewProjection(),
                         uiOptions->targetView);
            });
    }
    profiler::Profiler::EndSample();

    {
        FBZZ_PROFILE_SCOPE("RenderSystem::ScriptPreRender");
        for (EntityID id : scene.GetEntities<ScriptComponent>()) {
            auto* sc = scene.GetComponent<ScriptComponent>(id);
            auto* go = scene.GetGameObject(id);
            if (!sc || !go)
                continue;
            for (auto& entry : sc->scripts) {
                if (!entry.script || !entry.script->enabled)
                    continue;
                entry.script->SetContext(&scene, go);
                entry.script->ExecuteCallback(&Script::OnPreRender, "OnPreRender");
            }
        }
    }

    // =========================================================================
    // RenderPipeline 実行 + デバッグスナップショット更新
    // =========================================================================

    // GPU Timestamp Query の前フレーム結果を収集してからフレームを開始する。
    // WHY: GpuProfCollect を先に呼ぶことで前フレームの非同期クエリが確定している可能性を最大化する。
    {
        FBZZ_PROFILE_SCOPE("RenderSystem::GpuProfilerSetup");
        renderer.GpuProfCollect();
        renderer.GpuProfBeginFrame();

        // GPU フックを RenderPipeline に設定する。CPU フックとは独立しているため、
        // Profiler の CPU スコープ計測と干渉しない。
        pipeline.SetGpuProfilerHooks(
            [&](std::string_view name) { renderer.GpuProfBeginPass(name.data()); },
            [&](std::string_view name) { renderer.GpuProfEndPass(name.data()); }
        );
    }

    const bool graphExecuted = pipeline.Execute(passCtx);

    renderer.GpuProfEndFrame();
    assert(graphExecuted);
    (void)graphExecuted;

    {
        FBZZ_PROFILE_SCOPE("RenderSystem::ScriptPostRender");
        for (EntityID id : scene.GetEntities<ScriptComponent>()) {
            auto* sc = scene.GetComponent<ScriptComponent>(id);
            auto* go = scene.GetGameObject(id);
            if (!sc || !go)
                continue;
            for (auto& entry : sc->scripts) {
                if (!entry.script || !entry.script->enabled)
                    continue;
                entry.script->SetContext(&scene, go);
                entry.script->ExecuteCallback(&Script::OnPostRender, "OnPostRender");
            }
        }
    }

    {
        FBZZ_PROFILE_SCOPE("RenderSystem::DebugSnapshot");
        renderer::RenderDebugOverlay::Snapshot dbgSnap;
        dbgSnap.hdrRT           = hdrRT;
        dbgSnap.ldrRT           = ldrRT;
        dbgSnap.selectionMaskRT = selectionMaskRT;
        dbgSnap.outlineRT       = outlineRT;
        dbgSnap.gbufferRT       = gbufferRT;
        dbgSnap.width           = sHdrW;
        dbgSnap.height          = sHdrH;
        for (const auto& profile : pipeline.LastReport().profiles)
            dbgSnap.passTimings.push_back({ profile.name, profile.cpuMilliseconds });

        // GPU 計測結果を Snapshot に詰める。QUERY_LATENCY フレーム以内は空になる。
        for (const auto& gp : renderer.GpuProfGetResults())
            dbgSnap.gpuPassTimings.push_back({ gp.name, gp.gpuMs });

        // カリング統計を Snapshot に詰める
        dbgSnap.renderStats.totalObjects    = passCtx.statsTotalObjects;
        dbgSnap.renderStats.frustumCulled   = passCtx.statsFrustumCulled;
        dbgSnap.renderStats.occlusionCulled = passCtx.statsOcclusionCulled;
        dbgSnap.renderStats.distanceCulled    = passCtx.statsDistanceCulled;
        dbgSnap.renderStats.smallObjectCulled = passCtx.statsSmallObjectCulled;
        dbgSnap.renderStats.drawCalls       = passCtx.statsDrawCalls;
        dbgSnap.renderStats.vertexCount     = passCtx.statsVertexCount;
        dbgSnap.renderStats.triangleCount   = passCtx.statsTriangleCount;
        dbgSnap.renderStats.skinningVertexCount = passCtx.statsSkinningVertexCount;
        dbgSnap.renderStats.skinningDispatchCount = passCtx.statsSkinningDispatchCount;
        dbgSnap.renderStats.shadowDrawCalls     = passCtx.statsShadowDrawCalls;
        dbgSnap.renderStats.shadowTriangleCount = passCtx.statsShadowTriangleCount;

        renderer::RenderDebugOverlay::UpdateSnapshot(dbgSnap, rs.passViewerEnabled);
    }
}

} // namespace fbzz::scene
