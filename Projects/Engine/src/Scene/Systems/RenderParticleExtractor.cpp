/// @file    RenderParticleExtractor.cpp
/// @brief   パーティクルの CPU シミュレーション・GPU ディスパッチ・描画。
/// @author  Hasegawa Jin
/// @date    2026-06-18

#include <Engine/Asset/StreamedTextureResolver.hpp>
#include "RenderPasses/Geometry/GeometryPasses.hpp"
#include <Engine/Scene/Systems/RenderParticleExtractor.hpp>
#include <Graphics/Renderer/RenderScene.hpp>
#include "RenderPasses/Geometry/FlowFieldGpu.hpp"
#include <Engine/Scene/Systems/ParticleOverdrawStats.hpp>
#include <Engine/Scene/Systems/ParticleSimulationRuntime.hpp>
#include "Engine/Scene/Scene.hpp"
#include "Engine/Scene/Transform.hpp"
#include "Engine/Scene/Components/ParticleEmitter.hpp"
#include "Engine/Scene/Fields/FlowField.hpp"
#include "Engine/Scene/Components/ParticleGpuSimulation.hpp"
#include "Engine/Scene/Components/MeshRenderer.hpp"
#include "Engine/Scene/Components/AnimatorComponent.hpp"
#include "Engine/Asset/AssetManager.hpp"
#include "Engine/Asset/MaterialAsset.hpp"
#include "Engine/Asset/MaterialParamBinding.hpp"
#include "Fluid/VectorFieldAsset.hpp"
#include "Engine/Asset/VelocityFieldAtlas.hpp"
#include <Engine/Scene/Components/ParticleEmitterSpace.hpp>
#include <Engine/Scene/Fields/FlowFieldEval.hpp>
#include <Engine/Scene/Fields/FlowFieldFrame.hpp>
#include "Engine/Renderer/ShaderDescriptor.hpp"
#include "Engine/Asset/Model.hpp"
#include "Engine/Core/Logger.hpp"
#include "Engine/Core/Time.hpp"
#include "Engine/Renderer/ComputeCall.hpp"
#include "Engine/Renderer/DrawCall.hpp"
#include "Engine/Renderer/DynamicBufferPool.hpp"
#include "Engine/Renderer/RenderState.hpp"
#include <Physics/World.hpp>
#include <Math/MathUtils.hpp>
#include <Math/Vector4.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fbzz::scene {

namespace {

inline math::Vector4 LerpVec4(const math::Vector4& a, const math::Vector4& b, float t)
{
    return { a.x + (b.x - a.x) * t,
             a.y + (b.y - a.y) * t,
             a.z + (b.z - a.z) * t,
             a.w + (b.w - a.w) * t };
}

float Clamp01(float value)
{
    return (std::max)(0.0f, (std::min)(value, 1.0f));
}

/// @brief 粒子の軌跡を一定間隔でサンプリングして履歴へ積む。
/// @brief [0] を最新として後ろへずらす。点数が最大 8 と小さいので、リングバッファではなく
/// @brief 素直なシフトにする (描画側が「新しい順」を仮定でき、読み手が追いやすい)。
void AppendParticleTrailPoint(const ParticleEmitter& emitter, Particle& particle, float dt)
{
    if (!emitter.settings.trail.trailEnabled) {
        particle.trailCount = 0;
        return;
    }
    const int capacity = std::clamp(emitter.settings.trail.trailPointCount, 1, kMaxParticleTrailPoints);
    particle.trailSampleTimer += dt;
    /// @note 間隔 0 を許すと 1 フレームに何度も積んで履歴が一瞬で埋まるため下限を切る。
    const float interval = (std::max)(emitter.settings.trail.trailSampleInterval, 0.001f);
    if (particle.trailSampleTimer < interval) return;
    particle.trailSampleTimer = 0.0f;

    for (int index = (std::min)(static_cast<int>(particle.trailCount), capacity - 1); index > 0; --index)
        particle.trailPoints[static_cast<std::size_t>(index)] =
            particle.trailPoints[static_cast<std::size_t>(index - 1)];
    particle.trailPoints[0] = particle.position;
    if (particle.trailCount < capacity)
        particle.trailCount = static_cast<uint8_t>(particle.trailCount + 1);
}

uint32_t NextParticleRandom(ParticleEmitter& emitter)
{
    /// @note Numerical Recipes 系 LCG。軽量で、エミッターごとの seed から決定的な乱数列を作る。
    /// @note `std::rand()` はグローバル状態のため、複数エミッターや再生順序で結果が変わりやすい。
    if (emitter.runtime.randomState == 0) {
        emitter.runtime.randomState = emitter.settings.randomSeed != 0 ? emitter.settings.randomSeed : 1;
    }
    emitter.runtime.randomState = emitter.runtime.randomState * 1664525u + 1013904223u;
    return emitter.runtime.randomState;
}

float RandomSigned01(ParticleEmitter& emitter)
{
    constexpr float INV_MAX_UINT = 1.0f / 4294967295.0f;
    return static_cast<float>(NextParticleRandom(emitter)) * INV_MAX_UINT * 2.0f - 1.0f;
}

float Random01(ParticleEmitter& emitter)
{
    return Clamp01(RandomSigned01(emitter) * 0.5f + 0.5f);
}

/// @brief 粒子ごとの色ゆらぎ倍率を 1 粒子ぶん引く。RGB を各チャンネル独立に [1-v, 1+v] 倍して
/// @brief 群れの単調さを崩す。alpha は返さない (フェード制御なのでゆらすと消え際が汚くなる)。
/// @brief 色ではなく倍率を返すのは、グラデーション使用時に色が毎フレーム作り直されるため
/// @brief (スポーン時に焼き込むと翌フレームには消える)。
/// @brief CPU/GPU どちらのスポーン経路からも同じ乱数列で呼ぶので結果は決定論的に一致する。
math::Vector3 NextColorVariation(ParticleEmitter& emitter)
{
    const float variation = Clamp01(emitter.settings.colorVariation);
    if (variation <= 0.0f) return { 1.0f, 1.0f, 1.0f };
    const auto jitter = [&emitter, variation]() {
        return (std::max)(0.0f, 1.0f + RandomSigned01(emitter) * variation);
    };
    const float scaleR = jitter();
    const float scaleG = jitter();
    const float scaleB = jitter();
    return { scaleR, scaleG, scaleB };
}

/// @brief 寿命 t における粒子色をリニアで求める。グラデーション経路と start/end 経路の
/// @brief どちらでも、色空間変換とゆらぎの適用順序を 1 か所に集める。
/// @brief 散らすと (以前そうだったように) 更新ループだけがゆらぎを取りこぼす。
/// @brief 発生量の時間倍率。emitRate へ掛ける。
/// @brief 粒の寿命ではなく再生時刻で引く — これはエミッターの «出し方» であって粒の性質ではない。
[[nodiscard]] float EmitRateCurveScale(const ParticleEmitter& emitter)
{
    if (!emitter.settings.useEmitRateCurve) return 1.0f;
    const float span = (std::max)(emitter.settings.duration, 0.0001f);
    float t = emitter.runtime.playTime / span;
    /// @note loop するエミッターは 1 周ごとに同じ形を繰り返す。
    if (emitter.settings.loop) t -= std::floor(t);
    return (std::max)(emitter.settings.emitRateCurve.Evaluate(Clamp01(t)), 0.0f);
}

/// @brief 粒の «速さ» を speedRange で正規化した 0..1。
/// @brief Local 空間では velocity もローカル量なので、エミッターをスケールすると読みが変わる。
[[nodiscard]] float NormalizedParticleSpeed(const ParticleEmitter& emitter, const Particle& particle)
{
    const float range = (std::max)(emitter.settings.speedRange, 0.0001f);
    return Clamp01(particle.velocity.Length() / range);
}

math::Vector4 EvaluateParticleColorLinear(const ParticleEmitter& emitter,
                                          const Particle& particle, float normalizedAge)
{
    math::Vector4 color = emitter.settings.useColorGradient
        ? emitter.runtime.runtimeGradient.EvaluateLinear(normalizedAge)
        : ParticleSrgbToLinear(LerpVec4(particle.startColor, particle.endColor,
                                        std::pow(normalizedAge, emitter.settings.colorCurvePower)));
    color.x *= particle.colorScale.x;
    color.y *= particle.colorScale.y;
    color.z *= particle.colorScale.z;
    /// @note 速さによる色。寿命の色へ «乗算» する。置き換えると、跳ね返って減速した粒だけ
    /// @note 寿命の色を失う。
    if (emitter.settings.useSpeedColorGradient) {
        const math::Vector4 tint = emitter.settings.speedColorGradient.EvaluateLinear(
            NormalizedParticleSpeed(emitter, particle));
        color.x *= tint.x;
        color.y *= tint.y;
        color.z *= tint.z;
        color.w *= tint.w;
    }
    return color;
}

math::Vector3 RandomUnitVector(ParticleEmitter& emitter)
{
    constexpr float PI = 3.14159265358979323846f;
    const float z = RandomSigned01(emitter);
    const float a = Random01(emitter) * PI * 2.0f;
    const float r = std::sqrt((std::max)(0.0f, 1.0f - z * z));
    return { r * std::cos(a), z, r * std::sin(a) };
}

struct SpriteFrameState {
    math::Vector4 currentRect;
    math::Vector4 nextRect;
    float blend = 0.0f;
};

math::Vector4 SpriteRectForFrame(int frame, int columns, int rows)
{
    const int x = frame % columns;
    const int y = frame / columns;
    const float invColumns = 1.0f / static_cast<float>(columns);
    const float invRows = 1.0f / static_cast<float>(rows);
    return { static_cast<float>(x) * invColumns, static_cast<float>(y) * invRows,
             static_cast<float>(x + 1) * invColumns, static_cast<float>(y + 1) * invRows };
}

SpriteFrameState ComputeSpriteFrameState(const ParticleEmitter& emitter, float normalizedAge,
                                         float ageSeconds = 0.0f, float spriteSeed = 0.0f)
{
    /// @note コマ選びの規則は EvaluateFlipbookFrame が正本。.mat Inspector のプレビューも同じ関数を
    /// @note 呼ぶので、ここで独自に丸めると «プレビューとゲームで 1 コマずれる» になる。
    const asset::ParticleFlipbookSettings& flipbook = emitter.runtime.material.flipbook;
    const asset::FlipbookFrameSample sample =
        asset::EvaluateFlipbookFrame(flipbook, normalizedAge, ageSeconds, spriteSeed);
    const int columns = (std::max)(flipbook.spriteColumns, 1);
    const int rows    = (std::max)(flipbook.spriteRows, 1);
    SpriteFrameState state;
    state.currentRect = SpriteRectForFrame(sample.frame, columns, rows);
    state.nextRect    = SpriteRectForFrame(sample.nextFrame, columns, rows);
    state.blend       = sample.blend;
    return state;
}

math::Vector4 ComputeSpriteRect(const ParticleEmitter& emitter, float normalizedAge,
                                float ageSeconds = 0.0f, float spriteSeed = 0.0f)
{
    return ComputeSpriteFrameState(emitter, normalizedAge, ageSeconds, spriteSeed).currentRect;
}

const AnimatorComponent* FindParticleAnimator(GameObject& object)
{
    /// @note VFX Graphの生成Particleはownerの子になるため、Skinned Mesh spawnは祖先Animatorも参照する。
    for (GameObject* current = &object; current != nullptr; current = current->GetParent())
        if (const auto* animator = current->GetComponent<AnimatorComponent>()) return animator;
    return nullptr;
}

/// @brief FBX / Model の頂点を MeshSurface Shape 用の軽量な点群へ変換する。
/// @note AssetManager の Model 所有権を侵さず、スポーンごとのモデル走査も避けるための複製。
void EnsureMeshShapePoints(ParticleEmitter& emitter)
{
    if (emitter.runtime.loadedMeshShapePath == emitter.settings.meshShapePath
        && emitter.runtime.loadedMeshShapeIndex == emitter.settings.meshShapeIndex)
        return;

    emitter.runtime.meshShapeVertices.clear();
    emitter.runtime.meshShapeTriangles.clear();
    emitter.runtime.loadedMeshShapePath = emitter.settings.meshShapePath;
    emitter.runtime.loadedMeshShapeIndex = emitter.settings.meshShapeIndex;
    if (emitter.settings.meshShapePath.empty()) return;

    const asset::Model* model = asset::AssetManager::LoadAndGet<asset::Model>(emitter.settings.meshShapePath);
    if (!model) return;

    auto appendMesh = [&](const renderer::Mesh& mesh) {
        /// @note サブメッシュを 1 本の頂点列へ連結するので、三角形の添字はこの分だけずらす。
        const uint32_t vertexBase = static_cast<uint32_t>(emitter.runtime.meshShapeVertices.size());

        if (!mesh.cpuSkinnedVertices.empty()) {
            emitter.runtime.meshShapeVertices.reserve(
                emitter.runtime.meshShapeVertices.size() + mesh.cpuSkinnedVertices.size());
            for (const renderer::SkinnedVertex& vertex : mesh.cpuSkinnedVertices) {
                MeshShapeVertex cached{};
                cached.position = vertex.position;
                cached.normal = vertex.normal;
                cached.skinned = true;
                for (int influence = 0; influence < 4; ++influence) {
                    cached.boneIndices[influence] = vertex.boneIndices[influence];
                    cached.boneWeights[influence] = vertex.boneWeights[influence];
                }
                emitter.runtime.meshShapeVertices.push_back(cached);
            }
        } else {
            emitter.runtime.meshShapeVertices.reserve(
                emitter.runtime.meshShapeVertices.size() + mesh.cpuVertices.size());
            for (const renderer::Vertex& vertex : mesh.cpuVertices) {
                MeshShapeVertex cached{};
                cached.position = vertex.position;
                cached.normal = vertex.normal;
                emitter.runtime.meshShapeVertices.push_back(cached);
            }
        }

        const uint32_t appended =
            static_cast<uint32_t>(emitter.runtime.meshShapeVertices.size()) - vertexBase;
        if (appended == 0 || mesh.cpuIndices.size() < 3) return;

        emitter.runtime.meshShapeTriangles.reserve(
            emitter.runtime.meshShapeTriangles.size() + mesh.cpuIndices.size() / 3);
        for (size_t i = 0; i + 2 < mesh.cpuIndices.size(); i += 3) {
            MeshShapeTriangle triangle{};
            bool inRange = true;
            for (int corner = 0; corner < 3; ++corner) {
                const uint32_t localIndex = mesh.cpuIndices[i + corner];
                if (localIndex >= appended) { inRange = false; break; }
                triangle.indices[corner] = vertexBase + localIndex;
            }
            /// @note 壊れたインデックス 1 本で形状全体を捨てるより、その面だけ落として続ける。
            if (inRange) emitter.runtime.meshShapeTriangles.push_back(triangle);
        }
    };

    if (emitter.settings.meshShapeIndex >= 0) {
        const size_t meshIndex = static_cast<size_t>(emitter.settings.meshShapeIndex);
        if (meshIndex < model->meshes.size() && model->meshes[meshIndex])
            appendMesh(*model->meshes[meshIndex]);
    } else {
        for (const auto& mesh : model->meshes) {
            if (mesh) appendMesh(*mesh);
        }
    }

    float totalArea = 0.0f;
    for (MeshShapeTriangle& triangle : emitter.runtime.meshShapeTriangles) {
        const math::Vector3& a = emitter.runtime.meshShapeVertices[triangle.indices[0]].position;
        const math::Vector3& b = emitter.runtime.meshShapeVertices[triangle.indices[1]].position;
        const math::Vector3& c = emitter.runtime.meshShapeVertices[triangle.indices[2]].position;
        totalArea += math::Vector3::Cross(b - a, c - a).Length() * 0.5f;
        triangle.cumulativeArea = totalArea;
    }
    /// @note 面積が実質ゼロ (退化三角形しかない) なら面積抽選は成立しない。頂点一様抽選へ落とす。
    if (totalArea <= 1.0e-12f) emitter.runtime.meshShapeTriangles.clear();
}

/// @brief 面積の累積分布を 1 回の乱数で引く。三角形が無いときは呼ばない。
const MeshShapeTriangle& PickMeshShapeTriangle(ParticleEmitter& emitter)
{
    const std::vector<MeshShapeTriangle>& triangles = emitter.runtime.meshShapeTriangles;
    const float target = Random01(emitter) * triangles.back().cumulativeArea;
    const auto found = std::lower_bound(
        triangles.begin(), triangles.end(), target,
        [](const MeshShapeTriangle& triangle, float value) { return triangle.cumulativeArea < value; });
    return found == triangles.end() ? triangles.back() : *found;
}

/// @brief GPU スキニングと同じ 4 ウェイト線形ブレンドを CPU 側の発生点にだけ適用する。
/// @brief 粒子本体は GPU シミュレーションのまま、読み戻しなしで SkinnedAnimation へ追従できる。
/// @brief 法線を w=0 で流すのは逆転置行列を Animator が持たないため。ボーン行列は回転と平行移動が
/// @brief 主なので、平行移動だけ落とせば向きは十分合う。
void SkinMeshShapeVertex(const MeshShapeVertex& vertex, const AnimatorComponent& animator,
                         math::Vector3& outPosition, math::Vector3& outNormal)
{
    math::Vector3 position = math::Vector3::ZERO;
    math::Vector3 normal = math::Vector3::ZERO;
    float totalWeight = 0.0f;
    for (int influence = 0; influence < 4; ++influence) {
        const float weight = vertex.boneWeights[influence];
        const size_t boneIndex = static_cast<size_t>(vertex.boneIndices[influence]);
        if (weight <= 0.0f || boneIndex >= animator.boneMatrices.size()) continue;
        const math::Matrix4& bone = animator.boneMatrices[boneIndex];
        const math::Vector4 skinnedPosition =
            bone * math::Vector4{ vertex.position.x, vertex.position.y, vertex.position.z, 1.0f };
        const math::Vector4 skinnedNormal =
            bone * math::Vector4{ vertex.normal.x, vertex.normal.y, vertex.normal.z, 0.0f };
        position = position
            + math::Vector3{ skinnedPosition.x, skinnedPosition.y, skinnedPosition.z } * weight;
        normal = normal
            + math::Vector3{ skinnedNormal.x, skinnedNormal.y, skinnedNormal.z } * weight;
        totalWeight += weight;
    }
    if (totalWeight <= 0.0001f) {
        outPosition = vertex.position;
        outNormal = vertex.normal;
        return;
    }
    outPosition = position * (1.0f / totalWeight);
    outNormal = normal * (1.0f / totalWeight);
}

bool SampleMeshShapePoint(ParticleEmitter& emitter, const AnimatorComponent* animator,
                          math::Vector3& outPoint, math::Vector3& outNormal)
{
    EnsureMeshShapePoints(emitter);
    if (emitter.runtime.meshShapeVertices.empty()) return false;

    const bool skin = emitter.settings.meshShapeFollowSkinnedAnimation
        && animator != nullptr && !animator->boneMatrices.empty();
    const auto resolveVertex = [&](const MeshShapeVertex& vertex,
                                   math::Vector3& position, math::Vector3& normal) {
        if (skin && vertex.skinned) {
            SkinMeshShapeVertex(vertex, *animator, position, normal);
            return;
        }
        position = vertex.position;
        normal = vertex.normal;
    };

    if (!emitter.runtime.meshShapeTriangles.empty()) {
        const MeshShapeTriangle& triangle = PickMeshShapeTriangle(emitter);
        /// @note 三角形内部の一様分布。(u, v) が外側へ出たら折り返す — sqrt を使う定番より
        /// @note 分岐 1 つ分安く、乱数の消費が 2 回で固定される。
        float u = Random01(emitter);
        float v = Random01(emitter);
        if (u + v > 1.0f) {
            u = 1.0f - u;
            v = 1.0f - v;
        }
        const float w = 1.0f - u - v;

        math::Vector3 position0, position1, position2;
        math::Vector3 normal0, normal1, normal2;
        resolveVertex(emitter.runtime.meshShapeVertices[triangle.indices[0]], position0, normal0);
        resolveVertex(emitter.runtime.meshShapeVertices[triangle.indices[1]], position1, normal1);
        resolveVertex(emitter.runtime.meshShapeVertices[triangle.indices[2]], position2, normal2);
        outPoint = position0 * w + position1 * u + position2 * v;
        outNormal = (normal0 * w + normal1 * u + normal2 * v).NormalizedOr(math::Vector3::UP);
    } else {
        /// @note インデックスを持たないメッシュは面を組めない。従来どおり頂点を一様に引く。
        const size_t lastIndex = emitter.runtime.meshShapeVertices.size() - 1;
        const size_t index = (std::min)(
            static_cast<size_t>(Random01(emitter) * static_cast<float>(emitter.runtime.meshShapeVertices.size())),
            lastIndex);
        resolveVertex(emitter.runtime.meshShapeVertices[index], outPoint, outNormal);
        outNormal = outNormal.NormalizedOr(math::Vector3::UP);
    }

    /// @note meshShapeScale は位置にだけ掛ける。法線は向きしか表さないので、
    /// @note ここで掛けると負のスケール指定で «内向き» に反転してしまう。
    outPoint = outPoint * emitter.settings.meshShapeScale;
    return true;
}

/// @brief 同じ .mat の設定ミスを毎フレーム記録するとログが埋まって他の警告が読めなくなる。
std::unordered_set<std::string> g_warnedParticleMaterials;

bool WarnParticleMaterialOnce(const std::string& path)
{
    return g_warnedParticleMaterials.insert(path).second;
}

/// @brief カスタムシェーダーが宣言した MaterialConstants (b2) 1 本ぶんの解決結果。
/// @brief 解決にはシェーダーのロードとリフレクションが要るので .mat 単位でキャッシュする。
/// @brief 値で持つのは下のキャッシュの要素としてしか存在しないから (unordered_map はノード単位で
/// @brief 確保するので rehash しても要素のアドレスは動かない)。
struct ParticleMaterialBinding {
    renderer::ShaderDescriptor                            descriptor;
    renderer::ResourceHandle<renderer::ConstantBufferTag> paramsCB;
    std::vector<uint8_t>                                  paramData;
    /// @brief paramsCB を確保したときのサイズ。シェーダーのホットリロードで MaterialConstants の
    /// @brief 大きさが変わったら作り直す必要がある (古い容量のまま書くと末尾が落ちる)。
    uint32_t                                              paramsCBSize = 0;
    /// @brief このパス呼び出しで既に値を適用したか。.mat の編集を絵へ出しつつ、
    /// @brief 同じ .mat を共有するエミッターぶん解決をやり直さないための通番。
    uint64_t                                              resolvedPass = 0;
    bool                                                  resolvedOk   = false;
};

std::unordered_map<std::string, ParticleMaterialBinding> g_particleMaterials;
/// @brief ExecuteParticlePass の呼び出し通番。0 は「未解決」を表すため 1 から始める。
uint64_t                                                 g_particlePassSerial = 0;

/// @brief .mat の [params] をカスタムシェーダーの MaterialConstants へ束縛し、b2 へ流す定数バッファを返す。
/// @brief 組み込みシェーダー (MaterialConstants を宣言しない) では無効ハンドルを返す。
/// @brief 束縛規則そのものは asset::MaterialParamBinding が持つ — メッシュ / UI / デカールと同じ経路。
renderer::ResourceHandle<renderer::ConstantBufferTag> ResolveParticleMaterialParams(
    renderer::ResourceManager& resources,
    const std::string& materialPath,
    const asset::MaterialAsset& material,
    renderer::ResourceHandle<renderer::ShaderTag> shader)
{
    if (!shader.IsValid()) return {};

    ParticleMaterialBinding& binding = g_particleMaterials[materialPath];
    if (binding.resolvedPass == g_particlePassSerial)
        return binding.resolvedOk ? binding.paramsCB : renderer::ResourceHandle<renderer::ConstantBufferTag>{};
    binding.resolvedPass = g_particlePassSerial;
    binding.resolvedOk   = false;

    /// @note 記述子は値で持つ。シェーダーはホットリロードで差し替わりうるので、
    /// @note ポインタで持つと解決時の中身と食い違う瞬間ができる。
    binding.descriptor = {};
    if (auto* compiled = resources.Get(shader))
        binding.descriptor = compiled->GetDescriptor();
    /// @note MaterialConstants を宣言していないシェーダーは cbufferSize が 0 (= IsValid() が false)。
    /// @note その場合 b2 は何も束縛しない — 組み込み Particle.hlsl がこれに当たる。
    if (!binding.descriptor.IsValid()) return {};

    binding.paramData.assign(binding.descriptor.cbufferSize, uint8_t{ 0 });
    asset::InitDefaultMaterialParams(binding.descriptor, binding.paramData);
    asset::ApplyMaterialAssetParams(material, binding.descriptor, binding.paramData);

    /// @note IsValid() だけでは判定できない: このキャッシュはシーンの寿命もデバイスリセットも跨ぎ、
    /// @note ハンドルの体裁は残るため、実体が居るかどうかで作り直しを判断する。
    const bool cbLive = resources.Get(binding.paramsCB) != nullptr;
    if (!cbLive) {
        binding.paramsCB = {};
    } else if (binding.paramsCBSize != binding.descriptor.cbufferSize) {
        resources.Release(binding.paramsCB);
        binding.paramsCB = {};
    }
    if (!binding.paramsCB.IsValid()) {
        binding.paramsCB = resources.CreateConstantBuffer(binding.descriptor.cbufferSize);
        binding.paramsCBSize = binding.descriptor.cbufferSize;
    }
    if (!binding.paramsCB.IsValid()) return {};
    resources.Update(binding.paramsCB, binding.paramData.data(),
                     static_cast<uint32_t>(binding.paramData.size()));

    binding.resolvedOk = true;
    return binding.paramsCB;
}

/// @brief .mat の blend_mode をパーティクルの合成モードへ。パーティクルは半透明前提で PSO が
/// @brief 3 種類しか無いので、Opaque が来たら加算へ倒す («板が並ぶ» より気付きやすい)。
ParticleBlendMode ParticleBlendFromMaterial(renderer::BlendMode blend)
{
    switch (blend) {
    case renderer::BlendMode::ALPHA_BLEND:   return ParticleBlendMode::Alpha;
    case renderer::BlendMode::PREMULTIPLIED: return ParticleBlendMode::Premultiplied;
    default:                                 return ParticleBlendMode::Additive;
    }
}

renderer::ResourceHandle<renderer::TextureTag> LoadParticleTextureOrWhite(
    renderer::ResourceManager& resources,
    const std::string& texturePath)
{
    /// @note Particle は色カーブだけでも成立する VFX なので、参照先テクスチャの欠落で
    /// @note DrawCall 全体を無効化せず、白テクスチャにフォールバックして色だけは表示する。
    if (!texturePath.empty()) {
        auto texture = asset::StreamedTextureResolver::Engine().ResolveGpu(resources, texturePath);
        if (texture.IsValid())
            return texture;
    }

    /// @note 白テクスチャは共有の 1 枚を返す。エミッターごとに作ると Despawn / Stop で
    /// @note 返却されないまま漏れる (LoadTexture 由来のハンドルを返すと共有キャッシュを壊すため)。
    return resources.GetWhiteTexture();
}


/// @brief .mat の [textures] から 1 スロット引く。未設定なら空文字列。
const std::string& ParticleMaterialTexture(const asset::MaterialAsset& material, const char* slot)
{
    static const std::string kEmpty;
    const auto it = material.textures.find(slot);
    return (it != material.textures.end()) ? it->second : kEmpty;
}

/// @brief 陰影の受け口。b3 (平行光・環境光) と 6-way の Negative (t3) は常に、点光源は .mat が求めたときだけ。
/// @brief 点光源は DeferredPasses の Lighting と同じ束縛: b9 + t29/t30 (Legacy では束縛しない。b9 が未束縛だと
/// @brief clusterLightMode = Legacy として読まれ、b3 の固定長配列へ落ちる) と、影・Cookie の b12 + t28/t31。
void EnsureParticleTexture(ParticleEmitter& emitter, renderer::ResourceManager& resources)
{
    /// @note 見た目のテクスチャは 3 枚とも .mat の [textures] から来る。
    /// @note albedo = 素材 / normal = 歪みベクトル専用マップ / tex5 = Motion Vector アトラス
    /// @note 未設定なら既定 .mat へ落とす。1x1 白は alpha=1 なので、そのまま描くと粒子が
    /// @note 「不透明な四角」になり、素材の付け忘れが最も分かりにくい形で表に出る。
    /// @note 既定 .mat が無いプロジェクトでは `Load<MaterialAsset>` が失敗し、従来どおり白へ落ちる。
    const bool usingFallback = emitter.settings.materialPath.empty();
    const std::string resolvedMaterial =
        usingFallback ? std::string(PARTICLE_FALLBACK_MATERIAL) : emitter.settings.materialPath;
    {
        const bool matChanged = (emitter.runtime.loadedMaterialPath != resolvedMaterial);
        if (matChanged) {
            emitter.runtime.loadedMaterialPath = resolvedMaterial;
            /// @note .mat の変更でテクスチャも再ロードさせる
            emitter.runtime.loadedTexturePath.clear();
        }
        const auto matHandle = asset::AssetManager::Load<asset::MaterialAsset>(resolvedMaterial);
        if (const auto* mat = asset::AssetManager::Get<asset::MaterialAsset>(matHandle)) {
            const std::string& resolvedTex = ParticleMaterialTexture(*mat, "albedo");
            {
                emitter.runtime.texture = LoadParticleTextureOrWhite(resources, resolvedTex);
                emitter.runtime.loadedTexturePath = resolvedTex;
                emitter.runtime.textureIsSrgb = IsEffectTextureSrgb(resolvedTex);
            }
            /// @note 歪みベクトル専用マップ (normal)。未設定なら無効ハンドルのままにして、
            /// @note シェーダー側は effectsFlags を見て albedo の RG へ縮退する。
            const std::string& distortionTex = ParticleMaterialTexture(*mat, "normal");
            {
                emitter.runtime.distortionTexture = distortionTex.empty()
                    ? renderer::ResourceHandle<renderer::TextureTag>::Null()
                    : LoadParticleTextureOrWhite(resources, distortionTex);
                emitter.runtime.loadedDistortionTexturePath = distortionTex;
            }
            /// @note Motion Vector アトラス (tex5)。
            const std::string& motionTex = ParticleMaterialTexture(*mat, "tex5");
            {
                emitter.runtime.motionVectorTexture = motionTex.empty()
                    ? renderer::ResourceHandle<renderer::TextureTag>::Null()
                    : LoadParticleTextureOrWhite(resources, motionTex);
                emitter.runtime.loadedMotionVectorTexturePath = motionTex;
            }
            /// @note 6 方向ライトマップの Negative (emissive)。sixWayMaps のときだけ読む。
            static const std::string kNoTexture;
            const std::string& sixWayTex = mat->particle.sixWayMaps ? ParticleMaterialTexture(*mat, "emissive")
                                                                    : kNoTexture;
            {
                emitter.runtime.sixWayNegativeTexture = sixWayTex.empty()
                    ? renderer::ResourceHandle<renderer::TextureTag>::Null()
                    : LoadParticleTextureOrWhite(resources, sixWayTex);
                emitter.runtime.loadedSixWayNegativeTexturePath = sixWayTex;
            }
            const std::string& sixWayColorTex = mat->particle.sixWayMaps
                ? ParticleMaterialTexture(*mat, "six_way_color") : kNoTexture;
            if (emitter.runtime.loadedSixWayAlbedoColorTexturePath != sixWayColorTex) {
                emitter.runtime.sixWayAlbedoColorTexture = sixWayColorTex.empty()
                    ? renderer::ResourceHandle<renderer::TextureTag>::Null()
                    : resources.LoadTexture(sixWayColorTex);
                emitter.runtime.loadedSixWayAlbedoColorTexturePath = sixWayColorTex;
            }
            const std::string& sixWayEmissionTex = mat->particle.sixWayMaps
                ? ParticleMaterialTexture(*mat, "six_way_emission") : kNoTexture;
            if (emitter.runtime.loadedSixWayEmissionColorTexturePath != sixWayEmissionTex) {
                emitter.runtime.sixWayEmissionColorTexture = sixWayEmissionTex.empty()
                    ? renderer::ResourceHandle<renderer::TextureTag>::Null()
                    : resources.LoadTexture(sixWayEmissionTex);
                emitter.runtime.loadedSixWayEmissionColorTexturePath = sixWayEmissionTex;
            }
            /// @note .mat の [params] albedo を色調整として引き継ぐ。
            /// @note オーサリング値は sRGB なので、他の色と同じくリニアへ揃えて渡す。
            math::Vector4 tint{ 1.0f, 1.0f, 1.0f, 1.0f };
            if (const auto param = mat->params.find("albedo"); param != mat->params.end()) {
                const auto& values = param->second;
                /// @note float / float2 / float3 / float4 が同じ形式で入る。足りない成分は既定のまま。
                if (values.size() > 0) tint.x = values[0];
                if (values.size() > 1) tint.y = values[1];
                if (values.size() > 2) tint.z = values[2];
                if (values.size() > 3) tint.w = values[3];
            }
            emitter.runtime.materialTint = ParticleSrgbToLinear(tint);

            /// @note .mat が shader を指定していれば描画シェーダーを差し替える。
            /// @note 既定 .mat では見ない — 未設定にした瞬間に描画が化けるのは避ける (blendMode と同じ)。
            /// @note render_path を要求するのは、頂点が ParticleVertex (92B)・b2 が
            /// @note ParticleRenderConstants で、メッシュ用シェーダーを割り当てると
            /// @note «クラッシュせず静かに壊れた絵» になるため。UI / Decal と同じ判断。
            const bool declaredForParticles = (mat->renderPath == asset::RenderPath::Particle);
            if (!usingFallback && !declaredForParticles && !mat->shaderPath.empty()
                && WarnParticleMaterialOnce(resolvedMaterial)) {
                FBZZ_LOG_WARN("Particle material '%s' declares shader '%s' but is not declared for "
                              "particles (render_path must be \"particle\") -> ignoring the shader "
                              "and drawing with the built-in particle shader.",
                              resolvedMaterial.c_str(), mat->shaderPath.c_str());
            }
            const std::string shaderPath =
                (usingFallback || !declaredForParticles) ? std::string{} : mat->shaderPath;
            if (emitter.runtime.loadedShaderPath != shaderPath) {
                emitter.runtime.loadedShaderPath = shaderPath;
                emitter.runtime.customShader = shaderPath.empty()
                    ? renderer::ResourceHandle<renderer::ShaderTag>{}
                    : resources.LoadShader(shaderPath);
                /// @note 黙って組み込みへ落ちると、テクスチャを持たない .mat では «白い四角» が並ぶ。
                /// @note 手続きシェーダーほどテクスチャを持たないので、一番起きやすい失敗が
                /// @note 一番分かりにくい絵になる。
                if (!shaderPath.empty() && !emitter.runtime.customShader.IsValid()) {
                    FBZZ_LOG_WARN("Particle material '%s' references shader '%s' but it failed to "
                                  "load (not compiled?). Falling back to the built-in particle "
                                  "shader; a material without an albedo texture will draw as "
                                  "white quads.",
                                  resolvedMaterial.c_str(), shaderPath.c_str());
                }
            }
            /// @note カスタムシェーダーが MaterialConstants を宣言していれば .mat の [params] を流す。
            /// @note 毎フレーム引き直すのは編集結果を即座に絵へ出すため (解決は .mat 単位で 1 回)。
            emitter.runtime.materialParamsCB = ResolveParticleMaterialParams(
                resources, resolvedMaterial, *mat, emitter.runtime.customShader);
            /// @note 見た目一式を .mat から «読む»。settings はシーン保存対象なので書き戻さない
            /// @note («触っていないのに保存内容が変わる» / «Inspector で変えても戻る» が起きる)。
            emitter.runtime.material      = mat->particle;
            emitter.runtime.resolvedBlend = ParticleBlendFromMaterial(mat->blendMode);
            /// @note アトラス分割は 0 だと UV 矩形が発散する。.mat は手書きできるのでここで丸める。
            emitter.runtime.material.flipbook.spriteColumns =
                (std::max)(emitter.runtime.material.flipbook.spriteColumns, 1);
            emitter.runtime.material.flipbook.spriteRows =
                (std::max)(emitter.runtime.material.flipbook.spriteRows, 1);
            return;
        }
    }

    /// @note .mat を解決できなかった。1x1 白 + 既定の見た目で描き続ける
    /// @note (エフェクトが丸ごと消えるより、素材が付いていないと分かる方がよい)。
    emitter.runtime.material      = asset::ParticleMaterialSettings{};
    emitter.runtime.resolvedBlend = ParticleBlendMode::Additive;
    emitter.runtime.distortionTexture = {};
    emitter.runtime.motionVectorTexture = {};
    emitter.runtime.sixWayNegativeTexture = {};
    {
        emitter.runtime.texture = LoadParticleTextureOrWhite(resources, {});
        emitter.runtime.loadedTexturePath.clear();
        /// @note 1x1 白フォールバック。リニアでも sRGB でも 1.0 は 1.0 なので変換しない。
        emitter.runtime.textureIsSrgb = false;
    }
}

/// @brief GPU シミュレーションへ渡す力場とスポーンの StructuredBuffer。
/// @note エミッターに 1 本ずつ持たせず、シミュレーションのたびにプールから借りる。DX12 の
/// @note 読み取り専用 StructuredBuffer は Upload ヒープへの直 memcpy のため、1 本を毎フレーム
/// @note 書き直すと GPU がまだ読んでいる前フレームの内容を上書きする (詳細は `DynamicBufferPool.hpp`)。
renderer::DynamicStructuredBufferPool g_gpuForcePool;
renderer::DynamicStructuredBufferPool g_gpuSpawnPool;

/// @brief 力場バッファを借りて今フレームの内容を書き込む。
void AcquireGpuForceBuffer(ParticleEmitter& emitter,
                           const std::vector<GpuFlowField>& forces,
                           renderer::ResourceManager& resources)
{
    /// @note 0 本でも 1 要素は借りる。要素数 0 の StructuredBuffer は作れず、
    /// @note 未束縛の SRV は DX12 で null ディスクリプタの次元不一致になる。
    const size_t required = (std::max)(forces.size(), size_t{ 1 });
    emitter.runtime.gpuForceBuffer =
        g_gpuForcePool.Acquire(resources, required, static_cast<uint32_t>(sizeof(GpuFlowField)));
    if (!forces.empty() && emitter.runtime.gpuForceBuffer.IsValid()) {
        resources.Update(emitter.runtime.gpuForceBuffer, forces.data(),
                         forces.size() * sizeof(GpuFlowField));
    }
}

bool ShouldSortGpuParticles(const ParticleEmitter& emitter, const RenderPassHandles& handles)
{
    return emitter.settings.sortMode != ParticleSortMode::None
        && handles.particleGpuSortKeysCS.IsValid()
        && handles.particleGpuSortStepCS.IsValid()
        && handles.particleGpuSortLocalCS.IsValid();
}

ParticleRenderCB BuildParticleRenderConstants(ParticleEmitter& emitter,
                                   renderer::ResourceManager& resources,
                                   int maxParticles,
                                   std::uint32_t screenWidth,
                                   std::uint32_t screenHeight,
                                   bool gpuSortEnabled = false)
{
    if (!emitter.runtime.renderCB.IsValid())
        emitter.runtime.renderCB = resources.CreateConstantBuffer(sizeof(ParticleRenderCB));
    ParticleRenderCB cb{};
    cb.renderMode = static_cast<uint32_t>(emitter.settings.renderMode);
    cb.stretchedVelocityScale = (std::max)(emitter.settings.stretchedVelocityScale, 0.0f);
    cb.stretchedLengthScale = (std::max)(emitter.settings.stretchedLengthScale, 0.0f);
    cb.softParticleFadeDistance = (std::max)(emitter.runtime.material.softParticleFadeDistance, 0.001f);
    cb.softParticles = emitter.runtime.material.softParticles ? 1u : 0u;
    cb.maxParticles = static_cast<uint32_t>((std::max)(maxParticles, 0));
    /// @note ビット割り当ては Rendering/ParticleCommon.hlsli の FBZZ_PFX_* / FBZZ_PALPHA_* と一致させること。
    cb.effectsFlags = (emitter.runtime.material.distortion ? kParticleFxDistortion : 0u)
        | (emitter.runtime.material.sixWayLighting ? kParticleFxSixWay : 0u)
        | (emitter.runtime.material.flipbook.motionVectorFlipbook && emitter.runtime.motionVectorTexture.IsValid()
               ? kParticleFxMotionVector : 0u)
        | (emitter.runtime.material.receiveShadows ? kParticleFxReceiveShadow : 0u)
        | (emitter.runtime.material.volumetric ? kParticleFxVolumetric : 0u)
        | (emitter.runtime.resolvedBlend == ParticleBlendMode::Premultiplied
               ? kParticleFxPremultiplied : 0u)
        | (emitter.runtime.textureIsSrgb ? kParticleFxSrgbTexture : 0u)
        | (emitter.runtime.distortionTexture.IsValid() ? kParticleFxDistortionMap : 0u)
        | (emitter.runtime.material.punctualLighting ? kParticleFxPunctual : 0u)
        | (emitter.runtime.resolvedBlend == ParticleBlendMode::Additive ? kParticleFxAdditive : 0u)
        | (emitter.runtime.material.sixWayMaps && emitter.runtime.sixWayNegativeTexture.IsValid()
               ? kParticleFxSixWayMaps : 0u)
        | (emitter.runtime.material.sixWayMaps && emitter.runtime.sixWayAlbedoColorTexture.IsValid()
               && emitter.runtime.sixWayEmissionColorTexture.IsValid() ? kParticleFxSixWayColorMaps : 0u)
        | ((static_cast<std::uint32_t>(emitter.runtime.material.alphaSource) & kParticleAlphaMask)
               << kParticleAlphaShift);
    cb.distortionStrength = (std::max)(emitter.runtime.material.distortionStrength, 0.0f);
    cb.distortionChromatic = (std::max)(emitter.runtime.material.distortionChromatic, 0.0f);
    cb.lightingStrength = (std::max)(emitter.runtime.material.lightingStrength, 0.0f);
    cb.smokeWrap = std::clamp(emitter.runtime.material.smokeWrap, 0.0f, 1.0f);
    cb.smokeTransmission = (std::max)(emitter.runtime.material.smokeTransmission, 0.0f);
    /// @note 0 以下だと pow が発散する。シェーダー側でも下限を切るが、値の意味をここで固定する。
    cb.smokeBackScatterPower = (std::max)(emitter.runtime.material.smokeBackScatterPower, 0.1f);
    cb.tintColor = emitter.runtime.materialTint;
    cb.emissiveScale = (std::max)(emitter.runtime.material.emissiveScale, 0.0f);
    const math::Vector3& sixWayEmission = emitter.runtime.material.sixWayEmissionColor;
    cb.sixWayEmission = { (std::max)(sixWayEmission.x, 0.0f), (std::max)(sixWayEmission.y, 0.0f),
                          (std::max)(sixWayEmission.z, 0.0f),
                          (std::max)(emitter.runtime.material.sixWayEmissionScale, 0.0f) };
    cb.motionVectorStrength = (std::max)(emitter.runtime.material.flipbook.motionVectorStrength, 0.0f);
    /// @note 歪みの画面UV算出に使う。0 だとUVが右下隅へ張り付いて屈折が出ない。
    cb.screenWidth = static_cast<float>((std::max)(screenWidth, 1u));
    cb.screenHeight = static_cast<float>((std::max)(screenHeight, 1u));
    /// @note 0 以下だと粒子が潰れて一切見えなくなるため、下限で切って「消える」事故を防ぐ。
    cb.sizeAxisScaleX = (std::max)(emitter.settings.sizeAxisScale.x, 0.0001f);
    cb.sizeAxisScaleY = (std::max)(emitter.settings.sizeAxisScale.y, 0.0001f);
    cb.shadowStrength = std::clamp(emitter.runtime.material.shadowStrength, 0.0f, 1.0f);
    /// @note ステップ数はピクセルあたりのループ回数に直結する。上限を切って
    /// @note 設定ミスで GPU が張り付くのを防ぐ。
    cb.volumetricSteps = static_cast<std::uint32_t>(std::clamp(emitter.runtime.material.volumetricSteps, 1, 64));
    cb.volumetricDensity = (std::max)(emitter.runtime.material.volumetricDensity, 0.0f);
    /// @note g = ±1 は位相関数が発散するため内側へ寄せる。
    cb.volumetricAnisotropy = std::clamp(emitter.runtime.material.volumetricAnisotropy, -0.95f, 0.95f);
    cb.volumetricNoiseScale = (std::max)(emitter.runtime.material.volumetricNoiseScale, 0.0f);
    /// @note GPU 経路の VS がソート済み index (t15) を経由するかどうか。CPU 経路では常に 0。
    cb.gpuSortEnabled = gpuSortEnabled ? 1u : 0u;
    /// @note 自己影。密度バッファ (t9) が無いフレームでも 0 なら参照しないので安全。
    cb.selfShadowStrength = (std::max)(emitter.runtime.material.selfShadowStrength, 0.0f);
    /// @note カメラ距離フェード。負値だけ潰す。near >= far (既定の 0 / 0 を含む) は
    /// @note シェーダー側が «無効» として扱うので、ここで組み替えたり警告したりはしない。
    cb.cameraFadeNear = (std::max)(emitter.runtime.material.cameraFadeNear, 0.0f);
    cb.cameraFadeFar = (std::max)(emitter.runtime.material.cameraFadeFar, 0.0f);
    return cb;
}
/// @brief scopeRoot 以下を名前で深さ優先探索する。VFX Graph の生成物は同名が複数存在しうるため、
/// @brief 「自分と同じエフェクトに属する Emitter」だけを候補にするための限定探索。
GameObject* FindInSubtree(GameObject& root, const std::string& objectName)
{
    if (root.name == objectName) return &root;
    for (int index = 0; index < root.GetChildCount(); ++index)
        if (GameObject* child = root.GetChild(index))
            if (GameObject* found = FindInSubtree(*child, objectName)) return found;
    return nullptr;
}

/// @brief SubEmitter へイベント数分のスポーンを積む。参照切れは VFX の縮退として無視する。
/// @brief subEmitterScopeRoot が有効ならその GameObject 配下だけを名前で探す (VFXSystem が配る)。
/// @brief .vfx の中の GO は同じ名前を名乗るので、シーン全体で引くと隣のインスタンスを掴む。
/// @brief origin / velocity は «発火元の粒子» のワールド位置と速度。これを渡さないと受け側は
/// @brief 自分の emitPosition からしか湧けず、«斬った位置で火花» / «消えた場所から煙» が作れない。
void QueueSubEmitter(Scene& scene, const ParticleEmitter& emitter,
                     const std::string& objectName, int count,
                     const math::Vector3& origin, const math::Vector3& velocity)
{
    if (objectName.empty() || count <= 0) return;
    GameObject* target = nullptr;
    if (GameObject* scopeRoot = scene.GetGameObject(emitter.settings.subEmitterScopeRoot))
        target = FindInSubtree(*scopeRoot, objectName);
    else
        /// @note シーン上の手置き Emitter は従来どおり全体から引く
        target = scene.Find(objectName);
    if (target == nullptr) return;
    auto* targetEmitter = target->GetComponent<ParticleEmitter>();
    if (targetEmitter == nullptr) return;
    /// @note 受け側が一度も回らない構成 (GPU 縮退・非アクティブ・maxParticles = 0) では
    /// @note 消費されないまま積まれ続ける。1 フレームで出せる上限を超えたら黙って捨てる
    /// @note (出せない数を覚えておいても絵には出ず、リストだけが伸びる)。
    if (static_cast<int>(targetEmitter->runtime.injectedSpawns.size())
        >= (std::max)(targetEmitter->settings.maxParticles, 1))
        return;
    targetEmitter->runtime.injectedSpawns.push_back({ origin, velocity, count });
}

/// @brief Particle 1個のPhysics/Plane衝突を解決し、Kill応答ならtrueを返す。
/// @brief filter は Physics 衝突のレイヤー/トリガー絞り込み。粒子ごとに組むと std::function の
/// @brief 生成がループ内に入るので、呼び出し側 (エミッター単位) が 1 本作って回す。
bool ResolveParticleCollision(ParticleEmitter& emitter, Particle& particle,
                              const math::Vector3& nextPosition,
                              const physics::World* world, Scene& scene,
                              const physics::World::ColliderFilter& filter)
{
    physics::World::RaycastHit hit{};
    bool collided = false;
    math::Vector3 hitPoint = nextPosition;
    math::Vector3 hitNormal = math::Vector3::UP;

    if (emitter.settings.collisionMode == ParticleCollisionMode::Plane) {
        if (nextPosition.y - emitter.settings.collisionRadius <= emitter.settings.collisionPlaneY) {
            collided = true;
            hitPoint = { nextPosition.x, emitter.settings.collisionPlaneY + emitter.settings.collisionRadius, nextPosition.z };
        }
    } else if (emitter.settings.collisionMode == ParticleCollisionMode::Physics && world) {
        const math::Vector3 travel = nextPosition - particle.position;
        const float distance = travel.Length();
        if (distance > 1.0e-5f) {
            const math::Vector3 direction = travel * (1.0f / distance);
            collided = world->SphereCast(particle.position,
                                         (std::max)(emitter.settings.collisionRadius, 0.0f),
                                         direction, distance, hit, filter);
            if (collided) {
                hitPoint = hit.point + hit.normal * emitter.settings.collisionRadius;
                hitNormal = hit.normal;
            }
        }
    }

    if (!collided) {
        particle.position = nextPosition;
        return false;
    }

    ++emitter.runtime.collisionCountThisFrame;
    /// @note 発火元は «当たった点» と «跳ね返る前の速度»。跳ね返り後を渡すと、火花が壁から
    /// @note 離れる向きに寄って «削れて飛んだ» ように見えない。
    QueueSubEmitter(scene, emitter, emitter.settings.collisionSubEmitter,
                    emitter.settings.subEmitterBurstCount, hitPoint, particle.velocity);
    if (emitter.settings.collisionResponse == ParticleCollisionResponse::Kill)
        return true;

    particle.position = hitPoint;
    if (emitter.settings.collisionResponse == ParticleCollisionResponse::Stop) {
        particle.velocity = math::Vector3::ZERO;
        return false;
    }

    const float normalVelocity = math::Vector3::Dot(particle.velocity, hitNormal);
    particle.velocity = (particle.velocity - hitNormal * (2.0f * normalVelocity))
        * Clamp01(emitter.settings.collisionBounciness);
    particle.velocity = particle.velocity
        * (std::max)(0.0f, 1.0f - emitter.settings.collisionDamping);
    return false;
}

/// @brief injected: SubEmitter が渡してきた «発火元» (無ければ nullptr)。位置は発生原点として
/// @brief 据え、速度は subEmitterInheritVelocity の割合だけ初速へ足す。
void SpawnParticle(ParticleEmitter& emitter, const Transform& transform,
                   const AnimatorComponent* animator, float initialAge = 0.0f,
                   const ParticleInjectedSpawn* injected = nullptr)
{
    Particle p;
    const bool localSpace = emitter.settings.simulationSpace == ParticleSimulationSpace::Local;
    const auto transformPoint = [&](const math::Vector3& value) {
        return localSpace ? value : TransformEmitterPoint(transform, value);
    };
    const auto transformVector = [&](const math::Vector3& value) {
        return localSpace ? value : TransformEmitterVector(transform, value);
    };
    p.position = transformPoint(emitter.settings.emitPosition);
    /// @note 発火元の位置を発生原点へ据える。«置き換え» を Shape より前に済ませるのは、
    /// @note Shape のばらつきをこの点の周りに乗せるため (後で上書きすると 1 点から出る糸になる)。
    if (injected != nullptr) {
        p.position = localSpace
            ? InverseTransformEmitterPoint(transform, injected->position)
            : injected->position;
    }
    math::Vector3 shapeVelocity = math::Vector3::ZERO;

    switch (emitter.settings.shape) {
    case ParticleEmitterShape::Sphere: {
        const math::Vector3 dir = RandomUnitVector(emitter);
        const float radius = (std::max)(emitter.settings.sphereRadius, 0.0f) * std::cbrt(Random01(emitter));
        p.position = p.position + transformVector(dir * radius);
        shapeVelocity = transformVector(dir * emitter.settings.velocitySpread);
        break;
    }
    case ParticleEmitterShape::Cone: {
        constexpr float DEG_TO_RAD = 3.14159265358979323846f / 180.0f;
        const float angle = (std::max)(emitter.settings.coneAngleDegrees, 0.0f) * DEG_TO_RAD;
        const float theta = Random01(emitter) * angle;
        const float phi = Random01(emitter) * 3.14159265358979323846f * 2.0f;
        const float radius = (std::max)(emitter.settings.coneRadius, 0.0f) * std::sqrt(Random01(emitter));
        const math::Vector3 localOffset = {
            std::cos(phi) * radius,
            0.0f,
            std::sin(phi) * radius
        };
        p.position = p.position + transformVector(localOffset);
        const math::Vector3 localShapeVelocity = {
            std::sin(theta) * std::cos(phi) * emitter.settings.velocitySpread,
            std::cos(theta) * emitter.settings.velocitySpread,
            std::sin(theta) * std::sin(phi) * emitter.settings.velocitySpread
        };
        shapeVelocity = transformVector(localShapeVelocity);
        break;
    }
    case ParticleEmitterShape::Box: {
        const math::Vector3 localOffset = {
            RandomSigned01(emitter) * emitter.settings.boxExtents.x,
            RandomSigned01(emitter) * emitter.settings.boxExtents.y,
            RandomSigned01(emitter) * emitter.settings.boxExtents.z
        };
        p.position = p.position + transformVector(localOffset);
        break;
    }
    case ParticleEmitterShape::MeshSurface: {
        math::Vector3 meshPoint;
        math::Vector3 meshNormal;
        if (SampleMeshShapePoint(emitter, animator, meshPoint, meshNormal)) {
            p.position = transformPoint(emitter.settings.emitPosition + meshPoint);
            shapeVelocity = transformVector(meshNormal * emitter.settings.meshShapeNormalVelocity);
        }
        break;
    }
    case ParticleEmitterShape::Point:
    default:
        break;
    }

    /// @note 初速のばらつきは 3 軸へ等方に掛ける。1 軸でも欠けると、その向きへ出すエミッターで
    /// @note 分散がゼロになり全粒子が同じ速度で進む硬い前線になる。
    /// @note 方向そのものは Sphere/Cone の shapeVelocity が持つので、ここは揺らぎだけを担当する。
    /// @note GPU 経路 (BuildGpuSpawnEntry) と必ず同じ式・同じ乱数消費順にすること。
    const float rx = RandomSigned01(emitter) * emitter.settings.velocitySpread;
    const float ry = RandomSigned01(emitter) * emitter.settings.velocitySpread;
    const float rz = RandomSigned01(emitter) * emitter.settings.velocitySpread;
    const math::Vector3 localVelocity = {
        emitter.settings.emitVelocity.x + rx,
        emitter.settings.emitVelocity.y + ry,
        emitter.settings.emitVelocity.z + rz
    };
    p.velocity = transformVector(localVelocity);
    p.velocity = p.velocity + shapeVelocity;
    /// @note エミッターの移動を初速へ引き継ぐ (移動する剣・ロケットの火花が置き去りにならない)。
    /// @note Local では親の移動が描画時の変換で既に乗るので、World のときだけ加算する。
    if (emitter.settings.inheritVelocity != 0.0f
        && emitter.settings.simulationSpace == ParticleSimulationSpace::World) {
        p.velocity = p.velocity + emitter.runtime.emitterVelocity * Clamp01(emitter.settings.inheritVelocity);
    }
    /// @note 発火元の粒子の速度は «割合» で継ぐ。丸ごと継ぐと親と同じ向きへ流れるだけになり、
    /// @note 火花が «弾けた» ように見えない (既定 0 = 継がない)。
    if (injected != nullptr && emitter.settings.subEmitterInheritVelocity != 0.0f) {
        const math::Vector3 inherited =
            injected->velocity * Clamp01(emitter.settings.subEmitterInheritVelocity);
        p.velocity = p.velocity
            + (localSpace ? InverseTransformEmitterVector(transform, inherited) : inherited);
    }
    p.color = emitter.settings.colorStart;
    p.size  = emitter.settings.sizeStart;
    p.age   = 0.0f;
    const float startRotation = Random01(emitter) * 3.14159265358979323846f * 2.0f;
    p.rotation = emitter.settings.randomStartRotation ? startRotation : 0.0f;
    p.angularVelocity = emitter.settings.angularVelocityMin
        + (emitter.settings.angularVelocityMax - emitter.settings.angularVelocityMin) * Random01(emitter);
    p.spriteSeed = Random01(emitter);
    p.lifetime = (std::max)(0.001f, emitter.settings.lifetime * (1.0f + RandomSigned01(emitter) * Clamp01(emitter.settings.lifetimeRandom)));
    p.startSize = emitter.settings.sizeStart;
    p.endSize = emitter.settings.sizeEnd;
    p.startColor = emitter.settings.colorStart;
    p.endColor = emitter.settings.colorEnd;
    p.colorScale = NextColorVariation(emitter);
    p.color = EvaluateParticleColorLinear(emitter, p, 0.0f);
    p.age = (std::max)(0.0f, (std::min)(initialAge, p.lifetime * 0.999f));
    if (p.age > 0.0f) {
        /// @note Prewarmは開始時点の寿命分布を作る。逐次更新を避け、重力下の解析解で初期状態を近似する。
        /// @note 近似に使うのは内蔵の Wind 力 (＝重力) だけ。渦や速度場まで解析解にはできないので、
        /// @note それらが主役のエミッターでは prewarm の分布が実際の流れとずれる。
        const math::Vector3 gravity = emitter.settings.GravityAcceleration();
        p.position = p.position + p.velocity * p.age + gravity * (0.5f * p.age * p.age);
        p.velocity = p.velocity + gravity * p.age;
        p.rotation += p.angularVelocity * p.age;
        const float normalizedAge = Clamp01(p.age / p.lifetime);
        p.color = EvaluateParticleColorLinear(emitter, p, normalizedAge);
        const float sizeT = emitter.settings.useSizeCurve
            ? Clamp01(emitter.settings.sizeCurve.Evaluate(normalizedAge))
            : std::pow(normalizedAge, emitter.settings.sizeCurvePower);
        p.size = p.startSize + (p.endSize - p.startSize) * sizeT;
    }
    const SpriteFrameState sprite = ComputeSpriteFrameState(
        emitter, Clamp01(p.age / p.lifetime), p.age, p.spriteSeed);
    p.uvRect = sprite.currentRect;
    p.nextUvRect = sprite.nextRect;
    p.spriteBlend = sprite.blend;
    emitter.runtime.particles.push_back(std::move(p));
}

void ClearEmitterRuntime(ParticleEmitter& emitter)
{
    emitter.runtime.particles.clear();
    emitter.runtime.emitAccum = 0.0f;
    emitter.runtime.prewarmSpawnPending = 0;
    emitter.runtime.burstPending = 0;
    emitter.runtime.injectedSpawns.clear();
}

/// @brief CPU の SpawnParticle と同じ Shape/Spread ロジックで GpuSpawnEntry を初期化する
void InitGpuSpawnEntry(GpuSpawnEntry& s, ParticleEmitter& emitter, const Transform& tf,
                       const AnimatorComponent* animator)
{
    s.position = TransformEmitterPoint(tf, emitter.settings.emitPosition);
    math::Vector3 shapeVelocity = math::Vector3::ZERO;

    switch (emitter.settings.shape) {
    case ParticleEmitterShape::Sphere: {
        const math::Vector3 dir = RandomUnitVector(emitter);
        const float radius = (std::max)(emitter.settings.sphereRadius, 0.0f) * std::cbrt(Random01(emitter));
        s.position = s.position + TransformEmitterVector(tf, dir * radius);
        shapeVelocity = TransformEmitterVector(tf, dir * emitter.settings.velocitySpread);
        break;
    }
    case ParticleEmitterShape::Cone: {
        constexpr float DEG_TO_RAD = 3.14159265358979323846f / 180.0f;
        const float angle = (std::max)(emitter.settings.coneAngleDegrees, 0.0f) * DEG_TO_RAD;
        const float theta = Random01(emitter) * angle;
        const float phi   = Random01(emitter) * 3.14159265358979323846f * 2.0f;
        const float radius = (std::max)(emitter.settings.coneRadius, 0.0f) * std::sqrt(Random01(emitter));
        const math::Vector3 localOffset = {
            std::cos(phi) * radius,
            0.0f,
            std::sin(phi) * radius
        };
        s.position = s.position + TransformEmitterVector(tf, localOffset);
        const math::Vector3 localShapeVelocity = {
            std::sin(theta) * std::cos(phi) * emitter.settings.velocitySpread,
            std::cos(theta) * emitter.settings.velocitySpread,
            std::sin(theta) * std::sin(phi) * emitter.settings.velocitySpread
        };
        shapeVelocity = TransformEmitterVector(tf, localShapeVelocity);
        break;
    }
    case ParticleEmitterShape::Box: {
        const math::Vector3 localOffset = {
            RandomSigned01(emitter) * emitter.settings.boxExtents.x,
            RandomSigned01(emitter) * emitter.settings.boxExtents.y,
            RandomSigned01(emitter) * emitter.settings.boxExtents.z
        };
        s.position = s.position + TransformEmitterVector(tf, localOffset);
        break;
    }
    case ParticleEmitterShape::MeshSurface: {
        math::Vector3 meshPoint;
        math::Vector3 meshNormal;
        if (SampleMeshShapePoint(emitter, animator, meshPoint, meshNormal)) {
            s.position = TransformEmitterPoint(tf, emitter.settings.emitPosition + meshPoint);
            shapeVelocity = TransformEmitterVector(
                tf, meshNormal * emitter.settings.meshShapeNormalVelocity);
        }
        break;
    }
    default:
        break;
    }

    /// @note SpawnParticle と同一の 3 軸等方ジッター。乱数の消費順まで揃えないと
    /// @note CPU/GPU を切り替えただけで見た目が変わってしまう。
    const float rx = RandomSigned01(emitter) * emitter.settings.velocitySpread;
    const float ry = RandomSigned01(emitter) * emitter.settings.velocitySpread;
    const float rz = RandomSigned01(emitter) * emitter.settings.velocitySpread;
    const math::Vector3 localVelocity = {
        emitter.settings.emitVelocity.x + rx,
        emitter.settings.emitVelocity.y + ry,
        emitter.settings.emitVelocity.z + rz
    };
    s.velocity        = TransformEmitterVector(tf, localVelocity);
    s.velocity        = s.velocity + shapeVelocity;
    /// @note inheritVelocity は CPU 経路 (SpawnParticle) と同じ条件・同じ式でここに足す。
    /// @note CS 側の変更は不要で、GpuSpawnEntry.velocity に加算済みの値が入る。
    if (emitter.settings.inheritVelocity != 0.0f
        && emitter.settings.simulationSpace == ParticleSimulationSpace::World) {
        s.velocity = s.velocity + emitter.runtime.emitterVelocity * Clamp01(emitter.settings.inheritVelocity);
    }
    s.lifetime        = (std::max)(0.001f, emitter.settings.lifetime
        * (1.0f + RandomSigned01(emitter) * Clamp01(emitter.settings.lifetimeRandom)));
    s.size            = emitter.settings.sizeStart;
    s.colorStart      = emitter.settings.colorStart;
    /// @note 乱数列上の位置は従来の ApplyColorVariation 呼び出しと同じに保つ
    /// @note (動かすと同じ seed から出る粒子の並びが変わる)。
    const math::Vector3 variation = NextColorVariation(emitter);
    s.colorScale      = { variation.x, variation.y, variation.z, 0.0f };
    s.spriteSeed      = Random01(emitter);
    s.uvRect          = ComputeSpriteRect(emitter, 0.0f, 0.0f, s.spriteSeed);
    const float startRotation = Random01(emitter) * 6.28318530717958647692f;
    s.rotation        = emitter.settings.randomStartRotation ? startRotation : 0.0f;
    s.angularVelocity = emitter.settings.angularVelocityMin
        + (emitter.settings.angularVelocityMax - emitter.settings.angularVelocityMin) * Random01(emitter);
}

/// @brief GPU パーティクル: バッファ初期化・スポーン・CS Dispatch・DrawInstanced
void PrepareGpuEmitter(ParticleEmitter&                     emitter,
                    const Transform&                     tf,
                    const AnimatorComponent*             animator,
                    float                                dt,
                    float                                time,
                    bool                                 canEmit,
                    const std::vector<ActiveFlowField>& flowFields,
                    renderer::ResourceHandle<renderer::TextureTag> sceneColor,
                    const MeshRenderer*                  meshParticleRenderer,
                    RenderPassContext&                   ctx, renderer::RenderParticleInput& input)
{
    auto& resources = ctx.resources;
    auto& renderer  = ctx.renderer;
    auto& h         = ctx.handles;

    if (!h.particleGpuSimCS.IsValid() || !h.particleGpuShader.IsValid()) return;

    const int maxP = (std::max)(emitter.settings.maxParticles, 1);

    /// @note Clear 要求・容量変更時は全スロットを死亡状態で再生成する。RWStructuredBuffer は
    /// @note CPU 側の vector clear では消えず、容量増加後の Dispatch は範囲外アクセスになるため。
    if (emitter.runtime.gpuClearPending || (emitter.runtime.gpuInitialized && emitter.runtime.gpuCapacity != static_cast<uint32_t>(maxP))) {
        emitter.runtime.gpuInitialized = false;
        emitter.runtime.gpuParticleBuffer = {};
        emitter.runtime.gpuSpawnBuffer = {};
        emitter.runtime.gpuEmitterCB = {};
        emitter.runtime.gpuWriteHead = 0;
        emitter.runtime.gpuSpawnCount = 0;
        emitter.runtime.gpuCapacity = 0;
        emitter.runtime.gpuClearPending = false;
        /// @note ソート表は粒子プールの index を持つので、プールを作り直したら必ず捨てる。
        emitter.runtime.gpuSortBuffer = {};
        emitter.runtime.gpuSortCB = {};
        emitter.runtime.gpuSortCapacity = 0;
        emitter.runtime.gpuForceBuffer = {};
    }

    /// @note デバイスリセット (Play Mode 移行など) 後は古いハンドルが無効になるため再初期化する
    const uint64_t currentResetVersion = resources.GetResetVersion();
    if (emitter.runtime.gpuInitialized && emitter.runtime.gpuResetVersion != currentResetVersion)
    {
        emitter.runtime.gpuInitialized  = false;
        emitter.runtime.gpuParticleBuffer = {};
        emitter.runtime.gpuSpawnBuffer    = {};
        emitter.runtime.gpuEmitterCB      = {};
        emitter.runtime.gpuWriteHead      = 0;
        emitter.runtime.gpuSpawnCount     = 0;
        /// @note デバイスリセット後は古いハンドルが全て無効。ソート表も作り直す。
        emitter.runtime.gpuSortBuffer     = {};
        emitter.runtime.gpuSortCB         = {};
        emitter.runtime.gpuSortCapacity   = 0;
        emitter.runtime.gpuForceBuffer     = {};
    }

    /// @note バッファ未作成なら初期化 (要素ゼロで確保し CS が age>=lifetime で無視する)
    if (!emitter.runtime.gpuInitialized)
    {
        emitter.runtime.flowMinimumAge = 0.0f;
        emitter.runtime.flowMaxLifetime = 0.0f;
        emitter.runtime.flowBoundsRadius = 0.0f;
        emitter.runtime.flowSpeedBound = 0.0f;
        std::vector<GpuParticle> init(static_cast<size_t>(maxP));
        /// @note 全粒子を「死亡済み」で初期化
        for (auto& p : init) p.age = p.lifetime = 1.0f;
        emitter.runtime.gpuParticleBuffer = resources.CreateRWStructuredBuffer(
            init.data(), static_cast<uint32_t>(maxP), sizeof(GpuParticle));

        emitter.runtime.gpuEmitterCB = resources.CreateConstantBuffer(sizeof(GpuParticleEmitterCB));
        emitter.runtime.gpuWriteHead     = 0;
        emitter.runtime.gpuSpawnCount    = 0;
        emitter.runtime.gpuResetVersion  = resources.GetResetVersion();
        emitter.runtime.gpuCapacity      = static_cast<uint32_t>(maxP);
        emitter.runtime.gpuInitialized   = true;
    }

    const bool simulateThisFrame = emitter.runtime.lastGpuSimulationFrame != Time::frameCount;
    if (simulateThisFrame) {
        emitter.runtime.lastGpuSimulationFrame = Time::frameCount;
    /// @note SubEmitter からの «発火元つき» スポーンは GPU 経路では出せない (InitGpuSpawnEntry は
    /// @note 位置と速度の上書きを持たない)。捨てないと、GPU のエミッターを SubEmitter に指した
    /// @note 構成で古い発火位置が溜まり続ける。
    emitter.runtime.injectedSpawns.clear();
    /// @note 今フレームのスポーンエントリを構築
    std::vector<GpuSpawnEntry> spawns;
    if (canEmit || emitter.runtime.burstPending > 0)
    {
        const int burstCount = (std::max)(emitter.runtime.burstPending, 0);
        emitter.runtime.burstPending = 0;
        for (int i = 0; i < burstCount && static_cast<int>(spawns.size()) < maxP; ++i)
        {
            GpuSpawnEntry s;
            InitGpuSpawnEntry(s, emitter, tf, animator);
            spawns.push_back(s);
        }
        if (canEmit)
        {
            /// @note lodRateScale は CPU 経路と同じく発生レートへ掛ける。掛け忘れると
            /// @note 遠距離のエミッターが GPU のときだけ間引かれない。
            emitter.runtime.emitAccum += emitter.settings.emitRate * emitter.runtime.lodRateScale
                * EmitRateCurveScale(emitter) * dt;
            while (emitter.runtime.emitAccum >= 1.0f && static_cast<int>(spawns.size()) < maxP)
            {
                emitter.runtime.emitAccum -= 1.0f;
                GpuSpawnEntry s;
                InitGpuSpawnEntry(s, emitter, tf, animator);
                spawns.push_back(s);
            }
        }
    }
    else
    {
        emitter.runtime.emitAccum = 0.0f;
    }

    /// @note 前回の移動上限で膨らませた球を引き継ぎ、今回の実スポーンを追加する。
    if (emitter.runtime.flowMinimumAge >= emitter.runtime.flowMaxLifetime) {
        emitter.runtime.flowBoundsCenter = TransformEmitterPoint(tf, emitter.settings.emitPosition);
        emitter.runtime.flowBoundsRadius = 0.0f;
        emitter.runtime.flowSpeedBound = 0.0f;
        emitter.runtime.flowMinimumAge = 0.0f;
        emitter.runtime.flowMaxLifetime = 0.0f;
    }
    for (const auto& spawn : spawns) {
        emitter.runtime.flowBoundsRadius = (std::max)(emitter.runtime.flowBoundsRadius,
            (spawn.position - emitter.runtime.flowBoundsCenter).Length());
        emitter.runtime.flowSpeedBound = (std::max)(emitter.runtime.flowSpeedBound, spawn.velocity.Length());
        emitter.runtime.flowMinimumAge = 0.0f;
        emitter.runtime.flowMaxLifetime = (std::max)(emitter.runtime.flowMaxLifetime, spawn.lifetime);
    }
    emitter.runtime.gpuSpawnCount = static_cast<uint32_t>(spawns.size());

    /// @note スポーンバッファを借りて CPU → GPU 転送する。0 件でも 1 要素は借りる (t15 を空にしない)。
    emitter.runtime.gpuSpawnBuffer = g_gpuSpawnPool.Acquire(
        resources, (std::max)(spawns.size(), size_t{ 1 }), static_cast<uint32_t>(sizeof(GpuSpawnEntry)));
    if (!emitter.runtime.gpuSpawnBuffer.IsValid())
        emitter.runtime.gpuSpawnCount = 0;
    if (emitter.runtime.gpuSpawnCount > 0)
        resources.Update(emitter.runtime.gpuSpawnBuffer, spawns.data(),
                         emitter.runtime.gpuSpawnCount * sizeof(GpuSpawnEntry));

    /// @note CS 用定数バッファ更新
    GpuParticleEmitterCB cb{};
    cb.emitterPos      = TransformEmitterPoint(tf, emitter.settings.emitPosition);
    cb.deltaTime       = dt;
    /// @note 乱流・周回・放射は専用スロットではなく gFlowFields へ入る。
    /// @note 重力と結合係数は «媒質の運動ではない» のでエミッターの値をそのまま渡す。
    cb.gravity         = emitter.settings.gravity;
    cb.maxParticles    = static_cast<uint32_t>(maxP);
    cb.colorStart      = emitter.settings.colorStart;
    cb.colorEnd        = emitter.settings.colorEnd;
    cb.spawnCount      = emitter.runtime.gpuSpawnCount;
    cb.spawnOffset     = emitter.runtime.gpuWriteHead;
    cb.colorCurvePower = emitter.settings.colorCurvePower;
    cb.flowCoupling    = emitter.settings.flowCoupling;
    cb.sizeStart       = emitter.settings.sizeStart;
    cb.sizeEnd         = emitter.settings.sizeEnd;
    cb.sizeCurvePower  = emitter.settings.sizeCurvePower;
    {
        /// @note 丸めは CPU 経路・Inspector プレビューと同じ関数で行う (end = 0 を «最後まで» と読む等)。
        const asset::FlipbookFrameRange range =
            asset::ResolveFlipbookRange(emitter.runtime.material.flipbook);
        cb.spriteColumns    = static_cast<uint32_t>(range.columns);
        cb.spriteRows       = static_cast<uint32_t>(range.rows);
        cb.spriteStartFrame = static_cast<uint32_t>(range.first);
        cb.spriteEndFrame   = static_cast<uint32_t>(range.last);
    }
    cb.time           = time;
    /// @note 乱流は gParticleForces 側へ
    cb.noiseStrength  = 0.0f;
    cb.noiseFrequency = 0.5f;
    cb.noiseSpeed     = 1.0f;
    cb.flipbookMode = static_cast<uint32_t>(emitter.runtime.material.flipbook.flipbookMode);
    cb.flipbookFramesPerSecond = (std::max)(emitter.runtime.material.flipbook.flipbookFramesPerSecond, 0.0f);

    /// @note 内蔵の流れもシーンの場も同じ StructuredBuffer へ詰める。CS 側は届いた分を全部
    /// @note 合流させればよい。本数の上限は無いので «場が捨てられて粒子が流れない» は起きない。
    std::vector<ActiveFlowField> emitterForces;
    ResolveEmitterForces(emitter, tf, flowFields, emitterForces, true);
    float flowSpeedBound = 0.0f;
    for (const auto& field : emitterForces) flowSpeedBound += FlowSpeedBound(field);
    /// @note 緩和は凸結合なので、重力後の速さと合成流速の大きい方が次の速さの上限になる。
    emitter.runtime.flowSpeedBound = (std::max)(
        emitter.runtime.flowSpeedBound + emitter.settings.gravity.Length() * std::abs(dt), flowSpeedBound) * 1.00001f;
    /// @note 任意の速度カーブの上限が未確定ならカリングを無効化して粒子を取りこぼさない。
    if (emitter.settings.useVelocityCurve)
        emitter.runtime.flowBoundsRadius = std::numeric_limits<float>::infinity();
    else
        emitter.runtime.flowBoundsRadius += emitter.runtime.flowSpeedBound * std::abs(dt) + 1.0e-4f;
    /// @note GPU と同じ加算で最小年齢を進める。残り寿命の減算では丸めが違い、早く球を捨て得る。
    emitter.runtime.flowMinimumAge += dt;

    std::vector<GpuFlowField> gpuForces;
    gpuForces.reserve(emitterForces.size());
    for (const ActiveFlowField& f : emitterForces) gpuForces.push_back(PackGpuFlowField(f, ctx.resources));
    cb.flowFieldCount = static_cast<uint32_t>(gpuForces.size());
    AcquireGpuForceBuffer(emitter, gpuForces, resources);
    cb.curveFlags = {
        emitter.settings.useSizeCurve ? 1.0f : 0.0f,
        emitter.settings.useVelocityCurve ? 1.0f : 0.0f,
        emitter.settings.useColorGradient ? 1.0f : 0.0f,
        emitter.runtime.material.flipbook.flipbookFrameBlending ? 1.0f : 0.0f
    };
    /// @note カーブは float4 1 本へ 2 キー (time,value) ずつ詰める。有効キー数を超えた分は
    /// @note 最終キーで埋め、GPU 側が余分な区間を踏んでも CPU の Evaluate と同じ値になるようにする。
    const auto packCurve = [](const ParticleCurve& curve, size_t first) {
        const size_t last = static_cast<size_t>(
            (std::max)(1u, (std::min)(curve.keyCount, kMaxParticleCurveKeys)) - 1u);
        const auto& firstKey = curve.keys[(std::min)(first, last)];
        const auto& secondKey = curve.keys[(std::min)(first + 1u, last)];
        return math::Vector4{
            firstKey.time, firstKey.value, secondKey.time, secondKey.value
        };
    };
    const auto curveKeyCount = [](const ParticleCurve& curve) {
        return static_cast<float>(
            (std::max)(1u, (std::min)(curve.keyCount, kMaxParticleCurveKeys)));
    };
    cb.sizeCurveKeys01 = packCurve(emitter.settings.sizeCurve, 0);
    cb.sizeCurveKeys23 = packCurve(emitter.settings.sizeCurve, 2);
    cb.sizeCurveKeys45 = packCurve(emitter.settings.sizeCurve, 4);
    cb.sizeCurveKeys67 = packCurve(emitter.settings.sizeCurve, 6);
    cb.velocityCurveKeys01 = packCurve(emitter.settings.velocityCurve, 0);
    cb.velocityCurveKeys23 = packCurve(emitter.settings.velocityCurve, 2);
    cb.velocityCurveKeys45 = packCurve(emitter.settings.velocityCurve, 4);
    cb.velocityCurveKeys67 = packCurve(emitter.settings.velocityCurve, 6);
    /// @note 黒体モードを焼き込んだ実効グラデーションを渡す。CPU 経路も同じ runtimeGradient を見る。
    const auto& gradient = emitter.runtime.runtimeGradient;
    const size_t lastGradientKey = static_cast<size_t>(
        (std::max)(1u, (std::min)(gradient.keyCount, kMaxParticleCurveKeys)) - 1u);
    const auto gradientTime = [&](size_t index) {
        return gradient.keys[(std::min)(index, lastGradientKey)].time;
    };
    cb.gradientTimes  = { gradientTime(0), gradientTime(1), gradientTime(2), gradientTime(3) };
    cb.gradientTimes47 = { gradientTime(4), gradientTime(5), gradientTime(6), gradientTime(7) };
    for (size_t index = 0; index < 4; ++index) {
        cb.gradientColors[index] = gradient.keys[(std::min)(index, lastGradientKey)].color;
        cb.gradientColors47[index] =
            gradient.keys[(std::min)(index + 4, lastGradientKey)].color;
    }
    /// @note z = 補間色空間 (ParticleColorSpace)。EvaluateGradient8 がこれを見て CPU と同じ空間で混ぜる。
    cb.gradientMeta = { static_cast<float>(lastGradientKey + 1),
                        static_cast<float>(gradient.interpolation),
                        static_cast<float>(gradient.colorSpace), 0.0f };
    /// @note 深度コリジョンは深度バッファを screen space で引くので、それを焼いたのと同じ
    /// @note (TAA ジッター込みの) 行列で射影しないと当たり位置が半ピクセルずれる。
    cb.viewProjection = MakeJitteredViewProjection(ctx.camera, ctx.taaJitterNdcX, ctx.taaJitterNdcY);
    cb.screenWidth = static_cast<float>(ctx.width);
    cb.screenHeight = static_cast<float>(ctx.height);
    cb.depthThickness = (std::max)(0.001f, emitter.settings.collisionRadius * 0.002f);
    cb.depthBounciness = std::clamp(emitter.settings.collisionBounciness, 0.0f, 1.0f);
    cb.depthCollision = emitter.settings.collisionMode == ParticleCollisionMode::Depth ? 1u : 0u;
    cb.depthResponse = static_cast<std::uint32_t>(emitter.settings.collisionResponse);
    cb.depthDamping = std::clamp(emitter.settings.collisionDamping, 0.0f, 1.0f);
    /// @name over-lifetime モジュール (回転カーブ / drag カーブ / 周回・放射)
    cb.curveFlags2 = {
        emitter.settings.useRotationCurve ? 1.0f : 0.0f,
        emitter.settings.useDragCurve ? 1.0f : 0.0f,
        0.0f, 0.0f
    };
    cb.rotationCurveKeys01 = packCurve(emitter.settings.rotationCurve, 0);
    cb.rotationCurveKeys23 = packCurve(emitter.settings.rotationCurve, 2);
    cb.rotationCurveKeys45 = packCurve(emitter.settings.rotationCurve, 4);
    cb.rotationCurveKeys67 = packCurve(emitter.settings.rotationCurve, 6);
    cb.dragCurveKeys01 = packCurve(emitter.settings.dragCurve, 0);
    cb.dragCurveKeys23 = packCurve(emitter.settings.dragCurve, 2);
    cb.dragCurveKeys45 = packCurve(emitter.settings.dragCurve, 4);
    cb.dragCurveKeys67 = packCurve(emitter.settings.dragCurve, 6);
    /// @note 4 本のカーブの有効キー数と補間モード。GPU 側はこれを見て CPU と同じ区間を選ぶ。
    cb.curveKeyCounts = {
        curveKeyCount(emitter.settings.sizeCurve), curveKeyCount(emitter.settings.velocityCurve),
        curveKeyCount(emitter.settings.rotationCurve), curveKeyCount(emitter.settings.dragCurve)
    };
    cb.curveModes = {
        static_cast<float>(emitter.settings.sizeCurve.interpolation),
        static_cast<float>(emitter.settings.velocityCurve.interpolation),
        static_cast<float>(emitter.settings.rotationCurve.interpolation),
        static_cast<float>(emitter.settings.dragCurve.interpolation)
    };
    /// @note 周回と放射は gParticleForces (Vortex / Source) 側へ移った。CB の orbitalAxis /
    /// @note orbitalVelocity / radialVelocity は誰も書かない死んだ枠で、cb{} のゼロ初期化のまま
    /// @note GPU へ渡る (書いていた 0 と同じ値)。フィールド自体は残す ─ この CB は
    /// @note ParticleGpuSim.cs.hlsl と 1 バイト単位で対で、途中を削ると以降の全オフセットがずれる。
    cb.spriteRandomFlags = (emitter.runtime.material.flipbook.spriteRandomStartFrame ? 1u : 0u)
        | (emitter.runtime.material.flipbook.spriteRandomRow ? 2u : 0u);
    input.simulation = cb;
    input.simulate = true;

    /// @note リングバッファヘッドを進める
    emitter.runtime.gpuWriteHead = (emitter.runtime.gpuWriteHead + emitter.runtime.gpuSpawnCount)
                           % static_cast<uint32_t>(maxP);
    }
    if (ShouldSortGpuParticles(emitter, h)) {
        uint32_t padded = kParticleSortBlock;
        while (padded < static_cast<uint32_t>(maxP)) padded <<= 1;
        if (!emitter.runtime.gpuSortBuffer.IsValid() || emitter.runtime.gpuSortCapacity != padded) {
            if (emitter.runtime.gpuSortBuffer.IsValid()) resources.Release(emitter.runtime.gpuSortBuffer);
            /// @note 1 要素 = uint2 (key, particleIndex)。
            emitter.runtime.gpuSortBuffer = resources.CreateRWStructuredBuffer(
                nullptr, padded, static_cast<std::uint32_t>(sizeof(std::uint32_t) * 2u));
            emitter.runtime.gpuSortCapacity = padded;
        }
        if (!emitter.runtime.gpuSortCB.IsValid())
            emitter.runtime.gpuSortCB = resources.CreateConstantBuffer(sizeof(GpuParticleSortCB));
    } else if (emitter.runtime.gpuSortBuffer.IsValid()) {
        resources.Release(emitter.runtime.gpuSortBuffer);
        emitter.runtime.gpuSortBuffer = {}; emitter.runtime.gpuSortCapacity = 0;
    }
    input.velocityField = asset::VelocityFieldAtlas::Texture(resources);
    if (meshParticleRenderer && meshParticleRenderer->enabled && meshParticleRenderer->mesh && !meshParticleRenderer->mesh->isSkinned) {
        const auto& mesh = *meshParticleRenderer->mesh;
        input.meshVertices = mesh.vertexBuffer; input.meshIndices = mesh.indexBuffer;
        input.meshIndexCount = mesh.indexCount; input.meshVertexCount = mesh.vertexCount;
    }
}
void UpdateParticleBounds(ParticleEmitter& emitter, const Transform& tf)
{
    /// @note 現在の粒子位置とサイズから球 Bounds を再計算する。粒子がない間は将来位置を予測して保守的に保持する。
    math::Vector3 center = TransformEmitterPoint(tf, emitter.settings.emitPosition);
    float radius = (std::max)(emitter.settings.sizeStart, emitter.settings.sizeEnd) + emitter.settings.culling.cullingBoundsPadding;
    if (!emitter.runtime.particles.empty()) {
        center = emitter.settings.simulationSpace == ParticleSimulationSpace::Local
            ? TransformEmitterPoint(tf, emitter.runtime.particles.front().position)
            : emitter.runtime.particles.front().position;
        for (const Particle& particle : emitter.runtime.particles) {
            const math::Vector3 position = emitter.settings.simulationSpace == ParticleSimulationSpace::Local
                ? TransformEmitterPoint(tf, particle.position) : particle.position;
            radius = (std::max)(radius, (position - center).Length()
                + particle.size + emitter.settings.culling.cullingBoundsPadding);
        }
    } else {
        radius += emitter.settings.emitVelocity.Length() * emitter.settings.lifetime;
    }
    emitter.runtime.boundsCenter = center;
    emitter.runtime.boundsRadius = (std::max)(radius, 0.01f);
}

/// @brief world は null 可。描画パス側が RenderPassContext::physicsWorld をそのまま渡せるよう
/// @brief ポインタで受ける (参照だと呼び出し側に null チェックとダミー World が要る)。
void SimulateCpuEmitter(ParticleEmitter& emitter, const Transform& tf,
                        const AnimatorComponent* animator, float dt, float time,
                        const std::vector<ActiveFlowField>& flowFields,
                        const physics::World* world, Scene& scene)
{
    emitter.runtime.lastCpuSimulationFrame = Time::frameCount;
    emitter.runtime.collisionCountThisFrame = 0;
    emitter.runtime.deathCountThisFrame = 0;
    const int particleCapacity = (std::max)(emitter.settings.maxParticles, 0);
    const bool localSpace = emitter.settings.simulationSpace == ParticleSimulationSpace::Local;
    /// @note 粒子 1 個のワールド位置・速度。SubEmitter へ «発火元» として渡すときに使う
    /// @note (保持しているのはシミュレーション空間の値で、Local なら世界の値ではない)。
    const auto particleWorldPosition = [&](const Particle& particle) {
        return localSpace ? TransformEmitterPoint(tf, particle.position) : particle.position;
    };
    const auto particleWorldVelocity = [&](const Particle& particle) {
        return localSpace ? TransformEmitterVector(tf, particle.velocity) : particle.velocity;
    };
    const auto queueBirth = [&] {
        if (emitter.settings.birthSubEmitter.empty() || emitter.runtime.particles.empty()) return;
        const Particle& spawned = emitter.runtime.particles.back();
        QueueSubEmitter(scene, emitter, emitter.settings.birthSubEmitter,
                        emitter.settings.subEmitterBurstCount,
                        particleWorldPosition(spawned), particleWorldVelocity(spawned));
    };
    const bool canEmit = emitter.runtime.emitThisFrame;
    if (canEmit || emitter.runtime.burstPending > 0 || !emitter.runtime.injectedSpawns.empty()) {
        /// @note 注入されたスポーンはレート/バーストより先に消費する。後回しにすると「斬った
        /// @note 位置の火花」が maxParticles 到達で黙って落ちる。イベントで 1 度だけ出る粒の
        /// @note 欠落は、常時湧く粒より目立つため。
        /// @note ループ中に injectedSpawns から消さない: SpawnParticle → queueBirth が自分自身を
        /// @note birthSubEmitter として積み返すことがあり、走査中の push_back で参照が無効化
        /// @note されるため、添字で回して最後にまとめて消す。
        for (std::size_t index = 0; index < emitter.runtime.injectedSpawns.size(); ++index) {
            const ParticleInjectedSpawn injected = emitter.runtime.injectedSpawns[index];
            for (int spawn = 0; spawn < injected.count
                 && static_cast<int>(emitter.runtime.particles.size()) < particleCapacity; ++spawn) {
                SpawnParticle(emitter, tf, animator, 0.0f, &injected);
                queueBirth();
            }
        }
        emitter.runtime.injectedSpawns.clear();

        const int burstCount = (std::max)(emitter.runtime.burstPending, 0);
        for (int index = 0; index < burstCount
             && static_cast<int>(emitter.runtime.particles.size()) < particleCapacity; ++index) {
            const float warmAge = emitter.runtime.prewarmSpawnPending > 0
                ? Random01(emitter) * (std::min)(
                    emitter.settings.duration > 0.0f ? emitter.settings.duration : emitter.settings.lifetime,
                    emitter.settings.lifetime)
                : 0.0f;
            SpawnParticle(emitter, tf, animator, warmAge);
            if (emitter.runtime.prewarmSpawnPending > 0) --emitter.runtime.prewarmSpawnPending;
            queueBirth();
        }
        emitter.runtime.burstPending = 0;

        if (static_cast<int>(emitter.runtime.particles.size()) >= particleCapacity) {
            emitter.runtime.emitAccum = 0.0f;
        } else if (canEmit) {
            emitter.runtime.emitAccum += emitter.settings.emitRate * emitter.runtime.lodRateScale
                * EmitRateCurveScale(emitter) * dt;
        }
        while (emitter.runtime.emitAccum >= 1.0f
               && static_cast<int>(emitter.runtime.particles.size()) < particleCapacity) {
            emitter.runtime.emitAccum -= 1.0f;
            SpawnParticle(emitter, tf, animator);
            queueBirth();
        }
        if (static_cast<int>(emitter.runtime.particles.size()) >= particleCapacity)
            emitter.runtime.emitAccum = 0.0f;
    } else {
        emitter.runtime.emitAccum = 0.0f;
        emitter.runtime.burstPending = 0;
    }

    /// @note このエミッターに効く力をワールド空間で 1 回だけ解決する
    /// @note (粒子ごとに解決すると同じ計算を粒子数ぶん繰り返すことになる)。
    std::vector<ActiveFlowField> emitterForces;
    ResolveEmitterForces(emitter, tf, flowFields, emitterForces);

    /// @note Physics 衝突で見るコライダーの絞り込み。エミッター単位で 1 本作って粒子数ぶん回す。
    const std::uint32_t collisionLayerMask = emitter.settings.collisionLayerMask;
    const physics::World::ColliderFilter collisionFilter =
        [collisionLayerMask](const physics::ColliderInstance& instance) {
            /// @note トリガーは «通過を検知する体積» で、跳ね返る面ではない。素通しにしないと
            /// @note 見えない判定ボリュームの表面で火花が止まる。
            if (instance.isTrigger) return false;
            if (instance.layer < 0 || instance.layer > 31) return true;
            return (collisionLayerMask & (1u << static_cast<unsigned>(instance.layer))) != 0u;
        };

    for (auto it = emitter.runtime.particles.begin(); it != emitter.runtime.particles.end();) {
        it->age += dt;
        if (it->age >= it->lifetime) {
            ++emitter.runtime.deathCountThisFrame;
            /// @note 消えた «その場所» と最後の速度。これが無いと煙が発生源へ戻って湧く。
            QueueSubEmitter(scene, emitter, emitter.settings.deathSubEmitter,
                            emitter.settings.subEmitterBurstCount,
                            particleWorldPosition(*it), particleWorldVelocity(*it));
            it = emitter.runtime.particles.erase(it);
            continue;
        }
        const float normalizedAge = Clamp01(it->age / (std::max)(it->lifetime, 0.001f));
        /// @note Drag over Lifetime カーブは flowCoupling への時間倍率。
        const float dragScale = emitter.settings.useDragCurve
            ? (std::max)(emitter.settings.dragCurve.Evaluate(normalizedAge), 0.0f) : 1.0f;
        const float coupling = emitter.settings.flowCoupling * dragScale;
        /// @note 流れはすべてワールドで効く。乱流・周回・放射・焼いた場・シーンの場が 1 本の
        /// @note 評価器を通るので、種類ごとに «どの空間で効くか» を覚える必要が無い。
        /// @note 順序は GPU (ParticleGpuSim.cs.hlsl) と同じ «重力 → 緩和»。
        if (emitter.settings.simulationSpace == ParticleSimulationSpace::Local) {
            const math::Vector3 worldPosition = TransformEmitterPoint(tf, it->position);
            math::Vector3 worldVelocity = TransformEmitterVector(tf, it->velocity);
            worldVelocity = worldVelocity + emitter.settings.gravity * dt;
            ApplyFlowFields(emitterForces, emitter.settings.flowFieldChannels,
                            worldPosition, worldVelocity, dt, time, coupling);
            it->velocity = InverseTransformEmitterVector(tf, worldVelocity);
        } else {
            it->velocity = it->velocity + emitter.settings.gravity * dt;
            ApplyFlowFields(emitterForces, emitter.settings.flowFieldChannels,
                            it->position, it->velocity, dt, time, coupling);
        }
        const float velocityScale = emitter.settings.useVelocityCurve
            ? (std::max)(emitter.settings.velocityCurve.Evaluate(normalizedAge), 0.0f) : 1.0f;
        const math::Vector3 nextPosition = it->position + it->velocity * (dt * velocityScale);
        bool killedByCollision = false;
        if (emitter.settings.simulationSpace == ParticleSimulationSpace::Local
            && emitter.settings.collisionMode != ParticleCollisionMode::None) {
            Particle worldParticle = *it;
            worldParticle.position = TransformEmitterPoint(tf, it->position);
            worldParticle.velocity = TransformEmitterVector(tf, it->velocity);
            killedByCollision = ResolveParticleCollision(
                emitter, worldParticle, TransformEmitterPoint(tf, nextPosition), world, scene,
                collisionFilter);
            it->position = InverseTransformEmitterPoint(tf, worldParticle.position);
            it->velocity = InverseTransformEmitterVector(tf, worldParticle.velocity);
        } else {
            killedByCollision = ResolveParticleCollision(emitter, *it, nextPosition, world, scene,
                                                         collisionFilter);
        }
        if (killedByCollision) {
            it = emitter.runtime.particles.erase(it);
            continue;
        }
        /// @note 回転カーブは角速度への時間倍率。勢いよく回り始めて減速する破片が作れる。
        it->rotation += it->angularVelocity * dt
            * (emitter.settings.useRotationCurve
                   ? emitter.settings.rotationCurve.Evaluate(normalizedAge) : 1.0f);
        const SpriteFrameState sprite = ComputeSpriteFrameState(
            emitter, normalizedAge, it->age, it->spriteSeed);
        it->uvRect = sprite.currentRect;
        it->nextUvRect = sprite.nextRect;
        it->spriteBlend = sprite.blend;
        it->color = EvaluateParticleColorLinear(emitter, *it, normalizedAge);
        const float sizeT = emitter.settings.useSizeCurve
            ? Clamp01(emitter.settings.sizeCurve.Evaluate(normalizedAge))
            : std::pow(normalizedAge, emitter.settings.sizeCurvePower);
        it->size = it->startSize + (it->endSize - it->startSize) * sizeT;
        /// @note 速さによる大きさ。火花が «速いほど大きく (= 長く) 見える» を作る。
        if (emitter.settings.useSpeedSizeCurve) {
            it->size *= (std::max)(emitter.settings.speedSizeCurve.Evaluate(
                NormalizedParticleSpeed(emitter, *it)), 0.0f);
        }
        AppendParticleTrailPoint(emitter, *it, dt);
        ++it;
    }
    UpdateParticleBounds(emitter, tf);
}

} /// @note namespace

/// @brief 再生進行の正本。ParticleSimulationSystem (LateUpdate) と ParticlePass の両方がこれを呼ぶ。
/// @note 以前は System と Pass に別実装があり、rateOverDistance の基準点が食い違っていた
/// @note (System は worldPosition、Pass は発生点)。基準点は発生点に寄せる: emitPosition を
/// @note ずらしたエミッターでは、実際に粒が出る場所が動いた量が正しいため。
static bool AdvanceParticleEmitterPlaybackAtFrame(ParticleEmitter& emitter, const Transform& tf,
                                                  float dt, std::uint64_t frameToken)
{
    if (emitter.runtime.lastPlaybackFrame == frameToken) return emitter.runtime.emitThisFrame;
    emitter.runtime.lastPlaybackFrame = frameToken;
    /// @note 黒体モードの焼き込みはここで 1 フレームに 1 回だけ行う。CPU 更新も GPU 定数バッファも
    /// @note この後の runtimeGradient を読むため、両経路の色が原理的にずれない。
    emitter.RefreshRuntimeGradient();

    bool canEmit = emitter.settings.playing;
    if (canEmit && emitter.runtime.delayTime < emitter.settings.startDelay) {
        emitter.runtime.delayTime += dt;
        canEmit = false;
    }
    /// @note duration の有無に関わらず進める (playTime は «再生開始からの経過»)。
    /// @note 止めると duration = 0 のエミッターで時刻指定 Burst が永久に発火しない。
    if (canEmit) emitter.runtime.playTime += dt;
    const float loopTolerance = emitter.settings.loop
        ? (std::min)((std::max)(dt, 0.0f) * 1.0e-3f, emitter.settings.duration * 1.0e-3f)
        : 0.0f;
    if (canEmit && emitter.settings.duration > 0.0f
        && emitter.runtime.playTime + loopTolerance >= emitter.settings.duration) {
        if (emitter.settings.loop) {
            /// @note lifetime と duration が等しい粒子は同じ刻みで死ぬ。float の累積誤差で
            /// @note 次の Burst が 1 刻み遅れると空白が出るので、境界近傍を同時刻とみなす。
            const float remainder = std::fmod(emitter.runtime.playTime, emitter.settings.duration);
            emitter.runtime.playTime = std::abs(emitter.runtime.playTime - emitter.settings.duration)
                <= loopTolerance ? 0.0f : remainder;
            emitter.runtime.delayTime = 0.0f;
            emitter.runtime.burstCyclesFired.clear();
        } else {
            emitter.settings.playing = false;
            canEmit = false;
            if (emitter.settings.clearOnStop) {
                ClearEmitterRuntime(emitter);
                emitter.runtime.gpuClearPending = true;
            }
        }
    }
    emitter.runtime.emitThisFrame = canEmit;

    /// @note 距離Emission: Emitterのワールド移動量を粒子数へ変換する。
    const math::Vector3 emitterWorldPosition = TransformEmitterPoint(tf, emitter.settings.emitPosition);
    /// @note エミッター自身のワールド速度。inheritVelocity がスポーン初速へ足すために使う。
    /// @note dt が 0 (完全停止・スクラブ中) のときは前回値を保つ (0 除算と速度の消失を避ける)。
    if (emitter.runtime.hasLastEmitterPosition && dt > 1.0e-6f) {
        emitter.runtime.emitterVelocity =
            (emitterWorldPosition - emitter.runtime.lastEmitterPosition) * (1.0f / dt);
    }
    if (emitter.runtime.hasLastEmitterPosition && canEmit && emitter.settings.rateOverDistance > 0.0f) {
        emitter.runtime.distanceEmitAccum += (emitterWorldPosition - emitter.runtime.lastEmitterPosition).Length()
            * emitter.settings.rateOverDistance;
        const int distanceCount = static_cast<int>(emitter.runtime.distanceEmitAccum);
        if (distanceCount > 0) {
            emitter.runtime.burstPending += distanceCount;
            emitter.runtime.distanceEmitAccum -= static_cast<float>(distanceCount);
        }
    }
    emitter.runtime.lastEmitterPosition = emitterWorldPosition;
    emitter.runtime.hasLastEmitterPosition = true;

    /// @note 時刻指定Burst。loop時はplayTimeの巻き戻しでcycle状態もリセットされる。
    if (emitter.runtime.burstCyclesFired.size() != emitter.settings.bursts.size())
        emitter.runtime.burstCyclesFired.assign(emitter.settings.bursts.size(), 0);
    for (size_t burstIndex = 0; burstIndex < emitter.settings.bursts.size(); ++burstIndex) {
        const ParticleBurst& burst = emitter.settings.bursts[burstIndex];
        int& fired = emitter.runtime.burstCyclesFired[burstIndex];
        const int cycles = (std::max)(burst.cycles, 1);
        while (fired < cycles
               && emitter.runtime.playTime >= burst.time + burst.interval * static_cast<float>(fired)) {
            if (Random01(emitter) <= Clamp01(burst.probability))
                emitter.runtime.burstPending += (std::max)(burst.count, 0);
            ++fired;
        }
    }

    /// @note Prewarmは初回描画前に定常個数を投入する。GPU readbackを避けるため履歴位置は近似する。
    if (emitter.settings.prewarm && emitter.settings.loop && !emitter.runtime.prewarmed) {
        const float warmDuration = emitter.settings.duration > 0.0f ? emitter.settings.duration : emitter.settings.lifetime;
        const int warmCount = static_cast<int>((std::max)(emitter.settings.emitRate, 0.0f)
            * (std::max)(warmDuration, 0.0f));
        const int clampedWarmCount = (std::min)(warmCount, (std::max)(emitter.settings.maxParticles, 0));
        emitter.runtime.burstPending += clampedWarmCount;
        emitter.runtime.prewarmSpawnPending += clampedWarmCount;
        emitter.runtime.prewarmed = true;
    }
    return canEmit;
}

bool AdvanceParticleEmitterPlayback(ParticleEmitter& emitter, const Transform& tf, float dt)
{
    return AdvanceParticleEmitterPlaybackAtFrame(emitter, tf, dt, Time::frameCount);
}

void UpdateParticleCpuSimulation(Scene& scene, physics::World& world, float deltaTime, float time)
{
    /// @note 場はカメラに属さない。Scene のフレームキャッシュを 3 経路 (CPU 更新・エディタの
    /// @note スクラブ・描画パス) で共有し、simulationMode やビューで軌道が割れないようにする。
    const auto flowSnapshot = scene.FlowFrame().fields;
    const std::vector<ActiveFlowField>& flowFields = *flowSnapshot;
    for (EntityID id : scene.GetEntities<ParticleEmitter>()) {
        auto* emitter = scene.GetComponent<ParticleEmitter>(id);
        GameObject* gameObject = scene.GetGameObject(id);
        if (!emitter || !gameObject || !gameObject->activeInHierarchy()
            || !emitter->settings.enabled
            || CanUseGpuSimulation(emitter->settings, &emitter->runtime.material)) continue;
        /// @note GPU指定を一時的にCPUへ縮退した場合、制約解除後に古いGPU粒子が復活しないよう履歴を破棄する。
        if (emitter->settings.simulationMode == ParticleSimulationMode::Gpu)
            emitter->runtime.gpuClearPending = true;
        /// @note 発生側は ParticleSimulationSystem が止めるが、粒子の更新はこちら。
        /// @note 見ないと «発生は止まるのに既存の粒子は動き続ける» 中途半端な一時停止になる。
        if (emitter->settings.culling.pauseWhenCulled && emitter->runtime.isCulledThisFrame) continue;
        if (emitter->runtime.lastCpuSimulationFrame == Time::frameCount) continue;
        const auto* animator = FindParticleAnimator(*gameObject);
        /// @note VFX Editor のプレビュー速度 (一時停止 / スロー / 倍速) をエミッター単位で dt へ注入する。
        const float scaledDt = deltaTime * emitter->GetEditorTimeScale(Time::frameCount);
        SimulateCpuEmitter(*emitter, gameObject->transform, animator, scaledDt, time,
                           flowFields, &world, scene);
    }
}

bool InspectParticleEmitterLoop(Scene& scene, physics::World& world,
                                GameObject& gameObject, ParticleEmitter& emitter,
                                int fps, int cycles, std::span<const float> frameCoverage,
                                ParticleLoopInspection& out)
{
    out = {};
    if (fps <= 0 || cycles <= 0 || emitter.settings.duration <= 0.0f || !emitter.settings.loop
        || emitter.settings.simulationMode != ParticleSimulationMode::Cpu)
        return false;
    out.fps = fps;
    const float dt = 1.0f / static_cast<float>(fps);
    const int cycleFrames = (std::max)(1, static_cast<int>(std::lround(emitter.settings.duration * fps)));
    const int totalFrames = cycleFrames * cycles;
    const int columns = (std::max)(emitter.runtime.material.flipbook.spriteColumns, 1);
    const int rows = (std::max)(emitter.runtime.material.flipbook.spriteRows, 1);
    const auto flowSnapshot = scene.FlowFrame().fields;
    const std::vector<ActiveFlowField>& flowFields = *flowSnapshot;
    const auto* animator = FindParticleAnimator(gameObject);
    emitter.ResetPlayback();
    emitter.runtime.lastPlaybackFrame = UINT64_MAX;
    for (int frame = 0; frame < totalFrames; ++frame) {
        const float time = static_cast<float>(frame + 1) * dt;
        (void)AdvanceParticleEmitterPlaybackAtFrame(emitter, gameObject.transform, dt,
                                                     static_cast<std::uint64_t>(frame));
        SimulateCpuEmitter(emitter, gameObject.transform, animator, dt, time, flowFields, &world, scene);
        bool visible = false;
        for (const Particle& particle : emitter.runtime.particles) {
            if (particle.size <= 0.0f || particle.color.w <= 0.01f) continue;
            if (frameCoverage.empty()) { visible = true; break; }
            const auto coverageOf = [&](const math::Vector4& uvRect) {
                const int column = std::clamp(static_cast<int>(std::lround(uvRect.x * columns)), 0, columns - 1);
                const int row = std::clamp(static_cast<int>(std::lround(uvRect.y * rows)), 0, rows - 1);
                const std::size_t index = static_cast<std::size_t>(row * columns + column);
                return index < frameCoverage.size() ? frameCoverage[index] : 0.0f;
            };
            const float coverage = coverageOf(particle.uvRect) * (1.0f - particle.spriteBlend)
                + coverageOf(particle.nextUvRect) * particle.spriteBlend;
            if (coverage * particle.color.w > 0.001f) { visible = true; break; }
        }
        ++out.sampledFrames;
        if (visible) continue;
        ++out.emptyFrames;
        if (out.firstEmptyFrame < 0) out.firstEmptyFrame = frame;
        const int phase = frame % cycleFrames;
        if (phase <= 1 || phase >= cycleFrames - 2) ++out.boundaryEmptyFrames;
    }
    return true;
}

void ScrubParticleEmitterForEditor(Scene& scene, physics::World& world,
                                   GameObject& gameObject, ParticleEmitter& emitter,
                                   float targetTime, float time)
{
    /// @note 巻き戻し。randomState が randomSeed に戻るため、同じ targetTime へのスクラブは常に同じ結果になる。
    emitter.ResetPlayback();

    /// @note GPU 粒子は CS 側バッファの履歴を任意時刻へ巻き戻せないため、リスタートのみで返す。
    if (CanUseGpuSimulation(emitter.settings, &emitter.runtime.material)) {
        /// @note 直後の Prewarm 一括投入でスクラブ結果が壊れないようにする
        emitter.runtime.prewarmed = true;
        return;
    }

    /// @note 固定ステップで決定論的に早送りする。上限を設けて極端な値でエディターが固まるのを防ぐ。
    constexpr float kFixedStep       = 1.0f / 60.0f;
    constexpr float kMaxScrubSeconds = 60.0f;
    const float target = (std::min)((std::max)(targetTime, 0.0f), kMaxScrubSeconds);

    const auto flowSnapshot = scene.FlowFrame().fields;
    const std::vector<ActiveFlowField>& flowFields = *flowSnapshot;
    const auto* animator = FindParticleAnimator(gameObject);

    float simulated = 0.0f;
    while (simulated < target) {
        const float step = (std::min)(kFixedStep, target - simulated);

        /// @note 再生状態 (delay / duration / loop / Burst) を 1 ステップ進める。
        /// @note ステップ単位で再現しないと Burst の発火やループ巻き戻しが実時間再生とずれる。
        bool canEmit = emitter.settings.playing;
        if (canEmit && emitter.runtime.delayTime < emitter.settings.startDelay) {
            emitter.runtime.delayTime += step;
            canEmit = false;
        }
        if (canEmit && emitter.settings.duration > 0.0f) {
            emitter.runtime.playTime += step;
            if (emitter.runtime.playTime >= emitter.settings.duration) {
                if (emitter.settings.loop) {
                    emitter.runtime.playTime = 0.0f;
                    emitter.runtime.delayTime = 0.0f;
                    emitter.runtime.burstCyclesFired.clear();
                } else {
                    emitter.settings.playing = false;
                    canEmit = false;
                    if (emitter.settings.clearOnStop) {
                        emitter.runtime.particles.clear();
                        emitter.runtime.gpuClearPending = true;
                    }
                }
            }
        }
        emitter.runtime.emitThisFrame = canEmit;

        if (emitter.runtime.burstCyclesFired.size() != emitter.settings.bursts.size())
            emitter.runtime.burstCyclesFired.assign(emitter.settings.bursts.size(), 0);
        for (size_t index = 0; index < emitter.settings.bursts.size(); ++index) {
            const ParticleBurst& burst = emitter.settings.bursts[index];
            int& fired = emitter.runtime.burstCyclesFired[index];
            const int cycles = (std::max)(burst.cycles, 1);
            while (fired < cycles
                   && emitter.runtime.playTime >= burst.time + burst.interval * static_cast<float>(fired)) {
                if (Random01(emitter) <= Clamp01(burst.probability))
                    emitter.runtime.burstPending += (std::max)(burst.count, 0);
                ++fired;
            }
        }

        SimulateCpuEmitter(emitter, gameObject.transform, animator, step, time,
                           flowFields, &world, scene);
        simulated += step;
    }

    /// @note スクラブ後に Prewarm の一括投入や同フレームの通常再生が重なって状態を壊さないようにする。
    emitter.runtime.prewarmed              = true;
    emitter.runtime.lastPlaybackFrame      = Time::frameCount;
    emitter.runtime.lastCpuSimulationFrame = Time::frameCount;
}
void ExtractRenderParticles(RenderPassContext& ctx, renderer::RenderScene& output) {
    auto& resources = ctx.resources;
    auto& h = ctx.handles;
    ++g_particlePassSerial;
    if (!h.particleShader.IsValid() || !h.particleIB.IsValid()) return;
    const float dt = Time::deltaTime, time = Time::time;
    const auto flowSnapshot = ctx.scene.FlowFrame().fields;
    const std::vector<ActiveFlowField>& flowFields = *flowSnapshot;
    int particleBudget = ctx.settings.particleBudgetEnabled
        ? (std::max)(ctx.settings.particleBudget, 0) : 0;

    /// @note 描画順を renderPriority → カメラ距離 (遠い順) で確定させる。GameObject の並び順の
    /// @note ままだと「炎の手前に煙」が保証されず、シーンを編集しただけで前後が入れ替わる。
    /// @note 距離はバウンズ更新前なので Transform 位置で近似する (順序決定には十分)。
    std::vector<GameObject*> sortedEmitters;
    for (auto& candidate : ctx.scene.GameObjects()) {
        if (!ShouldRenderGameObject(candidate, ctx.cullingMask)) continue;
        auto* candidateEmitter = candidate.GetComponent<ParticleEmitter>();
        if (candidateEmitter == nullptr || !candidateEmitter->settings.enabled) continue;
        sortedEmitters.push_back(&candidate);
    }
    const math::Vector3 cameraPosition = ctx.camera.m_position;
    /// @note GetComponent は非 const 版しかないため、比較関数も非 const ポインタで受ける。
    std::stable_sort(sortedEmitters.begin(), sortedEmitters.end(),
        [cameraPosition](GameObject* a, GameObject* b) {
            const int priorityA = a->GetComponent<ParticleEmitter>()->settings.renderPriority;
            const int priorityB = b->GetComponent<ParticleEmitter>()->settings.renderPriority;
            if (priorityA != priorityB) return priorityA < priorityB;
            const math::Vector3 deltaA = a->transform.worldPosition - cameraPosition;
            const math::Vector3 deltaB = b->transform.worldPosition - cameraPosition;
            return math::Vector3::Dot(deltaA, deltaA) > math::Vector3::Dot(deltaB, deltaB);
        });

    for (GameObject* emitterObject : sortedEmitters) {
        GameObject& go = *emitterObject;
        auto* emitter = go.GetComponent<ParticleEmitter>();
        const auto* animator = FindParticleAnimator(go);
        const Transform& tf = go.transform;
        ++ctx.statsParticleEmitters;

        /// @note VFX Editor のプレビュー速度 (一時停止 / スロー / 倍速)。ゲーム実行時は常に 1.0。
        const float dtEmitter = dt * emitter->GetEditorTimeScale(Time::frameCount);

        emitter->settings.duration = (std::max)(emitter->settings.duration, 0.0f);
        emitter->settings.startDelay = (std::max)(emitter->settings.startDelay, 0.0f);
        emitter->settings.sizeCurvePower = (std::max)(emitter->settings.sizeCurvePower, 0.001f);
        emitter->settings.colorCurvePower = (std::max)(emitter->settings.colorCurvePower, 0.001f);
        /// @note 結合係数が負だと «流れから遠ざかる» 向きに速度が積まれて毎フレーム発散する。
        emitter->settings.flowCoupling = (std::max)(emitter->settings.flowCoupling, 0.0f);
        UpdateParticleBounds(*emitter, tf);
        const float cameraDistance = (emitter->runtime.boundsCenter - ctx.camera.m_position).Length();
        const float coverage = emitter->runtime.boundsRadius / (std::max)(cameraDistance, 0.001f);
        const bool frustumVisible = !ctx.cameraFrustum
            || ctx.cameraFrustum->IntersectsSphere(emitter->runtime.boundsCenter, emitter->runtime.boundsRadius);
        const bool coverageVisible = emitter->settings.culling.screenCoverageThreshold <= 0.0f
            || coverage >= emitter->settings.culling.screenCoverageThreshold;
        emitter->runtime.isCulledThisFrame = emitter->settings.culling.cullingEnabled && (!frustumVisible || !coverageVisible);
        if (emitter->runtime.isCulledThisFrame) {
            ++ctx.statsParticleCulled;
            emitter->runtime.visibleParticleCount = 0;
            /// @note pauseWhenCulled が false なら「描かないだけ」で時間は進める。
            /// @note 非 Play のエディターではこのパスがシミュレーションの実体なので、進めないと
            /// @note カメラを画面外へ振った瞬間にエフェクトが凍る。
            if (!emitter->settings.culling.pauseWhenCulled) {
                (void)AdvanceParticleEmitterPlayback(*emitter, tf, dtEmitter);
                if (!CanUseGpuSimulation(emitter->settings, &emitter->runtime.material)
                    && emitter->runtime.lastCpuSimulationFrame != Time::frameCount)
                    SimulateCpuEmitter(*emitter, tf, animator, dtEmitter, time,
                                       flowFields, ctx.physicsWorld, ctx.scene);
            }
            continue;
        }
        if (emitter->settings.culling.lodEnabled && emitter->settings.culling.lodFarDistance > emitter->settings.culling.lodNearDistance) {
            const float alpha = Clamp01((cameraDistance - emitter->settings.culling.lodNearDistance)
                / (emitter->settings.culling.lodFarDistance - emitter->settings.culling.lodNearDistance));
            emitter->runtime.lodRateScale = emitter->settings.culling.lodNearRateScale
                + (emitter->settings.culling.lodFarRateScale - emitter->settings.culling.lodNearRateScale) * alpha;
        } else {
            emitter->runtime.lodRateScale = 1.0f;
        }
        EnsureParticleTexture(*emitter, resources);
        /// @note Physics Query、Local Space、厳密な透過ソートはCPU側で解決し、見た目の正しさを優先する。
        const bool isGpuMode = CanUseGpuSimulation(emitter->settings, &emitter->runtime.material);

        const bool canEmit = AdvanceParticleEmitterPlayback(*emitter, tf, dtEmitter);

        renderer::RenderParticleInput input;
        input.layer = static_cast<uint32_t>(go.layer); input.position = tf.worldPosition;
        input.priority = emitter->settings.renderPriority; input.gpu = isGpuMode;
        input.settings.sortMode = emitter->settings.sortMode;
        input.settings.trail = emitter->settings.trail;
        input.settings.colorStart = emitter->settings.colorStart; input.settings.colorEnd = emitter->settings.colorEnd;
        input.settings.meshParticle = !emitter->settings.meshParticlePath.empty();
        input.settings.maxParticles = emitter->settings.maxParticles;
        int count = 0;
        if (isGpuMode) {
            const int requested = (std::max)(emitter->settings.maxParticles, 0);
            count = particleBudget > 0 ? (std::min)(requested, particleBudget) : requested;
            emitter->runtime.visibleParticleCount = count;
            if (particleBudget > 0) particleBudget -= count;
            if (requested > count) ++ctx.statsParticleBudgetDropped;
            if (count <= 0) continue;
            ctx.statsParticleVisible += count;
            input.constants = BuildParticleRenderConstants(*emitter, resources, count, ctx.width, ctx.height, ShouldSortGpuParticles(*emitter, h));
            const auto* meshRenderer = input.settings.meshParticle ? go.GetComponent<MeshRenderer>() : nullptr;
            PrepareGpuEmitter(*emitter, tf, animator, dtEmitter, time, canEmit, flowFields, {}, meshRenderer, ctx, input);
        } else {
            const int available = static_cast<int>(emitter->runtime.particles.size());
            const int budget = particleBudget > 0 ? (std::min)(available, particleBudget) : available;
            count = (std::min)(budget, kMaxParticleDraw);
            emitter->runtime.visibleParticleCount = count;
            if (particleBudget > 0) particleBudget -= count;
            if (available > count) ++ctx.statsParticleBudgetDropped;
            ctx.statsParticleVisible += count;
            if (count == 0 || input.settings.meshParticle) continue;
            input.constants = BuildParticleRenderConstants(*emitter, resources, count, ctx.width, ctx.height);
            input.runtime.particles = emitter->runtime.particles;
            if (emitter->settings.simulationSpace == ParticleSimulationSpace::Local) {
                for (auto& particle : input.runtime.particles) {
                    particle.position = TransformEmitterPoint(tf, particle.position);
                    particle.velocity = TransformEmitterVector(tf, particle.velocity);
                    for (auto& point : particle.trailPoints) point = TransformEmitterPoint(tf, point);
                }
            }
        }
        input.drawCount = count;
        if (emitter->settings.trail.trailRibbon && !emitter->runtime.trailRibbonCB.IsValid())
            emitter->runtime.trailRibbonCB = resources.CreateConstantBuffer(sizeof(TrailCB));
        input.runtime.texture = emitter->runtime.texture;
        input.runtime.distortionTexture = emitter->runtime.distortionTexture;
        input.runtime.motionVectorTexture = emitter->runtime.motionVectorTexture;
        input.runtime.sixWayNegativeTexture = emitter->runtime.sixWayNegativeTexture;
        input.runtime.sixWayAlbedoColorTexture = emitter->runtime.sixWayAlbedoColorTexture;
        input.runtime.sixWayEmissionColorTexture = emitter->runtime.sixWayEmissionColorTexture;
        input.runtime.customShader = emitter->runtime.customShader;
        input.runtime.renderCB = emitter->runtime.renderCB;
        input.runtime.materialParamsCB = emitter->runtime.materialParamsCB;
        input.runtime.trailRibbonCB = emitter->runtime.trailRibbonCB;
        input.runtime.gpuEmitterCB = emitter->runtime.gpuEmitterCB;
        input.runtime.gpuSortCB = emitter->runtime.gpuSortCB;
        input.runtime.gpuParticleBuffer = emitter->runtime.gpuParticleBuffer;
        input.runtime.gpuSpawnBuffer = emitter->runtime.gpuSpawnBuffer;
        input.runtime.gpuForceBuffer = emitter->runtime.gpuForceBuffer;
        input.runtime.gpuSortBuffer = emitter->runtime.gpuSortBuffer;
        input.runtime.gpuSortCapacity = emitter->runtime.gpuSortCapacity;
        input.runtime.resolvedBlend = emitter->runtime.resolvedBlend;
        input.runtime.textureIsSrgb = emitter->runtime.textureIsSrgb;
        input.runtime.material.distortion = emitter->runtime.material.distortion;
        input.runtime.material.punctualLighting = emitter->runtime.material.punctualLighting;
        input.runtime.material.selfShadowStrength = emitter->runtime.material.selfShadowStrength;
        output.particles.push_back(std::move(input));
    }
}
}
