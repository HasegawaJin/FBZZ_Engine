// FBZZ Engine
// RenderPasses/ParticlePass.cpp | fbzz::scene
// パーティクル更新 (CPU) と描画
#include "GeometryPasses.hpp"
#include <Engine/Scene/Systems/ParticleSimulationRuntime.hpp>
#include "Engine/Scene/Scene.hpp"
#include "Engine/Scene/Transform.hpp"
#include "Engine/Scene/Components/ParticleEmitter.hpp"
#include "Engine/Scene/Components/ParticleForceField.hpp"
#include "Engine/Scene/Components/AnimatorComponent.hpp"
#include "Engine/Asset/AssetManager.hpp"
#include "Engine/Asset/MaterialAsset.hpp"
#include "Engine/Asset/Model.hpp"
#include "Engine/Core/Time.hpp"
#include "Engine/Renderer/ComputeCall.hpp"
#include "Engine/Renderer/DrawCall.hpp"
#include "Engine/Renderer/RenderState.hpp"
#include <Physics/World.hpp>
#include <Math/Vector4.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>
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

// ─────────────────────────────────────────────────────────────────────
// カールノイズ (乱流ベクトルフィールド)
// 式は ParticleGpuSim.cs.hlsl の同名関数と一致させること (CPU/GPU で挙動を揃える)。
// ─────────────────────────────────────────────────────────────────────

// 整数ハッシュ (PCG 系)。格子点から再現可能な擬似乱数を作る。
inline uint32_t PcgHash(uint32_t x)
{
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

// 格子点 (整数座標) → [-1, 1] の擬似乱数値
inline float LatticeValue(int xi, int yi, int zi)
{
    const uint32_t h = PcgHash(static_cast<uint32_t>(xi) * 73856093u
                             ^ static_cast<uint32_t>(yi) * 19349663u
                             ^ static_cast<uint32_t>(zi) * 83492791u);
    return static_cast<float>(h) * (2.0f / 4294967295.0f) - 1.0f;
}

// 3D 値ノイズ [-1, 1]。8 格子点を smoothstep 重みでトリリニア補間する。
float ValueNoise3D(const math::Vector3& p)
{
    const float fx = std::floor(p.x);
    const float fy = std::floor(p.y);
    const float fz = std::floor(p.z);
    const int xi = static_cast<int>(fx);
    const int yi = static_cast<int>(fy);
    const int zi = static_cast<int>(fz);
    float tx = p.x - fx;
    float ty = p.y - fy;
    float tz = p.z - fz;
    // smoothstep フェード: 格子境界で勾配を連続にする
    tx = tx * tx * (3.0f - 2.0f * tx);
    ty = ty * ty * (3.0f - 2.0f * ty);
    tz = tz * tz * (3.0f - 2.0f * tz);
    const float c000 = LatticeValue(xi,     yi,     zi);
    const float c100 = LatticeValue(xi + 1, yi,     zi);
    const float c010 = LatticeValue(xi,     yi + 1, zi);
    const float c110 = LatticeValue(xi + 1, yi + 1, zi);
    const float c001 = LatticeValue(xi,     yi,     zi + 1);
    const float c101 = LatticeValue(xi + 1, yi,     zi + 1);
    const float c011 = LatticeValue(xi,     yi + 1, zi + 1);
    const float c111 = LatticeValue(xi + 1, yi + 1, zi + 1);
    const float x00 = c000 + (c100 - c000) * tx;
    const float x10 = c010 + (c110 - c010) * tx;
    const float x01 = c001 + (c101 - c001) * tx;
    const float x11 = c011 + (c111 - c011) * tx;
    const float y0 = x00 + (x10 - x00) * ty;
    const float y1 = x01 + (x11 - x01) * ty;
    return y0 + (y1 - y0) * tz;
}

// カールノイズ: 3 成分のベクトルポテンシャル ψ の回転 (∇×ψ) を中心差分で求める。
// WHY: 回転場は発散ゼロのため粒子が一点に溜まらず、煙・炎らしい滑らかな渦を作れる。
math::Vector3 CurlNoise(const math::Vector3& p)
{
    // 各ポテンシャル成分は同じノイズを離れた位置からサンプリングして独立させる
    const math::Vector3 p1 = { p.x + 31.341f, p.y + 31.341f, p.z + 31.341f };
    const math::Vector3 p2 = { p.x - 47.853f, p.y - 47.853f, p.z - 47.853f };
    const math::Vector3 p3 = { p.x + 12.793f, p.y + 12.793f, p.z + 12.793f };
    constexpr float eps = 0.25f;
    constexpr float invTwoEps = 1.0f / (2.0f * eps);
    const math::Vector3 dx = { eps, 0.0f, 0.0f };
    const math::Vector3 dy = { 0.0f, eps, 0.0f };
    const math::Vector3 dz = { 0.0f, 0.0f, eps };
    const float dp1dy = (ValueNoise3D(p1 + dy) - ValueNoise3D(p1 - dy)) * invTwoEps;
    const float dp1dz = (ValueNoise3D(p1 + dz) - ValueNoise3D(p1 - dz)) * invTwoEps;
    const float dp2dx = (ValueNoise3D(p2 + dx) - ValueNoise3D(p2 - dx)) * invTwoEps;
    const float dp2dz = (ValueNoise3D(p2 + dz) - ValueNoise3D(p2 - dz)) * invTwoEps;
    const float dp3dx = (ValueNoise3D(p3 + dx) - ValueNoise3D(p3 - dx)) * invTwoEps;
    const float dp3dy = (ValueNoise3D(p3 + dy) - ValueNoise3D(p3 - dy)) * invTwoEps;
    return { dp3dy - dp2dz, dp1dz - dp3dx, dp2dx - dp1dy };
}

// Turbulence / Noise モジュール共通のサンプル座標。時間スクロールは軸ごとに
// 速度を変え、場全体が一方向へ流れて見えないようにする (HLSL 側と一致)。
inline math::Vector3 TurbulenceSamplePoint(const math::Vector3& position,
                                           float frequency, float speed, float time)
{
    const float scroll = time * speed;
    return { position.x * frequency + scroll,
             position.y * frequency + scroll * 0.35f,
             position.z * frequency + scroll * 0.7f };
}

// ─────────────────────────────────────────────────────────────────────
// 力場 (ParticleForceField) の収集と適用
// ─────────────────────────────────────────────────────────────────────

// 1 フレーム分に収集した力場 1 本 (ワールド空間へ解決済み)
struct ActiveForceField {
    math::Vector3          position;
    float                  radius;
    math::Vector3          direction; // Wind: 風向き / Vortex: 回転軸 (正規化済み)
    float                  strength;
    ParticleForceFieldType type;
    float                  falloffPower;
    float                  noiseFrequency;
    float                  noiseSpeed;
};

// シーンから有効な ParticleForceField を収集しワールド空間へ解決する。
// WHY: エミッターごとに全 GameObject を走査すると O(エミッター数×オブジェクト数) に
//      なるため、パス先頭で 1 回だけ収集して全エミッター (CPU/GPU) で共有する。
std::vector<ActiveForceField> GatherForceFields(Scene& scene, uint32_t cullingMask)
{
    std::vector<ActiveForceField> fields;
    for (auto& go : scene.GameObjects()) {
        if (!ShouldRenderGameObject(go, cullingMask)) continue;
        auto* ff = go.GetComponent<ParticleForceField>();
        if (!ff || !ff->enabled) continue;
        ActiveForceField f;
        f.position = go.transform.worldPosition;
        f.radius   = ff->radius;
        // direction はローカル指定。GameObject を回せば風向き・渦軸も回る。
        const math::Vector3 worldDir = go.transform.worldRotation * ff->direction;
        const float dirLen = worldDir.Length();
        f.direction      = dirLen > 1.0e-4f ? worldDir * (1.0f / dirLen)
                                            : math::Vector3{ 0.0f, 1.0f, 0.0f };
        f.strength       = ff->strength;
        f.type           = ff->fieldType;
        f.falloffPower   = (std::max)(ff->falloffPower, 0.001f);
        f.noiseFrequency = (std::max)(ff->noiseFrequency, 0.0001f);
        f.noiseSpeed     = ff->noiseSpeed;
        fields.push_back(f);
    }

    // WindZone をシーングローバルの風 (+乱流) として力場リストへ追加する。
    // WHY: 草・雲と同じ WindZone 1 つでパーティクルもなびかせるため。
    //      個別に強い風が欲しい場合は従来どおり ParticleForceField(Wind) を置けばよい。
    const ActiveWindZone windZone = FindActiveWindZone(scene);
    if (windZone.active && windZone.strength > 0.0f) {
        ActiveForceField wind{};
        wind.position       = math::Vector3::ZERO;
        wind.radius         = 0.0f; // 無限 (減衰なし)
        wind.direction      = windZone.direction;
        wind.strength       = windZone.strength;
        wind.type           = ParticleForceFieldType::Wind;
        wind.falloffPower   = 1.0f;
        wind.noiseFrequency = 0.5f;
        wind.noiseSpeed     = 1.0f;
        fields.push_back(wind);
    }
    if (windZone.active && windZone.turbulence > 0.0f) {
        ActiveForceField turb{};
        turb.position       = math::Vector3::ZERO;
        turb.radius         = 0.0f;
        turb.direction      = windZone.direction;
        turb.strength       = windZone.turbulence;
        turb.type           = ParticleForceFieldType::Turbulence;
        turb.falloffPower   = 1.0f;
        turb.noiseFrequency = 0.5f;
        turb.noiseSpeed     = windZone.pulseFrequency;
        fields.push_back(turb);
    }
    return fields;
}

std::vector<ActiveForceField> GatherForceFields(RenderPassContext& ctx)
{
    return GatherForceFields(ctx.scene, ctx.cullingMask);
}

// 力場を粒子速度へ適用する。式は ParticleGpuSim.cs.hlsl の ApplyForceFields と一致させること。
void ApplyForceFields(const std::vector<ActiveForceField>& fields,
                      const math::Vector3& position,
                      math::Vector3&       velocity,
                      float dt, float time)
{
    for (const auto& f : fields) {
        const math::Vector3 toParticle = position - f.position;
        float influence = 1.0f;
        if (f.radius > 0.0f) {
            const float dist = toParticle.Length();
            if (dist >= f.radius) continue;
            influence = std::pow(1.0f - dist / f.radius, f.falloffPower);
        }
        const float impulse = f.strength * influence * dt;
        switch (f.type) {
        case ParticleForceFieldType::Wind:
            velocity = velocity + f.direction * impulse;
            break;
        case ParticleForceFieldType::Attract:
        case ParticleForceFieldType::Repulse: {
            const float dist = (std::max)(toParticle.Length(), 1.0e-4f);
            const math::Vector3 dir = toParticle * (1.0f / dist);
            velocity = velocity + dir * (f.type == ParticleForceFieldType::Repulse
                                             ? impulse : -impulse);
            break;
        }
        case ParticleForceFieldType::Vortex: {
            // 軸×粒子方向の外積 = 接線方向。軸周りに回す
            const math::Vector3 tangent = math::Vector3::Cross(f.direction, toParticle);
            const float len = tangent.Length();
            if (len > 1.0e-4f)
                velocity = velocity + tangent * (impulse / len);
            break;
        }
        case ParticleForceFieldType::Turbulence:
            velocity = velocity + CurlNoise(TurbulenceSamplePoint(
                position, f.noiseFrequency, f.noiseSpeed, time)) * impulse;
            break;
        case ParticleForceFieldType::Drag:
            // strength を減衰係数 [1/s] として扱う (velocityDamping と同じ式)
            velocity = velocity * (std::max)(0.0f, 1.0f - impulse);
            break;
        }
    }
}

// エミッター固有ノイズ (Noise モジュール)。式は力場 Turbulence と同一。
void ApplyEmitterNoise(const ParticleEmitter& emitter,
                       const math::Vector3&   position,
                       math::Vector3&         velocity,
                       float dt, float time)
{
    if (emitter.noiseStrength <= 0.0f) return;
    velocity = velocity + CurlNoise(TurbulenceSamplePoint(
        position, emitter.noiseFrequency, emitter.noiseSpeed, time))
        * (emitter.noiseStrength * dt);
}

uint32_t NextParticleRandom(ParticleEmitter& emitter)
{
    // WHAT: Numerical Recipes 系 LCG。軽量で、エミッターごとの seed から決定的な乱数列を作る。
    // WHY: std::rand() はグローバル状態のため、複数エミッターや再生順序で結果が変わりやすい。
    if (emitter.randomState == 0) {
        emitter.randomState = emitter.randomSeed != 0 ? emitter.randomSeed : 1;
    }
    emitter.randomState = emitter.randomState * 1664525u + 1013904223u;
    return emitter.randomState;
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
    const int columns = (std::max)(emitter.spriteColumns, 1);
    const int rows = (std::max)(emitter.spriteRows, 1);
    const int frameCount = columns * rows;
    const int startFrame = std::clamp(emitter.spriteStartFrame, 0, frameCount - 1);
    const int endFrame = std::clamp(
        emitter.spriteEndFrame > 0 ? emitter.spriteEndFrame : frameCount - 1,
        startFrame,
        frameCount - 1);
    const int span = (std::max)(endFrame - startFrame, 0);
    float framePosition = 0.0f;
    bool wrapNext = false;
    switch (emitter.flipbookMode) {
    case ParticleFlipbookMode::FramesPerSecond:
        framePosition = span > 0
            ? std::fmod(ageSeconds * (std::max)(emitter.flipbookFramesPerSecond, 0.0f), static_cast<float>(span + 1))
            : 0.0f;
        wrapNext = true;
        break;
    case ParticleFlipbookMode::RandomFrame:
        framePosition = std::floor(Clamp01(spriteSeed) * static_cast<float>(span));
        break;
    case ParticleFlipbookMode::PingPong: {
        const float cycleLength = static_cast<float>((std::max)(span * 2, 1));
        const float cycleFrame = std::fmod(
            ageSeconds * (std::max)(emitter.flipbookFramesPerSecond, 0.0f), cycleLength);
        framePosition = cycleFrame <= static_cast<float>(span)
            ? cycleFrame : static_cast<float>(span * 2) - cycleFrame;
        break;
    }
    case ParticleFlipbookMode::Lifetime:
    default:
        framePosition = Clamp01(normalizedAge) * static_cast<float>(span);
        break;
    }
    const int relativeFrame = std::clamp(static_cast<int>(std::floor(framePosition)), 0, span);
    int nextRelativeFrame = (std::min)(relativeFrame + 1, span);
    if (wrapNext && relativeFrame == span) nextRelativeFrame = 0;
    SpriteFrameState state;
    state.currentRect = SpriteRectForFrame(startFrame + relativeFrame, columns, rows);
    state.nextRect = SpriteRectForFrame(startFrame + nextRelativeFrame, columns, rows);
    state.blend = emitter.flipbookFrameBlending && emitter.flipbookMode != ParticleFlipbookMode::RandomFrame
        ? framePosition - std::floor(framePosition) : 0.0f;
    return state;
}

math::Vector4 ComputeSpriteRect(const ParticleEmitter& emitter, float normalizedAge,
                                float ageSeconds = 0.0f, float spriteSeed = 0.0f)
{
    return ComputeSpriteFrameState(emitter, normalizedAge, ageSeconds, spriteSeed).currentRect;
}

math::Vector3 TransformEmitterPoint(const Transform& transform, const math::Vector3& localPoint)
{
    // WHY: Transform::position は親基準のローカル座標であり、子 GameObject に Emitter を置くと
    //      親 Player / Bone の移動が反映されない。Particle は描画時点のワールド空間で保持するため、
    //      worldPosition/worldRotation/worldScale から発生点を解決する。
    const math::Vector3 scaledLocal = {
        localPoint.x * transform.worldScale.x,
        localPoint.y * transform.worldScale.y,
        localPoint.z * transform.worldScale.z
    };
    return transform.worldPosition + transform.worldRotation * scaledLocal;
}

math::Vector3 TransformEmitterVector(const Transform& transform, const math::Vector3& localVector)
{
    // WHAT: 速度は位置ではないため平行移動を含めず、Emitter のワールド回転だけを適用する。
    return transform.worldRotation * localVector;
}

math::Vector3 InverseTransformEmitterPoint(const Transform& transform, const math::Vector3& worldPoint)
{
    const math::Vector3 rotated = transform.worldRotation.Inverse() * (worldPoint - transform.worldPosition);
    return {
        std::fabs(transform.worldScale.x) > 1.0e-6f ? rotated.x / transform.worldScale.x : 0.0f,
        std::fabs(transform.worldScale.y) > 1.0e-6f ? rotated.y / transform.worldScale.y : 0.0f,
        std::fabs(transform.worldScale.z) > 1.0e-6f ? rotated.z / transform.worldScale.z : 0.0f
    };
}

math::Vector3 InverseTransformEmitterVector(const Transform& transform, const math::Vector3& worldVector)
{
    return transform.worldRotation.Inverse() * worldVector;
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
    if (emitter.loadedMeshShapePath == emitter.meshShapePath
        && emitter.loadedMeshShapeIndex == emitter.meshShapeIndex)
        return;

    emitter.meshShapeVertices.clear();
    emitter.loadedMeshShapePath = emitter.meshShapePath;
    emitter.loadedMeshShapeIndex = emitter.meshShapeIndex;
    if (emitter.meshShapePath.empty()) return;

    const asset::Model* model = asset::AssetManager::LoadModel(emitter.meshShapePath);
    if (!model) return;

    auto appendMesh = [&](const renderer::Mesh& mesh) {
        if (!mesh.cpuSkinnedVertices.empty()) {
            emitter.meshShapeVertices.reserve(
                emitter.meshShapeVertices.size() + mesh.cpuSkinnedVertices.size());
            for (const renderer::SkinnedVertex& vertex : mesh.cpuSkinnedVertices) {
                MeshShapeVertex cached{};
                cached.position = vertex.position;
                cached.skinned = true;
                for (int influence = 0; influence < 4; ++influence) {
                    cached.boneIndices[influence] = vertex.boneIndices[influence];
                    cached.boneWeights[influence] = vertex.boneWeights[influence];
                }
                emitter.meshShapeVertices.push_back(cached);
            }
        } else {
            emitter.meshShapeVertices.reserve(
                emitter.meshShapeVertices.size() + mesh.cpuVertices.size());
            for (const renderer::Vertex& vertex : mesh.cpuVertices) {
                MeshShapeVertex cached{};
                cached.position = vertex.position;
                emitter.meshShapeVertices.push_back(cached);
            }
        }
    };

    if (emitter.meshShapeIndex >= 0) {
        const size_t meshIndex = static_cast<size_t>(emitter.meshShapeIndex);
        if (meshIndex < model->meshes.size() && model->meshes[meshIndex])
            appendMesh(*model->meshes[meshIndex]);
        return;
    }

    for (const auto& mesh : model->meshes) {
        if (mesh) appendMesh(*mesh);
    }
}

bool SampleMeshShapePoint(ParticleEmitter& emitter, const AnimatorComponent* animator,
                          math::Vector3& outPoint)
{
    EnsureMeshShapePoints(emitter);
    if (emitter.meshShapeVertices.empty()) return false;
    const size_t lastIndex = emitter.meshShapeVertices.size() - 1;
    const size_t index = (std::min)(
        static_cast<size_t>(Random01(emitter) * static_cast<float>(emitter.meshShapeVertices.size())),
        lastIndex);
    const MeshShapeVertex& vertex = emitter.meshShapeVertices[index];
    outPoint = vertex.position;

    // WHAT: GPUスキニングと同じ4ウェイト線形ブレンドをCPU側の発生点にだけ適用する。
    // WHY: 粒子本体はGPUシミュレーションのまま、読み戻しなしで現在のSkinnedAnimationへ追従できる。
    if (emitter.meshShapeFollowSkinnedAnimation && vertex.skinned && animator
        && !animator->boneMatrices.empty()) {
        math::Vector3 skinnedPoint = math::Vector3::ZERO;
        float totalWeight = 0.0f;
        for (int influence = 0; influence < 4; ++influence) {
            const float weight = vertex.boneWeights[influence];
            const size_t boneIndex = static_cast<size_t>(vertex.boneIndices[influence]);
            if (weight <= 0.0f || boneIndex >= animator->boneMatrices.size()) continue;
            const math::Vector4 transformed = animator->boneMatrices[boneIndex]
                * math::Vector4{ vertex.position.x, vertex.position.y, vertex.position.z, 1.0f };
            skinnedPoint = skinnedPoint
                + math::Vector3{ transformed.x, transformed.y, transformed.z } * weight;
            totalWeight += weight;
        }
        if (totalWeight > 0.0001f)
            outPoint = skinnedPoint * (1.0f / totalWeight);
    }
    outPoint = outPoint * emitter.meshShapeScale;
    return true;
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

    static const uint8_t white[4] = { 255, 255, 255, 255 };
    return resources.CreateTexture(white, 1, 1);
}

void EnsureParticleTexture(ParticleEmitter& emitter, renderer::ResourceManager& resources)
{
    if (emitter.motionVectorFlipbook && !emitter.motionVectorTexturePath.empty()
        && (!emitter.motionVectorTexture.IsValid()
            || emitter.loadedMotionVectorTexturePath != emitter.motionVectorTexturePath)) {
        emitter.motionVectorTexture = LoadParticleTextureOrWhite(resources, emitter.motionVectorTexturePath);
        emitter.loadedMotionVectorTexturePath = emitter.motionVectorTexturePath;
    }
    // materialPath が設定されている場合: .mat の albedo テクスチャと blendMode を優先する。
    // WHY: materialPath が単一の描画設定の信頼元になることで、複数エミッターで同じ .mat を共有できる。
    if (!emitter.materialPath.empty()) {
        const bool matChanged = (emitter.loadedMaterialPath != emitter.materialPath);
        if (matChanged) {
            emitter.loadedMaterialPath = emitter.materialPath;
            emitter.loadedTexturePath.clear(); // .mat の変更でテクスチャも再ロードさせる
        }
        const auto matHandle = asset::AssetManager::LoadMaterial(emitter.materialPath);
        if (const auto* mat = asset::AssetManager::GetMaterial(matHandle)) {
            const auto it = mat->textures.find("albedo");
            const std::string& resolvedTex = (it != mat->textures.end()) ? it->second : std::string{};
            if (!emitter.texture.IsValid() || emitter.loadedTexturePath != resolvedTex) {
                emitter.texture = LoadParticleTextureOrWhite(resources, resolvedTex);
                emitter.loadedTexturePath = resolvedTex;
            }
            // blendMode を .mat から上書きする。
            // WHY: materialPath が描画設定の単一の信頼元であるため、Inspector の blendMode より優先する。
            emitter.blendMode = (mat->blendMode == renderer::BlendMode::ALPHA_BLEND)
                                  ? ParticleBlendMode::Alpha : ParticleBlendMode::Additive;
            return;
        }
    }

    // フォールバック: texturePath を直接使用する (materialPath 未設定時の既存挙動を維持)。
    if (emitter.texture.IsValid() && emitter.loadedTexturePath == emitter.texturePath)
        return;
    emitter.texture = LoadParticleTextureOrWhite(resources, emitter.texturePath);
    emitter.loadedTexturePath = emitter.texturePath;
}

void UpdateParticleRenderConstants(ParticleEmitter& emitter,
                                   renderer::ResourceManager& resources,
                                   int maxParticles = 0)
{
    if (!emitter.renderCB.IsValid())
        emitter.renderCB = resources.CreateConstantBuffer(sizeof(ParticleRenderCB));
    ParticleRenderCB cb{};
    cb.renderMode = static_cast<uint32_t>(emitter.renderMode);
    cb.stretchedVelocityScale = (std::max)(emitter.stretchedVelocityScale, 0.0f);
    cb.stretchedLengthScale = (std::max)(emitter.stretchedLengthScale, 0.0f);
    cb.softParticleFadeDistance = (std::max)(emitter.softParticleFadeDistance, 0.001f);
    cb.softParticles = emitter.softParticles ? 1u : 0u;
    cb.maxParticles = static_cast<uint32_t>((std::max)(maxParticles, 0));
    cb.effectsFlags = (emitter.distortion ? 1u : 0u)
        | (emitter.sixWayLighting ? 2u : 0u)
        | (emitter.motionVectorFlipbook && emitter.motionVectorTexture.IsValid() ? 4u : 0u);
    cb.distortionStrength = (std::max)(emitter.distortionStrength, 0.0f);
    cb.lightingStrength = (std::max)(emitter.lightingStrength, 0.0f);
    cb.emissiveScale = (std::max)(emitter.emissiveScale, 0.0f);
    cb.motionVectorStrength = (std::max)(emitter.motionVectorStrength, 0.0f);
    resources.Update(emitter.renderCB, &cb, sizeof(cb));
}

// SubEmitterへイベント数分のBurstを積む。参照切れはVFXの縮退として無視する。
void QueueSubEmitter(Scene& scene, const std::string& objectName, int count)
{
    if (objectName.empty() || count <= 0) return;
    if (GameObject* target = scene.Find(objectName)) {
        if (auto* targetEmitter = target->GetComponent<ParticleEmitter>())
            targetEmitter->burstPending += count;
    }
}

// Particle 1個のPhysics/Plane衝突を解決し、Kill応答ならtrueを返す。
bool ResolveParticleCollision(ParticleEmitter& emitter, Particle& particle,
                              const math::Vector3& nextPosition,
                              const physics::World* world, Scene& scene)
{
    physics::World::RaycastHit hit{};
    bool collided = false;
    math::Vector3 hitPoint = nextPosition;
    math::Vector3 hitNormal = math::Vector3::UP;

    if (emitter.collisionMode == ParticleCollisionMode::Plane) {
        if (nextPosition.y - emitter.collisionRadius <= emitter.collisionPlaneY) {
            collided = true;
            hitPoint = { nextPosition.x, emitter.collisionPlaneY + emitter.collisionRadius, nextPosition.z };
        }
    } else if (emitter.collisionMode == ParticleCollisionMode::Physics && world) {
        const math::Vector3 travel = nextPosition - particle.position;
        const float distance = travel.Length();
        if (distance > 1.0e-5f) {
            const math::Vector3 direction = travel * (1.0f / distance);
            collided = world->SphereCast(particle.position,
                                         (std::max)(emitter.collisionRadius, 0.0f),
                                         direction, distance, hit);
            if (collided) {
                hitPoint = hit.point + hit.normal * emitter.collisionRadius;
                hitNormal = hit.normal;
            }
        }
    }

    if (!collided) {
        particle.position = nextPosition;
        return false;
    }

    ++emitter.collisionCountThisFrame;
    QueueSubEmitter(scene, emitter.collisionSubEmitter, emitter.subEmitterBurstCount);
    if (emitter.collisionResponse == ParticleCollisionResponse::Kill)
        return true;

    particle.position = hitPoint;
    if (emitter.collisionResponse == ParticleCollisionResponse::Stop) {
        particle.velocity = math::Vector3::ZERO;
        return false;
    }

    const float normalVelocity = math::Vector3::Dot(particle.velocity, hitNormal);
    particle.velocity = (particle.velocity - hitNormal * (2.0f * normalVelocity))
        * Clamp01(emitter.collisionBounciness);
    particle.velocity = particle.velocity
        * (std::max)(0.0f, 1.0f - emitter.collisionDamping);
    return false;
}

void SpawnParticle(ParticleEmitter& emitter, const Transform& transform,
                   const AnimatorComponent* animator, float initialAge = 0.0f)
{
    Particle p;
    const bool localSpace = emitter.simulationSpace == ParticleSimulationSpace::Local;
    const auto transformPoint = [&](const math::Vector3& value) {
        return localSpace ? value : TransformEmitterPoint(transform, value);
    };
    const auto transformVector = [&](const math::Vector3& value) {
        return localSpace ? value : TransformEmitterVector(transform, value);
    };
    p.position = transformPoint(emitter.emitPosition);
    math::Vector3 shapeVelocity = math::Vector3::ZERO;

    switch (emitter.shape) {
    case ParticleEmitterShape::Sphere: {
        const math::Vector3 dir = RandomUnitVector(emitter);
        const float radius = (std::max)(emitter.sphereRadius, 0.0f) * std::cbrt(Random01(emitter));
        p.position = p.position + transformVector(dir * radius);
        shapeVelocity = transformVector(dir * emitter.velocitySpread);
        break;
    }
    case ParticleEmitterShape::Cone: {
        constexpr float DEG_TO_RAD = 3.14159265358979323846f / 180.0f;
        const float angle = (std::max)(emitter.coneAngleDegrees, 0.0f) * DEG_TO_RAD;
        const float theta = Random01(emitter) * angle;
        const float phi = Random01(emitter) * 3.14159265358979323846f * 2.0f;
        const float radius = (std::max)(emitter.coneRadius, 0.0f) * std::sqrt(Random01(emitter));
        const math::Vector3 localOffset = {
            std::cos(phi) * radius,
            0.0f,
            std::sin(phi) * radius
        };
        p.position = p.position + transformVector(localOffset);
        const math::Vector3 localShapeVelocity = {
            std::sin(theta) * std::cos(phi) * emitter.velocitySpread,
            std::cos(theta) * emitter.velocitySpread,
            std::sin(theta) * std::sin(phi) * emitter.velocitySpread
        };
        shapeVelocity = transformVector(localShapeVelocity);
        break;
    }
    case ParticleEmitterShape::Box: {
        const math::Vector3 localOffset = {
            RandomSigned01(emitter) * emitter.boxExtents.x,
            RandomSigned01(emitter) * emitter.boxExtents.y,
            RandomSigned01(emitter) * emitter.boxExtents.z
        };
        p.position = p.position + transformVector(localOffset);
        break;
    }
    case ParticleEmitterShape::MeshSurface: {
        math::Vector3 meshPoint;
        if (SampleMeshShapePoint(emitter, animator, meshPoint))
            p.position = transformPoint(emitter.emitPosition + meshPoint);
        break;
    }
    case ParticleEmitterShape::Point:
    default:
        break;
    }

    const float rx = RandomSigned01(emitter) * emitter.velocitySpread;
    const float rz = RandomSigned01(emitter) * emitter.velocitySpread;
    const math::Vector3 localVelocity = {
        emitter.emitVelocity.x + rx,
        emitter.emitVelocity.y,
        emitter.emitVelocity.z + rz
    };
    p.velocity = transformVector(localVelocity);
    p.velocity = p.velocity + shapeVelocity;
    p.color = emitter.colorStart;
    p.size  = emitter.sizeStart;
    p.age   = 0.0f;
    p.rotation = Random01(emitter) * 3.14159265358979323846f * 2.0f;
    p.angularVelocity = emitter.angularVelocityMin
        + (emitter.angularVelocityMax - emitter.angularVelocityMin) * Random01(emitter);
    p.spriteSeed = Random01(emitter);
    p.lifetime = (std::max)(0.001f, emitter.lifetime * (1.0f + RandomSigned01(emitter) * Clamp01(emitter.lifetimeRandom)));
    p.startSize = emitter.sizeStart;
    p.endSize = emitter.sizeEnd;
    p.startColor = emitter.colorStart;
    p.endColor = emitter.colorEnd;
    p.age = (std::max)(0.0f, (std::min)(initialAge, p.lifetime * 0.999f));
    if (p.age > 0.0f) {
        // Prewarmは開始時点の寿命分布を作る。逐次更新を避け、重力下の解析解で初期状態を近似する。
        p.position = p.position + p.velocity * p.age + emitter.gravity * (0.5f * p.age * p.age);
        p.velocity = p.velocity + emitter.gravity * p.age;
        p.rotation += p.angularVelocity * p.age;
        const float normalizedAge = Clamp01(p.age / p.lifetime);
        p.color = emitter.useColorGradient
            ? emitter.colorGradient.Evaluate(normalizedAge)
            : LerpVec4(p.startColor, p.endColor, std::pow(normalizedAge, emitter.colorCurvePower));
        const float sizeT = emitter.useSizeCurve
            ? Clamp01(emitter.sizeCurve.Evaluate(normalizedAge))
            : std::pow(normalizedAge, emitter.sizeCurvePower);
        p.size = p.startSize + (p.endSize - p.startSize) * sizeT;
    }
    const SpriteFrameState sprite = ComputeSpriteFrameState(
        emitter, Clamp01(p.age / p.lifetime), p.age, p.spriteSeed);
    p.uvRect = sprite.currentRect;
    p.nextUvRect = sprite.nextRect;
    p.spriteBlend = sprite.blend;
    emitter.particles.push_back(std::move(p));
}

void ClearEmitterRuntime(ParticleEmitter& emitter)
{
    emitter.particles.clear();
    emitter.emitAccum = 0.0f;
    emitter.prewarmSpawnPending = 0;
    emitter.burstPending = 0;
}

// CPU の SpawnParticle と同じ Shape/Spread ロジックで GpuSpawnEntry を初期化する
void InitGpuSpawnEntry(GpuSpawnEntry& s, ParticleEmitter& emitter, const Transform& tf,
                       const AnimatorComponent* animator)
{
    s.position = TransformEmitterPoint(tf, emitter.emitPosition);
    math::Vector3 shapeVelocity = math::Vector3::ZERO;

    switch (emitter.shape) {
    case ParticleEmitterShape::Sphere: {
        const math::Vector3 dir = RandomUnitVector(emitter);
        const float radius = (std::max)(emitter.sphereRadius, 0.0f) * std::cbrt(Random01(emitter));
        s.position = s.position + TransformEmitterVector(tf, dir * radius);
        shapeVelocity = TransformEmitterVector(tf, dir * emitter.velocitySpread);
        break;
    }
    case ParticleEmitterShape::Cone: {
        constexpr float DEG_TO_RAD = 3.14159265358979323846f / 180.0f;
        const float angle = (std::max)(emitter.coneAngleDegrees, 0.0f) * DEG_TO_RAD;
        const float theta = Random01(emitter) * angle;
        const float phi   = Random01(emitter) * 3.14159265358979323846f * 2.0f;
        const float radius = (std::max)(emitter.coneRadius, 0.0f) * std::sqrt(Random01(emitter));
        const math::Vector3 localOffset = {
            std::cos(phi) * radius,
            0.0f,
            std::sin(phi) * radius
        };
        s.position = s.position + TransformEmitterVector(tf, localOffset);
        const math::Vector3 localShapeVelocity = {
            std::sin(theta) * std::cos(phi) * emitter.velocitySpread,
            std::cos(theta) * emitter.velocitySpread,
            std::sin(theta) * std::sin(phi) * emitter.velocitySpread
        };
        shapeVelocity = TransformEmitterVector(tf, localShapeVelocity);
        break;
    }
    case ParticleEmitterShape::Box: {
        const math::Vector3 localOffset = {
            RandomSigned01(emitter) * emitter.boxExtents.x,
            RandomSigned01(emitter) * emitter.boxExtents.y,
            RandomSigned01(emitter) * emitter.boxExtents.z
        };
        s.position = s.position + TransformEmitterVector(tf, localOffset);
        break;
    }
    case ParticleEmitterShape::MeshSurface: {
        math::Vector3 meshPoint;
        if (SampleMeshShapePoint(emitter, animator, meshPoint))
            s.position = TransformEmitterPoint(tf, emitter.emitPosition + meshPoint);
        break;
    }
    default:
        break;
    }

    const float rx = RandomSigned01(emitter) * emitter.velocitySpread;
    const float rz = RandomSigned01(emitter) * emitter.velocitySpread;
    const math::Vector3 localVelocity = {
        emitter.emitVelocity.x + rx,
        emitter.emitVelocity.y,
        emitter.emitVelocity.z + rz
    };
    s.velocity        = TransformEmitterVector(tf, localVelocity);
    s.velocity        = s.velocity + shapeVelocity;
    s.lifetime        = (std::max)(0.001f, emitter.lifetime
        * (1.0f + RandomSigned01(emitter) * Clamp01(emitter.lifetimeRandom)));
    s.size            = emitter.sizeStart;
    s.colorStart      = emitter.colorStart;
    s.colorEnd        = emitter.colorEnd;
    s.spriteSeed      = Random01(emitter);
    s.uvRect          = ComputeSpriteRect(emitter, 0.0f, 0.0f, s.spriteSeed);
    s.rotation        = Random01(emitter) * 6.28318530717958647692f;
    s.angularVelocity = emitter.angularVelocityMin
        + (emitter.angularVelocityMax - emitter.angularVelocityMin) * Random01(emitter);
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
                    RenderPassContext&                   ctx)
{
    auto& resources = ctx.resources;
    auto& renderer  = ctx.renderer;
    auto& h         = ctx.handles;

    if (!h.particleGpuSimCS.IsValid() || !h.particleGpuShader.IsValid()) return;

    const int maxP = (std::max)(emitter.maxParticles, 1);

    // Clear要求・容量変更時は全スロットを死亡状態で再生成する。
    // WHY: RWStructuredBufferはCPU vectorのclearでは消えず、容量増加後のDispatchは範囲外アクセスになるため。
    if (emitter.gpuClearPending || (emitter.gpuInitialized && emitter.gpuCapacity != static_cast<uint32_t>(maxP))) {
        emitter.gpuInitialized = false;
        emitter.gpuParticleBuffer = {};
        emitter.gpuSpawnBuffer = {};
        emitter.gpuEmitterCB = {};
        emitter.gpuWriteHead = 0;
        emitter.gpuSpawnCount = 0;
        emitter.gpuCapacity = 0;
        emitter.gpuClearPending = false;
    }

    // デバイスリセット (Play Mode 移行など) 後は古いハンドルが無効になるため再初期化する
    const uint64_t currentResetVersion = resources.GetResetVersion();
    if (emitter.gpuInitialized && emitter.gpuResetVersion != currentResetVersion)
    {
        emitter.gpuInitialized  = false;
        emitter.gpuParticleBuffer = {};
        emitter.gpuSpawnBuffer    = {};
        emitter.gpuEmitterCB      = {};
        emitter.gpuWriteHead      = 0;
        emitter.gpuSpawnCount     = 0;
    }

    // バッファ未作成なら初期化 (要素ゼロで確保し CS が age>=lifetime で無視する)
    if (!emitter.gpuInitialized)
    {
        std::vector<GpuParticle> init(static_cast<size_t>(maxP));
        for (auto& p : init) p.age = p.lifetime = 1.0f; // 全粒子を「死亡済み」で初期化
        emitter.gpuParticleBuffer = resources.CreateRWStructuredBuffer(
            init.data(), static_cast<uint32_t>(maxP), sizeof(GpuParticle));

        emitter.gpuSpawnBuffer = resources.CreateStructuredBuffer(
            nullptr, static_cast<uint32_t>(maxP), sizeof(GpuSpawnEntry));

        emitter.gpuEmitterCB = resources.CreateConstantBuffer(sizeof(GpuParticleEmitterCB));
        emitter.gpuWriteHead     = 0;
        emitter.gpuSpawnCount    = 0;
        emitter.gpuResetVersion  = resources.GetResetVersion();
        emitter.gpuCapacity      = static_cast<uint32_t>(maxP);
        emitter.gpuInitialized   = true;
    }

    const bool simulateThisFrame = emitter.lastGpuSimulationFrame != Time::frameCount;
    if (simulateThisFrame) {
        emitter.lastGpuSimulationFrame = Time::frameCount;
    // 今フレームのスポーンエントリを構築
    std::vector<GpuSpawnEntry> spawns;
    if (canEmit || emitter.burstPending > 0)
    {
        const int burstCount = (std::max)(emitter.burstPending, 0);
        emitter.burstPending = 0;
        for (int i = 0; i < burstCount && static_cast<int>(spawns.size()) < maxP; ++i)
        {
            GpuSpawnEntry s;
            InitGpuSpawnEntry(s, emitter, tf, animator);
            spawns.push_back(s);
        }
        if (canEmit)
        {
            emitter.emitAccum += emitter.emitRate * dt;
            while (emitter.emitAccum >= 1.0f && static_cast<int>(spawns.size()) < maxP)
            {
                emitter.emitAccum -= 1.0f;
                GpuSpawnEntry s;
                InitGpuSpawnEntry(s, emitter, tf, animator);
                spawns.push_back(s);
            }
        }
    }
    else
    {
        emitter.emitAccum = 0.0f;
    }

    emitter.gpuSpawnCount = static_cast<uint32_t>(spawns.size());

    // スポーンバッファを CPU → GPU 転送
    if (emitter.gpuSpawnCount > 0)
        resources.Update(emitter.gpuSpawnBuffer, spawns.data(),
                         emitter.gpuSpawnCount * sizeof(GpuSpawnEntry));

    // CS 用定数バッファ更新
    GpuParticleEmitterCB cb{};
    cb.emitterPos      = TransformEmitterPoint(tf, emitter.emitPosition);
    cb.deltaTime       = dt;
    cb.gravity         = emitter.gravity;
    cb.maxParticles    = static_cast<uint32_t>(maxP);
    cb.colorStart      = emitter.colorStart;
    cb.colorEnd        = emitter.colorEnd;
    cb.spawnCount      = emitter.gpuSpawnCount;
    cb.spawnOffset     = emitter.gpuWriteHead;
    cb.colorCurvePower = emitter.colorCurvePower;
    cb.velocityDamping = emitter.velocityDamping;
    cb.sizeStart       = emitter.sizeStart;
    cb.sizeEnd         = emitter.sizeEnd;
    cb.sizeCurvePower  = emitter.sizeCurvePower;
    {
        const int cols       = (std::max)(emitter.spriteColumns, 1);
        const int rows       = (std::max)(emitter.spriteRows, 1);
        const int frameCount = cols * rows;
        const int startFrame = std::clamp(emitter.spriteStartFrame, 0, frameCount - 1);
        const int endFrame   = std::clamp(
            emitter.spriteEndFrame > 0 ? emitter.spriteEndFrame : frameCount - 1,
            startFrame, frameCount - 1);
        cb.spriteColumns    = static_cast<uint32_t>(cols);
        cb.spriteRows       = static_cast<uint32_t>(rows);
        cb.spriteStartFrame = static_cast<uint32_t>(startFrame);
        cb.spriteEndFrame   = static_cast<uint32_t>(endFrame);
    }
    // ノイズモジュール + 力場 (CPU シミュレーションと同じ式を CS 側で適用する)
    cb.time           = time;
    cb.noiseStrength  = (std::max)(emitter.noiseStrength, 0.0f);
    cb.noiseFrequency = (std::max)(emitter.noiseFrequency, 0.0001f);
    cb.noiseSpeed     = emitter.noiseSpeed;
    const int fieldCount = emitter.receiveForceFields
        ? (std::min)(static_cast<int>(forceFields.size()), kMaxGpuForceFields)
        : 0;
    cb.forceFieldCount = static_cast<uint32_t>(fieldCount);
    cb.flipbookMode = static_cast<uint32_t>(emitter.flipbookMode);
    cb.flipbookFramesPerSecond = (std::max)(emitter.flipbookFramesPerSecond, 0.0f);
    for (int i = 0; i < fieldCount; ++i) {
        const ActiveForceField& f = forceFields[static_cast<size_t>(i)];
        cb.forceFields[i].posRadius   = { f.position.x, f.position.y, f.position.z, f.radius };
        cb.forceFields[i].dirStrength = { f.direction.x, f.direction.y, f.direction.z, f.strength };
        cb.forceFields[i].params      = { static_cast<float>(f.type), f.falloffPower,
                                          f.noiseFrequency, f.noiseSpeed };
    }
    cb.curveFlags = {
        emitter.useSizeCurve ? 1.0f : 0.0f,
        emitter.useVelocityCurve ? 1.0f : 0.0f,
        emitter.useColorGradient ? 1.0f : 0.0f,
        emitter.flipbookFrameBlending ? 1.0f : 0.0f
    };
    const auto packCurve = [](const ParticleCurve& curve, size_t first) {
        const size_t last = static_cast<size_t>((std::max)(1u, (std::min)(curve.keyCount, 4u)) - 1u);
        const auto& firstKey = curve.keys[(std::min)(first, last)];
        const auto& secondKey = curve.keys[(std::min)(first + 1u, last)];
        return math::Vector4{
            firstKey.time, firstKey.value, secondKey.time, secondKey.value
        };
    };
    cb.sizeCurveKeys01 = packCurve(emitter.sizeCurve, 0);
    cb.sizeCurveKeys23 = packCurve(emitter.sizeCurve, 2);
    cb.velocityCurveKeys01 = packCurve(emitter.velocityCurve, 0);
    cb.velocityCurveKeys23 = packCurve(emitter.velocityCurve, 2);
    const size_t lastGradientKey = static_cast<size_t>(
        (std::max)(1u, (std::min)(emitter.colorGradient.keyCount, 4u)) - 1u);
    cb.gradientTimes = {
        emitter.colorGradient.keys[0].time,
        emitter.colorGradient.keys[(std::min)(size_t{1}, lastGradientKey)].time,
        emitter.colorGradient.keys[(std::min)(size_t{2}, lastGradientKey)].time,
        emitter.colorGradient.keys[(std::min)(size_t{3}, lastGradientKey)].time
    };
    for (size_t index = 0; index < emitter.colorGradient.keys.size(); ++index)
        cb.gradientColors[index] = emitter.colorGradient.keys[(std::min)(index, lastGradientKey)].color;
    cb.viewProjection = ctx.camera.GetViewProjection();
    cb.screenWidth = static_cast<float>(ctx.width);
    cb.screenHeight = static_cast<float>(ctx.height);
    cb.depthThickness = (std::max)(0.001f, emitter.collisionRadius * 0.002f);
    cb.depthBounciness = std::clamp(emitter.collisionBounciness, 0.0f, 1.0f);
    cb.depthCollision = emitter.collisionMode == ParticleCollisionMode::Depth ? 1u : 0u;
    cb.depthResponse = static_cast<std::uint32_t>(emitter.collisionResponse);
    cb.depthDamping = std::clamp(emitter.collisionDamping, 0.0f, 1.0f);
    resources.Update(emitter.gpuEmitterCB, &cb, sizeof(cb));

    // リングバッファヘッドを進める
    emitter.gpuWriteHead = (emitter.gpuWriteHead + emitter.gpuSpawnCount)
                           % static_cast<uint32_t>(maxP);

    // Dispatch CS
    renderer::ComputeCall cc;
    cc.shader        = h.particleGpuSimCS;
    cc.constantBuffers[0] = emitter.gpuEmitterCB;
    cc.srvBuffers[1] = emitter.gpuSpawnBuffer;   // t15
    cc.srvInputs[7] = resources.GetDepthTexture(h.decalDepthRT);
    cc.uavBuffers[0] = emitter.gpuParticleBuffer; // u2
    cc.dispatchX = (static_cast<uint32_t>(maxP) + 63u) / 64u;
    cc.dispatchY = 1;
    cc.dispatchZ = 1;
    renderer.Dispatch(cc, resources);
    // Dispatch() は OM のレンダーターゲットをアンバインドする。
    // 後続の Draw が正しい HDR RT へ出力されるよう再バインドする。
    renderer.SetRenderTarget(h.hdrRT, resources);
    }

    // SV_VertexID ベース描画: 頂点バッファなし、VS が StructuredBuffer<GpuParticle> を t14 で読む
    const bool alphaBlend = emitter.blendMode == ParticleBlendMode::Alpha || emitter.distortion;
    const auto gpuShader = alphaBlend
        ? h.particleGpuAlphaShader : h.particleGpuShader;
    const auto gpuPSO = alphaBlend
        ? h.particleGpuAlphaPSO : h.particleGpuPSO;
    if (!gpuShader.IsValid() || !gpuPSO.IsValid() || !emitter.texture.IsValid())
        return;

    renderer::DrawCall dc;
    dc.shader        = gpuShader;
    dc.pipelineState = gpuPSO;
    dc.constantBuffers[0] = h.frameCB;
    dc.constantBuffers[2] = emitter.renderCB;
    dc.textures[0]        = emitter.texture;
    dc.textures[5]        = sceneColor;
    dc.textures[6]        = emitter.motionVectorTexture;
    dc.textures[7]        = resources.GetDepthTexture(h.decalDepthRT);
    dc.vsBuffers[0]       = emitter.gpuParticleBuffer; // t14: StructuredBuffer<GpuParticle>
    dc.vertexCount        = static_cast<uint32_t>(maxP) * 6u;
    renderer.SetSampler(0, renderer::SamplerMode::WRAP_BILINEAR);
    renderer.Submit(dc, resources);
}

bool CanUseGpuSimulation(const ParticleEmitter& emitter)
{
    return emitter.simulationMode == ParticleSimulationMode::Gpu
        && emitter.simulationSpace == ParticleSimulationSpace::World
        && (emitter.collisionMode == ParticleCollisionMode::None
            || emitter.collisionMode == ParticleCollisionMode::Depth)
        && emitter.sortMode == ParticleSortMode::None
        && !emitter.prewarm
        && !emitter.flipbookFrameBlending
        && !emitter.motionVectorFlipbook
        && emitter.meshParticlePath.empty()
        && emitter.birthSubEmitter.empty()
        && emitter.deathSubEmitter.empty()
        && emitter.collisionSubEmitter.empty();
}

void UpdateParticleBounds(ParticleEmitter& emitter, const Transform& tf)
{
    // WHAT: 現在の粒子位置とサイズから球Boundsを再計算する。粒子がない間は将来位置を予測して保守的に保持する。
    math::Vector3 center = TransformEmitterPoint(tf, emitter.emitPosition);
    float radius = (std::max)(emitter.sizeStart, emitter.sizeEnd) + emitter.cullingBoundsPadding;
    if (!emitter.particles.empty()) {
        center = emitter.simulationSpace == ParticleSimulationSpace::Local
            ? TransformEmitterPoint(tf, emitter.particles.front().position)
            : emitter.particles.front().position;
        for (const Particle& particle : emitter.particles) {
            const math::Vector3 position = emitter.simulationSpace == ParticleSimulationSpace::Local
                ? TransformEmitterPoint(tf, particle.position) : particle.position;
            radius = (std::max)(radius, (position - center).Length()
                + particle.size + emitter.cullingBoundsPadding);
        }
    } else {
        radius += emitter.emitVelocity.Length() * emitter.lifetime;
    }
    emitter.boundsCenter = center;
    emitter.boundsRadius = (std::max)(radius, 0.01f);
}

void SimulateCpuEmitter(ParticleEmitter& emitter, const Transform& tf,
                        const AnimatorComponent* animator, float dt, float time,
                        const std::vector<ActiveForceField>& forceFields,
                        physics::World& world, Scene& scene)
{
    emitter.lastCpuSimulationFrame = Time::frameCount;
    emitter.collisionCountThisFrame = 0;
    emitter.deathCountThisFrame = 0;
    const int particleCapacity = (std::max)(emitter.maxParticles, 0);
    const bool canEmit = emitter.emitThisFrame;
    if (canEmit || emitter.burstPending > 0) {
        const int burstCount = (std::max)(emitter.burstPending, 0);
        for (int index = 0; index < burstCount
             && static_cast<int>(emitter.particles.size()) < particleCapacity; ++index) {
            const float warmAge = emitter.prewarmSpawnPending > 0
                ? Random01(emitter) * (std::min)(
                    emitter.duration > 0.0f ? emitter.duration : emitter.lifetime,
                    emitter.lifetime)
                : 0.0f;
            SpawnParticle(emitter, tf, animator, warmAge);
            if (emitter.prewarmSpawnPending > 0) --emitter.prewarmSpawnPending;
            QueueSubEmitter(scene, emitter.birthSubEmitter, emitter.subEmitterBurstCount);
        }
        emitter.burstPending = 0;

        if (static_cast<int>(emitter.particles.size()) >= particleCapacity) {
            emitter.emitAccum = 0.0f;
        } else if (canEmit) {
            emitter.emitAccum += emitter.emitRate * emitter.lodRateScale * dt;
        }
        while (emitter.emitAccum >= 1.0f
               && static_cast<int>(emitter.particles.size()) < particleCapacity) {
            emitter.emitAccum -= 1.0f;
            SpawnParticle(emitter, tf, animator);
            QueueSubEmitter(scene, emitter.birthSubEmitter, emitter.subEmitterBurstCount);
        }
        if (static_cast<int>(emitter.particles.size()) >= particleCapacity)
            emitter.emitAccum = 0.0f;
    } else {
        emitter.emitAccum = 0.0f;
        emitter.burstPending = 0;
    }

    for (auto it = emitter.particles.begin(); it != emitter.particles.end();) {
        it->age += dt;
        if (it->age >= it->lifetime) {
            ++emitter.deathCountThisFrame;
            QueueSubEmitter(scene, emitter.deathSubEmitter, emitter.subEmitterBurstCount);
            it = emitter.particles.erase(it);
            continue;
        }
        const float normalizedAge = Clamp01(it->age / (std::max)(it->lifetime, 0.001f));
        it->velocity = (it->velocity + emitter.gravity * dt)
            * (std::max)(0.0f, 1.0f - emitter.velocityDamping * dt);
        if (emitter.simulationSpace == ParticleSimulationSpace::Local) {
            math::Vector3 worldPosition = TransformEmitterPoint(tf, it->position);
            math::Vector3 worldVelocity = TransformEmitterVector(tf, it->velocity);
            if (emitter.receiveForceFields)
                ApplyForceFields(forceFields, worldPosition, worldVelocity, dt, time);
            ApplyEmitterNoise(emitter, worldPosition, worldVelocity, dt, time);
            it->velocity = InverseTransformEmitterVector(tf, worldVelocity);
        } else {
            if (emitter.receiveForceFields)
                ApplyForceFields(forceFields, it->position, it->velocity, dt, time);
            ApplyEmitterNoise(emitter, it->position, it->velocity, dt, time);
        }
        const float velocityScale = emitter.useVelocityCurve
            ? (std::max)(emitter.velocityCurve.Evaluate(normalizedAge), 0.0f) : 1.0f;
        const math::Vector3 nextPosition = it->position + it->velocity * (dt * velocityScale);
        bool killedByCollision = false;
        if (emitter.simulationSpace == ParticleSimulationSpace::Local
            && emitter.collisionMode != ParticleCollisionMode::None) {
            Particle worldParticle = *it;
            worldParticle.position = TransformEmitterPoint(tf, it->position);
            worldParticle.velocity = TransformEmitterVector(tf, it->velocity);
            killedByCollision = ResolveParticleCollision(
                emitter, worldParticle, TransformEmitterPoint(tf, nextPosition), &world, scene);
            it->position = InverseTransformEmitterPoint(tf, worldParticle.position);
            it->velocity = InverseTransformEmitterVector(tf, worldParticle.velocity);
        } else {
            killedByCollision = ResolveParticleCollision(emitter, *it, nextPosition, &world, scene);
        }
        if (killedByCollision) {
            it = emitter.particles.erase(it);
            continue;
        }
        it->rotation += it->angularVelocity * dt;
        const SpriteFrameState sprite = ComputeSpriteFrameState(
            emitter, normalizedAge, it->age, it->spriteSeed);
        it->uvRect = sprite.currentRect;
        it->nextUvRect = sprite.nextRect;
        it->spriteBlend = sprite.blend;
        it->color = emitter.useColorGradient
            ? emitter.colorGradient.Evaluate(normalizedAge)
            : LerpVec4(it->startColor, it->endColor,
                std::pow(normalizedAge, emitter.colorCurvePower));
        const float sizeT = emitter.useSizeCurve
            ? Clamp01(emitter.sizeCurve.Evaluate(normalizedAge))
            : std::pow(normalizedAge, emitter.sizeCurvePower);
        it->size = it->startSize + (it->endSize - it->startSize) * sizeT;
        ++it;
    }
    UpdateParticleBounds(emitter, tf);
}

} // anonymous namespace

void UpdateParticleCpuSimulation(Scene& scene, physics::World& world, float deltaTime, float time)
{
    const std::vector<ActiveForceField> forceFields = GatherForceFields(scene, ~0u);
    for (EntityID id : scene.GetEntities<ParticleEmitter>()) {
        auto* emitter = scene.GetComponent<ParticleEmitter>(id);
        GameObject* gameObject = scene.GetGameObject(id);
        if (!emitter || !gameObject || !emitter->enabled || CanUseGpuSimulation(*emitter)) continue;
        // GPU指定を一時的にCPUへ縮退した場合、制約解除後に古いGPU粒子が復活しないよう履歴を破棄する。
        if (emitter->simulationMode == ParticleSimulationMode::Gpu)
            emitter->gpuClearPending = true;
        if (emitter->lastCpuSimulationFrame == Time::frameCount) continue;
        const auto* animator = FindParticleAnimator(*gameObject);
        // VFX Editor のプレビュー速度 (一時停止 / スロー / 倍速) をエミッター単位で dt へ注入する。
        const float scaledDt = deltaTime * emitter->GetEditorTimeScale(Time::frameCount);
        SimulateCpuEmitter(*emitter, gameObject->transform, animator, scaledDt, time,
                           forceFields, world, scene);
    }
}

void ScrubParticleEmitterForEditor(Scene& scene, physics::World& world,
                                   GameObject& gameObject, ParticleEmitter& emitter,
                                   float targetTime, float time)
{
    // 巻き戻し。randomState が randomSeed に戻るため、同じ targetTime へのスクラブは常に同じ結果になる。
    emitter.ResetPlayback();

    // GPU 粒子は CS 側バッファの履歴を任意時刻へ巻き戻せないため、リスタートのみで返す。
    if (CanUseGpuSimulation(emitter)) {
        emitter.prewarmed = true; // 直後の Prewarm 一括投入でスクラブ結果が壊れないようにする
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
        // WHY: ParticleSimulationSystem::Update の再生規則をステップ単位で再現しないと、
        //      Burst の発火タイミングやループ巻き戻しがリアルタイム再生とずれてしまう。
        bool canEmit = emitter.playing;
        if (canEmit && emitter.delayTime < emitter.startDelay) {
            emitter.delayTime += step;
            canEmit = false;
        }
        if (canEmit && emitter.duration > 0.0f) {
            emitter.playTime += step;
            if (emitter.playTime >= emitter.duration) {
                if (emitter.loop) {
                    emitter.playTime = 0.0f;
                    emitter.delayTime = 0.0f;
                    emitter.burstCyclesFired.clear();
                } else {
                    emitter.playing = false;
                    canEmit = false;
                    if (emitter.clearOnStop) {
                        emitter.particles.clear();
                        emitter.gpuClearPending = true;
                    }
                }
            }
        }
        emitter.emitThisFrame = canEmit;

        if (emitter.burstCyclesFired.size() != emitter.bursts.size())
            emitter.burstCyclesFired.assign(emitter.bursts.size(), 0);
        for (size_t index = 0; index < emitter.bursts.size(); ++index) {
            const ParticleBurst& burst = emitter.bursts[index];
            int& fired = emitter.burstCyclesFired[index];
            const int cycles = (std::max)(burst.cycles, 1);
            while (fired < cycles
                   && emitter.playTime >= burst.time + burst.interval * static_cast<float>(fired)) {
                if (Random01(emitter) <= Clamp01(burst.probability))
                    emitter.burstPending += (std::max)(burst.count, 0);
                ++fired;
            }
        }

        SimulateCpuEmitter(emitter, gameObject.transform, animator, step, time,
                           forceFields, world, scene);
        simulated += step;
    }

    // スクラブ後に Prewarm の一括投入や同フレームの通常再生が重なって状態を壊さないようにする。
    emitter.prewarmed              = true;
    emitter.lastPlaybackFrame      = Time::frameCount;
    emitter.lastCpuSimulationFrame = Time::frameCount;
}

void ExecuteParticlePass(RenderPassContext& ctx)
{
    auto& renderer  = ctx.renderer;
    auto& resources = ctx.resources;
    auto& h         = ctx.handles;

    if (!h.particleShader.IsValid() || !h.particleVB.IsValid() || !h.particleIB.IsValid()) return;

    const float dt   = Time::deltaTime;
    const float time = Time::time;

    // Distortionは現在のHDRを読みながら同じHDRへ書けないため、Particle描画前に背景を専用RTへ退避する。
    renderer::ResourceHandle<renderer::TextureTag> particleSceneColor;
    const bool needsSceneColor = std::any_of(ctx.scene.GameObjects().begin(), ctx.scene.GameObjects().end(),
        [](GameObject& object) {
            const auto* emitter = object.GetComponent<ParticleEmitter>();
            return emitter != nullptr && emitter->enabled && emitter->distortion;
        });
    if (needsSceneColor) {
        static renderer::ResourceHandle<renderer::RenderTargetTag> sceneColorRT;
        static std::uint64_t resetVersion = 0;
        static std::uint32_t sceneColorWidth = 0;
        static std::uint32_t sceneColorHeight = 0;
        static renderer::ResourceHandle<renderer::ShaderTag> copyShader;
        if (resetVersion != resources.GetResetVersion()) {
            resetVersion = resources.GetResetVersion();
            sceneColorRT = {}; sceneColorWidth = 0; sceneColorHeight = 0;
            copyShader = resources.LoadShader("Assets/Shaders/PostProcess/Color/CopyColor.hlsl");
        }
        if (!sceneColorRT.IsValid() || sceneColorWidth != ctx.width || sceneColorHeight != ctx.height) {
            if (sceneColorRT.IsValid()) resources.Release(sceneColorRT);
            sceneColorRT = resources.CreateRenderTarget(ctx.width, ctx.height, 1);
            sceneColorWidth = ctx.width; sceneColorHeight = ctx.height;
        }
        renderer.SetRenderTarget(sceneColorRT, resources);
        if (copyShader.IsValid()) {
            renderer::DrawCall copy;
            copy.shader = copyShader; copy.pipelineState = h.postprocPSO; copy.vertexCount = 3;
            copy.textures[5] = resources.GetColorTexture(h.hdrRT, 0);
            renderer.Submit(copy, resources);
        }
        particleSceneColor = resources.GetColorTexture(sceneColorRT, 0);
        renderer.SetRenderTarget(h.hdrRT, resources);
    }

    // シーン内の力場を 1 回だけ収集し、全エミッター (CPU/GPU) で共有する
    const std::vector<ActiveForceField> forceFields = GatherForceFields(ctx);
    int particleBudget = ctx.settings.particleBudgetEnabled
        ? (std::max)(ctx.settings.particleBudget, 0) : 0;

    for (auto& go : ctx.scene.GameObjects()) {
        if (!ShouldRenderGameObject(go, ctx.cullingMask)) continue;
        auto* emitter = go.GetComponent<ParticleEmitter>();
        if (!emitter || !emitter->enabled) continue;
        const auto* animator = FindParticleAnimator(go);
        const Transform& tf = go.transform;
        ++ctx.statsParticleEmitters;

        // VFX Editor のプレビュー速度 (一時停止 / スロー / 倍速)。ゲーム実行時は常に 1.0。
        const float dtEmitter = dt * emitter->GetEditorTimeScale(Time::frameCount);

        emitter->duration = (std::max)(emitter->duration, 0.0f);
        emitter->startDelay = (std::max)(emitter->startDelay, 0.0f);
        emitter->spriteColumns = (std::max)(emitter->spriteColumns, 1);
        emitter->spriteRows = (std::max)(emitter->spriteRows, 1);
        emitter->sizeCurvePower = (std::max)(emitter->sizeCurvePower, 0.001f);
        emitter->colorCurvePower = (std::max)(emitter->colorCurvePower, 0.001f);
        emitter->velocityDamping = (std::max)(emitter->velocityDamping, 0.0f);
        UpdateParticleBounds(*emitter, tf);
        const float cameraDistance = (emitter->boundsCenter - ctx.camera.m_position).Length();
        const float coverage = emitter->boundsRadius / (std::max)(cameraDistance, 0.001f);
        const bool frustumVisible = !ctx.cameraFrustum
            || ctx.cameraFrustum->IntersectsSphere(emitter->boundsCenter, emitter->boundsRadius);
        const bool coverageVisible = emitter->screenCoverageThreshold <= 0.0f
            || coverage >= emitter->screenCoverageThreshold;
        emitter->isCulledThisFrame = emitter->cullingEnabled && (!frustumVisible || !coverageVisible);
        if (emitter->isCulledThisFrame) {
            ++ctx.statsParticleCulled;
            emitter->visibleParticleCount = 0;
            if (emitter->pauseWhenCulled) continue;
            continue;
        }
        if (emitter->lodEnabled && emitter->lodFarDistance > emitter->lodNearDistance) {
            const float alpha = Clamp01((cameraDistance - emitter->lodNearDistance)
                / (emitter->lodFarDistance - emitter->lodNearDistance));
            emitter->lodRateScale = emitter->lodNearRateScale
                + (emitter->lodFarRateScale - emitter->lodNearRateScale) * alpha;
        } else {
            emitter->lodRateScale = 1.0f;
        }
        EnsureParticleTexture(*emitter, resources);
        // Physics Query、Local Space、厳密な透過ソートはCPU側で解決し、見た目の正しさを優先する。
        const bool isGpuMode = CanUseGpuSimulation(*emitter);

        bool canEmit = emitter->emitThisFrame;
        if (emitter->lastPlaybackFrame != Time::frameCount) {
            emitter->lastPlaybackFrame = Time::frameCount;
            canEmit = emitter->playing;
            if (canEmit && emitter->delayTime < emitter->startDelay) {
                emitter->delayTime += dtEmitter;
                canEmit = false;
            }
            if (canEmit && emitter->duration > 0.0f) {
                emitter->playTime += dtEmitter;
                if (emitter->playTime >= emitter->duration) {
                    if (emitter->loop) {
                        emitter->playTime = 0.0f;
                        emitter->delayTime = 0.0f;
                        emitter->burstCyclesFired.clear();
                        canEmit = false;
                    } else {
                        emitter->playing = false;
                        canEmit = false;
                        if (emitter->clearOnStop) {
                            ClearEmitterRuntime(*emitter);
                            emitter->gpuClearPending = true;
                        }
                    }
                }
            }
            emitter->emitThisFrame = canEmit;

            // 距離Emission: Emitterのワールド移動量を粒子数へ変換する。
            const math::Vector3 emitterWorldPosition = TransformEmitterPoint(tf, emitter->emitPosition);
            if (emitter->hasLastEmitterPosition && canEmit && emitter->rateOverDistance > 0.0f) {
                emitter->distanceEmitAccum += (emitterWorldPosition - emitter->lastEmitterPosition).Length()
                    * emitter->rateOverDistance;
                const int distanceCount = static_cast<int>(emitter->distanceEmitAccum);
                if (distanceCount > 0) {
                    emitter->burstPending += distanceCount;
                    emitter->distanceEmitAccum -= static_cast<float>(distanceCount);
                }
            }
            emitter->lastEmitterPosition = emitterWorldPosition;
            emitter->hasLastEmitterPosition = true;

            // 時刻指定Burst。loop時はplayTimeの巻き戻しでcycle状態もリセットされる。
            if (emitter->burstCyclesFired.size() != emitter->bursts.size())
                emitter->burstCyclesFired.assign(emitter->bursts.size(), 0);
            for (size_t burstIndex = 0; burstIndex < emitter->bursts.size(); ++burstIndex) {
                const ParticleBurst& burst = emitter->bursts[burstIndex];
                int& fired = emitter->burstCyclesFired[burstIndex];
                const int cycles = (std::max)(burst.cycles, 1);
                while (fired < cycles
                       && emitter->playTime >= burst.time + burst.interval * static_cast<float>(fired)) {
                    if (Random01(*emitter) <= Clamp01(burst.probability))
                        emitter->burstPending += (std::max)(burst.count, 0);
                    ++fired;
                }
            }

            // Prewarmは初回描画前に定常個数を投入する。GPU readbackを避けるため履歴位置は近似する。
            if (emitter->prewarm && emitter->loop && !emitter->prewarmed) {
                const float warmDuration = emitter->duration > 0.0f ? emitter->duration : emitter->lifetime;
                const int warmCount = static_cast<int>((std::max)(emitter->emitRate, 0.0f)
                    * (std::max)(warmDuration, 0.0f));
                const int clampedWarmCount = (std::min)(warmCount, (std::max)(emitter->maxParticles, 0));
                emitter->burstPending += clampedWarmCount;
                emitter->prewarmSpawnPending += clampedWarmCount;
                emitter->prewarmed = true;
            }
        }

        // GPU モードは CPU スポーン/更新をスキップして GPU パスへ
        if (isGpuMode) {
            const int requested = (std::max)(emitter->maxParticles, 0);
            const int drawLimit = particleBudget > 0 ? (std::min)(requested, particleBudget) : requested;
            emitter->visibleParticleCount = drawLimit;
            if (particleBudget > 0) particleBudget -= drawLimit;
            if (requested > drawLimit) ++ctx.statsParticleBudgetDropped;
            if (drawLimit <= 0) continue;
            EnsureParticleTexture(*emitter, resources);
            UpdateParticleRenderConstants(*emitter, resources, drawLimit);
            ctx.statsParticleVisible += drawLimit;
            TickGpuEmitter(*emitter, tf, animator, dtEmitter, time, canEmit, forceFields, particleSceneColor, ctx);
            continue;
        }

        const bool simulateCpuThisFrame = emitter->lastCpuSimulationFrame != Time::frameCount;
        if (simulateCpuThisFrame) {
        emitter->lastCpuSimulationFrame = Time::frameCount;
        emitter->collisionCountThisFrame = 0;
        emitter->deathCountThisFrame = 0;
        // パーティクル生成
        const int particleCapacity = (std::max)(emitter->maxParticles, 0);
        const bool hasBurst = emitter->burstPending > 0;
        if (canEmit || hasBurst) {
            const int burstCount = (std::max)(emitter->burstPending, 0);
            for (int i = 0; i < burstCount && static_cast<int>(emitter->particles.size()) < particleCapacity; ++i)
            {
                const float warmAge = emitter->prewarmSpawnPending > 0
                    ? Random01(*emitter) * (std::min)(
                        emitter->duration > 0.0f ? emitter->duration : emitter->lifetime,
                        emitter->lifetime)
                    : 0.0f;
                SpawnParticle(*emitter, tf, animator, warmAge);
                if (emitter->prewarmSpawnPending > 0) --emitter->prewarmSpawnPending;
                QueueSubEmitter(ctx.scene, emitter->birthSubEmitter, emitter->subEmitterBurstCount);
            }
            emitter->burstPending = 0;

            const int currentCount = static_cast<int>(emitter->particles.size());
            if (currentCount >= particleCapacity) {
                // WHY: 満杯中に emitAccum を積み続けると、寿命切れ直後に未放出分がまとめて出て不自然になる。
                //      生成できなかった分は破棄し、次フレーム以降の通常レートに戻す。
                emitter->emitAccum = 0.0f;
            } else if (canEmit) {
                emitter->emitAccum += emitter->emitRate * emitter->lodRateScale * dtEmitter;
            }
            while (emitter->emitAccum >= 1.0f
                   && static_cast<int>(emitter->particles.size()) < particleCapacity)
            {
                emitter->emitAccum -= 1.0f;
                SpawnParticle(*emitter, tf, animator);
                QueueSubEmitter(ctx.scene, emitter->birthSubEmitter, emitter->subEmitterBurstCount);
            }
            if (static_cast<int>(emitter->particles.size()) >= particleCapacity) {
                // WHY: 生成ループの途中で上限に達した場合も、残った未放出分を次フレームへ持ち越さない。
                emitter->emitAccum = 0.0f;
            }
        } else {
            emitter->emitAccum = 0.0f;
            emitter->burstPending = 0;
        }

        // パーティクル更新・寿命切れを削除
        for (auto it = emitter->particles.begin(); it != emitter->particles.end(); ) {
            it->age += dtEmitter;
            if (it->age >= it->lifetime) {
                ++emitter->deathCountThisFrame;
                QueueSubEmitter(ctx.scene, emitter->deathSubEmitter, emitter->subEmitterBurstCount);
                it = emitter->particles.erase(it);
                continue;
            }
            float t = Clamp01(it->age / (std::max)(it->lifetime, 0.001f));
            it->velocity.x += emitter->gravity.x * dtEmitter;
            it->velocity.y += emitter->gravity.y * dtEmitter;
            it->velocity.z += emitter->gravity.z * dtEmitter;
            const float damping = (std::max)(0.0f, 1.0f - emitter->velocityDamping * dtEmitter);
            it->velocity = it->velocity * damping;
            // ベクトルフィールド: シーンの力場 + エミッター固有ノイズを速度へ加算
            if (emitter->simulationSpace == ParticleSimulationSpace::Local) {
                math::Vector3 worldPosition = TransformEmitterPoint(tf, it->position);
                math::Vector3 worldVelocity = TransformEmitterVector(tf, it->velocity);
                if (emitter->receiveForceFields)
                    ApplyForceFields(forceFields, worldPosition, worldVelocity, dtEmitter, time);
                ApplyEmitterNoise(*emitter, worldPosition, worldVelocity, dtEmitter, time);
                it->velocity = InverseTransformEmitterVector(tf, worldVelocity);
            } else {
                if (emitter->receiveForceFields)
                    ApplyForceFields(forceFields, it->position, it->velocity, dtEmitter, time);
                ApplyEmitterNoise(*emitter, it->position, it->velocity, dtEmitter, time);
            }
            // GPU CS と同じ半陽的Euler順序: 加速度・減衰・力場を速度へ反映してから位置を進める。
            const float velocityScale = emitter->useVelocityCurve
                ? (std::max)(emitter->velocityCurve.Evaluate(t), 0.0f) : 1.0f;
            const math::Vector3 nextPosition = it->position + it->velocity * (dtEmitter * velocityScale);
            bool killedByCollision = false;
            if (emitter->simulationSpace == ParticleSimulationSpace::Local
                && emitter->collisionMode != ParticleCollisionMode::None) {
                Particle worldParticle = *it;
                worldParticle.position = TransformEmitterPoint(tf, it->position);
                worldParticle.velocity = TransformEmitterVector(tf, it->velocity);
                killedByCollision = ResolveParticleCollision(
                    *emitter, worldParticle, TransformEmitterPoint(tf, nextPosition),
                    ctx.physicsWorld, ctx.scene);
                it->position = InverseTransformEmitterPoint(tf, worldParticle.position);
                it->velocity = InverseTransformEmitterVector(tf, worldParticle.velocity);
            } else {
                killedByCollision = ResolveParticleCollision(
                    *emitter, *it, nextPosition, ctx.physicsWorld, ctx.scene);
            }
            if (killedByCollision) {
                it = emitter->particles.erase(it);
                continue;
            }
            it->rotation += it->angularVelocity * dtEmitter;
            const SpriteFrameState sprite = ComputeSpriteFrameState(*emitter, t, it->age, it->spriteSeed);
            it->uvRect = sprite.currentRect;
            it->nextUvRect = sprite.nextRect;
            it->spriteBlend = sprite.blend;
            it->color = emitter->useColorGradient
                ? emitter->colorGradient.Evaluate(t)
                : LerpVec4(it->startColor, it->endColor, std::pow(t, emitter->colorCurvePower));
            const float sizeT = emitter->useSizeCurve
                ? Clamp01(emitter->sizeCurve.Evaluate(t))
                : std::pow(t, emitter->sizeCurvePower);
            it->size = it->startSize + (it->endSize - it->startSize) * sizeT;
            ++it;
        }
        }

        const int available = static_cast<int>(emitter->particles.size());
        const int countBudget = particleBudget > 0 ? (std::min)(available, particleBudget) : available;
        const int count = std::min(countBudget, kMaxParticleDraw);
        emitter->visibleParticleCount = count;
        if (particleBudget > 0) particleBudget -= count;
        if (available > count) ++ctx.statsParticleBudgetDropped;
        ctx.statsParticleVisible += count;
        if (count == 0) continue;
        // MeshTrail passが同じCPU粒子列を静的Meshとして描く。billboardとの二重描画を避ける。
        if (!emitter->meshParticlePath.empty()) continue;
        UpdateParticleRenderConstants(*emitter, resources, count);
        if (emitter->sortMode == ParticleSortMode::BackToFront) {
            const math::Vector3 cameraPos = ctx.camera.m_position;
            std::sort(emitter->particles.begin(), emitter->particles.end(),
                [cameraPos, &tf, emitter](const Particle& a, const Particle& b) {
                    const math::Vector3 aPosition = emitter->simulationSpace == ParticleSimulationSpace::Local
                        ? TransformEmitterPoint(tf, a.position) : a.position;
                    const math::Vector3 bPosition = emitter->simulationSpace == ParticleSimulationSpace::Local
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
        for (int i = 0; i < count; ++i) {
            const auto& p = emitter->particles[i];
            const math::Vector3 renderPosition = emitter->simulationSpace == ParticleSimulationSpace::Local
                ? TransformEmitterPoint(tf, p.position) : p.position;
            for (int c = 0; c < 4; ++c) {
                ParticleVertex v;
                v.center[0] = renderPosition.x;
                v.center[1] = renderPosition.y;
                v.center[2] = renderPosition.z;
                v.uv[0]     = kUV[c][0];
                v.uv[1]     = kUV[c][1];
                v.color[0]  = p.color.x;
                v.color[1]  = p.color.y;
                v.color[2]  = p.color.z;
                v.color[3]  = p.color.w;
                v.size      = p.size;
                v.rotation  = p.rotation;
                v.uvRect[0] = p.uvRect.x;
                v.uvRect[1] = p.uvRect.y;
                v.uvRect[2] = p.uvRect.z;
                v.uvRect[3] = p.uvRect.w;
                const math::Vector3 renderVelocity = emitter->simulationSpace == ParticleSimulationSpace::Local
                    ? TransformEmitterVector(tf, p.velocity) : p.velocity;
                v.velocity[0] = renderVelocity.x;
                v.velocity[1] = renderVelocity.y;
                v.velocity[2] = renderVelocity.z;
                v.nextUvRect[0] = p.nextUvRect.x;
                v.nextUvRect[1] = p.nextUvRect.y;
                v.nextUvRect[2] = p.nextUvRect.z;
                v.nextUvRect[3] = p.nextUvRect.w;
                v.spriteBlend = p.spriteBlend;
                verts.push_back(v);
            }
        }

        resources.Update(h.particleVB, verts.data(),
                         static_cast<uint32_t>(verts.size() * sizeof(ParticleVertex)));

        const auto particlePSO = (emitter->blendMode == ParticleBlendMode::Alpha || emitter->distortion)
            ? h.particleAlphaPSO : h.particlePSO;
        if (!particlePSO.IsValid() || !emitter->texture.IsValid())
            continue;

        renderer::DrawCall dc;
        dc.vertexBuffer       = h.particleVB;
        dc.indexBuffer        = h.particleIB;
        dc.indexCount         = static_cast<uint32_t>(count * 6);
        dc.shader             = h.particleShader;
        dc.pipelineState      = particlePSO;
        dc.constantBuffers[0] = h.frameCB;
        dc.constantBuffers[2] = emitter->renderCB;
        dc.textures[0]        = emitter->texture;
        dc.textures[5]        = particleSceneColor;
        dc.textures[6]        = emitter->motionVectorTexture;
        dc.textures[7]        = resources.GetDepthTexture(h.decalDepthRT);
        renderer.SetSampler(0, renderer::SamplerMode::WRAP_BILINEAR);
        renderer.Submit(dc, resources);
    }
}

} // namespace fbzz::scene
