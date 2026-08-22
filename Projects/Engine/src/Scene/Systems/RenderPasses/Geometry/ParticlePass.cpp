// FBZZ Engine
// RenderPasses/ParticlePass.cpp | fbzz::scene
// パーティクル更新 (CPU) と描画
#include "GeometryPasses.hpp"
#include <Engine/Scene/Systems/ParticleOverdrawStats.hpp>
#include <Engine/Scene/Systems/ParticleSimulationRuntime.hpp>
#include "Engine/Scene/Scene.hpp"
#include "Engine/Scene/Transform.hpp"
#include "Engine/Scene/Components/ParticleEmitter.hpp"
#include "Engine/Scene/Components/ParticleForceField.hpp"
#include "Engine/Scene/Components/ParticleGpuSimulation.hpp"
#include "Engine/Scene/Components/MeshRenderer.hpp"
#include "Engine/Scene/Components/AnimatorComponent.hpp"
#include "Engine/Asset/AssetManager.hpp"
#include "Engine/Asset/MaterialAsset.hpp"
#include "Engine/Asset/Model.hpp"
#include "Engine/Core/Time.hpp"
#include "Engine/Renderer/ComputeCall.hpp"
#include "Engine/Renderer/DrawCall.hpp"
#include "Engine/Renderer/DynamicVertexBufferPool.hpp"
#include "Engine/Renderer/RenderState.hpp"
#include <Physics/World.hpp>
#include <Math/MathUtils.hpp>
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

// 周回 (orbital) と放射 (radial) の加速度を速度へ加える。
// 式は ParticleGpuSim.cs.hlsl の ApplyOrbitalVelocity と一致させること (CPU/GPU で挙動を揃える)。
//
// origin は position と同じ空間でのエミッター原点。
// WHY: World 空間シミュレーションでは粒子位置がワールド座標なのに emitPosition は
//      エミッターローカルのオフセットで、そのまま引くと回転中心がずれる。
//      呼び出し側に空間を合わせて渡させることで、Local/World の両方で同じ式が使える
//      (GPU 側の gEmitterPos も同じ理由でワールド変換済みの値が入っている)。
void ApplyOrbitalVelocity(const ParticleEmitter& emitter,
                          const math::Vector3&   origin,
                          const math::Vector3&   position,
                          math::Vector3&         velocity,
                          float dt)
{
    if (emitter.orbitalVelocity == 0.0f && emitter.radialVelocity == 0.0f) return;

    const math::Vector3 offset = position - origin;
    const float distance = offset.Length();
    // 原点に重なった粒子は接線・放射方向が定義できない。ゼロ除算を避けて素通しする。
    if (distance < 1.0e-5f) return;
    const math::Vector3 radialDirection = offset * (1.0f / distance);

    if (emitter.radialVelocity != 0.0f)
        velocity = velocity + radialDirection * (emitter.radialVelocity * dt);

    if (emitter.orbitalVelocity != 0.0f) {
        const float axisLength = emitter.orbitalAxis.Length();
        if (axisLength > 1.0e-5f) {
            const math::Vector3 axis = emitter.orbitalAxis * (1.0f / axisLength);
            // 接線 = axis × radial。軸と平行な粒子では長さ 0 になるので正規化前に確認する。
            const math::Vector3 tangent = math::Vector3::Cross(axis, radialDirection);
            const float tangentLength = tangent.Length();
            if (tangentLength > 1.0e-5f) {
                velocity = velocity
                    + tangent * (1.0f / tangentLength) * (emitter.orbitalVelocity * dt);
            }
        }
    }
}

// 粒子の軌跡を一定間隔でサンプリングして履歴へ積む。
// [0] を最新として後ろへずらす。点数が最大 8 と小さいので、リングバッファではなく
// 素直なシフトにする (描画側が「新しい順」を仮定でき、読み手が追いやすい)。
void AppendParticleTrailPoint(const ParticleEmitter& emitter, Particle& particle, float dt)
{
    if (!emitter.trailEnabled) {
        particle.trailCount = 0;
        return;
    }
    const int capacity = std::clamp(emitter.trailPointCount, 1, kMaxParticleTrailPoints);
    particle.trailSampleTimer += dt;
    // 間隔 0 を許すと 1 フレームに何度も積んで履歴が一瞬で埋まるため下限を切る。
    const float interval = (std::max)(emitter.trailSampleInterval, 0.001f);
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

// 粒子ごとの色ゆらぎ倍率を 1 粒子ぶん引く。RGB を各チャンネル独立に [1-v, 1+v] 倍して
// 群れの単調さを崩す。alpha は返さない (フェード制御なのでゆらすと消え際が汚くなる)。
//
// WHY 色そのものではなく倍率を返すか:
//   グラデーション使用時は色が毎フレーム作り直されるため、スポーン時に色へ焼き込むと
//   翌フレームには消える。全テンプレートが useColorGradient を使っており、
//   Fire の "Color Variation" / Explosion の "Fireball Variation" は CPU 経路で
//   完全に無効だった。倍率として持ち、色を作り直すたびに掛け直す。
// CPU/GPU どちらのスポーン経路からも同じ乱数列で呼ぶため、結果は決定論的に一致する。
math::Vector3 NextColorVariation(ParticleEmitter& emitter)
{
    const float variation = Clamp01(emitter.colorVariation);
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
// WHY: この 3 手順 (評価 → リニア化 → ゆらぎ) が 3 か所へ散っていたため、
//      更新ループだけがゆらぎを取りこぼしていた。
math::Vector4 EvaluateParticleColorLinear(const ParticleEmitter& emitter,
                                          const Particle& particle, float normalizedAge)
{
    math::Vector4 color = emitter.useColorGradient
        ? emitter.runtimeGradient.EvaluateLinear(normalizedAge)
        : ParticleSrgbToLinear(LerpVec4(particle.startColor, particle.endColor,
                                        std::pow(normalizedAge, emitter.colorCurvePower)));
    color.x *= particle.colorScale.x;
    color.y *= particle.colorScale.y;
    color.z *= particle.colorScale.z;
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
    const int columns = (std::max)(emitter.spriteColumns, 1);
    const int rows = (std::max)(emitter.spriteRows, 1);
    const int frameCount = columns * rows;
    int startFrame = std::clamp(emitter.spriteStartFrame, 0, frameCount - 1);
    int endFrame = std::clamp(
        emitter.spriteEndFrame > 0 ? emitter.spriteEndFrame : frameCount - 1,
        startFrame,
        frameCount - 1);
    // Random Row: アトラスの各行を「1 本のアニメーションのバリエーション」として扱い、
    // 粒子ごとに 1 行を選んでその中だけで再生する。1 枚のアトラスで見た目の異なる
    // 煙・爆炎を混ぜられる (AAA のアトラスはこの構成が標準)。
    if (emitter.spriteRandomRow && rows > 1) {
        const int row = std::clamp(
            static_cast<int>(Clamp01(spriteSeed) * static_cast<float>(rows)), 0, rows - 1);
        startFrame = row * columns;
        endFrame = startFrame + columns - 1;
    }
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
    // Random Start Frame: 再生位相を粒子ごとにずらす。
    // これが無いと同時に湧いた煙が全部同じコマで回り、群れが一枚の板に見えてしまう。
    // RandomFrame モードは元々コマ自体がランダムなので位相ずらしは適用しない。
    if (emitter.spriteRandomStartFrame && span > 0
        && emitter.flipbookMode != ParticleFlipbookMode::RandomFrame) {
        // 行選択と同じ seed をそのまま使うと「行と位相」が相関して不自然な規則性が出るため、
        // 適当な係数でずらしてから小数部を取り、独立した第 2 の乱数として扱う。
        const float phaseSeed = Clamp01(spriteSeed) * 7.13f + 0.37f;
        const float decorrelated = phaseSeed - std::floor(phaseSeed);
        const float cycle = static_cast<float>(span + 1);
        framePosition = std::fmod(
            framePosition + std::floor(decorrelated * cycle), cycle);
        wrapNext = true;
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
    if (emitter.loadedDistortionTexturePath != emitter.distortionTexturePath) {
        emitter.distortionTexture = emitter.distortionTexturePath.empty()
            ? renderer::ResourceHandle<renderer::TextureTag>::Null()
            : LoadParticleTextureOrWhite(resources, emitter.distortionTexturePath);
        emitter.loadedDistortionTexturePath = emitter.distortionTexturePath;
    }
    // materialPath が設定されている場合: .mat の albedo テクスチャと blendMode を優先する。
    // WHY: materialPath が単一の描画設定の信頼元になることで、複数エミッターで同じ .mat を共有できる。
    //
    // 未設定なら既定 .mat へ落とす。1x1 白は alpha=1 なので、そのまま描くと
    // 粒子が「不透明な四角」になり、素材の付け忘れが最も分かりにくい形で表に出る。
    // NOTE: 既定 .mat が無いプロジェクトでは LoadMaterial が失敗し、従来どおり白へ落ちる。
    const bool usingFallback = emitter.materialPath.empty();
    const std::string resolvedMaterial =
        usingFallback ? std::string(PARTICLE_FALLBACK_MATERIAL) : emitter.materialPath;
    {
        const bool matChanged = (emitter.loadedMaterialPath != resolvedMaterial);
        if (matChanged) {
            emitter.loadedMaterialPath = resolvedMaterial;
            emitter.loadedTexturePath.clear(); // .mat の変更でテクスチャも再ロードさせる
        }
        const auto matHandle = asset::AssetManager::LoadMaterial(resolvedMaterial);
        if (const auto* mat = asset::AssetManager::GetMaterial(matHandle)) {
            const auto it = mat->textures.find("albedo");
            const std::string& resolvedTex = (it != mat->textures.end()) ? it->second : std::string{};
            if (!emitter.texture.IsValid() || emitter.loadedTexturePath != resolvedTex) {
                emitter.texture = LoadParticleTextureOrWhite(resources, resolvedTex);
                emitter.loadedTexturePath = resolvedTex;
                emitter.textureIsSrgb = IsEffectTextureSrgb(resolvedTex);
            }
            // .mat の [params] albedo を色調整として引き継ぐ。
            // WHY: materialPath を「描画設定の単一の信頼元」と定義しておきながら、
            //      テクスチャとブレンドしか読んでいなかったため色調整だけが素通りしていた。
            //      オーサリング値は sRGB なので、他の色と同じくリニアへ揃えて渡す。
            math::Vector4 tint{ 1.0f, 1.0f, 1.0f, 1.0f };
            if (const auto param = mat->params.find("albedo"); param != mat->params.end()) {
                const auto& values = param->second;
                // float / float2 / float3 / float4 が同じ形式で入る。足りない成分は既定のまま。
                if (values.size() > 0) tint.x = values[0];
                if (values.size() > 1) tint.y = values[1];
                if (values.size() > 2) tint.z = values[2];
                if (values.size() > 3) tint.w = values[3];
            }
            emitter.materialTint = ParticleSrgbToLinear(tint);
            // blendMode を .mat から上書きする。
            // WHY: materialPath が描画設定の単一の信頼元であるため、Inspector の blendMode より優先する。
            // ただし既定 .mat は担当者が選んだものではないので、ブレンドまでは奪わない
            // (奪うと「未設定にした瞬間に Alpha 指定が加算へ化ける」ことになる)。
            if (!usingFallback) {
                switch (mat->blendMode) {
                case renderer::BlendMode::ALPHA_BLEND:
                    emitter.blendMode = ParticleBlendMode::Alpha; break;
                case renderer::BlendMode::PREMULTIPLIED:
                    emitter.blendMode = ParticleBlendMode::Premultiplied; break;
                default:
                    emitter.blendMode = ParticleBlendMode::Additive; break;
                }
            }
            return;
        }
    }

    if (!emitter.texture.IsValid()) {
        emitter.texture = LoadParticleTextureOrWhite(resources, {});
        emitter.loadedTexturePath.clear();
        // 1x1 白フォールバック。リニアでも sRGB でも 1.0 は 1.0 なので変換しない。
        emitter.textureIsSrgb = false;
    }
}

// GPU ソートを実際に走らせるか。
// WHY: 判定を「バッファが確保済みか」に置くと、renderCB を作る時点 (描画前) と
//      ソートを実行する時点 (Tick 内) で答えが食い違い、初回フレームだけ
//      ソート無効の絵が出る。判定材料をシェーダーの有無だけにして 1 か所へ寄せる。
bool ShouldSortGpuParticles(const ParticleEmitter& emitter, const RenderPassHandles& handles)
{
    return emitter.sortMode != ParticleSortMode::None
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
    if (!emitter.renderCB.IsValid())
        emitter.renderCB = resources.CreateConstantBuffer(sizeof(ParticleRenderCB));
    ParticleRenderCB cb{};
    cb.renderMode = static_cast<uint32_t>(emitter.renderMode);
    cb.stretchedVelocityScale = (std::max)(emitter.stretchedVelocityScale, 0.0f);
    cb.stretchedLengthScale = (std::max)(emitter.stretchedLengthScale, 0.0f);
    cb.softParticleFadeDistance = (std::max)(emitter.softParticleFadeDistance, 0.001f);
    cb.softParticles = emitter.softParticles ? 1u : 0u;
    cb.maxParticles = static_cast<uint32_t>((std::max)(maxParticles, 0));
    // ビット割り当ては Rendering/ParticleCommon.hlsli の FBZZ_PFX_* / FBZZ_PALPHA_* と一致させること。
    cb.effectsFlags = (emitter.distortion ? kParticleFxDistortion : 0u)
        | (emitter.sixWayLighting ? kParticleFxSixWay : 0u)
        | (emitter.motionVectorFlipbook && emitter.motionVectorTexture.IsValid()
               ? kParticleFxMotionVector : 0u)
        | (emitter.receiveShadows ? kParticleFxReceiveShadow : 0u)
        | (emitter.volumetric ? kParticleFxVolumetric : 0u)
        | (emitter.blendMode == ParticleBlendMode::Premultiplied
               ? kParticleFxPremultiplied : 0u)
        | (emitter.textureIsSrgb ? kParticleFxSrgbTexture : 0u)
        | (emitter.distortionTexture.IsValid() ? kParticleFxDistortionMap : 0u)
        | ((static_cast<std::uint32_t>(emitter.alphaSource) & kParticleAlphaMask)
               << kParticleAlphaShift);
    cb.distortionStrength = (std::max)(emitter.distortionStrength, 0.0f);
    cb.distortionChromatic = (std::max)(emitter.distortionChromatic, 0.0f);
    cb.lightingStrength = (std::max)(emitter.lightingStrength, 0.0f);
    cb.smokeWrap = std::clamp(emitter.smokeWrap, 0.0f, 1.0f);
    cb.smokeTransmission = (std::max)(emitter.smokeTransmission, 0.0f);
    // 0 以下だと pow が発散する。シェーダー側でも下限を切るが、値の意味をここで固定する。
    cb.smokeBackScatterPower = (std::max)(emitter.smokeBackScatterPower, 0.1f);
    cb.tintColor = emitter.materialTint;
    cb.emissiveScale = (std::max)(emitter.emissiveScale, 0.0f);
    cb.motionVectorStrength = (std::max)(emitter.motionVectorStrength, 0.0f);
    // 歪みの画面UV算出に使う。0 だとUVが右下隅へ張り付いて屈折が出ない。
    cb.screenWidth = static_cast<float>((std::max)(screenWidth, 1u));
    cb.screenHeight = static_cast<float>((std::max)(screenHeight, 1u));
    // 0 以下だと粒子が潰れて一切見えなくなるため、下限で切って「消える」事故を防ぐ。
    cb.sizeAxisScaleX = (std::max)(emitter.sizeAxisScale.x, 0.0001f);
    cb.sizeAxisScaleY = (std::max)(emitter.sizeAxisScale.y, 0.0001f);
    cb.shadowStrength = std::clamp(emitter.shadowStrength, 0.0f, 1.0f);
    // ステップ数はピクセルあたりのループ回数に直結する。上限を切って
    // 設定ミスで GPU が張り付くのを防ぐ。
    cb.volumetricSteps = static_cast<std::uint32_t>(std::clamp(emitter.volumetricSteps, 1, 64));
    cb.volumetricDensity = (std::max)(emitter.volumetricDensity, 0.0f);
    // g = ±1 は位相関数が発散するため内側へ寄せる。
    cb.volumetricAnisotropy = std::clamp(emitter.volumetricAnisotropy, -0.95f, 0.95f);
    cb.volumetricNoiseScale = (std::max)(emitter.volumetricNoiseScale, 0.0f);
    // GPU 経路の VS がソート済み index (t15) を経由するかどうか。CPU 経路では常に 0。
    cb.gpuSortEnabled = gpuSortEnabled ? 1u : 0u;
    // 自己影。密度バッファ (t9) が無いフレームでも 0 なら参照しないので安全。
    cb.selfShadowStrength = (std::max)(emitter.selfShadowStrength, 0.0f);
    resources.Update(emitter.renderCB, &cb, sizeof(cb));
}

// 自己影用の密度 RT を、このパスで最初に使うときだけクリアして光源行列を流し込む。
// RT / CB の生成は RenderSystem 側 (静的リソース) が持つ。
// WHY: RenderPassHandles はフレームごとに作り直される値型なので、
//      ここで遅延生成すると毎フレーム新しい RT を作って漏らす。
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
        ctx.renderer.SetRenderTarget(h.hdrRT, resources);

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
//
// WHY 共有の 1 本 (旧 h.particleVB) をやめたか:
//   エミッターごとに Update → Submit を繰り返す構造は DX12 で成立しない。Submit は
//   コマンドリストへの記録でしかなく、GPU が頂点を読むのはフレーム終端なので、
//   2 個目のエミッターの Update が 1 個目の Draw の中身まで差し替えてしまう
//   (詳細は DynamicVertexBufferPool.hpp)。エミッターごとに別バッファを借りる。
renderer::DynamicVertexBufferPool g_particleVertexPool;

// このフレームに本番描画したエミッターの記録 (どのバッファへ何クワッド積んだか)。
// WHY: Overdraw 可視化は別パスなので、本番描画の結果を引き継がないと測る対象がずれる。
//      従来は共有バッファ 1 本を前提に「最後のエミッターの中身」を全エミッターぶん
//      数え直しており、エミッターが 2 個以上あると枚数が実際と食い違っていた。
struct ParticleDrawRecord {
    const ParticleEmitter*                        emitter = nullptr;
    renderer::ResourceHandle<renderer::BufferTag> vertexBuffer;
    int                                           quadCount = 0;
};
std::vector<ParticleDrawRecord> g_particleDrawRecords;

// 連続リボン (trailRibbon) の帯頂点用。帯はカメラへ正対させるので形がビューごとに変わる。
// エミッターに 1 本持たせると、エディタの Scene View と Game View で同じバッファを
// 別の形で 2 回書くことになり、DX12 では先に記録した Draw まで巻き添えになる。
renderer::DynamicVertexBufferPool g_trailRibbonVertexPool;

// 1 エミッターぶんの密度を光源側 RT へ積む。
//
// 呼ぶ位置が重要: そのエミッターの頂点バッファをアップロードした直後、本番描画の前。
// WHY: 密度は「実際に描くのと同じ形」で測らなければ意味がない。
//
// NOTE: 光源行列は専用 CB から渡す。h.frameCB を書き換えると後続の全パスへ漏れる
//       (ShadowPass が書き換えて良いのは、あれがパスの先頭にいるから)。
//
// NOTE: この構造上、あるエミッターが受ける自己影は「自分自身 + 先に処理されたエミッター」
//       までしか含まれない。単一エミッターの煙・雲 (自己影が最も効く形) では完全に正しく、
//       複数エミッターを重ねた場合は後ろのものほど多く遮られる、という順序依存が残る。
//       完全にするには全エミッターの形を一度に保持する別バッファが要り、
//       粒子数ぶんのメモリを二重に持つことになるため、この近似を採る。
void AccumulateParticleSelfShadowDensity(const ParticleEmitter& emitter, int quadCount,
                                         renderer::ResourceHandle<renderer::BufferTag> vertexBuffer,
                                         RenderPassContext& ctx)
{
    auto& resources = ctx.resources;
    auto& renderer = ctx.renderer;
    auto& h = ctx.handles;
    if (quadCount <= 0 || !emitter.texture.IsValid()
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
    dc.constantBuffers[2] = emitter.renderCB;
    dc.textures[0]  = emitter.texture;
    renderer.SetSampler(0, renderer::SamplerMode::WRAP_BILINEAR);
    renderer.Submit(dc, resources);
    // 本番描画へ戻す。呼び出し側が続けて HDR RT へ描くため、ここで必ず張り直す。
    renderer.SetRenderTarget(h.hdrRT, resources);
}

// 1 エミッターぶんの per-particle Trail を、連続した帯 (リボン) として描く。
//
// WHY: ビルボードを履歴点へ並べる方式は「点を細かく打てば線に見える」だけで、
//      太くすると必ず粒の連なりが露見する。剣閃・魔法の軌跡のように
//      幅のある帯が主役の表現はそれでは作れない。
//      履歴点をポリラインとみなし、Trail ノードと同じマイター接合で帯を張る。
//
// NOTE: 色は帯の長さ方向へ colorStart → colorEnd を配る (Trail.hlsl の age)。
//       粒子ごとの色ゆらぎは 1 DrawCall へまとめる都合で乗らない。
//       粒ごとに色を変えたい場合はビルボード方式 (trailRibbon = false) を使う。
void DrawParticleTrailRibbons(ParticleEmitter& emitter, const Transform& tf,
                              int particleCount, RenderPassContext& ctx)
{
    auto& resources = ctx.resources;
    auto& h = ctx.handles;
    if (!h.trailShader.IsValid() || !h.trailPSO.IsValid() || !emitter.texture.IsValid()) return;

    const int historyPoints = std::clamp(emitter.trailPointCount, 1, kMaxParticleTrailPoints);
    // 帯 1 本 = 本体 + 履歴点。線分は historyPoints 本で、各線分が 6 頂点。
    const int segmentsPerParticle = historyPoints;
    // 帯は粒子 1 つあたり数十頂点になる。上限を切らないと、粒子数を上げた瞬間に
    // 頂点バッファが数十 MB へ膨れる。切った先は「尾が付かない粒子」として静かに落とす。
    constexpr int kMaxRibbonParticles = 2048;
    const int ribbonParticles = (std::min)(particleCount, kMaxRibbonParticles);
    if (ribbonParticles <= 0 || segmentsPerParticle <= 0) return;

    const std::uint32_t neededVertices =
        static_cast<std::uint32_t>(ribbonParticles) * static_cast<std::uint32_t>(segmentsPerParticle) * 6u;
    if (!emitter.trailRibbonCB.IsValid())
        emitter.trailRibbonCB = resources.CreateConstantBuffer(sizeof(TrailCB));
    if (!emitter.trailRibbonCB.IsValid()) return;

    const bool localSpace = emitter.simulationSpace == ParticleSimulationSpace::Local;
    const math::Vector3 cameraPos = ctx.camera.m_position;

    static std::vector<TrailVertex> vertices;
    vertices.clear();
    vertices.reserve(neededVertices);
    // 履歴点を毎回組み直すためのスクラッチ。粒子ごとに確保し直さない。
    static std::vector<math::Vector3> polyline;
    static std::vector<math::Vector3> normals;

    for (int index = 0; index < ribbonParticles; ++index) {
        const Particle& particle = emitter.particles[static_cast<std::size_t>(index)];
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

        // 各点の幅方向。隣り合う線分の法線を平均 (マイター) して継ぎ目の折れを消す。
        normals.assign(polyline.size(), math::Vector3::RIGHT);
        for (std::size_t point = 0; point < polyline.size(); ++point) {
            math::Vector3 left{}, right{};
            bool hasLeft = false, hasRight = false;
            if (point > 0) {
                const math::Vector3 direction = polyline[point] - polyline[point - 1u];
                if (direction.LengthSq() > math::EPSILON * math::EPSILON) {
                    left = ComputeCameraFacingRibbonNormal(direction.Normalized(), cameraPos, polyline[point]);
                    hasLeft = true;
                }
            }
            if (point + 1u < polyline.size()) {
                const math::Vector3 direction = polyline[point + 1u] - polyline[point];
                if (direction.LengthSq() > math::EPSILON * math::EPSILON) {
                    right = ComputeCameraFacingRibbonNormal(direction.Normalized(), cameraPos, polyline[point]);
                    hasRight = true;
                }
            }
            if (hasLeft && hasRight) {
                math::Vector3 miter = left + right;
                miter = miter.LengthSq() > math::EPSILON * math::EPSILON ? miter.Normalized() : right;
                // 鋭角では 1/cos が発散して帯が破裂する。0.5 (=120度) で頭打ちにする。
                const float cosHalfAngle = (std::max)(math::Vector3::Dot(miter, left), 0.5f);
                normals[point] = miter * (1.0f / cosHalfAngle);
            } else if (hasLeft || hasRight) {
                normals[point] = hasRight ? right : left;
            }
        }

        // 幅。trailRibbonWidth が 0 以下なら粒子サイズを流用する。
        const float baseWidth = emitter.trailRibbonWidth > 0.0f
            ? emitter.trailRibbonWidth : particle.size;
        const float tailDenominator = static_cast<float>(polyline.size() - 1u);
        for (std::size_t segment = 0; segment + 1u < polyline.size(); ++segment) {
            // age は 1 = 粒子本体側 (新しい) / 0 = 尾の先端 (古い)。Trail.hlsl が
            // colorEnd → colorStart の補間に使う。
            const float age0 = 1.0f - static_cast<float>(segment) / tailDenominator;
            const float age1 = 1.0f - static_cast<float>(segment + 1u) / tailDenominator;
            const float half0 = baseWidth
                * math::Lerp(emitter.trailWidthScale, 1.0f, age0) * 0.5f;
            const float half1 = baseWidth
                * math::Lerp(emitter.trailWidthScale, 1.0f, age1) * 0.5f;
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
    cb.colorStart = ParticleSrgbToLinear(emitter.colorStart);
    cb.colorEnd = ParticleSrgbToLinear({
        emitter.colorEnd.x * emitter.trailColorTint.x,
        emitter.colorEnd.y * emitter.trailColorTint.y,
        emitter.colorEnd.z * emitter.trailColorTint.z,
        emitter.colorEnd.w * emitter.trailColorTint.w * emitter.trailAlphaScale,
    });
    cb.uvTiling = 1.0f;
    cb.flags = emitter.textureIsSrgb ? kTrailFlagSrgbTexture : 0u;
    const auto ribbonVB = g_trailRibbonVertexPool.Acquire(
        resources, vertices.size(), static_cast<std::uint32_t>(sizeof(TrailVertex)));
    if (!ribbonVB.IsValid()) return;
    resources.Update(emitter.trailRibbonCB, &cb, sizeof(cb));
    resources.Update(ribbonVB, vertices.data(), vertices.size() * sizeof(TrailVertex));

    renderer::DrawCall dc;
    dc.vertexBuffer = ribbonVB;
    dc.vertexCount  = static_cast<uint32_t>(vertices.size());
    dc.shader       = h.trailShader;
    dc.pipelineState = h.trailPSO;
    dc.layer        = renderer::RenderLayer::TRANSPARENT_LAYER;
    dc.constantBuffers[0] = h.frameCB;
    dc.constantBuffers[2] = emitter.trailRibbonCB;
    dc.textures[0]  = emitter.texture;
    ctx.renderer.SetSampler(0, renderer::SamplerMode::WRAP_BILINEAR);
    SubmitCounted(ctx, dc);
}

// blendMode から PSO を選ぶ。CPU/GPU 双方の描画経路で同じ判定を使う。
// WHY: 判定が 2 か所に散っていると、ブレンドモードを増やしたときに片方だけ
//      追従して「CPU では正しいが GPU では加算のまま」という差が生まれる。
// distortion は背景色を差し替える都合上、加算では画が破綻するためアルファへ倒す。
renderer::ResourceHandle<renderer::PipelineStateTag> SelectParticlePSO(
    const ParticleEmitter& emitter,
    renderer::ResourceHandle<renderer::PipelineStateTag> additivePSO,
    renderer::ResourceHandle<renderer::PipelineStateTag> alphaPSO,
    renderer::ResourceHandle<renderer::PipelineStateTag> premultipliedPSO)
{
    if (emitter.distortion) return alphaPSO;
    switch (emitter.blendMode) {
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

// SubEmitterへイベント数分のBurstを積む。参照切れはVFXの縮退として無視する。
// emitter.subEmitterScopeRoot が有効なら、その GameObject 配下だけを名前で探す。
// WHY: VFX Graph が生成する実体はノード名そのものを名乗るため、同じ .vfx を 2 箇所へ置くと
//      シーン全体の名前引きでは隣のインスタンスを掴む。エフェクト内へ閉じることで、
//      「片方を撃つともう片方から煙が出る」種類の取り違えが起きなくなる。
void QueueSubEmitter(Scene& scene, const ParticleEmitter& emitter,
                     const std::string& objectName, int count)
{
    if (objectName.empty() || count <= 0) return;
    GameObject* target = nullptr;
    if (GameObject* scopeRoot = scene.GetGameObject(emitter.subEmitterScopeRoot))
        target = FindInSubtree(*scopeRoot, objectName);
    else
        target = scene.Find(objectName); // シーン上の手置き Emitter は従来どおり全体から引く
    if (target == nullptr) return;
    if (auto* targetEmitter = target->GetComponent<ParticleEmitter>())
        targetEmitter->burstPending += count;
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
    QueueSubEmitter(scene, emitter, emitter.collisionSubEmitter, emitter.subEmitterBurstCount);
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

    // 初速のばらつきは 3 軸へ等方に掛ける。
    // WHY: 以前は X/Z だけに乱数を掛けていたため、emitVelocity が上向き (炎・煙・噴煙) の
    //      エミッターでは主軸方向の分散がゼロになり、全粒子が同じ速度で上がる硬い前線に
    //      なっていた。Sphere/Cone 形状は shapeVelocity 側で別途方向を持つので、
    //      ここは純粋に「initial velocity の揺らぎ」だけを担当する。
    //      GPU 経路 (BuildGpuSpawnEntry) と必ず同じ式・同じ乱数消費順にすること。
    const float rx = RandomSigned01(emitter) * emitter.velocitySpread;
    const float ry = RandomSigned01(emitter) * emitter.velocitySpread;
    const float rz = RandomSigned01(emitter) * emitter.velocitySpread;
    const math::Vector3 localVelocity = {
        emitter.emitVelocity.x + rx,
        emitter.emitVelocity.y + ry,
        emitter.emitVelocity.z + rz
    };
    p.velocity = transformVector(localVelocity);
    p.velocity = p.velocity + shapeVelocity;
    // エミッターの移動を初速へ引き継ぐ。移動する剣・ロケットの火花が置き去りにならない。
    // Local space シミュレーションでは粒子座標がエミッター基準で、親の移動は
    // 描画時の変換で既に反映されるため、ここで足すと二重に効く。World のときだけ加算する。
    if (emitter.inheritVelocity != 0.0f
        && emitter.simulationSpace == ParticleSimulationSpace::World) {
        p.velocity = p.velocity + emitter.emitterVelocity * Clamp01(emitter.inheritVelocity);
    }
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
    p.colorScale = NextColorVariation(emitter);
    p.color = EvaluateParticleColorLinear(emitter, p, 0.0f);
    p.age = (std::max)(0.0f, (std::min)(initialAge, p.lifetime * 0.999f));
    if (p.age > 0.0f) {
        // Prewarmは開始時点の寿命分布を作る。逐次更新を避け、重力下の解析解で初期状態を近似する。
        p.position = p.position + p.velocity * p.age + emitter.gravity * (0.5f * p.age * p.age);
        p.velocity = p.velocity + emitter.gravity * p.age;
        p.rotation += p.angularVelocity * p.age;
        const float normalizedAge = Clamp01(p.age / p.lifetime);
        p.color = EvaluateParticleColorLinear(emitter, p, normalizedAge);
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

    // SpawnParticle と同一の 3 軸等方ジッター。乱数の消費順まで揃えないと
    // CPU/GPU を切り替えただけで見た目が変わってしまう。
    const float rx = RandomSigned01(emitter) * emitter.velocitySpread;
    const float ry = RandomSigned01(emitter) * emitter.velocitySpread;
    const float rz = RandomSigned01(emitter) * emitter.velocitySpread;
    const math::Vector3 localVelocity = {
        emitter.emitVelocity.x + rx,
        emitter.emitVelocity.y + ry,
        emitter.emitVelocity.z + rz
    };
    s.velocity        = TransformEmitterVector(tf, localVelocity);
    s.velocity        = s.velocity + shapeVelocity;
    // inheritVelocity は CPU 経路 (SpawnParticle) と同じ条件・同じ式でここに足す。
    // CS 側の変更は不要で、GpuSpawnEntry.velocity に加算済みの値が入る。
    if (emitter.inheritVelocity != 0.0f
        && emitter.simulationSpace == ParticleSimulationSpace::World) {
        s.velocity = s.velocity + emitter.emitterVelocity * Clamp01(emitter.inheritVelocity);
    }
    s.lifetime        = (std::max)(0.001f, emitter.lifetime
        * (1.0f + RandomSigned01(emitter) * Clamp01(emitter.lifetimeRandom)));
    s.size            = emitter.sizeStart;
    s.colorStart      = emitter.colorStart;
    // 乱数列上の位置は従来の ApplyColorVariation 呼び出しと同じに保つ
    // (動かすと同じ seed から出る粒子の並びが変わる)。
    const math::Vector3 variation = NextColorVariation(emitter);
    s.colorScale      = { variation.x, variation.y, variation.z, 0.0f };
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
                    const MeshRenderer*                  meshParticleRenderer,
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
        // ソート表は粒子プールの index を持つので、プールを作り直したら必ず捨てる。
        emitter.gpuSortBuffer = {};
        emitter.gpuSortCB = {};
        emitter.gpuSortCapacity = 0;
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
        // デバイスリセット後は古いハンドルが全て無効。ソート表も作り直す。
        emitter.gpuSortBuffer     = {};
        emitter.gpuSortCB         = {};
        emitter.gpuSortCapacity   = 0;
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
    cb.sizeCurveKeys01 = packCurve(emitter.sizeCurve, 0);
    cb.sizeCurveKeys23 = packCurve(emitter.sizeCurve, 2);
    cb.sizeCurveKeys45 = packCurve(emitter.sizeCurve, 4);
    cb.sizeCurveKeys67 = packCurve(emitter.sizeCurve, 6);
    cb.velocityCurveKeys01 = packCurve(emitter.velocityCurve, 0);
    cb.velocityCurveKeys23 = packCurve(emitter.velocityCurve, 2);
    cb.velocityCurveKeys45 = packCurve(emitter.velocityCurve, 4);
    cb.velocityCurveKeys67 = packCurve(emitter.velocityCurve, 6);
    // 黒体モードを焼き込んだ実効グラデーションを渡す。CPU 経路も同じ runtimeGradient を見る。
    const auto& gradient = emitter.runtimeGradient;
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
    cb.depthThickness = (std::max)(0.001f, emitter.collisionRadius * 0.002f);
    cb.depthBounciness = std::clamp(emitter.collisionBounciness, 0.0f, 1.0f);
    cb.depthCollision = emitter.collisionMode == ParticleCollisionMode::Depth ? 1u : 0u;
    cb.depthResponse = static_cast<std::uint32_t>(emitter.collisionResponse);
    cb.depthDamping = std::clamp(emitter.collisionDamping, 0.0f, 1.0f);
    // ── over-lifetime モジュール (回転カーブ / drag カーブ / 周回・放射) ──
    cb.curveFlags2 = {
        emitter.useRotationCurve ? 1.0f : 0.0f,
        emitter.useDragCurve ? 1.0f : 0.0f,
        0.0f, 0.0f
    };
    cb.rotationCurveKeys01 = packCurve(emitter.rotationCurve, 0);
    cb.rotationCurveKeys23 = packCurve(emitter.rotationCurve, 2);
    cb.rotationCurveKeys45 = packCurve(emitter.rotationCurve, 4);
    cb.rotationCurveKeys67 = packCurve(emitter.rotationCurve, 6);
    cb.dragCurveKeys01 = packCurve(emitter.dragCurve, 0);
    cb.dragCurveKeys23 = packCurve(emitter.dragCurve, 2);
    cb.dragCurveKeys45 = packCurve(emitter.dragCurve, 4);
    cb.dragCurveKeys67 = packCurve(emitter.dragCurve, 6);
    // 4 本のカーブの有効キー数と補間モード。GPU 側はこれを見て CPU と同じ区間を選ぶ。
    cb.curveKeyCounts = {
        curveKeyCount(emitter.sizeCurve), curveKeyCount(emitter.velocityCurve),
        curveKeyCount(emitter.rotationCurve), curveKeyCount(emitter.dragCurve)
    };
    cb.curveModes = {
        static_cast<float>(emitter.sizeCurve.interpolation),
        static_cast<float>(emitter.velocityCurve.interpolation),
        static_cast<float>(emitter.rotationCurve.interpolation),
        static_cast<float>(emitter.dragCurve.interpolation)
    };
    // 軸は CPU 側で正規化して渡す。CS 側で毎粒子 normalize するより安く、
    // 長さ 0 の軸を「周回なし」へ縮退させる判定も 1 か所で済む。
    const float orbitalAxisLength = emitter.orbitalAxis.Length();
    cb.orbitalAxis = orbitalAxisLength > 1.0e-5f
        ? emitter.orbitalAxis * (1.0f / orbitalAxisLength)
        : math::Vector3{ 0.0f, 1.0f, 0.0f };
    cb.orbitalVelocity = orbitalAxisLength > 1.0e-5f ? emitter.orbitalVelocity : 0.0f;
    cb.radialVelocity = emitter.radialVelocity;
    cb.spriteRandomFlags = (emitter.spriteRandomStartFrame ? 1u : 0u)
        | (emitter.spriteRandomRow ? 2u : 0u);
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

    // ---- GPU ソート ---------------------------------------------------------
    //
    // WHY: 半透明の重なりは描画順で結果が変わるため、「GPU で大量に出す」と
    //      「正しい前後関係」を両立するには GPU 側で並べ替えるしかない。
    //      CPU へ読み戻して並べると毎フレーム同期待ちになり、GPU シミュレーションの意味が消える。
    //
    // NOTE: 並べ替えるのは (キー, 粒子 index) の対だけで、粒子プール自体は動かさない。
    //       プールはスポーン用のリングバッファなので、要素の位置が変わると
    //       gpuWriteHead が指す場所が意味を失い、スポーンとシミュレーションが壊れる。
    //
    // NOTE: simulateThisFrame の外に置く。複数ビュー (Scene View + Game View) では
    //       同じフレームでもカメラが違い、正しい順序もビューごとに違うため。
    const bool wantSort = ShouldSortGpuParticles(emitter, h);
    if (wantSort) {
        // bitonic sort は要素数が 2 のべき乗である前提で組む。LDS 段が 1 グループ分を
        // 丸ごと扱うため、下限も 1 ブロック (256) に揃える。
        std::uint32_t padded = kParticleSortBlock;
        while (padded < static_cast<std::uint32_t>(maxP)) padded <<= 1;

        if (!emitter.gpuSortBuffer.IsValid() || emitter.gpuSortCapacity != padded) {
            if (emitter.gpuSortBuffer.IsValid()) resources.Release(emitter.gpuSortBuffer);
            // 1 要素 = uint2 (key, particleIndex)。
            emitter.gpuSortBuffer = resources.CreateRWStructuredBuffer(
                nullptr, padded, static_cast<std::uint32_t>(sizeof(std::uint32_t) * 2u));
            emitter.gpuSortCapacity = padded;
        }
        if (!emitter.gpuSortCB.IsValid())
            emitter.gpuSortCB = resources.CreateConstantBuffer(sizeof(GpuParticleSortCB));

        if (emitter.gpuSortBuffer.IsValid() && emitter.gpuSortCB.IsValid()) {
            GpuParticleSortCB sortCb{};
            sortCb.cameraPos   = ctx.camera.m_position;
            sortCb.aliveCount  = static_cast<std::uint32_t>(maxP);
            sortCb.paddedCount = padded;
            sortCb.backToFront = emitter.sortMode == ParticleSortMode::BackToFront ? 1u : 0u;
            const std::uint32_t groups = padded / kParticleSortBlock;

            const auto dispatchSortStage =
                [&](renderer::ResourceHandle<renderer::ShaderTag> shader,
                    std::uint32_t stageK, std::uint32_t stageJ, bool bindParticles) {
                    sortCb.stageK = stageK;
                    sortCb.stageJ = stageJ;
                    resources.Update(emitter.gpuSortCB, &sortCb, sizeof(sortCb));
                    renderer::ComputeCall call;
                    call.shader = shader;
                    call.constantBuffers[0] = emitter.gpuSortCB;
                    // 粒子プールは SRV (t14) で読むだけ。ソート結果は別バッファ (u3) なので、
                    // 同一リソースを SRV と UAV へ同時バインドするハザードにはならない。
                    if (bindParticles) call.srvBuffers[0] = emitter.gpuParticleBuffer;
                    call.uavBuffers[1] = emitter.gpuSortBuffer; // u3 = UAV_GPU_SORT
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
            renderer.SetRenderTarget(h.hdrRT, resources);
        }
    } else if (emitter.gpuSortBuffer.IsValid()) {
        // ソートを切ったら確保も解放する。VS 側は renderCB の gpuSortEnabled で判断するので、
        // ここを残したままだと「無効なのにバッファだけ生き続ける」状態になる。
        resources.Release(emitter.gpuSortBuffer);
        emitter.gpuSortBuffer = {};
        emitter.gpuSortCapacity = 0;
    }

    // renderCB は「ソートする」と言っているのにバッファが無い状態では描かない。
    // WHY: VS はソート有効なら必ず t15 を引く。未バインドの SRV は 0 を返すため、
    //      全インスタンスが粒子 0 番を指す明らかに誤った絵になる。
    //      誤った絵を出すより、その 1 フレームを落とすほうが原因を追いやすい。
    if (wantSort && !emitter.gpuSortBuffer.IsValid()) return;

    // ---- メッシュパーティクル (インスタンス描画) ------------------------------
    //
    // WHY: CPU 経路は粒子 1 個につき DrawCall 1 本 (MeshTrailRenderPass) で、
    //      GPU シミュレーションでは CPU 側に粒子配列が無いため描きようがなかった。
    //      粒子データは既に StructuredBuffer にあるので、maxParticles 個の
    //      インスタンス描画 1 本へ畳める。破片・瓦礫を大量に出す前提はこれで満たせる。
    if (!emitter.meshParticlePath.empty()) {
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
        meshDc.constantBuffers[2] = emitter.renderCB;
        meshDc.textures[0]  = emitter.texture;
        meshDc.vsBuffers[0] = emitter.gpuParticleBuffer; // t14
        meshDc.vsBuffers[1] = emitter.gpuSortBuffer;     // t15 (ソート無効時は無効ハンドル)
        meshDc.instanceCount = static_cast<uint32_t>(maxP);
        renderer.SetSampler(0, renderer::SamplerMode::WRAP_BILINEAR);
        SubmitCounted(ctx, meshDc);
        return;
    }

    // SV_VertexID ベース描画: 頂点バッファなし、VS が StructuredBuffer<GpuParticle> を t14 で読む
    // シェーダーは加算/アルファの 2 種しかないため、Premultiplied はアルファ側の
    // シェーダーを使い、ブレンド方程式だけ PSO で切り替える (出力は同じで合成だけが違う)。
    const bool alphaBlend = emitter.blendMode != ParticleBlendMode::Additive || emitter.distortion;
    const auto gpuShader = alphaBlend
        ? h.particleGpuAlphaShader : h.particleGpuShader;
    const auto gpuPSO = SelectParticlePSO(emitter, h.particleGpuPSO, h.particleGpuAlphaPSO,
                                          h.particleGpuPremultipliedPSO);
    if (!gpuShader.IsValid() || !gpuPSO.IsValid() || !emitter.texture.IsValid())
        return;

    renderer::DrawCall dc;
    dc.shader        = gpuShader;
    dc.pipelineState = gpuPSO;
    dc.constantBuffers[0] = h.frameCB;
    dc.constantBuffers[2] = emitter.renderCB;
    dc.textures[0]        = emitter.texture;
    dc.textures[1]        = emitter.distortionTexture; // t1: 歪み専用マップ (未設定なら無効)
    dc.textures[5]        = sceneColor;
    dc.textures[6]        = emitter.motionVectorTexture;
    dc.textures[7]        = resources.GetDepthTexture(h.decalDepthRT);
    // 受け影: CPU 経路と同じ b4 / t8 / サンプラー 1 を使う。
    dc.constantBuffers[4] = h.shadowCB;
    dc.textures[8]        = resources.GetDepthTexture(h.shadowMapRT);
    dc.vsBuffers[0]       = emitter.gpuParticleBuffer; // t14: StructuredBuffer<GpuParticle>
    // t15: ソート済み (key, index)。無効時は何もバインドしない
    // (VS は renderCB の gpuSortEnabled が 0 なら参照しない)。
    dc.vsBuffers[1]       = emitter.gpuSortBuffer;
    dc.vertexCount        = static_cast<uint32_t>(maxP) * 6u;
    renderer.SetSampler(0, renderer::SamplerMode::WRAP_BILINEAR);
    renderer.SetSampler(1, renderer::SamplerMode::BORDER_ZERO);
    SubmitCounted(ctx, dc);
}

// CanUseGpuSimulation の実体は ParticleGpuSimulation.cpp。
// WHY: 「なぜ GPU に載らなかったか」を lint / 実行状態 / Editor へ返す必要があり、
//      条件の羅列をここに閉じたままだと、判定と説明が別々に劣化していく。

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

// world は null 可 (Collision は ResolveParticleCollision 側でポインタとして扱う)。
// WHY ポインタか: 描画パス側は RenderPassContext::physicsWorld をそのまま渡す。
//     参照で受けると呼び出し側に null チェックとダミー World が要る。
void SimulateCpuEmitter(ParticleEmitter& emitter, const Transform& tf,
                        const AnimatorComponent* animator, float dt, float time,
                        const std::vector<ActiveForceField>& forceFields,
                        const physics::World* world, Scene& scene)
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
            QueueSubEmitter(scene, emitter, emitter.birthSubEmitter, emitter.subEmitterBurstCount);
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
            QueueSubEmitter(scene, emitter, emitter.birthSubEmitter, emitter.subEmitterBurstCount);
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
            QueueSubEmitter(scene, emitter, emitter.deathSubEmitter, emitter.subEmitterBurstCount);
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
                emitter, worldParticle, TransformEmitterPoint(tf, nextPosition), world, scene);
            it->position = InverseTransformEmitterPoint(tf, worldParticle.position);
            it->velocity = InverseTransformEmitterVector(tf, worldParticle.velocity);
        } else {
            killedByCollision = ResolveParticleCollision(emitter, *it, nextPosition, world, scene);
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
        it->color = EvaluateParticleColorLinear(emitter, *it, normalizedAge);
        const float sizeT = emitter.useSizeCurve
            ? Clamp01(emitter.sizeCurve.Evaluate(normalizedAge))
            : std::pow(normalizedAge, emitter.sizeCurvePower);
        it->size = it->startSize + (it->endSize - it->startSize) * sizeT;
        ++it;
    }
    UpdateParticleBounds(emitter, tf);
}

// 再生状態 (delay / duration / loop / 距離 Emission / 時刻 Burst / Prewarm) を 1 フレーム分進める。
// WHY 描画側から切り離すか: カリングされたエミッターでも「時間だけは進める」必要がある。
//     描画ループの途中に埋めたままだと、可視でないと 1 行も進まず、画面外へ振って戻すたびに
//     エフェクトが止まったところから再開してしまう。
// NOTE: 1 フレームに 2 度呼ばれても進まない (lastPlaybackFrame で自衛する)。
[[nodiscard]] bool AdvanceEmitterPlayback(ParticleEmitter& emitter, const Transform& tf, float dt)
{
    if (emitter.lastPlaybackFrame == Time::frameCount) return emitter.emitThisFrame;
    emitter.lastPlaybackFrame = Time::frameCount;
    // 黒体モードの焼き込みはここで 1 フレームに 1 回だけ行う。CPU 更新も GPU 定数バッファも
    // この後の runtimeGradient を読むため、両経路の色が原理的にずれない。
    emitter.RefreshRuntimeGradient();

    bool canEmit = emitter.playing;
    if (canEmit && emitter.delayTime < emitter.startDelay) {
        emitter.delayTime += dt;
        canEmit = false;
    }
    if (canEmit && emitter.duration > 0.0f) {
        emitter.playTime += dt;
        if (emitter.playTime >= emitter.duration) {
            if (emitter.loop) {
                emitter.playTime = 0.0f;
                emitter.delayTime = 0.0f;
                emitter.burstCyclesFired.clear();
                canEmit = false;
            } else {
                emitter.playing = false;
                canEmit = false;
                if (emitter.clearOnStop) {
                    ClearEmitterRuntime(emitter);
                    emitter.gpuClearPending = true;
                }
            }
        }
    }
    emitter.emitThisFrame = canEmit;

    // 距離Emission: Emitterのワールド移動量を粒子数へ変換する。
    const math::Vector3 emitterWorldPosition = TransformEmitterPoint(tf, emitter.emitPosition);
    if (emitter.hasLastEmitterPosition && canEmit && emitter.rateOverDistance > 0.0f) {
        emitter.distanceEmitAccum += (emitterWorldPosition - emitter.lastEmitterPosition).Length()
            * emitter.rateOverDistance;
        const int distanceCount = static_cast<int>(emitter.distanceEmitAccum);
        if (distanceCount > 0) {
            emitter.burstPending += distanceCount;
            emitter.distanceEmitAccum -= static_cast<float>(distanceCount);
        }
    }
    emitter.lastEmitterPosition = emitterWorldPosition;
    emitter.hasLastEmitterPosition = true;

    // 時刻指定Burst。loop時はplayTimeの巻き戻しでcycle状態もリセットされる。
    if (emitter.burstCyclesFired.size() != emitter.bursts.size())
        emitter.burstCyclesFired.assign(emitter.bursts.size(), 0);
    for (size_t burstIndex = 0; burstIndex < emitter.bursts.size(); ++burstIndex) {
        const ParticleBurst& burst = emitter.bursts[burstIndex];
        int& fired = emitter.burstCyclesFired[burstIndex];
        const int cycles = (std::max)(burst.cycles, 1);
        while (fired < cycles
               && emitter.playTime >= burst.time + burst.interval * static_cast<float>(fired)) {
            if (Random01(emitter) <= Clamp01(burst.probability))
                emitter.burstPending += (std::max)(burst.count, 0);
            ++fired;
        }
    }

    // Prewarmは初回描画前に定常個数を投入する。GPU readbackを避けるため履歴位置は近似する。
    if (emitter.prewarm && emitter.loop && !emitter.prewarmed) {
        const float warmDuration = emitter.duration > 0.0f ? emitter.duration : emitter.lifetime;
        const int warmCount = static_cast<int>((std::max)(emitter.emitRate, 0.0f)
            * (std::max)(warmDuration, 0.0f));
        const int clampedWarmCount = (std::min)(warmCount, (std::max)(emitter.maxParticles, 0));
        emitter.burstPending += clampedWarmCount;
        emitter.prewarmSpawnPending += clampedWarmCount;
        emitter.prewarmed = true;
    }
    return canEmit;
}

} // anonymous namespace

void UpdateParticleCpuSimulation(Scene& scene, physics::World& world, float deltaTime, float time)
{
    const std::vector<ActiveForceField> forceFields = GatherForceFields(scene, ~0u);
    for (EntityID id : scene.GetEntities<ParticleEmitter>()) {
        auto* emitter = scene.GetComponent<ParticleEmitter>(id);
        GameObject* gameObject = scene.GetGameObject(id);
        if (!emitter || !gameObject || !gameObject->activeInHierarchy()
            || !emitter->enabled || CanUseGpuSimulation(*emitter)) continue;
        // GPU指定を一時的にCPUへ縮退した場合、制約解除後に古いGPU粒子が復活しないよう履歴を破棄する。
        if (emitter->simulationMode == ParticleSimulationMode::Gpu)
            emitter->gpuClearPending = true;
        if (emitter->lastCpuSimulationFrame == Time::frameCount) continue;
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
                           forceFields, &world, scene);
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

    // Overdraw パスが読む記録は毎回このパスが作り直す。
    // 早期 return より前に捨てるのが要点で、描かなかったフレームに前フレームの
    // エミッターポインタが残ると、破棄済みオブジェクトを Overdraw パスが触りうる。
    g_particleDrawRecords.clear();

    if (!h.particleShader.IsValid() || !h.particleIB.IsValid()) return;

    const float dt   = Time::deltaTime;
    const float time = Time::time;
    // 自己影の密度 RT はこのパス内で 1 回だけクリアし、各エミッターが積み増していく。
    // 毎エミッターでクリアすると自分の密度しか見えず、自己影の意味が無くなる。
    bool selfShadowClearedThisPass = false;

    // Distortionは現在のHDRを読みながら同じHDRへ書けないため、背景を専用RTへ退避してから読む。
    //
    // WHY (エミッターごとに取り直す): 1 回だけ退避すると、全ての歪みが「パーティクルを
    //     1 つも描いていない背景」を屈折する。歪みを 2 枚重ねても後ろの歪みが手前へ伝わらず、
    //     歪みの前に描いた炎や煙も屈折に映らない ― 重なり順が完全に無視された絵になる。
    //     歪みを使うエミッターの描画直前に取り直せば、それまでに描いた全てが屈折へ入る。
    // NOTE: 同一エミッター内で重なる粒子は 1 DrawCall なので、依然として同じ背景を共有する。
    //       これは 1 パス方式の原理的な限界で、粒子単位の順序を出すには
    //       粒子ごとに DrawCall を分ける (= 大量に出せなくなる) しかない。
    renderer::ResourceHandle<renderer::TextureTag> particleSceneColor;
    const bool needsSceneColor = std::any_of(ctx.scene.GameObjects().begin(), ctx.scene.GameObjects().end(),
        [](GameObject& object) {
            const auto* emitter = object.GetComponent<ParticleEmitter>();
            return emitter != nullptr && emitter->enabled && emitter->distortion;
        });
    // 現在の HDR を退避 RT へコピーし、そのテクスチャを返す。失敗時は無効ハンドル。
    const auto captureSceneColor = [&]() -> renderer::ResourceHandle<renderer::TextureTag> {
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
        if (!sceneColorRT.IsValid() || !copyShader.IsValid()) return {};
        renderer.SetRenderTarget(sceneColorRT, resources);
        renderer::DrawCall copy;
        copy.shader = copyShader; copy.pipelineState = h.postprocPSO; copy.vertexCount = 3;
        copy.textures[5] = resources.GetColorTexture(h.hdrRT, 0);
        renderer.Submit(copy, resources);
        renderer.SetRenderTarget(h.hdrRT, resources);
        return resources.GetColorTexture(sceneColorRT, 0);
    };
    if (needsSceneColor) particleSceneColor = captureSceneColor();

    // シーン内の力場を 1 回だけ収集し、全エミッター (CPU/GPU) で共有する
    const std::vector<ActiveForceField> forceFields = GatherForceFields(ctx);
    int particleBudget = ctx.settings.particleBudgetEnabled
        ? (std::max)(ctx.settings.particleBudget, 0) : 0;

    // 描画順を renderPriority → カメラ距離 (遠い順) で確定させる。
    // WHY: 半透明は描いた順に合成されるため、GameObject の並び順のままだと
    //      「炎の手前に煙」が保証されず、シーンを編集しただけで前後が入れ替わる。
    //      距離はバウンズ更新前なので Transform 位置で近似する (順序決定には十分)。
    // GameObjectRange は要素数を公開しないため reserve せずに積む
    // (パーティクルを持つ GameObject は通常わずかなので再確保は問題にならない)。
    std::vector<GameObject*> sortedEmitters;
    for (auto& candidate : ctx.scene.GameObjects()) {
        if (!ShouldRenderGameObject(candidate, ctx.cullingMask)) continue;
        auto* candidateEmitter = candidate.GetComponent<ParticleEmitter>();
        if (candidateEmitter == nullptr || !candidateEmitter->enabled) continue;
        sortedEmitters.push_back(&candidate);
    }
    const math::Vector3 cameraPosition = ctx.camera.m_position;
    // GetComponent は非 const 版しかないため、比較関数も非 const ポインタで受ける。
    std::stable_sort(sortedEmitters.begin(), sortedEmitters.end(),
        [cameraPosition](GameObject* a, GameObject* b) {
            const int priorityA = a->GetComponent<ParticleEmitter>()->renderPriority;
            const int priorityB = b->GetComponent<ParticleEmitter>()->renderPriority;
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
            // pauseWhenCulled が false なら「描かないだけ」で時間は進める。
            // WHY: 以前は両分岐とも continue で、このフラグは何の意味も持っていなかった。
            //      Play 中は ParticleSimulationSystem が別に回るので露見しないが、
            //      エディター (非 Play) ではこのパスがシミュレーションの実体そのものなので、
            //      カメラを画面外へ振った瞬間にエフェクトが凍り、戻すと止まった粒子が残っていた。
            if (!emitter->pauseWhenCulled) {
                (void)AdvanceEmitterPlayback(*emitter, tf, dtEmitter);
                if (!CanUseGpuSimulation(*emitter)
                    && emitter->lastCpuSimulationFrame != Time::frameCount)
                    SimulateCpuEmitter(*emitter, tf, animator, dtEmitter, time,
                                       forceFields, ctx.physicsWorld, ctx.scene);
            }
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

        const bool canEmit = AdvanceEmitterPlayback(*emitter, tf, dtEmitter);

        // GPU モードは CPU スポーン/更新をスキップして GPU パスへ
        if (isGpuMode) {
            const int requested = (std::max)(emitter->maxParticles, 0);
            const int drawLimit = particleBudget > 0 ? (std::min)(requested, particleBudget) : requested;
            emitter->visibleParticleCount = drawLimit;
            if (particleBudget > 0) particleBudget -= drawLimit;
            if (requested > drawLimit) ++ctx.statsParticleBudgetDropped;
            if (drawLimit <= 0) continue;
            EnsureParticleTexture(*emitter, resources);
            UpdateParticleRenderConstants(*emitter, resources, drawLimit, ctx.width, ctx.height,
                                          ShouldSortGpuParticles(*emitter, ctx.handles));
            ctx.statsParticleVisible += drawLimit;
            // CPU 経路と同じ理由でここでも取り直す (歪みの前後関係を保つ)。
            if (emitter->distortion && needsSceneColor) particleSceneColor = captureSceneColor();
            // メッシュパーティクルは同じ GameObject の MeshRenderer が形状を持つ
            // (VFXGraphSystem が meshParticlePath から生成する)。GPU 経路はこれをインスタンス描画する。
            const MeshRenderer* meshParticleRenderer = emitter->meshParticlePath.empty()
                ? nullptr : go.GetComponent<MeshRenderer>();
            TickGpuEmitter(*emitter, tf, animator, dtEmitter, time, canEmit, forceFields,
                           particleSceneColor, meshParticleRenderer, ctx);
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
                QueueSubEmitter(ctx.scene, *emitter, emitter->birthSubEmitter, emitter->subEmitterBurstCount);
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
                QueueSubEmitter(ctx.scene, *emitter, emitter->birthSubEmitter, emitter->subEmitterBurstCount);
            }
            if (static_cast<int>(emitter->particles.size()) >= particleCapacity) {
                // WHY: 生成ループの途中で上限に達した場合も、残った未放出分を次フレームへ持ち越さない。
                emitter->emitAccum = 0.0f;
            }
        } else {
            emitter->emitAccum = 0.0f;
            emitter->burstPending = 0;
        }

        // 周回・放射の回転中心。粒子位置と同じ空間へ揃えるため、ここで一度だけ解決する
        // (粒子ごとに変換すると同じ計算を粒子数ぶん繰り返すことになる)。
        const math::Vector3 orbitalOrigin =
            emitter->simulationSpace == ParticleSimulationSpace::Local
                ? emitter->emitPosition
                : TransformEmitterPoint(tf, emitter->emitPosition);

        // パーティクル更新・寿命切れを削除
        for (auto it = emitter->particles.begin(); it != emitter->particles.end(); ) {
            it->age += dtEmitter;
            if (it->age >= it->lifetime) {
                ++emitter->deathCountThisFrame;
                QueueSubEmitter(ctx.scene, *emitter, emitter->deathSubEmitter, emitter->subEmitterBurstCount);
                it = emitter->particles.erase(it);
                continue;
            }
            float t = Clamp01(it->age / (std::max)(it->lifetime, 0.001f));
            it->velocity.x += emitter->gravity.x * dtEmitter;
            it->velocity.y += emitter->gravity.y * dtEmitter;
            it->velocity.z += emitter->gravity.z * dtEmitter;
            // 周回・放射。式は ParticleGpuSim.cs.hlsl の ApplyOrbitalVelocity と一致させること。
            // 回転中心は粒子位置と同じ空間で渡す (Local はローカル原点、World はワールド変換後)。
            ApplyOrbitalVelocity(*emitter, orbitalOrigin, it->position, it->velocity, dtEmitter);
            // drag カーブは既存の velocityDamping に対する時間倍率として掛ける。
            const float dragScale = emitter->useDragCurve
                ? (std::max)(emitter->dragCurve.Evaluate(t), 0.0f) : 1.0f;
            const float damping =
                (std::max)(0.0f, 1.0f - emitter->velocityDamping * dragScale * dtEmitter);
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
            // 回転カーブは角速度への時間倍率。勢いよく回り始めて減速する破片が作れる。
            it->rotation += it->angularVelocity * dtEmitter
                * (emitter->useRotationCurve ? emitter->rotationCurve.Evaluate(t) : 1.0f);
            const SpriteFrameState sprite = ComputeSpriteFrameState(*emitter, t, it->age, it->spriteSeed);
            it->uvRect = sprite.currentRect;
            it->nextUvRect = sprite.nextRect;
            it->spriteBlend = sprite.blend;
            it->color = EvaluateParticleColorLinear(*emitter, *it, t);
            const float sizeT = emitter->useSizeCurve
                ? Clamp01(emitter->sizeCurve.Evaluate(t))
                : std::pow(t, emitter->sizeCurvePower);
            it->size = it->startSize + (it->endSize - it->startSize) * sizeT;
            AppendParticleTrailPoint(*emitter, *it, dtEmitter);
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
        UpdateParticleRenderConstants(*emitter, resources, count, ctx.width, ctx.height);
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

        const bool localSpace = emitter->simulationSpace == ParticleSimulationSpace::Local;
        // 連続リボン指定なら、尾はビルボードではなく帯として別 DrawCall で描く。
        // ここで 0 にしておかないと、帯とビルボードの二重描画になる。
        const bool ribbonTrail = emitter->trailEnabled && emitter->trailRibbon;
        const int trailPoints = (emitter->trailEnabled && !ribbonTrail)
            ? std::clamp(emitter->trailPointCount, 1, kMaxParticleTrailPoints) : 0;
        for (int i = 0; i < count; ++i) {
            const auto& p = emitter->particles[i];
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
                const float widthLerp = emitter->trailWidthScale
                    + (1.0f - emitter->trailWidthScale) * fade;
                const float alphaLerp = emitter->trailAlphaScale
                    + (1.0f - emitter->trailAlphaScale) * fade;
                // p.color はリニア、tint はオーサリング値 (sRGB)。tint をリニアへ揃えてから
                // 掛ける。揃えないと、同じ tint がビルボード尾とリボン尾で違う色になる。
                const math::Vector4 tint = ParticleSrgbToLinear(emitter->trailColorTint);
                const math::Vector4 trailColor = {
                    p.color.x * tint.x,
                    p.color.y * tint.y,
                    p.color.z * tint.z,
                    p.color.w * emitter->trailColorTint.w * alphaLerp
                };
                emitQuad(trailPosition, renderVelocity, p.size * widthLerp, p.rotation,
                         trailColor, p);
            }
        }

        const auto particlePSO = SelectParticlePSO(*emitter, h.particlePSO, h.particleAlphaPSO,
                                                   h.particlePremultipliedPSO);
        if (!particlePSO.IsValid() || !emitter->texture.IsValid())
            continue;

        // 頂点はエミッターごとに別バッファへ載せる (共有 1 本だと DX12 で先の Draw が壊れる)。
        const auto particleVB = g_particleVertexPool.Acquire(
            resources, verts.size(), static_cast<uint32_t>(sizeof(ParticleVertex)));
        if (!particleVB.IsValid()) continue;
        resources.Update(particleVB, verts.data(),
                         static_cast<uint32_t>(verts.size() * sizeof(ParticleVertex)));

        // 歪みを使うエミッターは、その直前までに描いた絵を屈折させる。
        // 退避を取り直さないと「パーティクルを 1 つも描いていない背景」を屈折し続け、
        // 歪みを重ねたときの前後関係が完全に失われる。
        if (emitter->distortion && needsSceneColor) particleSceneColor = captureSceneColor();

        // 自己影: このエミッターの密度を光源側 RT へ積む。頂点バッファに今の形が
        // 乗っている間しか測れないので、本番描画の直前に行う。
        bool selfShadowReady = false;
        if (emitter->selfShadowStrength > 0.0f
            && PrepareParticleSelfShadowTarget(ctx, selfShadowClearedThisPass)) {
            AccumulateParticleSelfShadowDensity(*emitter, quadCount, particleVB, ctx);
            selfShadowReady = true;
        }

        renderer::DrawCall dc;
        dc.vertexBuffer       = particleVB;
        dc.indexBuffer        = h.particleIB;
        // 粒子本体 + トレイルの合計クワッド数。count のままだと尾が描かれない。
        dc.indexCount         = static_cast<uint32_t>(quadCount * 6);
        dc.shader             = h.particleShader;
        dc.pipelineState      = particlePSO;
        dc.constantBuffers[0] = h.frameCB;
        dc.constantBuffers[2] = emitter->renderCB;
        // 受け影は Surface マテリアルと同じ b4 (ShadowConstants) / t8 (シャドウマップ) を使う。
        // シェーダー側は常に宣言しているため、有効/無効に関わらずバインドしておく
        // (未バインドの SRV を読むと環境によっては未定義値になる)。
        dc.constantBuffers[4] = h.shadowCB;
        dc.textures[0]        = emitter->texture;
        // t1: 歪み専用ノーマルマップ。未設定なら無効ハンドルのままで、
        // PS は effectsFlags を見て albedo の RG へ縮退する。
        dc.textures[1]        = emitter->distortionTexture;
        dc.textures[5]        = particleSceneColor;
        dc.textures[6]        = emitter->motionVectorTexture;
        dc.textures[7]        = resources.GetDepthTexture(h.decalDepthRT);
        dc.textures[8]        = resources.GetDepthTexture(h.shadowMapRT);
        // t9: 自己影の密度。有効でないときは何もバインドしない
        // (シェーダーは selfShadowStrength が 0 なら参照しない)。
        if (selfShadowReady)
            dc.textures[9] = resources.GetColorTexture(h.particleSelfShadowRT, 0);
        renderer.SetSampler(0, renderer::SamplerMode::WRAP_BILINEAR);
        renderer.SetSampler(1, renderer::SamplerMode::BORDER_ZERO);
        SubmitCounted(ctx, dc);
        g_particleDrawRecords.push_back({ emitter, particleVB, quadCount });

        // 帯は本体の後に描く。帯の方が面積が大きく、先に描くと本体が沈んで見えるため。
        if (ribbonTrail) DrawParticleTrailRibbons(*emitter, tf, count, ctx);
    }
}

// パーティクルの重なり枚数を可視化する。
// Particle パスの直後に、同じ頂点バッファを計数シェーダーで専用 RT へ描き直し、
// ヒートマップへ変換して HDR RT を上書きする。
//
// WHY (Particle パス内でやらない): 計数には「加算のみ・専用 RT」が要るが、
//     Particle パスは HDR へ通常の色を描く。同じパスに同居させると
//     RT 切り替えとブレンド状態の分岐が本番描画側へ漏れ込み、
//     診断機能のために本番の描画順が変わりかねない。独立したパスへ分ける。
void ExecuteParticleOverdrawPass(RenderPassContext& ctx)
{
    if (!ctx.settings.particleOverdrawView) return;

    auto& renderer = ctx.renderer;
    auto& resources = ctx.resources;
    auto& h = ctx.handles;
    if (!h.particleIB.IsValid()) return;

    // 計数 RT とシェーダーは診断を有効にしたときだけ作る。
    // resetVersion を見て、デバイスロストや再初期化のあとで作り直す。
    static renderer::ResourceHandle<renderer::RenderTargetTag> overdrawRT;
    static renderer::ResourceHandle<renderer::ShaderTag> countShader;
    static renderer::ResourceHandle<renderer::ShaderTag> heatmapShader;
    static std::uint64_t resetVersion = 0;
    static std::uint32_t rtWidth = 0;
    static std::uint32_t rtHeight = 0;
    if (resetVersion != resources.GetResetVersion()) {
        resetVersion = resources.GetResetVersion();
        overdrawRT = {}; rtWidth = 0; rtHeight = 0;
        countShader = resources.LoadShader("Assets/Shaders/Debug/ParticleOverdraw.hlsl");
        heatmapShader = resources.LoadShader("Assets/Shaders/Debug/OverdrawHeatmap.hlsl");
    }
    if (!countShader.IsValid() || !heatmapShader.IsValid()) return;
    if (!overdrawRT.IsValid() || rtWidth != ctx.width || rtHeight != ctx.height) {
        if (overdrawRT.IsValid()) resources.Release(overdrawRT);
        overdrawRT = resources.CreateRenderTarget(ctx.width, ctx.height, 1);
        rtWidth = ctx.width; rtHeight = ctx.height;
    }
    if (!overdrawRT.IsValid()) return;

    // 計数: 黒でクリアし、パーティクルを 1 枚あたり R+1 で積む。
    renderer.SetRenderTarget(overdrawRT, resources);
    renderer.Clear({ 0.0f, 0.0f, 0.0f, 1.0f });

    // Particle パスが本番描画に使ったバッファとクワッド数をそのまま数え直す。
    // ここで作り直すと「実際に描いた形」とずれ、測る意味がなくなる。
    // GPU シミュレーションとメッシュパーティクルは頂点バッファを持たないため記録に載らず、
    // この計数経路の対象外になる (CPU ビルボードだけを測る)。
    for (const ParticleDrawRecord& record : g_particleDrawRecords) {
        if (!record.vertexBuffer.IsValid() || record.quadCount <= 0) continue;

        renderer::DrawCall dc;
        dc.vertexBuffer       = record.vertexBuffer;
        dc.indexBuffer        = h.particleIB;
        dc.indexCount         = static_cast<uint32_t>(record.quadCount * 6);
        dc.shader             = countShader;
        dc.pipelineState      = h.particlePSO;   // ADDITIVE + DEPTH_READ
        dc.constantBuffers[0] = h.frameCB;
        dc.constantBuffers[2] = record.emitter->renderCB;
        dc.textures[0]        = record.emitter->texture;
        renderer.SetSampler(0, renderer::SamplerMode::WRAP_BILINEAR);
        renderer.Submit(dc, resources);
    }

    // ヒートマップ化して HDR へ上書きする。
    renderer.SetRenderTarget(h.hdrRT, resources);

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

} // namespace fbzz::scene
