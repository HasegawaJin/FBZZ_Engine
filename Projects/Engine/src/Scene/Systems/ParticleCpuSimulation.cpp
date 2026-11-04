/// @file    ParticleCpuSimulation.cpp
/// @brief   描画回数に依存しない粒子の CPU 更新・スクラブ・ループ検証。
/// @author  Hasegawa Jin
/// @date    2026-10-31
#include <Engine/Scene/Systems/ParticleSimulationRuntime.hpp>
#include "Engine/Scene/Scene.hpp"
#include <Engine/Scene/SceneRenderResources.hpp>
#include <Graphics/Renderer/OwnedResource.hpp>
#include "ParticlePlayback.hpp"
#include "ParticleRandom.hpp"
#include "Engine/Scene/Transform.hpp"
#include "Engine/Scene/Components/ParticleEmitter.hpp"
#include "Engine/Scene/Fields/FlowField.hpp"
#include "Engine/Scene/Components/ParticleGpuSimulation.hpp"
#include "Engine/Scene/Components/MeshRenderer.hpp"
#include "Engine/Scene/Components/AnimatorComponent.hpp"
#include "Engine/Asset/AssetManager.hpp"
#include "Engine/Asset/MaterialAsset.hpp"
#include "Fluid/VectorFieldAsset.hpp"
#include <Engine/Scene/Components/ParticleEmitterSpace.hpp>
#include <Engine/Scene/Fields/FlowFieldEval.hpp>
#include <Engine/Scene/Fields/FlowFieldFrame.hpp>
#include "Engine/Asset/Model.hpp"
#include "Engine/Core/Logger.hpp"
#include "Engine/Core/Time.hpp"
#include <Physics/World.hpp>
#include <Math/MathUtils.hpp>
#include <Math/Vector4.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>


#include "ParticleSimulationHelpers.hpp"
namespace fbzz::scene {
namespace particle_simulation_detail {
using namespace particle_runtime_detail;
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


void PrepareParticleSimulationInputs(ParticleEmitter& emitter)
{
    emitter.settings.duration = (std::max)(emitter.settings.duration, 0.0f);
    emitter.settings.startDelay = (std::max)(emitter.settings.startDelay, 0.0f);
    emitter.settings.sizeCurvePower = (std::max)(emitter.settings.sizeCurvePower, 0.001f);
    emitter.settings.colorCurvePower = (std::max)(emitter.settings.colorCurvePower, 0.001f);
    emitter.settings.flowCoupling = (std::max)(emitter.settings.flowCoupling, 0.0f);
    const std::string path = emitter.settings.materialPath.empty()
        ? std::string(PARTICLE_FALLBACK_MATERIAL) : emitter.settings.materialPath;
    const auto handle = asset::AssetManager::Load<asset::MaterialAsset>(path);
    if (const auto snapshot = asset::ReadMaterialAuthoring(handle)) {
        const auto* mat = &snapshot->Data();
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

            /// @note 見た目一式を .mat から «読む»。settings はシーン保存対象なので書き戻さない
            /// @note («触っていないのに保存内容が変わる» / «Inspector で変えても戻る» が起きる)。
            emitter.runtime.material      = mat->particle;
            emitter.runtime.resolvedBlend = ParticleBlendFromMaterial(mat->blendMode);
            /// @note アトラス分割は 0 だと UV 矩形が発散する。.mat は手書きできるのでここで丸める。
            emitter.runtime.material.flipbook.spriteColumns =
                (std::max)(emitter.runtime.material.flipbook.spriteColumns, 1);
            emitter.runtime.material.flipbook.spriteRows =
                (std::max)(emitter.runtime.material.flipbook.spriteRows, 1);

    } else {
        emitter.runtime.material = asset::ParticleMaterialSettings{};
        emitter.runtime.resolvedBlend = ParticleBlendMode::Additive;
        emitter.runtime.materialTint = {1, 1, 1, 1};
    }
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
        : ParticleSrgbToLinear(math::Vector4::Lerp(particle.startColor, particle.endColor,
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
    if (injected && std::isfinite(injected->lifetime) && injected->lifetime > 0.0f)
        p.lifetime = injected->lifetime;
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
void UpdateParticleBounds(ParticleEmitter& emitter, const Transform& tf)
{
    const auto& settings = emitter.settings;
    const bool localSpace = settings.simulationSpace == ParticleSimulationSpace::Local;
    bool uncertain = false;
    const auto finitePoint = [](const math::Vector3& point) {
        return std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z);
    };
    const auto finiteValue = [&](float value, double fallback = 0.0) {
        if (std::isfinite(value)) return static_cast<double>(value);
        uncertain = true;
        return fallback;
    };
    const auto distance = [&](const math::Vector3& first, const math::Vector3& second) {
        if (!finitePoint(first) || !finitePoint(second)) {
            uncertain = true;
            return 0.0;
        }
        /// @note Subtract in double: finite, distant float positions can overflow Vector3::LengthSq().
        const double x = static_cast<double>(first.x) - second.x;
        const double y = static_cast<double>(first.y) - second.y;
        const double z = static_cast<double>(first.z) - second.z;
        return std::sqrt(x * x + y * y + z * z);
    };
    const auto worldPoint = [&](const math::Vector3& point) {
        return localSpace ? TransformEmitterPoint(tf, point) : point;
    };
    math::Vector3 center = TransformEmitterPoint(tf, settings.emitPosition);
    if (!finitePoint(center)) {
        uncertain = true;
        center = finitePoint(tf.worldPosition) ? tf.worldPosition : math::Vector3{};
    }
    const double padding = (std::max)(finiteValue(settings.culling.cullingBoundsPadding), 0.0);
    const double axisX = (std::max)(finiteValue(settings.sizeAxisScale.x), 0.0001);
    const double axisY = (std::max)(finiteValue(settings.sizeAxisScale.y), 0.0001);
    const double stretchLength = (std::max)(finiteValue(settings.stretchedLengthScale), 0.0);
    const double stretchVelocity = (std::max)(finiteValue(settings.stretchedVelocityScale), 0.0);
    const auto cardRadius = [&](float size, double speed) {
        double stretch = 1.0;
        if (settings.renderMode == ParticleRenderMode::StretchedBillboard && speed > 1.0e-4)
            stretch = stretchLength + speed * stretchVelocity;
        /// @note ParticleCommon rotates a square BEFORE axis scaling; any rotation fits sqrt(2)/2 * max axis.
        return std::abs(finiteValue(size)) * 0.7071067811865476
            * (std::max)(axisX, axisY * stretch);
    };
    double radius = 0.01;
    const auto include = [&](const math::Vector3& point, double extent) {
        radius = (std::max)(radius, distance(point, center) + extent + padding);
    };
    if (!emitter.runtime.particles.empty()) {
        const math::Vector3 first = worldPoint(emitter.runtime.particles.front().position);
        if (finitePoint(first)) center = first;
        else uncertain = true;
        const double widthScale = settings.trail.trailEnabled
            ? (std::max)(1.0, std::abs(finiteValue(settings.trail.trailWidthScale))) : 1.0;
        const int historyLimit = std::clamp(settings.trail.trailPointCount, 1, kMaxParticleTrailPoints);
        for (const Particle& particle : emitter.runtime.particles) {
            const double speed = settings.renderMode == ParticleRenderMode::StretchedBillboard
                ? distance(localSpace ? TransformEmitterVector(tf, particle.velocity) : particle.velocity, {}) : 0.0;
            const double bodyRadius = cardRadius(particle.size, speed);
            double historyRadius = bodyRadius * widthScale;
            if (settings.trail.trailEnabled && settings.trail.trailRibbon) {
                const double configuredWidth = finiteValue(settings.trail.trailRibbonWidth);
                const double width = configuredWidth > 0.0 ? configuredWidth : std::abs(finiteValue(particle.size));
                /// @note Ribbon half width * miter limit 2; BuildRibbonMiterNormals clamps 1/cos to 2.
                historyRadius = width * widthScale;
            }
            include(worldPoint(particle.position), settings.trail.trailEnabled
                ? (std::max)(bodyRadius, historyRadius) : bodyRadius);
            if (!settings.trail.trailEnabled) continue;
            const int used = (std::min)(static_cast<int>(particle.trailCount), historyLimit);
            for (int point = 0; point < used; ++point)
                include(worldPoint(particle.trailPoints[static_cast<std::size_t>(point)]), historyRadius);
        }
    } else {
        /// @note Empty/CPU-unreadable GPU pools need a spawn and travel envelope, including negative Transform scale.
        const double transformScale = (std::max)({std::abs(finiteValue(tf.worldScale.x)),
            std::abs(finiteValue(tf.worldScale.y)), std::abs(finiteValue(tf.worldScale.z))});
        double shapeRadius = 0.0;
        if (settings.shape == ParticleEmitterShape::Sphere) shapeRadius = std::abs(finiteValue(settings.sphereRadius));
        if (settings.shape == ParticleEmitterShape::Cone) shapeRadius = std::abs(finiteValue(settings.coneRadius));
        if (settings.shape == ParticleEmitterShape::Box) shapeRadius = distance(settings.boxExtents, {});
        if (settings.shape == ParticleEmitterShape::MeshSurface) {
            for (const auto& vertex : emitter.runtime.meshShapeVertices)
                shapeRadius = (std::max)(shapeRadius, distance(vertex.position, {}));
        }
        const double lifetime = (std::max)(finiteValue(settings.lifetime), 0.0)
            * (1.0 + std::clamp(finiteValue(settings.lifetimeRandom), 0.0, 1.0));
        const double speed = distance(settings.emitVelocity, {})
            + std::sqrt(3.0) * std::abs(finiteValue(settings.velocitySpread))
            + std::abs(finiteValue(settings.inheritVelocity)) * distance(emitter.runtime.emitterVelocity, {})
            + distance(settings.gravity, {}) * lifetime;
        double travelScale = 1.0;
        if (settings.useVelocityCurve) {
            travelScale = 0.0;
            const auto count = (std::min)(settings.velocityCurve.keyCount,
                static_cast<uint32_t>(settings.velocityCurve.keys.size()));
            for (uint32_t key = 0; key < count; ++key)
                travelScale = (std::max)(travelScale, finiteValue(settings.velocityCurve.keys[key].value));
        }
        const float size = static_cast<float>((std::max)(std::abs(finiteValue(settings.sizeStart)),
            std::abs(finiteValue(settings.sizeEnd))));
        const double positionScale = localSpace ? transformScale : 1.0;
        radius += shapeRadius * transformScale + speed * lifetime * travelScale * positionScale + cardRadius(size, speed) + padding;
        /// @note GPU flow tracking already bounds actual spawn/history positions without a readback; retain that envelope.
        if (emitter.runtime.gpuInitialized && !emitter.runtime.gpuClearPending
            && emitter.runtime.flowMinimumAge < emitter.runtime.flowMaxLifetime) {
            const double flowSpeed = (std::max)(finiteValue(emitter.runtime.flowSpeedBound), speed);
            include(emitter.runtime.flowBoundsCenter, (std::max)(finiteValue(emitter.runtime.flowBoundsRadius), 0.0)
                + cardRadius(size, flowSpeed));
        }
    }
    /// @note Invalid input fails open with a finite sphere, rather than letting NaN silently discard the entire emitter.
    const double maximumRadius = (std::numeric_limits<float>::max)();
    const bool radiusOverflow = !std::isfinite(radius) || radius * 1.00001 + 1.0e-4 > maximumRadius;
    const bool unbounded = uncertain || radiusOverflow;
    emitter.runtime.boundsCenter = radiusOverflow ? math::Vector3{} : center;
    emitter.runtime.boundsRadius = unbounded
        ? static_cast<float>(maximumRadius)
        : static_cast<float>((std::min)(radius * 1.00001 + 1.0e-4, maximumRadius));
}

/// @note world が null なら物理衝突を解決しない。Scene の更新と検証ツールは同じ CPU 経路を使う。
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
}
using namespace particle_simulation_detail;
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
    constexpr int SCRUB_FPS = 60;
    constexpr float FIXED_STEP = 1.0f / static_cast<float>(SCRUB_FPS);
    constexpr float MAX_SCRUB_SECONDS = 60.0f;
    const float target = (std::min)((std::max)(targetTime, 0.0f), MAX_SCRUB_SECONDS);

    const auto flowSnapshot = scene.FlowFrame().fields;
    const std::vector<ActiveFlowField>& flowFields = *flowSnapshot;
    const auto* animator = FindParticleAnimator(gameObject);

    /// @note 累積 float 時刻から最後の刻みを切ると発生量の境界を取りこぼす。整数フレームと端数に分ける。
    int wholeSteps = std::isfinite(target)
        ? static_cast<int>(static_cast<double>(target) * SCRUB_FPS) : 0;
    /// @note 31/60 秒など float に丸めると境界の直前になる値も、同じ整数フレームへ戻す。
    if (target == static_cast<float>(static_cast<double>(wholeSteps + 1) / SCRUB_FPS)) ++wholeSteps;
    const auto simulateStep = [&](float step) {
        (void)StepParticleEmitterPlayback(emitter, gameObject.transform, step);
        SimulateCpuEmitter(emitter, gameObject.transform, animator, step, time,
                           flowFields, &world, scene);
    };
    for (int index = 0; index < wholeSteps; ++index) simulateStep(FIXED_STEP);
    const float remainder = target - static_cast<float>(static_cast<double>(wholeSteps) / SCRUB_FPS);
    if (remainder > 0.0f) simulateStep(remainder);

    /// @note スクラブ後に Prewarm の一括投入や同フレームの通常再生が重なって状態を壊さないようにする。
    emitter.runtime.prewarmed              = true;
    emitter.runtime.lastPlaybackFrame      = Time::frameCount;
    emitter.runtime.lastCpuSimulationFrame = Time::frameCount;
}
}
