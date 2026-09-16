/// @file    ParticlePass.cpp
/// @brief   パーティクルの CPU シミュレーション・GPU ディスパッチ・描画。
/// @author  Hasegawa Jin
/// @date    2026-06-18

#include "GeometryPasses.hpp"
#include <Engine/Scene/Systems/ParticleOverdrawStats.hpp>
#include <Engine/Scene/Systems/ParticleSimulationRuntime.hpp>
#include "Engine/Scene/Scene.hpp"
#include "Engine/Scene/Transform.hpp"
#include "Engine/Scene/Components/ParticleEmitter.hpp"
#include "Engine/Scene/Components/ForceField.hpp"
#include "Engine/Scene/Components/ParticleGpuSimulation.hpp"
#include "Engine/Scene/Components/MeshRenderer.hpp"
#include "Engine/Scene/Components/AnimatorComponent.hpp"
#include "Engine/Asset/AssetManager.hpp"
#include "Engine/Asset/MaterialAsset.hpp"
#include "Engine/Asset/MaterialParamBinding.hpp"
#include "Engine/Asset/VectorFieldAsset.hpp"
#include "Engine/Asset/VelocityFieldAtlas.hpp"
#include "ParticleEmitterSpace.hpp"
#include "ParticleForces.hpp"
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

// 粒子の軌跡を一定間隔でサンプリングして履歴へ積む。
// [0] を最新として後ろへずらす。点数が最大 8 と小さいので、リングバッファではなく
// 素直なシフトにする (描画側が「新しい順」を仮定でき、読み手が追いやすい)。
void AppendParticleTrailPoint(const ParticleEmitter& emitter, Particle& particle, float dt)
{
    if (!emitter.settings.trail.trailEnabled) {
        particle.trailCount = 0;
        return;
    }
    const int capacity = std::clamp(emitter.settings.trail.trailPointCount, 1, kMaxParticleTrailPoints);
    particle.trailSampleTimer += dt;
    // 間隔 0 を許すと 1 フレームに何度も積んで履歴が一瞬で埋まるため下限を切る。
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
    // WHAT: Numerical Recipes 系 LCG。軽量で、エミッターごとの seed から決定的な乱数列を作る。
    // WHY: std::rand() はグローバル状態のため、複数エミッターや再生順序で結果が変わりやすい。
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

// 粒子ごとの色ゆらぎ倍率を 1 粒子ぶん引く。RGB を各チャンネル独立に [1-v, 1+v] 倍して
// 群れの単調さを崩す。alpha は返さない (フェード制御なのでゆらすと消え際が汚くなる)。
//
// 色ではなく倍率を返すのは、グラデーション使用時に色が毎フレーム作り直されるため
// (スポーン時に焼き込むと翌フレームには消える)。
// CPU/GPU どちらのスポーン経路からも同じ乱数列で呼ぶので結果は決定論的に一致する。
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

// 寿命 t における粒子色をリニアで求める。グラデーション経路と start/end 経路の
// どちらでも、色空間変換とゆらぎの適用順序を 1 か所に集める。
// 散らすと (以前そうだったように) 更新ループだけがゆらぎを取りこぼす。
// 発生量の時間倍率。emitRate へ掛ける。
// 粒の寿命ではなく再生時刻で引く — これはエミッターの «出し方» であって粒の性質ではない。
[[nodiscard]] float EmitRateCurveScale(const ParticleEmitter& emitter)
{
    if (!emitter.settings.useEmitRateCurve) return 1.0f;
    const float span = (std::max)(emitter.settings.duration, 0.0001f);
    float t = emitter.runtime.playTime / span;
    // loop するエミッターは 1 周ごとに同じ形を繰り返す。
    if (emitter.settings.loop) t -= std::floor(t);
    return (std::max)(emitter.settings.emitRateCurve.Evaluate(Clamp01(t)), 0.0f);
}

// 粒の «速さ» を speedRange で正規化した 0..1。
// Local 空間では velocity もローカル量なので、エミッターをスケールすると読みが変わる。
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
    // 速さによる色。寿命の色へ «乗算» する。置き換えると、跳ね返って減速した粒だけ
    // 寿命の色を失う。
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
    // コマ選びの規則は EvaluateFlipbookFrame が正本。.mat Inspector のプレビューも同じ関数を
    // 呼ぶので、ここで独自に丸めると «プレビューとゲームで 1 コマずれる» になる。
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
    // VFX Graphの生成Particleはownerの子になるため、Skinned Mesh spawnは祖先Animatorも参照する。
    for (GameObject* current = &object; current != nullptr; current = current->GetParent())
        if (const auto* animator = current->GetComponent<AnimatorComponent>()) return animator;
    return nullptr;
}

// FBX / Modelの頂点をMeshSurface Shape用の軽量な点群へ変換する。
// WHY: AssetManagerのModel所有権を侵さず、スポーンごとのモデル走査も避ける。
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
        // サブメッシュを 1 本の頂点列へ連結するので、三角形の添字はこの分だけずらす。
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
            // 壊れたインデックス 1 本で形状全体を捨てるより、その面だけ落として続ける。
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
    // 面積が実質ゼロ (退化三角形しかない) なら面積抽選は成立しない。頂点一様抽選へ落とす。
    if (totalArea <= 1.0e-12f) emitter.runtime.meshShapeTriangles.clear();
}

// 面積の累積分布を 1 回の乱数で引く。三角形が無いときは呼ばない。
const MeshShapeTriangle& PickMeshShapeTriangle(ParticleEmitter& emitter)
{
    const std::vector<MeshShapeTriangle>& triangles = emitter.runtime.meshShapeTriangles;
    const float target = Random01(emitter) * triangles.back().cumulativeArea;
    const auto found = std::lower_bound(
        triangles.begin(), triangles.end(), target,
        [](const MeshShapeTriangle& triangle, float value) { return triangle.cumulativeArea < value; });
    return found == triangles.end() ? triangles.back() : *found;
}

// GPU スキニングと同じ 4 ウェイト線形ブレンドを CPU 側の発生点にだけ適用する。
// 粒子本体は GPU シミュレーションのまま、読み戻しなしで SkinnedAnimation へ追従できる。
// 法線を w=0 で流すのは逆転置行列を Animator が持たないため。ボーン行列は回転と平行移動が
// 主なので、平行移動だけ落とせば向きは十分合う。
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
        // 三角形内部の一様分布。(u, v) が外側へ出たら折り返す — sqrt を使う定番より
        // 分岐 1 つ分安く、乱数の消費が 2 回で固定される。
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
        // インデックスを持たないメッシュは面を組めない。従来どおり頂点を一様に引く。
        const size_t lastIndex = emitter.runtime.meshShapeVertices.size() - 1;
        const size_t index = (std::min)(
            static_cast<size_t>(Random01(emitter) * static_cast<float>(emitter.runtime.meshShapeVertices.size())),
            lastIndex);
        resolveVertex(emitter.runtime.meshShapeVertices[index], outPoint, outNormal);
        outNormal = outNormal.NormalizedOr(math::Vector3::UP);
    }

    // meshShapeScale は位置にだけ掛ける。法線は向きしか表さないので、
    // ここで掛けると負のスケール指定で «内向き» に反転してしまう。
    outPoint = outPoint * emitter.settings.meshShapeScale;
    return true;
}

// 同じ .mat の設定ミスを毎フレーム記録するとログが埋まって他の警告が読めなくなる。
std::unordered_set<std::string> g_warnedParticleMaterials;

bool WarnParticleMaterialOnce(const std::string& path)
{
    return g_warnedParticleMaterials.insert(path).second;
}

// カスタムシェーダーが宣言した MaterialConstants (b2) 1 本ぶんの解決結果。
// 解決にはシェーダーのロードとリフレクションが要るので .mat 単位でキャッシュする。
// 値で持つのは下のキャッシュの要素としてしか存在しないから (unordered_map はノード単位で
// 確保するので rehash しても要素のアドレスは動かない)。
struct ParticleMaterialBinding {
    renderer::ShaderDescriptor                            descriptor;
    renderer::ResourceHandle<renderer::ConstantBufferTag> paramsCB;
    std::vector<uint8_t>                                  paramData;
    // paramsCB を確保したときのサイズ。シェーダーのホットリロードで MaterialConstants の
    // 大きさが変わったら作り直す必要がある (古い容量のまま書くと末尾が落ちる)。
    uint32_t                                              paramsCBSize = 0;
    // このパス呼び出しで既に値を適用したか。.mat の編集を絵へ出しつつ、
    // 同じ .mat を共有するエミッターぶん解決をやり直さないための通番。
    uint64_t                                              resolvedPass = 0;
    bool                                                  resolvedOk   = false;
};

std::unordered_map<std::string, ParticleMaterialBinding> g_particleMaterials;
// ExecuteParticlePass の呼び出し通番。0 は「未解決」を表すため 1 から始める。
uint64_t                                                 g_particlePassSerial = 0;

// .mat の [params] をカスタムシェーダーの MaterialConstants へ束縛し、b2 へ流す定数バッファを返す。
// 組み込みシェーダー (MaterialConstants を宣言しない) では無効ハンドルを返す。
//
// 束縛規則そのものは asset::MaterialParamBinding が持つ — メッシュ / UI / デカールと同じ経路。
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

    // 記述子は値で持つ。シェーダーはホットリロードで差し替わりうるので、
    // ポインタで持つと解決時の中身と食い違う瞬間ができる。
    binding.descriptor = {};
    if (auto* compiled = resources.Get(shader))
        binding.descriptor = compiled->GetDescriptor();
    // MaterialConstants を宣言していないシェーダーは cbufferSize が 0 (= IsValid() が false)。
    // その場合 b2 は何も束縛しない — 組み込み Particle.hlsl がこれに当たる。
    if (!binding.descriptor.IsValid()) return {};

    binding.paramData.assign(binding.descriptor.cbufferSize, uint8_t{ 0 });
    asset::InitDefaultMaterialParams(binding.descriptor, binding.paramData);
    asset::ApplyMaterialAssetParams(material, binding.descriptor, binding.paramData);

    // WHY IsValid() で足りないか: このキャッシュはシーンの寿命もデバイスリセットも跨ぐ。
    //     ハンドルの体裁は残るので、実体が居るかどうかで «作り直し» を判断する。
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

// .mat の blend_mode をパーティクルの合成モードへ。パーティクルは半透明前提で PSO が
// 3 種類しか無いので、Opaque が来たら加算へ倒す («板が並ぶ» より気付きやすい)。
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
    // WHY: Particle は色カーブだけでも成立する VFX なので、参照先テクスチャの欠落で
    //      DrawCall 全体を無効化せず、白テクスチャにフォールバックして色だけは表示する。
    if (!texturePath.empty()) {
        auto texture = resources.LoadTexture(texturePath);
        if (texture.IsValid())
            return texture;
    }

    // WHY 共有の 1 枚か: ここで作ると «誰も返さない 1x1» がエミッターの数だけ残る。
    //     エミッターは Despawn / Stop で畳まれてもテクスチャを返さない (LoadTexture 由来の
    //     ハンドルを返すと共有キャッシュを壊すため) ので、作った枚数がそのまま漏れる。
    return resources.GetWhiteTexture();
}


// .mat の [textures] から 1 スロット引く。未設定なら空文字列。
const std::string& ParticleMaterialTexture(const asset::MaterialAsset& material, const char* slot)
{
    static const std::string kEmpty;
    const auto it = material.textures.find(slot);
    return (it != material.textures.end()) ? it->second : kEmpty;
}

// 陰影の受け口。b3 (平行光・環境光) と 6-way の Negative (t3) は常に、点光源は .mat が求めたときだけ。
// 点光源は DeferredPasses の Lighting と同じ束縛: b9 + t29/t30 (Legacy では束縛しない。b9 が未束縛だと
// clusterLightMode = Legacy として読まれ、b3 の固定長配列へ落ちる) と、影・Cookie の b12 + t28/t31。
void BindParticleLighting(renderer::DrawCall& dc, const ParticleEmitter& emitter, RenderPassContext& ctx)
{
    auto& h = ctx.handles;
    auto& resources = ctx.resources;
    dc.constantBuffers[3] = h.lightCB;
    dc.textures[3]        = emitter.runtime.sixWayNegativeTexture;
    // b8 + t16: 空の照度 (IBL)。iblIntensity が 0 なら PS は読まずに ambientColor を使う。
    dc.constantBuffers[8] = h.advancedGraphicsCB;
    dc.textures[16]       = h.iblIrradiance;
    // b13 + t23: フロクセル霧。粒子の奥行きの霧を逆算するのに使う (無効なら froxelGridZ = 0 で素通り)。
    if (h.froxelFogCB.IsValid()) {
        dc.constantBuffers[13] = h.froxelFogCB;
        if (h.froxelIntegrated.IsValid()) dc.textures[23] = h.froxelIntegrated;
    }
    if (!emitter.runtime.material.punctualLighting) return;
    if (ctx.clusterLightMode != ClusterLightMode::Legacy) {
        dc.constantBuffers[9] = h.clusterCB;
        dc.psBuffers[0]       = h.punctualLightBuffer;  // t29
        if (ctx.clusterLightMode == ClusterLightMode::Clustered)
            dc.psBuffers[1]   = h.clusterIndexBuffer;   // t30
    }
    if (h.punctualShadowCB.IsValid()) {
        dc.constantBuffers[12] = h.punctualShadowCB;
        dc.textures[28]        = resources.GetDepthTexture(ctx.Res().Target("PunctualShadowMap"));
        dc.textures[31]        = resources.GetColorTexture(ctx.Res().Target("LightCookieAtlas"), 0);
    }
}

void EnsureParticleTexture(ParticleEmitter& emitter, renderer::ResourceManager& resources)
{
    // 見た目のテクスチャは 3 枚とも .mat の [textures] から来る。
    //   albedo = 素材 / normal = 歪みベクトル専用マップ / tex5 = Motion Vector アトラス
    // 未設定なら既定 .mat へ落とす。1x1 白は alpha=1 なので、そのまま描くと粒子が
    // 「不透明な四角」になり、素材の付け忘れが最も分かりにくい形で表に出る。
    // 既定 .mat が無いプロジェクトでは Load<MaterialAsset> が失敗し、従来どおり白へ落ちる。
    const bool usingFallback = emitter.settings.materialPath.empty();
    const std::string resolvedMaterial =
        usingFallback ? std::string(PARTICLE_FALLBACK_MATERIAL) : emitter.settings.materialPath;
    {
        const bool matChanged = (emitter.runtime.loadedMaterialPath != resolvedMaterial);
        if (matChanged) {
            emitter.runtime.loadedMaterialPath = resolvedMaterial;
            emitter.runtime.loadedTexturePath.clear(); // .mat の変更でテクスチャも再ロードさせる
        }
        const auto matHandle = asset::AssetManager::Load<asset::MaterialAsset>(resolvedMaterial);
        if (const auto* mat = asset::AssetManager::Get<asset::MaterialAsset>(matHandle)) {
            const std::string& resolvedTex = ParticleMaterialTexture(*mat, "albedo");
            if (!emitter.runtime.texture.IsValid() || emitter.runtime.loadedTexturePath != resolvedTex) {
                emitter.runtime.texture = LoadParticleTextureOrWhite(resources, resolvedTex);
                emitter.runtime.loadedTexturePath = resolvedTex;
                emitter.runtime.textureIsSrgb = IsEffectTextureSrgb(resolvedTex);
            }
            // 歪みベクトル専用マップ (normal)。未設定なら無効ハンドルのままにして、
            // シェーダー側は effectsFlags を見て albedo の RG へ縮退する。
            const std::string& distortionTex = ParticleMaterialTexture(*mat, "normal");
            if (emitter.runtime.loadedDistortionTexturePath != distortionTex) {
                emitter.runtime.distortionTexture = distortionTex.empty()
                    ? renderer::ResourceHandle<renderer::TextureTag>::Null()
                    : LoadParticleTextureOrWhite(resources, distortionTex);
                emitter.runtime.loadedDistortionTexturePath = distortionTex;
            }
            // Motion Vector アトラス (tex5)。
            const std::string& motionTex = ParticleMaterialTexture(*mat, "tex5");
            if (emitter.runtime.loadedMotionVectorTexturePath != motionTex) {
                emitter.runtime.motionVectorTexture = motionTex.empty()
                    ? renderer::ResourceHandle<renderer::TextureTag>::Null()
                    : LoadParticleTextureOrWhite(resources, motionTex);
                emitter.runtime.loadedMotionVectorTexturePath = motionTex;
            }
            // 6 方向ライトマップの Negative (emissive)。sixWayMaps のときだけ読む。
            static const std::string kNoTexture;
            const std::string& sixWayTex = mat->particle.sixWayMaps ? ParticleMaterialTexture(*mat, "emissive")
                                                                    : kNoTexture;
            if (emitter.runtime.loadedSixWayNegativeTexturePath != sixWayTex) {
                emitter.runtime.sixWayNegativeTexture = sixWayTex.empty()
                    ? renderer::ResourceHandle<renderer::TextureTag>::Null()
                    : LoadParticleTextureOrWhite(resources, sixWayTex);
                emitter.runtime.loadedSixWayNegativeTexturePath = sixWayTex;
            }
            // .mat の [params] albedo を色調整として引き継ぐ。
            // オーサリング値は sRGB なので、他の色と同じくリニアへ揃えて渡す。
            math::Vector4 tint{ 1.0f, 1.0f, 1.0f, 1.0f };
            if (const auto param = mat->params.find("albedo"); param != mat->params.end()) {
                const auto& values = param->second;
                // float / float2 / float3 / float4 が同じ形式で入る。足りない成分は既定のまま。
                if (values.size() > 0) tint.x = values[0];
                if (values.size() > 1) tint.y = values[1];
                if (values.size() > 2) tint.z = values[2];
                if (values.size() > 3) tint.w = values[3];
            }
            emitter.runtime.materialTint = ParticleSrgbToLinear(tint);

            // .mat が shader を指定していれば描画シェーダーを差し替える。
            // 既定 .mat では見ない — 未設定にした瞬間に描画が化けるのは避ける (blendMode と同じ)。
            // render_path を要求するのは、頂点が ParticleVertex (92B)・b2 が
            // ParticleRenderConstants で、メッシュ用シェーダーを割り当てると
            // «クラッシュせず静かに壊れた絵» になるため。UI / Decal と同じ判断。
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
                // 黙って組み込みへ落ちると、テクスチャを持たない .mat では «白い四角» が並ぶ。
                // 手続きシェーダーほどテクスチャを持たないので、一番起きやすい失敗が
                // 一番分かりにくい絵になる。
                if (!shaderPath.empty() && !emitter.runtime.customShader.IsValid()) {
                    FBZZ_LOG_WARN("Particle material '%s' references shader '%s' but it failed to "
                                  "load (not compiled?). Falling back to the built-in particle "
                                  "shader; a material without an albedo texture will draw as "
                                  "white quads.",
                                  resolvedMaterial.c_str(), shaderPath.c_str());
                }
            }
            // カスタムシェーダーが MaterialConstants を宣言していれば .mat の [params] を流す。
            // 毎フレーム引き直すのは編集結果を即座に絵へ出すため (解決は .mat 単位で 1 回)。
            emitter.runtime.materialParamsCB = ResolveParticleMaterialParams(
                resources, resolvedMaterial, *mat, emitter.runtime.customShader);
            // 見た目一式を .mat から «読む»。settings はシーン保存対象なので書き戻さない
            // («触っていないのに保存内容が変わる» / «Inspector で変えても戻る» が起きる)。
            emitter.runtime.material      = mat->particle;
            emitter.runtime.resolvedBlend = ParticleBlendFromMaterial(mat->blendMode);
            // アトラス分割は 0 だと UV 矩形が発散する。.mat は手書きできるのでここで丸める。
            emitter.runtime.material.flipbook.spriteColumns =
                (std::max)(emitter.runtime.material.flipbook.spriteColumns, 1);
            emitter.runtime.material.flipbook.spriteRows =
                (std::max)(emitter.runtime.material.flipbook.spriteRows, 1);
            return;
        }
    }

    // .mat を解決できなかった。1x1 白 + 既定の見た目で描き続ける
    // (エフェクトが丸ごと消えるより、素材が付いていないと分かる方がよい)。
    emitter.runtime.material      = asset::ParticleMaterialSettings{};
    emitter.runtime.resolvedBlend = ParticleBlendMode::Additive;
    if (!emitter.runtime.texture.IsValid()) {
        emitter.runtime.texture = LoadParticleTextureOrWhite(resources, {});
        emitter.runtime.loadedTexturePath.clear();
        // 1x1 白フォールバック。リニアでも sRGB でも 1.0 は 1.0 なので変換しない。
        emitter.runtime.textureIsSrgb = false;
    }
}

// GPU ソートを実際に走らせるか。判定材料はシェーダーの有無だけ。
// 「バッファが確保済みか」で見ると renderCB を作る時点と実行する時点で答えが食い違い、
// 初回フレームだけソート無効の絵が出る。
// 速度場をアトラスへ常駐させ、タイル番号を返す。常駐できなければ -1。
int ResolveVelocityFieldTile(const asset::VectorFieldAsset& field,
                             renderer::ResourceManager& resources)
{
    return asset::VelocityFieldAtlas::Acquire(field, resources);
}

// GPU シミュレーションへ渡す力場とスポーンの StructuredBuffer。エミッターに 1 本ずつ
// 持たせず、シミュレーションのたびにプールから借りる。
// WHY: DX12 の読み取り専用 StructuredBuffer は Upload ヒープへの直 memcpy。1 本を毎フレーム
//      書き直すと、GPU がまだ読んでいる前フレームのスポーン / 力場を上書きする
//      (詳細は DynamicBufferPool.hpp)。
renderer::DynamicStructuredBufferPool g_gpuForcePool;
renderer::DynamicStructuredBufferPool g_gpuSpawnPool;

// 力場バッファを借りて今フレームの内容を書き込む。
void AcquireGpuForceBuffer(ParticleEmitter& emitter,
                           const std::vector<GpuForceField>& forces,
                           renderer::ResourceManager& resources)
{
    // 0 本でも 1 要素は借りる。要素数 0 の StructuredBuffer は作れず、
    // 未束縛の SRV は DX12 で null ディスクリプタの次元不一致になる。
    const size_t required = (std::max)(forces.size(), size_t{ 1 });
    emitter.runtime.gpuForceBuffer =
        g_gpuForcePool.Acquire(resources, required, static_cast<uint32_t>(sizeof(GpuForceField)));
    if (!forces.empty() && emitter.runtime.gpuForceBuffer.IsValid()) {
        resources.Update(emitter.runtime.gpuForceBuffer, forces.data(),
                         forces.size() * sizeof(GpuForceField));
    }
}

bool ShouldSortGpuParticles(const ParticleEmitter& emitter, const RenderPassHandles& handles)
{
    return emitter.settings.sortMode != ParticleSortMode::None
        && handles.particleGpuSortKeysCS.IsValid()
        && handles.particleGpuSortStepCS.IsValid()
        && handles.particleGpuSortLocalCS.IsValid();
}

void UpdateParticleRenderConstants(ParticleEmitter& emitter,
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
    // ビット割り当ては Rendering/ParticleCommon.hlsli の FBZZ_PFX_* / FBZZ_PALPHA_* と一致させること。
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
        | ((static_cast<std::uint32_t>(emitter.runtime.material.alphaSource) & kParticleAlphaMask)
               << kParticleAlphaShift);
    cb.distortionStrength = (std::max)(emitter.runtime.material.distortionStrength, 0.0f);
    cb.distortionChromatic = (std::max)(emitter.runtime.material.distortionChromatic, 0.0f);
    cb.lightingStrength = (std::max)(emitter.runtime.material.lightingStrength, 0.0f);
    cb.smokeWrap = std::clamp(emitter.runtime.material.smokeWrap, 0.0f, 1.0f);
    cb.smokeTransmission = (std::max)(emitter.runtime.material.smokeTransmission, 0.0f);
    // 0 以下だと pow が発散する。シェーダー側でも下限を切るが、値の意味をここで固定する。
    cb.smokeBackScatterPower = (std::max)(emitter.runtime.material.smokeBackScatterPower, 0.1f);
    cb.tintColor = emitter.runtime.materialTint;
    cb.emissiveScale = (std::max)(emitter.runtime.material.emissiveScale, 0.0f);
    const math::Vector3& sixWayEmission = emitter.runtime.material.sixWayEmissionColor;
    cb.sixWayEmission = { (std::max)(sixWayEmission.x, 0.0f), (std::max)(sixWayEmission.y, 0.0f),
                          (std::max)(sixWayEmission.z, 0.0f), 0.0f };
    cb.motionVectorStrength = (std::max)(emitter.runtime.material.flipbook.motionVectorStrength, 0.0f);
    // 歪みの画面UV算出に使う。0 だとUVが右下隅へ張り付いて屈折が出ない。
    cb.screenWidth = static_cast<float>((std::max)(screenWidth, 1u));
    cb.screenHeight = static_cast<float>((std::max)(screenHeight, 1u));
    // 0 以下だと粒子が潰れて一切見えなくなるため、下限で切って「消える」事故を防ぐ。
    cb.sizeAxisScaleX = (std::max)(emitter.settings.sizeAxisScale.x, 0.0001f);
    cb.sizeAxisScaleY = (std::max)(emitter.settings.sizeAxisScale.y, 0.0001f);
    cb.shadowStrength = std::clamp(emitter.runtime.material.shadowStrength, 0.0f, 1.0f);
    // ステップ数はピクセルあたりのループ回数に直結する。上限を切って
    // 設定ミスで GPU が張り付くのを防ぐ。
    cb.volumetricSteps = static_cast<std::uint32_t>(std::clamp(emitter.runtime.material.volumetricSteps, 1, 64));
    cb.volumetricDensity = (std::max)(emitter.runtime.material.volumetricDensity, 0.0f);
    // g = ±1 は位相関数が発散するため内側へ寄せる。
    cb.volumetricAnisotropy = std::clamp(emitter.runtime.material.volumetricAnisotropy, -0.95f, 0.95f);
    cb.volumetricNoiseScale = (std::max)(emitter.runtime.material.volumetricNoiseScale, 0.0f);
    // GPU 経路の VS がソート済み index (t15) を経由するかどうか。CPU 経路では常に 0。
    cb.gpuSortEnabled = gpuSortEnabled ? 1u : 0u;
    // 自己影。密度バッファ (t9) が無いフレームでも 0 なら参照しないので安全。
    cb.selfShadowStrength = (std::max)(emitter.runtime.material.selfShadowStrength, 0.0f);
    // カメラ距離フェード。負値だけ潰す。near >= far (既定の 0 / 0 を含む) は
    // シェーダー側が «無効» として扱うので、ここで組み替えたり警告したりはしない。
    cb.cameraFadeNear = (std::max)(emitter.runtime.material.cameraFadeNear, 0.0f);
    cb.cameraFadeFar = (std::max)(emitter.runtime.material.cameraFadeFar, 0.0f);
    resources.Update(emitter.runtime.renderCB, &cb, sizeof(cb));
}

// 自己影用の密度 RT を、このパスで最初に使うときだけクリアして光源行列を流し込む。
// RT / CB の生成は RenderSystem 側 (静的リソース) が持つ ─ RenderPassHandles は毎フレーム
// 作り直される値型なので、ここで遅延生成すると RT を漏らす。
// 戻り値 false = 使えない (未生成 / シェーダー未ロード)。
bool PrepareParticleSelfShadowTarget(RenderPassContext& ctx, bool& inoutClearedThisPass)
{
    auto& resources = ctx.resources;
    auto& h = ctx.handles;
    if (!h.particleSelfShadowShader.IsValid()) return false;
    if (!h.particleSelfShadowRT.IsValid() || !h.particleSelfShadowFrameCB.IsValid()) return false;

    if (!inoutClearedThisPass) {
        ctx.renderer.SetRenderTarget(h.particleSelfShadowRT, resources);
        // 密度 0 でクリア。alpha=1 は積算へ影響しないが、他所で読み違えないよう明示する。
        ctx.renderer.Clear({ 0.0f, 0.0f, 0.0f, 1.0f });
        ctx.renderer.SetRenderTarget(ctx.Res().Target("HDR"), resources);

        // b0 を光源視点へ差し替える CB。VS が view の列 0/1 から右/上を取るので、
        // これだけでビルボードが光源へ正対する (シャドウマップと同じ扱いになる)。
        PerFrameCB lightFrame{};
        lightFrame.view = ctx.lightView;
        lightFrame.viewProjection = ctx.lightVP;
        lightFrame.cameraPos = ctx.lightEyePos;
        lightFrame.nearZ = 1.0f;
        lightFrame.farZ = 1000.0f;
        resources.Update(h.particleSelfShadowFrameCB, &lightFrame, sizeof(lightFrame));
        inoutClearedThisPass = true;
    }
    return true;
}

// ビルボード頂点をエミッター 1 個ぶんずつ貸し出すプール。
// 共有 1 本だと DX12 で成立しない。Submit はコマンドリストへの記録でしかなく GPU が
// 頂点を読むのはフレーム終端なので、2 個目の Update が 1 個目の Draw まで差し替える
// (詳細は DynamicBufferPool.hpp)。
renderer::DynamicVertexBufferPool g_particleVertexPool;

// このフレームに本番描画したエミッターの記録 (どのバッファへ何クワッド積んだか)。
// Overdraw 可視化は別パスなので、本番描画の結果を引き継がないと測る対象がずれる。
struct ParticleDrawRecord {
    const ParticleEmitter*                        emitter = nullptr;
    renderer::ResourceHandle<renderer::BufferTag> vertexBuffer;
    int                                           quadCount = 0;
};
std::vector<ParticleDrawRecord> g_particleDrawRecords;

// GPU シミュレーションの粒子の描画記録。頂点バッファを持たないので、描き直すには粒子数が要る。
struct GpuParticleDrawRecord {
    const ParticleEmitter* emitter = nullptr;
    int                    maxParticles = 0;
};
std::vector<GpuParticleDrawRecord> g_gpuParticleDrawRecords;

// 連続リボン (trailRibbon) の帯頂点用。帯はカメラへ正対するのでビューごとに形が変わり、
// エミッターに 1 本だと Scene View と Game View が同じバッファを 2 回書いてしまう。
renderer::DynamicVertexBufferPool g_trailRibbonVertexPool;

// 1 エミッターぶんの密度を光源側 RT へ積む。
// 呼ぶ位置が重要: 頂点バッファをアップロードした直後、本番描画の前
// (密度は「実際に描くのと同じ形」で測らないと意味がない)。
// 光源行列は専用 CB から渡す。h.frameCB を書き換えると後続の全パスへ漏れる。
// 自己影に含まれるのは「自分自身 + 先に処理されたエミッター」まで。単一エミッターの
// 煙・雲では完全に正しく、重ねた場合だけ順序依存が残る (完全にすると粒子数ぶんの
// メモリを二重に持つことになるのでこの近似を採る)。
void AccumulateParticleSelfShadowDensity(const ParticleEmitter& emitter, int quadCount,
                                         renderer::ResourceHandle<renderer::BufferTag> vertexBuffer,
                                         RenderPassContext& ctx)
{
    auto& resources = ctx.resources;
    auto& renderer = ctx.renderer;
    auto& h = ctx.handles;
    if (quadCount <= 0 || !emitter.runtime.texture.IsValid()
        || !vertexBuffer.IsValid() || !h.particleIB.IsValid() || !h.particlePSO.IsValid())
        return;

    renderer.SetRenderTarget(h.particleSelfShadowRT, resources);
    renderer::DrawCall dc;
    dc.vertexBuffer = vertexBuffer;
    dc.indexBuffer  = h.particleIB;
    dc.indexCount   = static_cast<uint32_t>(quadCount * 6);
    dc.shader       = h.particleSelfShadowShader;
    // ADDITIVE + DEPTH_READ。光源側 RT に深度は無いので比較も書き込みも起きない。
    dc.pipelineState = h.particlePSO;
    dc.constantBuffers[0] = h.particleSelfShadowFrameCB;
    dc.constantBuffers[kParticleConstantSlot] = emitter.runtime.renderCB;
    dc.textures[0]  = emitter.runtime.texture;
    renderer.Submit(dc, resources);
    // 本番描画へ戻す。呼び出し側が続けて HDR RT へ描くため、ここで必ず張り直す。
    renderer.SetRenderTarget(ctx.Res().Target("HDR"), resources);
}

// 1 エミッターぶんの per-particle Trail を、連続した帯 (リボン) として描く。
// ビルボードを履歴点へ並べる方式は太くすると粒の連なりが露見するので、履歴点を
// ポリラインとみなして Trail ノードと同じマイター接合で帯を張る。
// 色は帯の長さ方向へ colorStart → colorEnd を配る (Trail.hlsl の age)。粒子ごとの
// 色ゆらぎは 1 DrawCall へまとめる都合で乗らない (要るならビルボード方式を使う)。
void DrawParticleTrailRibbons(ParticleEmitter& emitter, const Transform& tf,
                              int particleCount, RenderPassContext& ctx)
{
    auto& resources = ctx.resources;
    auto& h = ctx.handles;
    if (!h.trailShader.IsValid() || !h.trailPSO.IsValid() || !emitter.runtime.texture.IsValid()) return;

    const int historyPoints = std::clamp(emitter.settings.trail.trailPointCount, 1, kMaxParticleTrailPoints);
    // 帯 1 本 = 本体 + 履歴点。線分は historyPoints 本で、各線分が 6 頂点。
    const int segmentsPerParticle = historyPoints;
    // 帯は粒子 1 つあたり数十頂点になる。上限を切らないと、粒子数を上げた瞬間に
    // 頂点バッファが数十 MB へ膨れる。切った先は「尾が付かない粒子」として静かに落とす。
    constexpr int kMaxRibbonParticles = 2048;
    const int ribbonParticles = (std::min)(particleCount, kMaxRibbonParticles);
    if (ribbonParticles <= 0 || segmentsPerParticle <= 0) return;

    const std::uint32_t neededVertices =
        static_cast<std::uint32_t>(ribbonParticles) * static_cast<std::uint32_t>(segmentsPerParticle) * 6u;
    if (!emitter.runtime.trailRibbonCB.IsValid())
        emitter.runtime.trailRibbonCB = resources.CreateConstantBuffer(sizeof(TrailCB));
    if (!emitter.runtime.trailRibbonCB.IsValid()) return;

    const bool localSpace = emitter.settings.simulationSpace == ParticleSimulationSpace::Local;
    const math::Vector3 cameraPos = ctx.camera.m_position;

    static std::vector<TrailVertex> vertices;
    vertices.clear();
    vertices.reserve(neededVertices);
    // 履歴点を毎回組み直すためのスクラッチ。粒子ごとに確保し直さない。
    static std::vector<math::Vector3> polyline;
    static std::vector<math::Vector3> normals;

    for (int index = 0; index < ribbonParticles; ++index) {
        const Particle& particle = emitter.runtime.particles[static_cast<std::size_t>(index)];
        const int used = (std::min)(static_cast<int>(particle.trailCount), historyPoints);
        // 線分を張るには最低 2 点要る。履歴が溜まる前の粒子は帯を持たない。
        if (used < 1) continue;

        polyline.clear();
        polyline.push_back(localSpace ? TransformEmitterPoint(tf, particle.position) : particle.position);
        for (int point = 0; point < used; ++point) {
            const math::Vector3& raw = particle.trailPoints[static_cast<std::size_t>(point)];
            polyline.push_back(localSpace ? TransformEmitterPoint(tf, raw) : raw);
        }
        if (polyline.size() < 2) continue;

        // 各点の幅方向。マイター接合そのものは Trail ノードと共通で、粒子リボンは
        // 幅方向を常にカメラ正対で決める。
        BuildRibbonMiterNormals(polyline,
            [&](const math::Vector3& direction, const math::Vector3& point) {
                return ComputeCameraFacingRibbonNormal(direction, cameraPos, point);
            },
            normals);

        // 幅。trailRibbonWidth が 0 以下なら粒子サイズを流用する。
        const float baseWidth = emitter.settings.trail.trailRibbonWidth > 0.0f
            ? emitter.settings.trail.trailRibbonWidth : particle.size;
        const float tailDenominator = static_cast<float>(polyline.size() - 1u);
        for (std::size_t segment = 0; segment + 1u < polyline.size(); ++segment) {
            // age は 1 = 粒子本体側 (新しい) / 0 = 尾の先端 (古い)。Trail.hlsl が
            // colorEnd → colorStart の補間に使う。
            const float age0 = 1.0f - static_cast<float>(segment) / tailDenominator;
            const float age1 = 1.0f - static_cast<float>(segment + 1u) / tailDenominator;
            const float half0 = baseWidth
                * math::Lerp(emitter.settings.trail.trailWidthScale, 1.0f, age0) * 0.5f;
            const float half1 = baseWidth
                * math::Lerp(emitter.settings.trail.trailWidthScale, 1.0f, age1) * 0.5f;
            const float u0 = static_cast<float>(segment) / tailDenominator;
            const float u1 = static_cast<float>(segment + 1u) / tailDenominator;

            const TrailVertex topLeft{ polyline[segment] + normals[segment] * half0, age0, 0.0f, u0 };
            const TrailVertex bottomLeft{ polyline[segment] - normals[segment] * half0, age0, 1.0f, u0 };
            const TrailVertex topRight{ polyline[segment + 1u] + normals[segment + 1u] * half1, age1, 0.0f, u1 };
            const TrailVertex bottomRight{ polyline[segment + 1u] - normals[segment + 1u] * half1, age1, 1.0f, u1 };
            vertices.insert(vertices.end(),
                { topLeft, topRight, bottomLeft, bottomLeft, topRight, bottomRight });
        }
    }
    if (vertices.empty()) return;

    TrailCB cb{};
    // 帯の根元 (粒子本体側) は本体と同じ色、先端は tint とフェードを掛けた色。
    // 色調整はオーサリング空間で掛けてから一度だけリニアへ落とす (ビルボードと同じ順序)。
    cb.colorStart = ParticleSrgbToLinear(emitter.settings.colorStart);
    cb.colorEnd = ParticleSrgbToLinear({
        emitter.settings.colorEnd.x * emitter.settings.trail.trailColorTint.x,
        emitter.settings.colorEnd.y * emitter.settings.trail.trailColorTint.y,
        emitter.settings.colorEnd.z * emitter.settings.trail.trailColorTint.z,
        emitter.settings.colorEnd.w * emitter.settings.trail.trailColorTint.w * emitter.settings.trail.trailAlphaScale,
    });
    cb.uvTiling = 1.0f;
    cb.flags = emitter.runtime.textureIsSrgb ? kTrailFlagSrgbTexture : 0u;
    const auto ribbonVB = g_trailRibbonVertexPool.Acquire(
        resources, vertices.size(), static_cast<std::uint32_t>(sizeof(TrailVertex)));
    if (!ribbonVB.IsValid()) return;
    resources.Update(emitter.runtime.trailRibbonCB, &cb, sizeof(cb));
    resources.Update(ribbonVB, vertices.data(), vertices.size() * sizeof(TrailVertex));

    renderer::DrawCall dc;
    dc.vertexBuffer = ribbonVB;
    dc.vertexCount  = static_cast<uint32_t>(vertices.size());
    dc.shader       = h.trailShader;
    dc.pipelineState = h.trailPSO;
    dc.layer        = renderer::RenderLayer::TRANSPARENT_LAYER;
    dc.constantBuffers[0] = h.frameCB;
    dc.constantBuffers[2] = emitter.runtime.trailRibbonCB;
    dc.textures[0]  = emitter.runtime.texture;
    SubmitCounted(ctx, dc);
}

// blendMode から PSO を選ぶ。CPU/GPU 双方の描画経路で同じ判定を使う
// (散らすと「CPU では正しいが GPU では加算のまま」という差が生まれる)。
// distortion は背景色を差し替えるので、加算では画が破綻する。アルファへ倒す。
renderer::ResourceHandle<renderer::PipelineStateTag> SelectParticlePSO(
    const ParticleEmitter& emitter,
    renderer::ResourceHandle<renderer::PipelineStateTag> additivePSO,
    renderer::ResourceHandle<renderer::PipelineStateTag> alphaPSO,
    renderer::ResourceHandle<renderer::PipelineStateTag> premultipliedPSO)
{
    if (emitter.runtime.material.distortion) return alphaPSO;
    switch (emitter.runtime.resolvedBlend) {
    case ParticleBlendMode::Alpha:         return alphaPSO;
    case ParticleBlendMode::Premultiplied: return premultipliedPSO;
    case ParticleBlendMode::Additive:
    default:                               return additivePSO;
    }
}

// scopeRoot 以下を名前で深さ優先探索する。VFX Graph の生成物は同名が複数存在しうるため、
// 「自分と同じエフェクトに属する Emitter」だけを候補にするための限定探索。
GameObject* FindInSubtree(GameObject& root, const std::string& objectName)
{
    if (root.name == objectName) return &root;
    for (int index = 0; index < root.GetChildCount(); ++index)
        if (GameObject* child = root.GetChild(index))
            if (GameObject* found = FindInSubtree(*child, objectName)) return found;
    return nullptr;
}

// SubEmitter へイベント数分のスポーンを積む。参照切れは VFX の縮退として無視する。
// subEmitterScopeRoot が有効ならその GameObject 配下だけを名前で探す (VFXSystem が配る)。
// .vfx の中の GO は同じ名前を名乗るので、シーン全体で引くと隣のインスタンスを掴む。
//
// origin / velocity は «発火元の粒子» のワールド位置と速度。これを渡さないと受け側は
// 自分の emitPosition からしか湧けず、«斬った位置で火花» / «消えた場所から煙» が作れない。
void QueueSubEmitter(Scene& scene, const ParticleEmitter& emitter,
                     const std::string& objectName, int count,
                     const math::Vector3& origin, const math::Vector3& velocity)
{
    if (objectName.empty() || count <= 0) return;
    GameObject* target = nullptr;
    if (GameObject* scopeRoot = scene.GetGameObject(emitter.settings.subEmitterScopeRoot))
        target = FindInSubtree(*scopeRoot, objectName);
    else
        target = scene.Find(objectName); // シーン上の手置き Emitter は従来どおり全体から引く
    if (target == nullptr) return;
    auto* targetEmitter = target->GetComponent<ParticleEmitter>();
    if (targetEmitter == nullptr) return;
    // 受け側が一度も回らない構成 (GPU 縮退・非アクティブ・maxParticles = 0) では
    // 消費されないまま積まれ続ける。1 フレームで出せる上限を超えたら黙って捨てる
    // (出せない数を覚えておいても絵には出ず、リストだけが伸びる)。
    if (static_cast<int>(targetEmitter->runtime.injectedSpawns.size())
        >= (std::max)(targetEmitter->settings.maxParticles, 1))
        return;
    targetEmitter->runtime.injectedSpawns.push_back({ origin, velocity, count });
}

// Particle 1個のPhysics/Plane衝突を解決し、Kill応答ならtrueを返す。
// filter は Physics 衝突のレイヤー/トリガー絞り込み。粒子ごとに組むと std::function の
// 生成がループ内に入るので、呼び出し側 (エミッター単位) が 1 本作って回す。
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
    // 発火元は «当たった点» と «跳ね返る前の速度»。跳ね返り後を渡すと、火花が壁から
    // 離れる向きに寄って «削れて飛んだ» ように見えない。
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

// injected: SubEmitter が渡してきた «発火元» (無ければ nullptr)。位置は発生原点として
// 据え、速度は subEmitterInheritVelocity の割合だけ初速へ足す。
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
    // 発火元の位置を発生原点へ据える。«置き換え» を Shape より前に済ませるのは、
    // Shape のばらつきをこの点の周りに乗せるため (後で上書きすると 1 点から出る糸になる)。
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

    // 初速のばらつきは 3 軸へ等方に掛ける。1 軸でも欠けると、その向きへ出すエミッターで
    // 分散がゼロになり全粒子が同じ速度で進む硬い前線になる。
    // 方向そのものは Sphere/Cone の shapeVelocity が持つので、ここは揺らぎだけを担当する。
    // GPU 経路 (BuildGpuSpawnEntry) と必ず同じ式・同じ乱数消費順にすること。
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
    // エミッターの移動を初速へ引き継ぐ (移動する剣・ロケットの火花が置き去りにならない)。
    // Local では親の移動が描画時の変換で既に乗るので、World のときだけ加算する。
    if (emitter.settings.inheritVelocity != 0.0f
        && emitter.settings.simulationSpace == ParticleSimulationSpace::World) {
        p.velocity = p.velocity + emitter.runtime.emitterVelocity * Clamp01(emitter.settings.inheritVelocity);
    }
    // 発火元の粒子の速度は «割合» で継ぐ。丸ごと継ぐと親と同じ向きへ流れるだけになり、
    // 火花が «弾けた» ように見えない (既定 0 = 継がない)。
    if (injected != nullptr && emitter.settings.subEmitterInheritVelocity != 0.0f) {
        const math::Vector3 inherited =
            injected->velocity * Clamp01(emitter.settings.subEmitterInheritVelocity);
        p.velocity = p.velocity
            + (localSpace ? InverseTransformEmitterVector(transform, inherited) : inherited);
    }
    p.color = emitter.settings.colorStart;
    p.size  = emitter.settings.sizeStart;
    p.age   = 0.0f;
    p.rotation = Random01(emitter) * 3.14159265358979323846f * 2.0f;
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
        // Prewarmは開始時点の寿命分布を作る。逐次更新を避け、重力下の解析解で初期状態を近似する。
        // 近似に使うのは内蔵の Wind 力 (＝重力) だけ。渦や速度場まで解析解にはできないので、
        // それらが主役のエミッターでは prewarm の分布が実際の流れとずれる。
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

// CPU の SpawnParticle と同じ Shape/Spread ロジックで GpuSpawnEntry を初期化する
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

    // SpawnParticle と同一の 3 軸等方ジッター。乱数の消費順まで揃えないと
    // CPU/GPU を切り替えただけで見た目が変わってしまう。
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
    // inheritVelocity は CPU 経路 (SpawnParticle) と同じ条件・同じ式でここに足す。
    // CS 側の変更は不要で、GpuSpawnEntry.velocity に加算済みの値が入る。
    if (emitter.settings.inheritVelocity != 0.0f
        && emitter.settings.simulationSpace == ParticleSimulationSpace::World) {
        s.velocity = s.velocity + emitter.runtime.emitterVelocity * Clamp01(emitter.settings.inheritVelocity);
    }
    s.lifetime        = (std::max)(0.001f, emitter.settings.lifetime
        * (1.0f + RandomSigned01(emitter) * Clamp01(emitter.settings.lifetimeRandom)));
    s.size            = emitter.settings.sizeStart;
    s.colorStart      = emitter.settings.colorStart;
    // 乱数列上の位置は従来の ApplyColorVariation 呼び出しと同じに保つ
    // (動かすと同じ seed から出る粒子の並びが変わる)。
    const math::Vector3 variation = NextColorVariation(emitter);
    s.colorScale      = { variation.x, variation.y, variation.z, 0.0f };
    s.spriteSeed      = Random01(emitter);
    s.uvRect          = ComputeSpriteRect(emitter, 0.0f, 0.0f, s.spriteSeed);
    s.rotation        = Random01(emitter) * 6.28318530717958647692f;
    s.angularVelocity = emitter.settings.angularVelocityMin
        + (emitter.settings.angularVelocityMax - emitter.settings.angularVelocityMin) * Random01(emitter);
}

// GPU パーティクル: バッファ初期化・スポーン・CS Dispatch・DrawInstanced
void TickGpuEmitter(ParticleEmitter&                     emitter,
                    const Transform&                     tf,
                    const AnimatorComponent*             animator,
                    float                                dt,
                    float                                time,
                    bool                                 canEmit,
                    const std::vector<ActiveForceField>& forceFields,
                    renderer::ResourceHandle<renderer::TextureTag> sceneColor,
                    const MeshRenderer*                  meshParticleRenderer,
                    RenderPassContext&                   ctx)
{
    auto& resources = ctx.resources;
    auto& renderer  = ctx.renderer;
    auto& h         = ctx.handles;

    if (!h.particleGpuSimCS.IsValid() || !h.particleGpuShader.IsValid()) return;

    const int maxP = (std::max)(emitter.settings.maxParticles, 1);

    // Clear要求・容量変更時は全スロットを死亡状態で再生成する。
    // WHY: RWStructuredBufferはCPU vectorのclearでは消えず、容量増加後のDispatchは範囲外アクセスになるため。
    if (emitter.runtime.gpuClearPending || (emitter.runtime.gpuInitialized && emitter.runtime.gpuCapacity != static_cast<uint32_t>(maxP))) {
        emitter.runtime.gpuInitialized = false;
        emitter.runtime.gpuParticleBuffer = {};
        emitter.runtime.gpuSpawnBuffer = {};
        emitter.runtime.gpuEmitterCB = {};
        emitter.runtime.gpuWriteHead = 0;
        emitter.runtime.gpuSpawnCount = 0;
        emitter.runtime.gpuCapacity = 0;
        emitter.runtime.gpuClearPending = false;
        // ソート表は粒子プールの index を持つので、プールを作り直したら必ず捨てる。
        emitter.runtime.gpuSortBuffer = {};
        emitter.runtime.gpuSortCB = {};
        emitter.runtime.gpuSortCapacity = 0;
        emitter.runtime.gpuForceBuffer = {};
    }

    // デバイスリセット (Play Mode 移行など) 後は古いハンドルが無効になるため再初期化する
    const uint64_t currentResetVersion = resources.GetResetVersion();
    if (emitter.runtime.gpuInitialized && emitter.runtime.gpuResetVersion != currentResetVersion)
    {
        emitter.runtime.gpuInitialized  = false;
        emitter.runtime.gpuParticleBuffer = {};
        emitter.runtime.gpuSpawnBuffer    = {};
        emitter.runtime.gpuEmitterCB      = {};
        emitter.runtime.gpuWriteHead      = 0;
        emitter.runtime.gpuSpawnCount     = 0;
        // デバイスリセット後は古いハンドルが全て無効。ソート表も作り直す。
        emitter.runtime.gpuSortBuffer     = {};
        emitter.runtime.gpuSortCB         = {};
        emitter.runtime.gpuSortCapacity   = 0;
        emitter.runtime.gpuForceBuffer     = {};
    }

    // バッファ未作成なら初期化 (要素ゼロで確保し CS が age>=lifetime で無視する)
    if (!emitter.runtime.gpuInitialized)
    {
        std::vector<GpuParticle> init(static_cast<size_t>(maxP));
        for (auto& p : init) p.age = p.lifetime = 1.0f; // 全粒子を「死亡済み」で初期化
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
    // SubEmitter からの «発火元つき» スポーンは GPU 経路では出せない (InitGpuSpawnEntry は
    // 位置と速度の上書きを持たない)。捨てないと、GPU のエミッターを SubEmitter に指した
    // 構成で古い発火位置が溜まり続ける。
    emitter.runtime.injectedSpawns.clear();
    // 今フレームのスポーンエントリを構築
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
            // lodRateScale は CPU 経路と同じく発生レートへ掛ける。掛け忘れると
            // 遠距離のエミッターが GPU のときだけ間引かれない。
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

    emitter.runtime.gpuSpawnCount = static_cast<uint32_t>(spawns.size());

    // スポーンバッファを借りて CPU → GPU 転送する。0 件でも 1 要素は借りる (t15 を空にしない)。
    emitter.runtime.gpuSpawnBuffer = g_gpuSpawnPool.Acquire(
        resources, (std::max)(spawns.size(), size_t{ 1 }), static_cast<uint32_t>(sizeof(GpuSpawnEntry)));
    if (!emitter.runtime.gpuSpawnBuffer.IsValid())
        emitter.runtime.gpuSpawnCount = 0;
    if (emitter.runtime.gpuSpawnCount > 0)
        resources.Update(emitter.runtime.gpuSpawnBuffer, spawns.data(),
                         emitter.runtime.gpuSpawnCount * sizeof(GpuSpawnEntry));

    // CS 用定数バッファ更新
    GpuParticleEmitterCB cb{};
    cb.emitterPos      = TransformEmitterPoint(tf, emitter.settings.emitPosition);
    cb.deltaTime       = dt;
    // 重力・空気抵抗・乱流・周回・放射は専用スロットではなく gForceFields へ入る。
    // HLSL 側はこれらのスロットを今も読むので、0 を入れて «何もしない» にしておく
    // (定数バッファのレイアウトは末尾追加のみが規約で、途中を削ると全オフセットがずれる)。
    cb.gravity         = math::Vector3::ZERO;
    cb.maxParticles    = static_cast<uint32_t>(maxP);
    cb.colorStart      = emitter.settings.colorStart;
    cb.colorEnd        = emitter.settings.colorEnd;
    cb.spawnCount      = emitter.runtime.gpuSpawnCount;
    cb.spawnOffset     = emitter.runtime.gpuWriteHead;
    cb.colorCurvePower = emitter.settings.colorCurvePower;
    cb.velocityDamping = 0.0f; // Drag は gForceFields 側へ
    cb.sizeStart       = emitter.settings.sizeStart;
    cb.sizeEnd         = emitter.settings.sizeEnd;
    cb.sizeCurvePower  = emitter.settings.sizeCurvePower;
    {
        // 丸めは CPU 経路・Inspector プレビューと同じ関数で行う (end = 0 を «最後まで» と読む等)。
        const asset::FlipbookFrameRange range =
            asset::ResolveFlipbookRange(emitter.runtime.material.flipbook);
        cb.spriteColumns    = static_cast<uint32_t>(range.columns);
        cb.spriteRows       = static_cast<uint32_t>(range.rows);
        cb.spriteStartFrame = static_cast<uint32_t>(range.first);
        cb.spriteEndFrame   = static_cast<uint32_t>(range.last);
    }
    cb.time           = time;
    cb.noiseStrength  = 0.0f; // Turbulence は gForceFields 側へ
    cb.noiseFrequency = 0.5f;
    cb.noiseSpeed     = 1.0f;
    cb.flipbookMode = static_cast<uint32_t>(emitter.runtime.material.flipbook.flipbookMode);
    cb.flipbookFramesPerSecond = (std::max)(emitter.runtime.material.flipbook.flipbookFramesPerSecond, 0.0f);

    // 内蔵の力もシーンの力場も同じ StructuredBuffer へ詰める。CS 側は届いた分を全部
    // 適用すればよい。本数の上限は無いので «重力が捨てられて粒子が浮く» は起きない。
    std::vector<ActiveForceField> emitterForces;
    ResolveEmitterForces(emitter, tf, forceFields, emitterForces);
    std::vector<GpuForceField> gpuForces;
    gpuForces.reserve(emitterForces.size());
    for (const ActiveForceField& f : emitterForces) {
        GpuForceField gf{};
        gf.posRadius   = { f.position.x, f.position.y, f.position.z, f.radius };
        gf.dirStrength = { f.direction.x, f.direction.y, f.direction.z, f.strength };
        // params.z は Turbulence では noiseFrequency、Drag では «内蔵の力か» の印。
        // WHY 兼用するか: Drag は乱流のパラメーターを使わない。1 枠を空けて足すより、
        //     使われない枠に意味を持たせるほうが構造体が小さく保てる。
        const bool isDrag = f.type == ForceFieldType::Drag;
        const float paramZ = isDrag ? (f.local ? 1.0f : 0.0f) : f.noiseFrequency;
        gf.params = { static_cast<float>(f.type), f.falloffPower, paramZ, f.noiseSpeed };

        // 速度場はアトラスのタイル番号で指す。常駐していない場は tile < 0 で «無効»
        // として送り、CS 側は何もしない (送らないと本数がずれて別の力に化ける)。
        gf.fieldTile = { -1.0f, 1.0f, 0.0f, 0.0f };
        if (f.type == ForceFieldType::VectorField && f.vectorField != nullptr) {
            const int tile = ResolveVelocityFieldTile(*f.vectorField, ctx.resources);
            gf.fieldRotation = { f.inverseRotation.x, f.inverseRotation.y,
                                 f.inverseRotation.z, f.inverseRotation.w };
            gf.fieldExtents  = { f.extents.x, f.extents.y, f.extents.z, f.tightness };
            gf.fieldTile     = { static_cast<float>(tile), f.vectorField->maxMagnitude, 0.0f, 0.0f };
        }
        gpuForces.push_back(gf);
    }
    cb.forceFieldCount = static_cast<uint32_t>(gpuForces.size());
    AcquireGpuForceBuffer(emitter, gpuForces, resources);
    cb.curveFlags = {
        emitter.settings.useSizeCurve ? 1.0f : 0.0f,
        emitter.settings.useVelocityCurve ? 1.0f : 0.0f,
        emitter.settings.useColorGradient ? 1.0f : 0.0f,
        emitter.runtime.material.flipbook.flipbookFrameBlending ? 1.0f : 0.0f
    };
    // カーブは float4 1 本へ 2 キー (time,value) ずつ詰める。有効キー数を超えた分は
    // 最終キーで埋め、GPU 側が余分な区間を踏んでも CPU の Evaluate と同じ値になるようにする。
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
    // 黒体モードを焼き込んだ実効グラデーションを渡す。CPU 経路も同じ runtimeGradient を見る。
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
    // z = 補間色空間 (ParticleColorSpace)。EvaluateGradient8 がこれを見て CPU と同じ空間で混ぜる。
    cb.gradientMeta = { static_cast<float>(lastGradientKey + 1),
                        static_cast<float>(gradient.interpolation),
                        static_cast<float>(gradient.colorSpace), 0.0f };
    // 深度コリジョンは深度バッファを screen space で引くので、それを焼いたのと同じ
    // (TAA ジッター込みの) 行列で射影しないと当たり位置が半ピクセルずれる。
    cb.viewProjection = MakeJitteredViewProjection(ctx.camera, ctx.taaJitterNdcX, ctx.taaJitterNdcY);
    cb.screenWidth = static_cast<float>(ctx.width);
    cb.screenHeight = static_cast<float>(ctx.height);
    cb.depthThickness = (std::max)(0.001f, emitter.settings.collisionRadius * 0.002f);
    cb.depthBounciness = std::clamp(emitter.settings.collisionBounciness, 0.0f, 1.0f);
    cb.depthCollision = emitter.settings.collisionMode == ParticleCollisionMode::Depth ? 1u : 0u;
    cb.depthResponse = static_cast<std::uint32_t>(emitter.settings.collisionResponse);
    cb.depthDamping = std::clamp(emitter.settings.collisionDamping, 0.0f, 1.0f);
    // ── over-lifetime モジュール (回転カーブ / drag カーブ / 周回・放射) ──
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
    // 4 本のカーブの有効キー数と補間モード。GPU 側はこれを見て CPU と同じ区間を選ぶ。
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
    // 周回と放射は gForceFields (Vortex / Repulse) 側へ移った。CB の orbitalAxis /
    // orbitalVelocity / radialVelocity は誰も書かない死んだ枠で、cb{} のゼロ初期化のまま
    // GPU へ渡る (書いていた 0 と同じ値)。フィールド自体は残す ─ この CB は
    // ParticleGpuSim.cs.hlsl と 1 バイト単位で対で、途中を削ると以降の全オフセットがずれる。
    cb.spriteRandomFlags = (emitter.runtime.material.flipbook.spriteRandomStartFrame ? 1u : 0u)
        | (emitter.runtime.material.flipbook.spriteRandomRow ? 2u : 0u);
    resources.Update(emitter.runtime.gpuEmitterCB, &cb, sizeof(cb));

    // リングバッファヘッドを進める
    emitter.runtime.gpuWriteHead = (emitter.runtime.gpuWriteHead + emitter.runtime.gpuSpawnCount)
                           % static_cast<uint32_t>(maxP);

    // Dispatch CS
    renderer::ComputeCall cc;
    cc.shader        = h.particleGpuSimCS;
    cc.constantBuffers[0] = emitter.runtime.gpuEmitterCB;
    cc.srvBuffers[15] = emitter.runtime.gpuSpawnBuffer;  // t15
    cc.srvBuffers[29] = emitter.runtime.gpuForceBuffer;  // t29 (SB_PARTICLE_FORCES)
    cc.srvInputs[7] = resources.GetDepthTexture(ctx.Res().Target("DecalDepth"));
    // 速度場アトラス。場が 1 枚も無くても **必ず束縛する** — DX12 の null ディスクリプタは
    // Texture2D 固定で、Texture3D を宣言したスロットを空にすると次元が食い違う。
    cc.srvInputs[26] = asset::VelocityFieldAtlas::Texture(resources); // t26
    cc.uavBuffers[0] = emitter.runtime.gpuParticleBuffer; // u2
    cc.dispatchX = (static_cast<uint32_t>(maxP) + 63u) / 64u;
    cc.dispatchY = 1;
    cc.dispatchZ = 1;
    renderer.Dispatch(cc, resources);
    // Dispatch() は OM のレンダーターゲットをアンバインドする。
    // 後続の Draw が正しい HDR RT へ出力されるよう再バインドする。
    renderer.SetRenderTarget(ctx.Res().Target("HDR"), resources);
    }

    // ---- GPU ソート ---------------------------------------------------------
    // CPU へ読み戻して並べると毎フレーム同期待ちになるので GPU 側で並べ替える。
    // 動かすのは (キー, 粒子 index) の対だけ。プールはスポーン用のリングバッファなので、
    // 要素の位置が変わると gpuWriteHead の指す場所が意味を失う。
    // simulateThisFrame の外に置く — 複数ビューでは正しい順序もビューごとに違う。
    const bool wantSort = ShouldSortGpuParticles(emitter, h);
    if (wantSort) {
        // bitonic sort は要素数が 2 のべき乗である前提で組む。LDS 段が 1 グループ分を
        // 丸ごと扱うため、下限も 1 ブロック (256) に揃える。
        std::uint32_t padded = kParticleSortBlock;
        while (padded < static_cast<std::uint32_t>(maxP)) padded <<= 1;

        if (!emitter.runtime.gpuSortBuffer.IsValid() || emitter.runtime.gpuSortCapacity != padded) {
            if (emitter.runtime.gpuSortBuffer.IsValid()) resources.Release(emitter.runtime.gpuSortBuffer);
            // 1 要素 = uint2 (key, particleIndex)。
            emitter.runtime.gpuSortBuffer = resources.CreateRWStructuredBuffer(
                nullptr, padded, static_cast<std::uint32_t>(sizeof(std::uint32_t) * 2u));
            emitter.runtime.gpuSortCapacity = padded;
        }
        if (!emitter.runtime.gpuSortCB.IsValid())
            emitter.runtime.gpuSortCB = resources.CreateConstantBuffer(sizeof(GpuParticleSortCB));

        if (emitter.runtime.gpuSortBuffer.IsValid() && emitter.runtime.gpuSortCB.IsValid()) {
            GpuParticleSortCB sortCb{};
            sortCb.cameraPos   = ctx.camera.m_position;
            sortCb.aliveCount  = static_cast<std::uint32_t>(maxP);
            sortCb.paddedCount = padded;
            sortCb.backToFront = emitter.settings.sortMode == ParticleSortMode::BackToFront ? 1u : 0u;
            const std::uint32_t groups = padded / kParticleSortBlock;

            const auto dispatchSortStage =
                [&](renderer::ResourceHandle<renderer::ShaderTag> shader,
                    std::uint32_t stageK, std::uint32_t stageJ, bool bindParticles) {
                    sortCb.stageK = stageK;
                    sortCb.stageJ = stageJ;
                    resources.Update(emitter.runtime.gpuSortCB, &sortCb, sizeof(sortCb));
                    renderer::ComputeCall call;
                    call.shader = shader;
                    call.constantBuffers[0] = emitter.runtime.gpuSortCB;
                    // 粒子プールは SRV (t14) で読むだけ。ソート結果は別バッファ (u3) なので、
                    // 同一リソースを SRV と UAV へ同時バインドするハザードにはならない。
                    if (bindParticles) call.srvBuffers[14] = emitter.runtime.gpuParticleBuffer;
                    call.uavBuffers[1] = emitter.runtime.gpuSortBuffer; // u3 = UAV_GPU_SORT
                    call.dispatchX = groups;
                    renderer.Dispatch(call, resources);
                };

            dispatchSortStage(h.particleGpuSortKeysCS, 0u, 0u, /*bindParticles=*/true);
            for (std::uint32_t k = 2u; k <= padded; k <<= 1) {
                for (std::uint32_t j = k >> 1; j > 0u; j >>= 1) {
                    if (j <= kParticleSortBlock / 2u) {
                        // 比較距離がグループ幅の半分以下になったら、残る全段はグループ内で閉じる。
                        // LDS で j を 1 まで一気に下げ、グローバル往復を省く。
                        dispatchSortStage(h.particleGpuSortLocalCS, k, j, false);
                        break;
                    }
                    dispatchSortStage(h.particleGpuSortStepCS, k, j, false);
                }
            }
            renderer.SetRenderTarget(ctx.Res().Target("HDR"), resources);
        }
    } else if (emitter.runtime.gpuSortBuffer.IsValid()) {
        // ソートを切ったら確保も解放する。VS 側は renderCB の gpuSortEnabled で判断するので、
        // ここを残したままだと「無効なのにバッファだけ生き続ける」状態になる。
        resources.Release(emitter.runtime.gpuSortBuffer);
        emitter.runtime.gpuSortBuffer = {};
        emitter.runtime.gpuSortCapacity = 0;
    }

    // renderCB は「ソートする」と言っているのにバッファが無い状態では描かない。
    // VS はソート有効なら必ず t15 を引き、未バインドの SRV は 0 を返すので
    // 全インスタンスが粒子 0 番を指す絵になる。1 フレーム落とす方が原因を追いやすい。
    if (wantSort && !emitter.runtime.gpuSortBuffer.IsValid()) return;

    // ---- メッシュパーティクル (インスタンス描画) ------------------------------
    // CPU 経路は粒子 1 個につき DrawCall 1 本だが、GPU では粒子データが既に
    // StructuredBuffer にあるので maxParticles 個のインスタンス描画 1 本へ畳める。
    if (!emitter.settings.meshParticlePath.empty()) {
        if (meshParticleRenderer == nullptr || !h.particleGpuMeshShader.IsValid()
            || !h.meshTrailPSO.IsValid())
            return;
        const renderer::Mesh* mesh = meshParticleRenderer->mesh;
        if (!meshParticleRenderer->enabled || mesh == nullptr || mesh->isSkinned
            || !mesh->vertexBuffer.IsValid() || !mesh->indexBuffer.IsValid())
            return;

        renderer::DrawCall meshDc;
        meshDc.vertexBuffer = mesh->vertexBuffer;
        meshDc.indexBuffer  = mesh->indexBuffer;
        meshDc.indexCount   = mesh->indexCount;
        meshDc.vertexCount  = mesh->vertexCount;
        meshDc.shader       = h.particleGpuMeshShader;
        // ビルボード用 PSO は頂点レイアウトを持たない。メッシュ残像と同じ
        // (標準頂点レイアウト + ALPHA_BLEND + DEPTH_READ) を流用する。
        meshDc.pipelineState = h.meshTrailPSO;
        meshDc.layer = renderer::RenderLayer::TRANSPARENT_LAYER;
        meshDc.constantBuffers[0] = h.frameCB;
        meshDc.constantBuffers[kParticleConstantSlot] = emitter.runtime.renderCB;
        meshDc.textures[0]  = emitter.runtime.texture;
        meshDc.vsBuffers[0] = emitter.runtime.gpuParticleBuffer; // t14
        meshDc.vsBuffers[1] = emitter.runtime.gpuSortBuffer;     // t15 (ソート無効時は無効ハンドル)
        meshDc.instanceCount = static_cast<uint32_t>(maxP);
        SubmitCounted(ctx, meshDc);
        return;
    }

    // SV_VertexID ベース描画: 頂点バッファなし、VS が StructuredBuffer<GpuParticle> を t14 で読む。
    // .mat がシェーダーを指していれば CPU 経路と同じようにそれで描く。
    // 頂点バッファを持たないので、差すシェーダーは `#define FBZZ_PARTICLE_GPU` 付きで
    // ParticleMaterial.hlsli を include していること (CPU 用を差すと何も出ない)。
    const auto gpuShader = emitter.runtime.customShader.IsValid()
        ? emitter.runtime.customShader
        : h.particleGpuShader;
    const auto gpuPSO = SelectParticlePSO(emitter, h.particleGpuPSO, h.particleGpuAlphaPSO,
                                          h.particleGpuPremultipliedPSO);
    if (!gpuShader.IsValid() || !gpuPSO.IsValid() || !emitter.runtime.texture.IsValid())
        return;

    renderer::DrawCall dc;
    dc.shader        = gpuShader;
    dc.pipelineState = gpuPSO;
    dc.constantBuffers[0] = h.frameCB;
    dc.constantBuffers[kParticleConstantSlot] = emitter.runtime.renderCB;
    // b2: カスタムシェーダーの MaterialConstants (.mat の [params])。組み込みでは無効ハンドル。
    dc.constantBuffers[2] = emitter.runtime.materialParamsCB;
    dc.textures[0]        = emitter.runtime.texture;
    dc.textures[1]        = emitter.runtime.distortionTexture; // t1: 歪み専用マップ (未設定なら無効)
    dc.textures[5]        = sceneColor;
    dc.textures[6]        = emitter.runtime.motionVectorTexture;
    dc.textures[7]        = resources.GetDepthTexture(ctx.Res().Target("DecalDepth"));
    // 受け影: CPU 経路と同じ b4 / t8 / サンプラー 1 を使う。
    dc.constantBuffers[4] = h.shadowCB;
    dc.textures[8]        = resources.GetDepthTexture(ctx.Res().Target("ShadowMap"));
    BindParticleLighting(dc, emitter, ctx);
    dc.vsBuffers[0]       = emitter.runtime.gpuParticleBuffer; // t14: StructuredBuffer<GpuParticle>
    // t15: ソート済み (key, index)。無効時は何もバインドしない
    // (VS は renderCB の gpuSortEnabled が 0 なら参照しない)。
    dc.vsBuffers[1]       = emitter.runtime.gpuSortBuffer;
    dc.vertexCount        = static_cast<uint32_t>(maxP) * 6u;
    SubmitCounted(ctx, dc);
    g_gpuParticleDrawRecords.push_back({ &emitter, static_cast<int>(maxP) });
}

// CanUseGpuSimulation の実体は ParticleGpuSimulation.cpp。
// 「なぜ GPU に載らなかったか」を lint / 実行状態 / Editor へ返す必要があるため。

void UpdateParticleBounds(ParticleEmitter& emitter, const Transform& tf)
{
    // WHAT: 現在の粒子位置とサイズから球Boundsを再計算する。粒子がない間は将来位置を予測して保守的に保持する。
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

// world は null 可。描画パス側が RenderPassContext::physicsWorld をそのまま渡せるよう
// ポインタで受ける (参照だと呼び出し側に null チェックとダミー World が要る)。
void SimulateCpuEmitter(ParticleEmitter& emitter, const Transform& tf,
                        const AnimatorComponent* animator, float dt, float time,
                        const std::vector<ActiveForceField>& forceFields,
                        const physics::World* world, Scene& scene)
{
    emitter.runtime.lastCpuSimulationFrame = Time::frameCount;
    emitter.runtime.collisionCountThisFrame = 0;
    emitter.runtime.deathCountThisFrame = 0;
    const int particleCapacity = (std::max)(emitter.settings.maxParticles, 0);
    const bool localSpace = emitter.settings.simulationSpace == ParticleSimulationSpace::Local;
    // 粒子 1 個のワールド位置・速度。SubEmitter へ «発火元» として渡すときに使う
    // (保持しているのはシミュレーション空間の値で、Local なら世界の値ではない)。
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
        // 注入されたスポーンはレート/バーストより先に消費する。後回しにすると
        // «斬った位置の火花» が maxParticles 到達で黙って落ちる ─ 常時湧いている粒より、
        // イベントで 1 度だけ出る粒のほうが落ちたときに目立つ。
        //
        // WHY ループ中に injectedSpawns を消さないか: SpawnParticle → queueBirth が
        //     «自分自身» を birthSubEmitter に持つ構成でこのリストへ積み返す。
        //     走査中に push_back されると参照が無効化されるので、添字で回して最後に消す。
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

    // このエミッターに効く力をワールド空間で 1 回だけ解決する
    // (粒子ごとに解決すると同じ計算を粒子数ぶん繰り返すことになる)。
    std::vector<ActiveForceField> emitterForces;
    ResolveEmitterForces(emitter, tf, forceFields, emitterForces);

    // Physics 衝突で見るコライダーの絞り込み。エミッター単位で 1 本作って粒子数ぶん回す。
    const std::uint32_t collisionLayerMask = emitter.settings.collisionLayerMask;
    const physics::World::ColliderFilter collisionFilter =
        [collisionLayerMask](const physics::ColliderInstance& instance) {
            // トリガーは «通過を検知する体積» で、跳ね返る面ではない。素通しにしないと
            // 見えない判定ボリュームの表面で火花が止まる。
            if (instance.isTrigger) return false;
            if (instance.layer < 0 || instance.layer > 31) return true;
            return (collisionLayerMask & (1u << static_cast<unsigned>(instance.layer))) != 0u;
        };

    for (auto it = emitter.runtime.particles.begin(); it != emitter.runtime.particles.end();) {
        it->age += dt;
        if (it->age >= it->lifetime) {
            ++emitter.runtime.deathCountThisFrame;
            // 消えた «その場所» と最後の速度。これが無いと煙が発生源へ戻って湧く。
            QueueSubEmitter(scene, emitter, emitter.settings.deathSubEmitter,
                            emitter.settings.subEmitterBurstCount,
                            particleWorldPosition(*it), particleWorldVelocity(*it));
            it = emitter.runtime.particles.erase(it);
            continue;
        }
        const float normalizedAge = Clamp01(it->age / (std::max)(it->lifetime, 0.001f));
        // drag カーブは内蔵の Drag 力への時間倍率。ApplyForceFields が相手を選ぶ。
        const float dragScale = emitter.settings.useDragCurve
            ? (std::max)(emitter.settings.dragCurve.Evaluate(normalizedAge), 0.0f) : 1.0f;
        // 力はすべてワールドで効く。重力・空気抵抗・乱流・周回・放射・速度場・シーンの力場が
        // 1 本の評価器を通るので、種類ごとに «どの空間で効くか» を覚える必要が無い。
        if (emitter.settings.simulationSpace == ParticleSimulationSpace::Local) {
            const math::Vector3 worldPosition = TransformEmitterPoint(tf, it->position);
            math::Vector3 worldVelocity = TransformEmitterVector(tf, it->velocity);
            ApplyForceFields(emitterForces, emitter.settings.forceFieldChannels,
                             worldPosition, worldVelocity, dt, time, dragScale);
            it->velocity = InverseTransformEmitterVector(tf, worldVelocity);
        } else {
            ApplyForceFields(emitterForces, emitter.settings.forceFieldChannels,
                             it->position, it->velocity, dt, time, dragScale);
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
        // 回転カーブは角速度への時間倍率。勢いよく回り始めて減速する破片が作れる。
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
        // 速さによる大きさ。火花が «速いほど大きく (= 長く) 見える» を作る。
        if (emitter.settings.useSpeedSizeCurve) {
            it->size *= (std::max)(emitter.settings.speedSizeCurve.Evaluate(
                NormalizedParticleSpeed(emitter, *it)), 0.0f);
        }
        AppendParticleTrailPoint(emitter, *it, dt);
        ++it;
    }
    UpdateParticleBounds(emitter, tf);
}

} // anonymous namespace

// 再生進行の正本。ParticleSimulationSystem (LateUpdate) と ParticlePass の両方がこれを呼ぶ。
//
// WHY 1 本にしたか: 以前は System と Pass に «ほぼ同じ» 実装が 2 つあり、
//     rateOverDistance の基準点が食い違っていた (System は GameObject の worldPosition、
//     Pass は発生点)。同じシーンでもどちらが先に走るかで «移動で湧く量» が変わり、
//     しかも Pass 側は emitterVelocity を求めないため、カリング中は inheritVelocity が
//     前回値で固まっていた。基準点は «発生点» に寄せる ─ emitPosition をずらした
//     エミッターでは、親の原点ではなく実際に粒が出る場所が動いた量が正しい。
bool AdvanceParticleEmitterPlayback(ParticleEmitter& emitter, const Transform& tf, float dt)
{
    if (emitter.runtime.lastPlaybackFrame == Time::frameCount) return emitter.runtime.emitThisFrame;
    emitter.runtime.lastPlaybackFrame = Time::frameCount;
    // 黒体モードの焼き込みはここで 1 フレームに 1 回だけ行う。CPU 更新も GPU 定数バッファも
    // この後の runtimeGradient を読むため、両経路の色が原理的にずれない。
    emitter.RefreshRuntimeGradient();

    bool canEmit = emitter.settings.playing;
    if (canEmit && emitter.runtime.delayTime < emitter.settings.startDelay) {
        emitter.runtime.delayTime += dt;
        canEmit = false;
    }
    // duration の有無に関わらず進める (playTime は «再生開始からの経過»)。
    // 止めると duration = 0 のエミッターで時刻指定 Burst が永久に発火しない。
    if (canEmit) emitter.runtime.playTime += dt;
    if (canEmit && emitter.settings.duration > 0.0f
        && emitter.runtime.playTime >= emitter.settings.duration) {
        if (emitter.settings.loop) {
            emitter.runtime.playTime = 0.0f;
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

    // 距離Emission: Emitterのワールド移動量を粒子数へ変換する。
    const math::Vector3 emitterWorldPosition = TransformEmitterPoint(tf, emitter.settings.emitPosition);
    // エミッター自身のワールド速度。inheritVelocity がスポーン初速へ足すために使う。
    // dt が 0 (完全停止・スクラブ中) のときは前回値を保つ (0 除算と速度の消失を避ける)。
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

    // 時刻指定Burst。loop時はplayTimeの巻き戻しでcycle状態もリセットされる。
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

    // Prewarmは初回描画前に定常個数を投入する。GPU readbackを避けるため履歴位置は近似する。
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

void UpdateParticleCpuSimulation(Scene& scene, physics::World& world, float deltaTime, float time)
{
    const std::vector<ActiveForceField> forceFields = GatherForceFields(scene, ~0u);
    for (EntityID id : scene.GetEntities<ParticleEmitter>()) {
        auto* emitter = scene.GetComponent<ParticleEmitter>(id);
        GameObject* gameObject = scene.GetGameObject(id);
        if (!emitter || !gameObject || !gameObject->activeInHierarchy()
            || !emitter->settings.enabled
            || CanUseGpuSimulation(emitter->settings, &emitter->runtime.material)) continue;
        // GPU指定を一時的にCPUへ縮退した場合、制約解除後に古いGPU粒子が復活しないよう履歴を破棄する。
        if (emitter->settings.simulationMode == ParticleSimulationMode::Gpu)
            emitter->runtime.gpuClearPending = true;
        // 発生側は ParticleSimulationSystem が止めるが、粒子の更新はこちら。
        // 見ないと «発生は止まるのに既存の粒子は動き続ける» 中途半端な一時停止になる。
        if (emitter->settings.culling.pauseWhenCulled && emitter->runtime.isCulledThisFrame) continue;
        if (emitter->runtime.lastCpuSimulationFrame == Time::frameCount) continue;
        const auto* animator = FindParticleAnimator(*gameObject);
        // VFX Editor のプレビュー速度 (一時停止 / スロー / 倍速) をエミッター単位で dt へ注入する。
        const float scaledDt = deltaTime * emitter->GetEditorTimeScale(Time::frameCount);
        SimulateCpuEmitter(*emitter, gameObject->transform, animator, scaledDt, time,
                           forceFields, &world, scene);
    }
}

void ScrubParticleEmitterForEditor(Scene& scene, physics::World& world,
                                   GameObject& gameObject, ParticleEmitter& emitter,
                                   float targetTime, float time)
{
    // 巻き戻し。randomState が randomSeed に戻るため、同じ targetTime へのスクラブは常に同じ結果になる。
    emitter.ResetPlayback();

    // GPU 粒子は CS 側バッファの履歴を任意時刻へ巻き戻せないため、リスタートのみで返す。
    if (CanUseGpuSimulation(emitter.settings, &emitter.runtime.material)) {
        emitter.runtime.prewarmed = true; // 直後の Prewarm 一括投入でスクラブ結果が壊れないようにする
        return;
    }

    // 固定ステップで決定論的に早送りする。上限を設けて極端な値でエディターが固まるのを防ぐ。
    constexpr float kFixedStep       = 1.0f / 60.0f;
    constexpr float kMaxScrubSeconds = 60.0f;
    const float target = (std::min)((std::max)(targetTime, 0.0f), kMaxScrubSeconds);

    const std::vector<ActiveForceField> forceFields = GatherForceFields(scene, ~0u);
    const auto* animator = FindParticleAnimator(gameObject);

    float simulated = 0.0f;
    while (simulated < target) {
        const float step = (std::min)(kFixedStep, target - simulated);

        // 再生状態 (delay / duration / loop / Burst) を 1 ステップ進める。
        // ステップ単位で再現しないと Burst の発火やループ巻き戻しが実時間再生とずれる。
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
                           forceFields, &world, scene);
        simulated += step;
    }

    // スクラブ後に Prewarm の一括投入や同フレームの通常再生が重なって状態を壊さないようにする。
    emitter.runtime.prewarmed              = true;
    emitter.runtime.lastPlaybackFrame      = Time::frameCount;
    emitter.runtime.lastCpuSimulationFrame = Time::frameCount;
}

void ExecuteParticlePass(RenderPassContext& ctx)
{
    auto& renderer  = ctx.renderer;
    auto& resources = ctx.resources;
    auto& h         = ctx.handles;

    // Overdraw パスが読む記録は毎回このパスが作り直す。早期 return より前に捨てないと、
    // 描かなかったフレームに残ったポインタで破棄済みオブジェクトを触りうる。
    g_particleDrawRecords.clear();
    g_gpuParticleDrawRecords.clear();

    // .mat の [params] 解決をこのパスで 1 回だけやり直すための通番。編集が次のフレームで
    // 絵に出つつ、同じ .mat を共有するエミッターぶん名前引きを繰り返さない。
    ++g_particlePassSerial;

    if (!h.particleShader.IsValid() || !h.particleIB.IsValid()) return;

    const float dt   = Time::deltaTime;
    const float time = Time::time;
    // 自己影の密度 RT はこのパス内で 1 回だけクリアし、各エミッターが積み増していく。
    // 毎エミッターでクリアすると自分の密度しか見えず、自己影の意味が無くなる。
    bool selfShadowClearedThisPass = false;

    // Distortion は現在の HDR を読みながら同じ HDR へ書けないので、背景を専用 RT へ退避する。
    // 退避はエミッターごとに取り直す。1 回だけだと全ての歪みが「パーティクルを 1 つも
    // 描いていない背景」を屈折し、重なり順が無視された絵になる。
    // 同一エミッター内で重なる粒子は 1 DrawCall なので依然として同じ背景を共有する
    // (粒子単位の順序を出すには DrawCall を分けるしかなく、大量に出せなくなる)。
    renderer::ResourceHandle<renderer::TextureTag> particleSceneColor;
    const bool needsSceneColor = std::any_of(ctx.scene.GameObjects().begin(), ctx.scene.GameObjects().end(),
        [](GameObject& object) {
            const auto* emitter = object.GetComponent<ParticleEmitter>();
            return emitter != nullptr && emitter->settings.enabled && emitter->runtime.material.distortion;
        });
    // 現在の HDR を退避 RT へコピーし、そのテクスチャを返す。失敗時は無効ハンドル。
    const auto captureSceneColor = [&]() -> renderer::ResourceHandle<renderer::TextureTag> {
        // 退避先はビューが持つ (RenderPassHandles::particleSceneColorRT の WHY)。
        if (!h.particleSceneColorRT) return {};
        renderer::SizedRenderTarget& sceneColorRT = *h.particleSceneColorRT;
        static std::uint64_t resetVersion = 0;
        static renderer::ResourceHandle<renderer::ShaderTag> copyShader;
        if (resetVersion != resources.GetResetVersion()) {
            resetVersion = resources.GetResetVersion();
            copyShader = resources.LoadShader("Assets/Shaders/PostProcess/Color/CopyColor.hlsl");
        }
        (void)sceneColorRT.Ensure(resources, ctx.width, ctx.height, 1);
        if (!sceneColorRT.IsValid() || !copyShader.IsValid()) return {};
        renderer.SetRenderTarget(sceneColorRT, resources);
        renderer::DrawCall copy;
        copy.shader = copyShader; copy.pipelineState = h.postprocPSO; copy.vertexCount = 3;
        copy.textures[5] = resources.GetColorTexture(ctx.Res().Target("HDR"), 0);
        renderer.Submit(copy, resources);
        renderer.SetRenderTarget(ctx.Res().Target("HDR"), resources);
        return resources.GetColorTexture(sceneColorRT, 0);
    };
    if (needsSceneColor) particleSceneColor = captureSceneColor();

    // 取り直しの枚数を絞る。
    //
    // WHY 上限が要るか: 退避はフルスクリーンのコピー (RT 切り替え 2 回 + 全画面描画) で、
    //   «同時に歪んでいるエミッターの数» がそのままフレーム時間に乗る。爆発の演出に
    //   陽炎の層が 1 枚入っているだけで «1 発 = 1 コピー» になり、ボスが一度に十数発
    //   撒く場面 (GreenWare の Boss02) では画面コピーだけで十数回走った。
    //   先着数枚までを取り直せば重なり順は十分に出る ─ それ以降は既に «粒でびっしり»
    //   なので、屈折元が 1 世代古いことは絵として読み取れない。
    constexpr int kMaxDistortionRecaptures = 3;
    int distortionRecaptures = 0;
    const auto recaptureSceneColor = [&] {
        if (!needsSceneColor || distortionRecaptures >= kMaxDistortionRecaptures) return;
        ++distortionRecaptures;
        particleSceneColor = captureSceneColor();
    };

    // シーン内の力場を 1 回だけ収集し、全エミッター (CPU/GPU) で共有する
    const std::vector<ActiveForceField> forceFields = GatherForceFields(ctx);
    int particleBudget = ctx.settings.particleBudgetEnabled
        ? (std::max)(ctx.settings.particleBudget, 0) : 0;

    // 描画順を renderPriority → カメラ距離 (遠い順) で確定させる。GameObject の並び順の
    // ままだと「炎の手前に煙」が保証されず、シーンを編集しただけで前後が入れ替わる。
    // 距離はバウンズ更新前なので Transform 位置で近似する (順序決定には十分)。
    std::vector<GameObject*> sortedEmitters;
    for (auto& candidate : ctx.scene.GameObjects()) {
        if (!ShouldRenderGameObject(candidate, ctx.cullingMask)) continue;
        auto* candidateEmitter = candidate.GetComponent<ParticleEmitter>();
        if (candidateEmitter == nullptr || !candidateEmitter->settings.enabled) continue;
        sortedEmitters.push_back(&candidate);
    }
    const math::Vector3 cameraPosition = ctx.camera.m_position;
    // GetComponent は非 const 版しかないため、比較関数も非 const ポインタで受ける。
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

        // VFX Editor のプレビュー速度 (一時停止 / スロー / 倍速)。ゲーム実行時は常に 1.0。
        const float dtEmitter = dt * emitter->GetEditorTimeScale(Time::frameCount);

        emitter->settings.duration = (std::max)(emitter->settings.duration, 0.0f);
        emitter->settings.startDelay = (std::max)(emitter->settings.startDelay, 0.0f);
        emitter->settings.sizeCurvePower = (std::max)(emitter->settings.sizeCurvePower, 0.001f);
        emitter->settings.colorCurvePower = (std::max)(emitter->settings.colorCurvePower, 0.001f);
        // 空気抵抗が負だと 1 - impulse が 1 を超え、速度が毎フレーム増えて発散する。
        for (ForceFieldSettings& force : emitter->settings.localForces) {
            if (force.fieldType == ForceFieldType::Drag)
                force.strength = (std::max)(force.strength, 0.0f);
        }
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
            // pauseWhenCulled が false なら「描かないだけ」で時間は進める。
            // 非 Play のエディターではこのパスがシミュレーションの実体なので、進めないと
            // カメラを画面外へ振った瞬間にエフェクトが凍る。
            if (!emitter->settings.culling.pauseWhenCulled) {
                (void)AdvanceParticleEmitterPlayback(*emitter, tf, dtEmitter);
                if (!CanUseGpuSimulation(emitter->settings, &emitter->runtime.material)
                    && emitter->runtime.lastCpuSimulationFrame != Time::frameCount)
                    SimulateCpuEmitter(*emitter, tf, animator, dtEmitter, time,
                                       forceFields, ctx.physicsWorld, ctx.scene);
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
        // Physics Query、Local Space、厳密な透過ソートはCPU側で解決し、見た目の正しさを優先する。
        const bool isGpuMode = CanUseGpuSimulation(emitter->settings, &emitter->runtime.material);

        const bool canEmit = AdvanceParticleEmitterPlayback(*emitter, tf, dtEmitter);

        // GPU モードは CPU スポーン/更新をスキップして GPU パスへ
        if (isGpuMode) {
            const int requested = (std::max)(emitter->settings.maxParticles, 0);
            const int drawLimit = particleBudget > 0 ? (std::min)(requested, particleBudget) : requested;
            emitter->runtime.visibleParticleCount = drawLimit;
            if (particleBudget > 0) particleBudget -= drawLimit;
            if (requested > drawLimit) ++ctx.statsParticleBudgetDropped;
            if (drawLimit <= 0) continue;
            EnsureParticleTexture(*emitter, resources);
            UpdateParticleRenderConstants(*emitter, resources, drawLimit, ctx.width, ctx.height,
                                          ShouldSortGpuParticles(*emitter, ctx.handles));
            ctx.statsParticleVisible += drawLimit;
            // CPU 経路と同じ理由でここでも取り直す (歪みの前後関係を保つ)。
            if (emitter->runtime.material.distortion) recaptureSceneColor();
            // メッシュパーティクルは同じ GameObject の MeshRenderer が形状を持つ
            // (meshParticlePath から解決する)。GPU 経路はこれをインスタンス描画する。
            const MeshRenderer* meshParticleRenderer = emitter->settings.meshParticlePath.empty()
                ? nullptr : go.GetComponent<MeshRenderer>();
            TickGpuEmitter(*emitter, tf, animator, dtEmitter, time, canEmit, forceFields,
                           particleSceneColor, meshParticleRenderer, ctx);
            continue;
        }

        // シミュレーションはここでは行わない。ParticleSimulationSystem (LateUpdate) が
        // 描画より前に SimulateCpuEmitter で 1 フレーム分を進め終えている。
        // 描画パスに複製を置くと、先に走る側が lastCpuSimulationFrame を立てるため
        // 一度も実行されず «実装済みなのに効かない» 状態になる。

        const int available = static_cast<int>(emitter->runtime.particles.size());
        const int countBudget = particleBudget > 0 ? (std::min)(available, particleBudget) : available;
        const int count = std::min(countBudget, kMaxParticleDraw);
        emitter->runtime.visibleParticleCount = count;
        if (particleBudget > 0) particleBudget -= count;
        if (available > count) ++ctx.statsParticleBudgetDropped;
        ctx.statsParticleVisible += count;
        if (count == 0) continue;
        // MeshTrail passが同じCPU粒子列を静的Meshとして描く。billboardとの二重描画を避ける。
        if (!emitter->settings.meshParticlePath.empty()) continue;
        UpdateParticleRenderConstants(*emitter, resources, count, ctx.width, ctx.height);
        if (emitter->settings.sortMode == ParticleSortMode::BackToFront) {
            const math::Vector3 cameraPos = ctx.camera.m_position;
            std::sort(emitter->runtime.particles.begin(), emitter->runtime.particles.end(),
                [cameraPos, &tf, emitter](const Particle& a, const Particle& b) {
                    const math::Vector3 aPosition = emitter->settings.simulationSpace == ParticleSimulationSpace::Local
                        ? TransformEmitterPoint(tf, a.position) : a.position;
                    const math::Vector3 bPosition = emitter->settings.simulationSpace == ParticleSimulationSpace::Local
                        ? TransformEmitterPoint(tf, b.position) : b.position;
                    const math::Vector3 da = aPosition - cameraPos;
                    const math::Vector3 db = bPosition - cameraPos;
                    return math::Vector3::Dot(da, da) > math::Vector3::Dot(db, db);
                });
        }

        // CPU で頂点バッファを構築 (ビルボードは VS でスクリーン展開)
        static const float kUV[4][2] = { {0,0},{1,0},{0,1},{1,1} };
        std::vector<ParticleVertex> verts;
        verts.reserve(static_cast<size_t>(count * 4));
        // クワッド数は粒子本体 + トレイル履歴の合計。共有インデックスバッファの容量
        // (kMaxParticleDraw クワッド分) を超えないよう積むたびに確認する。
        int quadCount = 0;
        const auto emitQuad = [&](const math::Vector3& center, const math::Vector3& velocity,
                                  float size, float rotation, const math::Vector4& color,
                                  const Particle& source) {
            if (quadCount >= kMaxParticleDraw) return;
            for (int c = 0; c < 4; ++c) {
                ParticleVertex v;
                v.center[0] = center.x;
                v.center[1] = center.y;
                v.center[2] = center.z;
                v.uv[0]     = kUV[c][0];
                v.uv[1]     = kUV[c][1];
                v.color[0]  = color.x;
                v.color[1]  = color.y;
                v.color[2]  = color.z;
                v.color[3]  = color.w;
                v.size      = size;
                v.rotation  = rotation;
                v.uvRect[0] = source.uvRect.x;
                v.uvRect[1] = source.uvRect.y;
                v.uvRect[2] = source.uvRect.z;
                v.uvRect[3] = source.uvRect.w;
                v.velocity[0] = velocity.x;
                v.velocity[1] = velocity.y;
                v.velocity[2] = velocity.z;
                v.nextUvRect[0] = source.nextUvRect.x;
                v.nextUvRect[1] = source.nextUvRect.y;
                v.nextUvRect[2] = source.nextUvRect.z;
                v.nextUvRect[3] = source.nextUvRect.w;
                v.spriteBlend = source.spriteBlend;
                verts.push_back(v);
            }
            ++quadCount;
        };

        const bool localSpace = emitter->settings.simulationSpace == ParticleSimulationSpace::Local;
        // 連続リボン指定なら、尾はビルボードではなく帯として別 DrawCall で描く。
        // ここで 0 にしておかないと、帯とビルボードの二重描画になる。
        const bool ribbonTrail = emitter->settings.trail.trailEnabled && emitter->settings.trail.trailRibbon;
        const int trailPoints = (emitter->settings.trail.trailEnabled && !ribbonTrail)
            ? std::clamp(emitter->settings.trail.trailPointCount, 1, kMaxParticleTrailPoints) : 0;
        for (int i = 0; i < count; ++i) {
            const auto& p = emitter->runtime.particles[i];
            const math::Vector3 renderPosition = localSpace
                ? TransformEmitterPoint(tf, p.position) : p.position;
            const math::Vector3 renderVelocity = localSpace
                ? TransformEmitterVector(tf, p.velocity) : p.velocity;
            emitQuad(renderPosition, renderVelocity, p.size, p.rotation, p.color, p);

            // 尾: 履歴点を古いほど細く・薄くしながら並べる。
            // 回転は本体と同じ値を使い、尾がバラバラに回って見えないようにする。
            const int usedTrail = (std::min)(static_cast<int>(p.trailCount), trailPoints);
            for (int t = 0; t < usedTrail; ++t) {
                // 先端 (最古) へ向かうほど 1 → 0 に近づく係数。
                const float fade = 1.0f - static_cast<float>(t + 1)
                    / static_cast<float>(trailPoints + 1);
                const math::Vector3 trailPosition = localSpace
                    ? TransformEmitterPoint(tf, p.trailPoints[static_cast<std::size_t>(t)])
                    : p.trailPoints[static_cast<std::size_t>(t)];
                const float widthLerp = emitter->settings.trail.trailWidthScale
                    + (1.0f - emitter->settings.trail.trailWidthScale) * fade;
                const float alphaLerp = emitter->settings.trail.trailAlphaScale
                    + (1.0f - emitter->settings.trail.trailAlphaScale) * fade;
                // p.color はリニア、tint はオーサリング値 (sRGB)。tint をリニアへ揃えてから
                // 掛ける。揃えないと、同じ tint がビルボード尾とリボン尾で違う色になる。
                const math::Vector4 tint = ParticleSrgbToLinear(emitter->settings.trail.trailColorTint);
                const math::Vector4 trailColor = {
                    p.color.x * tint.x,
                    p.color.y * tint.y,
                    p.color.z * tint.z,
                    p.color.w * emitter->settings.trail.trailColorTint.w * alphaLerp
                };
                emitQuad(trailPosition, renderVelocity, p.size * widthLerp, p.rotation,
                         trailColor, p);
            }
        }

        const auto particlePSO = SelectParticlePSO(*emitter, h.particlePSO, h.particleAlphaPSO,
                                                   h.particlePremultipliedPSO);
        if (!particlePSO.IsValid() || !emitter->runtime.texture.IsValid())
            continue;

        // 頂点はエミッターごとに別バッファへ載せる (共有 1 本だと DX12 で先の Draw が壊れる)。
        const auto particleVB = g_particleVertexPool.Acquire(
            resources, verts.size(), static_cast<uint32_t>(sizeof(ParticleVertex)));
        if (!particleVB.IsValid()) continue;
        resources.Update(particleVB, verts.data(),
                         static_cast<uint32_t>(verts.size() * sizeof(ParticleVertex)));

        // 歪みを使うエミッターは、その直前までに描いた絵を屈折させる。取り直さないと
        // 歪みを重ねたときの前後関係が失われる。
        if (emitter->runtime.material.distortion) recaptureSceneColor();

        // 自己影: このエミッターの密度を光源側 RT へ積む。頂点バッファに今の形が
        // 乗っている間しか測れないので、本番描画の直前に行う。
        bool selfShadowReady = false;
        if (emitter->runtime.material.selfShadowStrength > 0.0f
            && PrepareParticleSelfShadowTarget(ctx, selfShadowClearedThisPass)) {
            AccumulateParticleSelfShadowDensity(*emitter, quadCount, particleVB, ctx);
            selfShadowReady = true;
        }

        renderer::DrawCall dc;
        dc.vertexBuffer       = particleVB;
        dc.indexBuffer        = h.particleIB;
        // 粒子本体 + トレイルの合計クワッド数。count のままだと尾が描かれない。
        dc.indexCount         = static_cast<uint32_t>(quadCount * 6);
        // .mat がシェーダーを指していればそれで描く。未指定・ロード失敗なら組み込みへ落ちる。
        dc.shader             = emitter->runtime.customShader.IsValid() ? emitter->runtime.customShader
                                                                : h.particleShader;
        dc.pipelineState      = particlePSO;
        dc.constantBuffers[0] = h.frameCB;
        dc.constantBuffers[kParticleConstantSlot] = emitter->runtime.renderCB;
        // b2: カスタムシェーダーの MaterialConstants (.mat の [params])。組み込みでは無効ハンドル。
        dc.constantBuffers[2] = emitter->runtime.materialParamsCB;
        // 受け影は Surface マテリアルと同じ b4 (ShadowConstants) / t8 (シャドウマップ) を使う。
        // シェーダー側は常に宣言しているため、有効/無効に関わらずバインドしておく
        // (未バインドの SRV を読むと環境によっては未定義値になる)。
        dc.constantBuffers[4] = h.shadowCB;
        dc.textures[0]        = emitter->runtime.texture;
        // t1: 歪み専用ノーマルマップ。未設定なら無効ハンドルのままで、
        // PS は effectsFlags を見て albedo の RG へ縮退する。
        dc.textures[1]        = emitter->runtime.distortionTexture;
        dc.textures[5]        = particleSceneColor;
        dc.textures[6]        = emitter->runtime.motionVectorTexture;
        dc.textures[7]        = resources.GetDepthTexture(ctx.Res().Target("DecalDepth"));
        dc.textures[8]        = resources.GetDepthTexture(ctx.Res().Target("ShadowMap"));
        BindParticleLighting(dc, *emitter, ctx);
        // t9: 自己影の密度。有効でないときは何もバインドしない
        // (シェーダーは selfShadowStrength が 0 なら参照しない)。
        if (selfShadowReady)
            dc.textures[9] = resources.GetColorTexture(h.particleSelfShadowRT, 0);
        SubmitCounted(ctx, dc);
        g_particleDrawRecords.push_back({ emitter, particleVB, quadCount });

        // 帯は本体の後に描く。帯の方が面積が大きく、先に描くと本体が沈んで見えるため。
        if (ribbonTrail) DrawParticleTrailRibbons(*emitter, tf, count, ctx);
    }
}

// パーティクルの重なり枚数を可視化する。Particle パスの直後に同じ頂点バッファを
// 計数シェーダーで専用 RT へ描き直し、ヒートマップへ変換して HDR RT を上書きする。
// 独立したパスにするのは、計数に要る「加算のみ・専用 RT」を同居させると RT 切り替えと
// ブレンドの分岐が本番描画側へ漏れるため。
void ExecuteParticleReactivePass(RenderPassContext& ctx)
{
    auto& renderer = ctx.renderer;
    auto& resources = ctx.resources;
    auto& h = ctx.handles;
    h.particleReactiveValid = false;
    if (h.particleReactiveRT == nullptr || !h.particleIB.IsValid()) return;

    static renderer::ResourceHandle<renderer::ShaderTag> cpuShader;
    static renderer::ResourceHandle<renderer::ShaderTag> gpuShader;
    static std::uint64_t resetVersion = 0;
    if (resetVersion != resources.GetResetVersion()) {
        resetVersion = resources.GetResetVersion();
        cpuShader = resources.LoadShader("Assets/Shaders/Material/Effects/ParticleReactive.hlsl");
        gpuShader = resources.LoadShader("Assets/Shaders/Material/Effects/ParticleReactiveGPU.hlsl");
    }
    if (!cpuShader.IsValid()) return;
    renderer::SizedRenderTarget& reactiveRT = *h.particleReactiveRT;
    (void)reactiveRT.Ensure(resources, ctx.width, ctx.height, 1);
    if (!reactiveRT.IsValid()) return;

    renderer.SetRenderTarget(reactiveRT, resources);
    renderer.Clear({ 0.0f, 0.0f, 0.0f, 0.0f });
    const auto sceneDepth = resources.GetDepthTexture(ctx.Res().Target("DecalDepth"));
    // 本番の描画と同じバッファ・同じクワッド数で描き直す (作り直すと «実際に描いた形» とずれる)。
    // 加算で積むので、重なった粒子ほどマスクが濃くなる。
    for (const ParticleDrawRecord& record : g_particleDrawRecords) {
        if (!record.vertexBuffer.IsValid() || record.quadCount <= 0) continue;
        renderer::DrawCall dc;
        dc.vertexBuffer       = record.vertexBuffer;
        dc.indexBuffer        = h.particleIB;
        dc.indexCount         = static_cast<uint32_t>(record.quadCount * 6);
        dc.shader             = cpuShader;
        dc.pipelineState      = h.particlePSO;   // ADDITIVE + DEPTH_READ
        dc.constantBuffers[0] = h.frameCB;
        dc.constantBuffers[kParticleConstantSlot] = record.emitter->runtime.renderCB;
        dc.textures[0]        = record.emitter->runtime.texture;
        dc.textures[6]        = record.emitter->runtime.motionVectorTexture;
        dc.textures[7]        = sceneDepth;
        renderer.Submit(dc, resources);
    }
    if (gpuShader.IsValid() && h.particleGpuPSO.IsValid()) {
        for (const GpuParticleDrawRecord& record : g_gpuParticleDrawRecords) {
            const ParticleEmitter& emitter = *record.emitter;
            if (!emitter.runtime.gpuParticleBuffer.IsValid() || record.maxParticles <= 0) continue;
            renderer::DrawCall dc;
            dc.shader             = gpuShader;
            dc.pipelineState      = h.particleGpuPSO;   // ADDITIVE
            dc.constantBuffers[0] = h.frameCB;
            dc.constantBuffers[kParticleConstantSlot] = emitter.runtime.renderCB;
            dc.textures[0]        = emitter.runtime.texture;
            dc.textures[6]        = emitter.runtime.motionVectorTexture;
            dc.textures[7]        = sceneDepth;
            dc.vsBuffers[0]       = emitter.runtime.gpuParticleBuffer;
            dc.vsBuffers[1]       = emitter.runtime.gpuSortBuffer;
            dc.vertexCount        = static_cast<uint32_t>(record.maxParticles) * 6u;
            renderer.Submit(dc, resources);
        }
    }
    // DX12 は別の RT へ切り替えたときに初めて、この RT を読み取り状態へ戻す。TAA はその状態を前提に読む。
    renderer.SetRenderTarget(ctx.Res().Target("HDR"), resources);
    h.particleReactiveValid = true;
}

void ExecuteParticleOverdrawPass(RenderPassContext& ctx)
{
    if (!ctx.settings.particleOverdrawView) return;

    auto& renderer = ctx.renderer;
    auto& resources = ctx.resources;
    auto& h = ctx.handles;
    if (!h.particleIB.IsValid()) return;

    // 計数 RT とシェーダーは診断を有効にしたときだけ作る。
    // resetVersion を見て、デバイスロストや再初期化のあとで作り直す。
    // 計数先はビューが持つ (RenderPassHandles::particleOverdrawRT の WHY)。
    if (!h.particleOverdrawRT) return;
    renderer::SizedRenderTarget& overdrawRT = *h.particleOverdrawRT;
    static renderer::ResourceHandle<renderer::ShaderTag> countShader;
    static renderer::ResourceHandle<renderer::ShaderTag> heatmapShader;
    static std::uint64_t resetVersion = 0;
    if (resetVersion != resources.GetResetVersion()) {
        resetVersion = resources.GetResetVersion();
        countShader = resources.LoadShader("Assets/Shaders/Debug/ParticleOverdraw.hlsl");
        heatmapShader = resources.LoadShader("Assets/Shaders/Debug/OverdrawHeatmap.hlsl");
    }
    if (!countShader.IsValid() || !heatmapShader.IsValid()) return;
    (void)overdrawRT.Ensure(resources, ctx.width, ctx.height, 1);
    if (!overdrawRT.IsValid()) return;

    // 計数: 黒でクリアし、パーティクルを 1 枚あたり R+1 で積む。
    renderer.SetRenderTarget(overdrawRT, resources);
    renderer.Clear({ 0.0f, 0.0f, 0.0f, 1.0f });

    // Particle パスが本番描画に使ったバッファとクワッド数をそのまま数え直す
    // (作り直すと「実際に描いた形」とずれて測る意味がなくなる)。
    // 頂点バッファを持たない GPU シミュレーションとメッシュパーティクルは対象外。
    for (const ParticleDrawRecord& record : g_particleDrawRecords) {
        if (!record.vertexBuffer.IsValid() || record.quadCount <= 0) continue;

        renderer::DrawCall dc;
        dc.vertexBuffer       = record.vertexBuffer;
        dc.indexBuffer        = h.particleIB;
        dc.indexCount         = static_cast<uint32_t>(record.quadCount * 6);
        dc.shader             = countShader;
        dc.pipelineState      = h.particlePSO;   // ADDITIVE + DEPTH_READ
        dc.constantBuffers[0] = h.frameCB;
        dc.constantBuffers[kParticleConstantSlot] = record.emitter->runtime.renderCB;
        dc.textures[0]        = record.emitter->runtime.texture;
        renderer.Submit(dc, resources);
    }

    // ヒートマップ化して HDR へ上書きする。
    renderer.SetRenderTarget(ctx.Res().Target("HDR"), resources);

    // 要求があったフレームだけ、重なり枚数を数値として読み戻す。
    // WHY: ヒートマップは目で見る用で、閾値を持てない。「重なりすぎ」を機械的に言うには
    //      枚数そのものが要る。読み戻しは GPU 同期でフレームを止めるため、常時はやらない。
    // NOTE: RTV を外したあと (SetRenderTarget(hdrRT) の後) に読む。
    //       バインドしたまま CopyResource すると同一サブリソースのハザードになる。
    if (ctx.settings.particleOverdrawReadback) {
        std::vector<float> counts;
        std::uint32_t readWidth = 0;
        std::uint32_t readHeight = 0;
        ParticleOverdrawStats stats;
        if (renderer.CaptureRenderTargetToLinearRGBA(overdrawRT, resources, counts, readWidth, readHeight)
            && readWidth > 0 && readHeight > 0) {
            const std::size_t pixelCount = static_cast<std::size_t>(readWidth) * readHeight;
            double layerSum = 0.0;
            std::size_t coveredCount = 0;
            std::size_t heavyCount = 0;
            float maxLayers = 0.0f;
            for (std::size_t index = 0; index < pixelCount; ++index) {
                // 計数シェーダーは 1 レイヤーにつき R へ 1.0 を加算する (ParticleOverdraw.hlsl)。
                const float layers = counts[index * 4u];
                if (layers < 0.5f) continue;
                ++coveredCount;
                layerSum += layers;
                if (layers >= 5.0f) ++heavyCount;
                maxLayers = (std::max)(maxLayers, layers);
            }
            const auto pixels = static_cast<double>(pixelCount);
            stats.valid = true;
            stats.frame = Time::frameCount;
            stats.coveredRatio = static_cast<float>(static_cast<double>(coveredCount) / pixels);
            stats.meanLayers = coveredCount > 0
                ? static_cast<float>(layerSum / static_cast<double>(coveredCount)) : 0.0f;
            stats.maxLayers = maxLayers;
            stats.heavyRatio = static_cast<float>(static_cast<double>(heavyCount) / pixels);
            stats.overdrawFactor = static_cast<float>(layerSum / pixels);
        }
        SetLastParticleOverdrawStats(stats);
    }

    renderer::DrawCall heatmap;
    heatmap.shader = heatmapShader;
    // Alpha Blendではlayers=0の透明ピクセルに元のモデル描画を残せる。
    // 計数対象はParticleのままなので、モデル自身をOverdraw枚数へ加算はしない。
    heatmap.pipelineState = ctx.settings.particleOverdrawIncludeModels
        ? h.volumetricCloudPSO : h.postprocPSO;
    heatmap.vertexCount = 3;
    heatmap.textures[5] = resources.GetColorTexture(overdrawRT, 0);
    renderer.Submit(heatmap, resources);
}


void ParticlePass::Setup(PassBuilder& builder, const RenderPassContext&) const
{
    builder.Read("DecalDepth").Read("ShadowMap").Read("PunctualShadowMap")
           .Read("LightCookieAtlas").ReadWrite("HDR");
}

void ParticlePass::Execute(PassResources&, RenderPassContext& ctx)
{
    ExecuteParticlePass(ctx);
}

void ParticleOverdrawPass::Setup(PassBuilder& builder, const RenderPassContext&) const
{
    builder.ReadWrite("HDR");
}

bool ParticleOverdrawPass::IsEnabled(const RenderPassContext& ctx) const
{
    return ctx.settings.particleOverdrawView;
}

void ParticleOverdrawPass::Execute(PassResources&, RenderPassContext& ctx)
{
    ExecuteParticleOverdrawPass(ctx);
}

void ParticleReactivePass::Setup(PassBuilder& builder, const RenderPassContext&) const
{
    builder.Read("DecalDepth").ReadWrite("HDR");
}

bool ParticleReactivePass::IsEnabled(const RenderPassContext& ctx) const
{
    return ctx.settings.IsTaaActive();
}

void ParticleReactivePass::Execute(PassResources&, RenderPassContext& ctx)
{
    ExecuteParticleReactivePass(ctx);
}
} // namespace fbzz::scene
