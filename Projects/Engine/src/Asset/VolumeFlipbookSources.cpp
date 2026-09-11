/// @file    VolumeFlipbookSources.cpp
/// @brief   ソースの登録表と、同梱ソース (煙・炎・水・血・汎用エミッター) の組み立て。
/// @author  Hasegawa Jin
/// @date    2026-09-11
#include <Engine/Asset/VolumeFlipbookSources.hpp>

#include <Engine/Asset/VolumeFlipbookBaker.hpp>
#include <Engine/Core/CurlNoise.hpp>

#include <algorithm>
#include <cmath>

namespace fbzz::asset {
namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kDegreesToRadians = kPi / 180.0f;

float Hash01(std::uint32_t seed, std::uint32_t index, std::uint32_t channel)
{
    return VolumeHash01(seed, index, channel);
}

float Signed(std::uint32_t seed, std::uint32_t index, std::uint32_t channel)
{
    return VolumeHashSigned(seed, index, channel);
}

math::Vector3 RandomAxis(std::uint32_t seed, std::uint32_t index)
{
    const math::Vector3 axis = { Signed(seed, index, 11), Signed(seed, index, 12), Signed(seed, index, 13) };
    return axis.NormalizedOr(math::Vector3::UP);
}

float BakeDuration(const VolumeSourceSettings& settings)
{
    return static_cast<float>((std::max)(settings.frameCount, 1)) * (std::max)(settings.frameDt, 1.0e-4f);
}

// ---- Puff / Fireball ---------------------------------------------------------
// 一発ものはベイク全体の長さで正規化する。移動量・膨張・回転・冷却を «ベイクの始めから終わりまでに»
// どれだけ進むかで決めるので、コマ数や FPS を変えても構図が変わらず、箱からもはみ出さない。

std::vector<VolumePuff> BuildPuff(const VolumeSourceSettings& settings, const VolumeSourceRange& range)
{
    const float duration = range.duration;
    VolumePuff puff;
    puff.birthTime = range.start;
    puff.startCenter = { 0.0f, -0.4f, 0.0f };
    puff.velocity = math::Vector3{ 0.13f, 0.65f, 0.0f } / duration;
    puff.angularVelocity = math::Vector3{ 0.3f, 1.0f, 0.2f }.NormalizedOr(math::Vector3::UP) * (3.0f / duration);
    puff.expansionRate = std::log(1.8f) / duration;
    puff.radius = 0.35f;
    puff.density = 1.0f;
    puff.noiseSeedOffset = Hash01(settings.seed, 0, 8) * 97.0f;
    return { puff };
}

std::vector<VolumePuff> BuildFireball(const VolumeSourceSettings& settings, const VolumeSourceRange& range)
{
    const std::uint32_t seed = settings.seed;
    const float duration = range.duration;
    std::vector<VolumePuff> puffs;
    VolumePuff center;
    center.birthTime = range.start;
    // 最終コマでちょうど消え切るよう、寿命をベイクの長さに合わせる。
    center.lifetime = duration * 1.001f;
    center.fadeOut = duration * 0.5f;
    center.startCenter = { 0.0f, -0.3f, 0.0f };
    center.velocity = { 0.0f, 0.25f / duration, 0.0f };
    center.angularVelocity = RandomAxis(seed, 0) * (4.0f / duration);
    center.expansionRate = std::log(2.2f) / duration;
    center.radius = 0.16f;
    center.density = 1.5f;
    center.temperature = 1.0f;
    center.coolingTime = duration * 0.2f;
    center.noiseSeedOffset = Hash01(seed, 0, 8) * 97.0f;
    puffs.push_back(center);
    const math::Vector3 directions[] = {
        { 1.0f, 0.2f, 0.0f }, { -1.0f, 0.2f, 0.0f }, { 0.0f, 0.2f, 1.0f },
        { 0.0f, 0.2f, -1.0f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, -0.6f, 0.0f } };
    std::uint32_t index = 1;
    for (const math::Vector3& raw : directions) {
        const math::Vector3 direction = raw.NormalizedOr(math::Vector3::UP);
        VolumePuff lobe = center;
        lobe.startCenter = center.startCenter + direction * 0.12f;
        lobe.velocity = (direction * 0.45f + math::Vector3{ 0.0f, 0.25f, 0.0f }) / duration;
        lobe.angularVelocity = RandomAxis(seed, index) * (6.0f / duration);
        lobe.expansionRate = std::log(2.6f) / duration;
        lobe.radius = 0.13f;
        lobe.noiseSeedOffset = Hash01(seed, index, 8) * 97.0f;
        puffs.push_back(lobe);
        ++index;
    }
    return puffs;
}

// ---- RisingPlume -------------------------------------------------------------

// 上端が bake 空間の箱 (y = 1) に届かない寿命と上昇速度。届くと箱の面で煙の頭が切れる。
// 見積もり: -0.78 + (0.42 + 0.1)·2.2 + 0.2·e^{0.45·2.2}·1.1 ≈ 0.9
constexpr float kPlumeLifetime = 2.2f;
constexpr float kPlumeSpawnPeriod = 0.12f;
constexpr float kPlumeBaseY = -0.78f;
constexpr float kPlumeRiseSpeed = 0.42f;

VolumePuff MakePlumePuff(std::uint32_t seed, std::uint32_t variant, float birthTime)
{
    VolumePuff puff;
    puff.birthTime = birthTime;
    puff.lifetime = kPlumeLifetime;
    puff.fadeIn = 0.25f;
    puff.fadeOut = 0.6f;
    puff.startCenter = { Signed(seed, variant, 1) * 0.06f, kPlumeBaseY, Signed(seed, variant, 2) * 0.06f };
    puff.velocity = { Signed(seed, variant, 3) * 0.08f, kPlumeRiseSpeed + Signed(seed, variant, 4) * 0.1f,
                      Signed(seed, variant, 5) * 0.08f };
    puff.angularVelocity = RandomAxis(seed, variant) * (1.2f + Hash01(seed, variant, 6) * 0.6f);
    puff.expansionRate = 0.45f;
    puff.radius = 0.16f + Hash01(seed, variant, 7) * 0.04f;
    puff.density = 1.2f;
    puff.temperature = 1.0f;
    // 0.5 秒だと見えている puff の大半が冷え切り、炎の芯が暗赤にしかならなかった (発光の最大 0.22)。
    puff.coolingTime = 0.8f;
    puff.noiseSeedOffset = Hash01(seed, variant, 8) * 97.0f;
    return puff;
}

// ---- Torch (ループする炎) ------------------------------------------------------

constexpr float kTorchLifetime = 0.95f;
constexpr float kTorchSpawnPeriod = 0.05f;

VolumePuff MakeTorchPuff(std::uint32_t seed, std::uint32_t variant, float birthTime)
{
    VolumePuff puff;
    puff.birthTime = birthTime;
    puff.lifetime = kTorchLifetime;
    puff.fadeIn = 0.08f;
    puff.fadeOut = 0.45f;
    puff.startCenter = { Signed(seed, variant, 1) * 0.07f, -0.72f, Signed(seed, variant, 2) * 0.07f };
    // 外側で生まれた puff ほど芯へ寄せる。炎が根元から先へ細る。
    puff.velocity = { -puff.startCenter.x * 0.6f + Signed(seed, variant, 3) * 0.05f,
                      0.95f + Signed(seed, variant, 4) * 0.15f,
                      -puff.startCenter.z * 0.6f + Signed(seed, variant, 5) * 0.05f };
    puff.acceleration = { 0.0f, 0.5f, 0.0f };
    puff.angularVelocity = RandomAxis(seed, variant) * (2.0f + Hash01(seed, variant, 6) * 1.5f);
    puff.expansionRate = std::log(1.7f) / kTorchLifetime;
    puff.radius = 0.1f + Hash01(seed, variant, 7) * 0.03f;
    // 上へ伸ばして «舌» にする (速度が上向きなので伸びる向きも上)。
    puff.stretch = 1.5f;
    puff.density = 1.1f;
    puff.temperature = 1.0f;
    puff.coolingTime = 0.5f;
    puff.noiseSeedOffset = Hash01(seed, variant, 8) * 97.0f;
    puff.colorKey = Hash01(seed, variant, 9);
    return puff;
}

// ---- Fountain (ループする水柱) --------------------------------------------------

constexpr float kFountainLifetime = 1.35f;
constexpr float kFountainSpawnPeriod = 0.025f;

VolumePuff MakeFountainPuff(std::uint32_t seed, std::uint32_t variant, float birthTime)
{
    VolumePuff puff;
    puff.birthTime = birthTime;
    puff.lifetime = kFountainLifetime;
    puff.fadeIn = 0.04f;
    puff.fadeOut = 0.3f;
    puff.startCenter = { Signed(seed, variant, 1) * 0.03f, -0.82f, Signed(seed, variant, 2) * 0.03f };
    const math::Vector3 direction = VolumeRandomInCone(math::Vector3::UP, 12.0f * kDegreesToRadians, seed, variant, 3);
    puff.velocity = direction * (2.15f + Signed(seed, variant, 5) * 0.12f);
    puff.acceleration = { 0.0f, -3.0f, 0.0f };
    puff.angularVelocity = RandomAxis(seed, variant) * 1.5f;
    puff.expansionRate = std::log(1.3f) / kFountainLifetime;
    puff.radius = 0.055f + Hash01(seed, variant, 7) * 0.02f;
    puff.stretchPerSpeed = 0.35f;
    puff.density = 1.8f;
    puff.liquid = 1.0f;
    puff.noiseScale = 0.3f;
    puff.noiseSeedOffset = Hash01(seed, variant, 8) * 97.0f;
    puff.colorKey = Hash01(seed, variant, 9);
    return puff;
}

// ---- 一発ものの液体 (duration で正規化) ----------------------------------------
// 速度は «ベイク全体で進む量»、加速度は «ベイク全体の 2 乗» で割る。stretchPerSpeed は速さ [単位/秒] に
// 掛かるので duration を掛けておく (掛けないとコマ数で液滴の伸びが変わる)。

VolumePuff MakeDroplet(float start, float duration, const math::Vector3& origin, const math::Vector3& velocity,
                       float gravity, float radius, float stretchPerSpeed)
{
    VolumePuff puff;
    puff.birthTime = start;
    puff.lifetime = duration * 1.001f;
    puff.fadeIn = duration * 0.02f;
    puff.fadeOut = duration * 0.25f;
    puff.startCenter = origin;
    puff.velocity = velocity / duration;
    puff.acceleration = { 0.0f, gravity / (duration * duration), 0.0f };
    puff.radius = radius;
    puff.stretchPerSpeed = stretchPerSpeed * duration;
    puff.density = 1.8f;
    puff.liquid = 1.0f;
    puff.noiseScale = 0.25f;
    return puff;
}

VolumePuff MakeMist(float start, float duration, const math::Vector3& origin, const math::Vector3& velocity,
                    float radius, float colorKey)
{
    VolumePuff puff;
    puff.birthTime = start;
    puff.lifetime = duration * 1.001f;
    puff.fadeIn = duration * 0.05f;
    puff.fadeOut = duration * 0.6f;
    puff.startCenter = origin;
    puff.velocity = velocity / duration;
    puff.expansionRate = std::log(2.4f) / duration;
    puff.radius = radius;
    puff.density = 0.35f;
    puff.colorKey = colorKey;
    return puff;
}

std::vector<VolumePuff> BuildWaterSplash(const VolumeSourceSettings& settings, const VolumeSourceRange& range)
{
    const std::uint32_t seed = settings.seed;
    const float duration = range.duration;
    constexpr float kGravity = -4.2f;
    const math::Vector3 origin = { 0.0f, -0.62f, 0.0f };
    std::vector<VolumePuff> puffs;

    // 王冠: 輪から外へ、上へ飛んで落ちる。
    constexpr std::uint32_t kCrown = 32;
    for (std::uint32_t i = 0; i < kCrown; ++i) {
        const float angle = 2.0f * kPi * (static_cast<float>(i) + 0.3f * Signed(seed, i, 1)) / static_cast<float>(kCrown);
        const math::Vector3 outward = { std::cos(angle), 0.0f, std::sin(angle) };
        const math::Vector3 velocity = outward * (0.55f + 0.15f * Hash01(seed, i, 2))
            + math::Vector3{ 0.0f, 1.9f + 0.35f * Hash01(seed, i, 3), 0.0f };
        VolumePuff drop = MakeDroplet(range.start, duration, origin + outward * 0.1f, velocity, kGravity,
                                      0.045f + 0.015f * Hash01(seed, i, 4), 0.28f);
        drop.angularVelocity = RandomAxis(seed, i) * (2.0f / duration);
        drop.noiseSeedOffset = Hash01(seed, i, 8) * 97.0f;
        drop.colorKey = Hash01(seed, i, 9);
        puffs.push_back(drop);
    }

    // 中央の水柱: 王冠より少し遅れて、細く高く上がる。
    const float columnDelay = duration * 0.18f;
    const float speeds[] = { 2.0f, 2.35f, 2.7f };
    const float radii[] = { 0.07f, 0.06f, 0.05f };
    for (std::uint32_t i = 0; i < 3; ++i) {
        const std::uint32_t index = kCrown + i;
        const math::Vector3 velocity = { Signed(seed, index, 2) * 0.05f, speeds[i], Signed(seed, index, 3) * 0.05f };
        VolumePuff column = MakeDroplet(range.start + columnDelay, duration, origin, velocity, kGravity, radii[i], 0.28f);
        column.lifetime = (duration - columnDelay) * 1.001f;
        column.noiseSeedOffset = Hash01(seed, index, 8) * 97.0f;
        column.colorKey = 0.2f * Hash01(seed, index, 9);
        puffs.push_back(column);
    }

    // 水煙: 着水点の周りに白く漂う。
    for (std::uint32_t i = 0; i < 5; ++i) {
        const std::uint32_t index = kCrown + 3 + i;
        const float angle = 2.0f * kPi * (static_cast<float>(i) + Hash01(seed, index, 1)) / 5.0f;
        const math::Vector3 outward = { std::cos(angle), 0.0f, std::sin(angle) };
        VolumePuff mist = MakeMist(range.start, duration, origin + outward * 0.08f,
                                   outward * 0.3f + math::Vector3{ 0.0f, 0.25f, 0.0f }, 0.12f, 0.0f);
        mist.angularVelocity = RandomAxis(seed, index) * (1.5f / duration);
        mist.noiseSeedOffset = Hash01(seed, index, 8) * 97.0f;
        puffs.push_back(mist);
    }
    return puffs;
}

std::vector<VolumePuff> BuildBloodSpray(const VolumeSourceSettings& settings, const VolumeSourceRange& range)
{
    const std::uint32_t seed = settings.seed;
    const float duration = range.duration;
    constexpr float kGravity = -1.6f;
    const math::Vector3 origin = { -0.6f, 0.1f, 0.0f };
    const math::Vector3 axis = math::Vector3{ 1.0f, 0.35f, 0.0f }.NormalizedOr(math::Vector3::RIGHT);
    std::vector<VolumePuff> puffs;

    // 細かい飛沫: 円錐に広がり、速いものほど細長い筋になる。
    constexpr std::uint32_t kDroplets = 56;
    for (std::uint32_t i = 0; i < kDroplets; ++i) {
        const math::Vector3 direction = VolumeRandomInCone(axis, 20.0f * kDegreesToRadians, seed, i, 1);
        const float delay = duration * 0.08f * Hash01(seed, i, 3);
        VolumePuff drop = MakeDroplet(range.start + delay, duration, origin,
                                      direction * (0.85f + 0.45f * Hash01(seed, i, 4)), kGravity,
                                      0.022f + 0.022f * Hash01(seed, i, 5), 1.2f);
        drop.lifetime = (duration - delay) * (0.75f + 0.25f * Hash01(seed, i, 6)) * 1.001f;
        drop.fadeOut = duration * 0.2f;
        drop.density = 2.0f;
        drop.noiseScale = 0.2f;
        drop.noiseSeedOffset = Hash01(seed, i, 8) * 97.0f;
        drop.colorKey = Hash01(seed, i, 9);
        puffs.push_back(drop);
    }

    // 太い塊: 遅れて続けて出て、互いに繋がって «流れ» に見える。
    for (std::uint32_t i = 0; i < 6; ++i) {
        const std::uint32_t index = kDroplets + i;
        const math::Vector3 direction = VolumeRandomInCone(axis, 8.0f * kDegreesToRadians, seed, index, 1);
        const float delay = duration * 0.03f * static_cast<float>(i);
        VolumePuff gush = MakeDroplet(range.start + delay, duration, origin,
                                      direction * (0.55f + 0.2f * Hash01(seed, index, 4)), kGravity,
                                      0.06f + 0.015f * Hash01(seed, index, 5), 0.8f);
        gush.lifetime = (duration - delay) * 1.001f;
        gush.density = 2.0f;
        gush.noiseScale = 0.35f;
        gush.angularVelocity = RandomAxis(seed, index) * (3.0f / duration);
        gush.noiseSeedOffset = Hash01(seed, index, 8) * 97.0f;
        gush.colorKey = 0.3f * Hash01(seed, index, 9);
        puffs.push_back(gush);
    }

    // 赤い霧: 出どころにだけ薄く残る。
    for (std::uint32_t i = 0; i < 3; ++i) {
        const std::uint32_t index = kDroplets + 6 + i;
        VolumePuff mist = MakeMist(range.start, duration, origin + axis * (0.05f * static_cast<float>(i)),
                                   axis * (0.25f + 0.1f * static_cast<float>(i)), 0.09f, 0.6f);
        mist.density = 0.3f;
        mist.angularVelocity = RandomAxis(seed, index) * (2.0f / duration);
        mist.noiseSeedOffset = Hash01(seed, index, 8) * 97.0f;
        puffs.push_back(mist);
    }
    return puffs;
}

std::vector<VolumePuff> BuildBloodBurst(const VolumeSourceSettings& settings, const VolumeSourceRange& range)
{
    const std::uint32_t seed = settings.seed;
    const float duration = range.duration;
    constexpr float kGravity = -1.4f;
    const math::Vector3 origin = { 0.0f, -0.05f, 0.0f };
    std::vector<VolumePuff> puffs;

    // 着弾点から上半球へ散る。下向きに飛ぶ飛沫は箱の底をすぐ抜けるので出さない。
    constexpr std::uint32_t kDroplets = 48;
    for (std::uint32_t i = 0; i < kDroplets; ++i) {
        const math::Vector3 direction = VolumeRandomInCone(math::Vector3::UP, 80.0f * kDegreesToRadians, seed, i, 1);
        VolumePuff drop = MakeDroplet(range.start, duration, origin,
                                      direction * (0.45f + 0.35f * Hash01(seed, i, 4)), kGravity,
                                      0.02f + 0.025f * Hash01(seed, i, 5), 1.2f);
        drop.lifetime = duration * (0.7f + 0.3f * Hash01(seed, i, 6)) * 1.001f;
        drop.fadeOut = duration * 0.2f;
        drop.density = 2.0f;
        drop.noiseScale = 0.2f;
        drop.noiseSeedOffset = Hash01(seed, i, 8) * 97.0f;
        drop.colorKey = Hash01(seed, i, 9);
        puffs.push_back(drop);
    }

    // 芯: 一瞬膨らんで砕ける塊。
    VolumePuff core = MakeDroplet(range.start, duration, origin, { 0.0f, 0.1f, 0.0f }, 0.0f, 0.1f, 0.0f);
    core.lifetime = duration * 0.45f;
    core.fadeOut = duration * 0.3f;
    core.expansionRate = std::log(1.8f) / (duration * 0.45f);
    core.density = 2.0f;
    core.noiseScale = 0.8f;
    core.angularVelocity = RandomAxis(seed, kDroplets) * (4.0f / duration);
    core.noiseSeedOffset = Hash01(seed, kDroplets, 8) * 97.0f;
    core.colorKey = 0.15f;
    puffs.push_back(core);

    for (std::uint32_t i = 0; i < 4; ++i) {
        const std::uint32_t index = kDroplets + 1 + i;
        const math::Vector3 direction = VolumeRandomInCone(math::Vector3::UP, 60.0f * kDegreesToRadians, seed, index, 1);
        VolumePuff mist = MakeMist(range.start, duration, origin, direction * 0.3f, 0.08f, 0.6f);
        mist.density = 0.3f;
        mist.angularVelocity = RandomAxis(seed, index) * (2.0f / duration);
        mist.noiseSeedOffset = Hash01(seed, index, 8) * 97.0f;
        puffs.push_back(mist);
    }
    return puffs;
}

// ---- Emitter (C++ を書かずに作る汎用ソース) -------------------------------------

float EmitterMaxLifetime(const VolumeEmitterSettings& emitter)
{
    return (std::max)(emitter.lifetime, 0.05f) * (1.0f + std::clamp(emitter.lifetimeRandom, 0.0f, 1.0f));
}

VolumePuff MakeEmitterPuff(const VolumeEmitterSettings& emitter, std::uint32_t seed, std::uint32_t variant,
                           float birthTime)
{
    const float lifetime = (std::max)(0.05f,
        (std::max)(emitter.lifetime, 0.05f) * (1.0f + std::clamp(emitter.lifetimeRandom, 0.0f, 1.0f) * Signed(seed, variant, 20)));
    VolumePuff puff;
    puff.birthTime = birthTime;
    puff.lifetime = lifetime;
    puff.fadeIn = (std::max)(emitter.fadeIn, 0.0f);
    puff.fadeOut = (std::max)(emitter.fadeOut, 0.0f);
    // 球の中で一様にするため、半径は一様乱数の立方根。
    const float offset = (std::max)(emitter.originRadius, 0.0f) * std::cbrt(Hash01(seed, variant, 23));
    puff.startCenter = emitter.origin + VolumeRandomDirection(seed, variant, 21) * offset;
    const math::Vector3 direction = VolumeRandomInCone(emitter.direction.NormalizedOr(math::Vector3::UP),
                                                       std::clamp(emitter.coneAngleDegrees, 0.0f, 180.0f) * kDegreesToRadians,
                                                       seed, variant, 24);
    puff.velocity = direction
        * (emitter.speed * (1.0f + std::clamp(emitter.speedRandom, 0.0f, 1.0f) * Signed(seed, variant, 26)));
    puff.acceleration = emitter.gravity;
    puff.drag = (std::max)(emitter.drag, 0.0f);
    puff.angularVelocity = RandomAxis(seed, variant) * emitter.spin;
    puff.expansionRate = std::log((std::max)(emitter.growth, 0.01f)) / lifetime;
    puff.radius = (std::max)(0.005f,
        emitter.radius * (1.0f + std::clamp(emitter.radiusRandom, 0.0f, 1.0f) * Signed(seed, variant, 27)));
    puff.stretch = emitter.stretch;
    puff.stretchPerSpeed = emitter.stretchPerSpeed;
    puff.density = (std::max)(emitter.density, 0.0f);
    puff.temperature = std::clamp(emitter.temperature, 0.0f, 1.0f);
    puff.coolingTime = (std::max)(emitter.coolingTime, 0.01f);
    puff.liquid = std::clamp(emitter.liquid, 0.0f, 1.0f);
    puff.noiseScale = (std::max)(emitter.noiseScale, 0.0f);
    puff.noiseSeedOffset = Hash01(seed, variant, 8) * 97.0f;
    const float t = Hash01(seed, variant, 28);
    puff.colorKey = emitter.colorKeyMin + (emitter.colorKeyMax - emitter.colorKeyMin) * t;
    return puff;
}

std::vector<VolumePuff> BuildEmitter(const VolumeSourceSettings& settings, const VolumeSourceRange& range)
{
    const VolumeEmitterSettings& emitter = settings.emitter;
    std::vector<VolumePuff> puffs;
    if (emitter.burst) {
        const int count = std::clamp(emitter.count, 1, static_cast<int>(kVolumeFillMaxPuffs));
        for (int i = 0; i < count; ++i)
            puffs.push_back(MakeEmitterPuff(emitter, settings.seed, static_cast<std::uint32_t>(i), range.start));
        return puffs;
    }
    const float period = 1.0f / static_cast<float>((std::max)(emitter.count, 1));
    AppendPeriodicVolumePuffs(settings, range, period, EmitterMaxLifetime(emitter),
        [&emitter, seed = settings.seed](std::uint32_t variant, float birthTime) {
            return MakeEmitterPuff(emitter, seed, variant, birthTime);
        },
        puffs);
    return puffs;
}

// ---- Look ----------------------------------------------------------------------

void ApplyTorchLook(VolumeFlipbookBakeSettings& settings)
{
    // 冷えた puff は煤けた暗い煙になる。
    settings.albedoRamp = UniformVolumeRamp({ 0.28f, 0.26f, 0.25f });
    settings.extinction = 8.0f;
    settings.emissionIntensity = 8.0f;
    settings.exposure = 0.7f;
}

void ApplyWaterLook(VolumeFlipbookBakeSettings& settings)
{
    // colorKey 0 = 泡の白 → 1 = 深い青。水煙 (colorKey 0) も白くなる。
    settings.albedoRamp = EvenVolumeRamp({ 0.9f, 0.95f, 1.0f }, { 0.6f, 0.78f, 0.95f },
                                         { 0.32f, 0.56f, 0.86f }, { 0.2f, 0.42f, 0.78f });
    settings.liquid.threshold = 0.3f;
    settings.liquid.softness = 0.1f;
    settings.liquid.extinction = 20.0f;
    settings.liquid.specular = 1.4f;
    settings.liquid.gloss = 140.0f;
    settings.liquid.fresnelF0 = 0.02f;
    settings.raySteps = 192;
}

void ApplyBloodLook(VolumeFlipbookBakeSettings& settings)
{
    settings.albedoRamp = EvenVolumeRamp({ 0.1f, 0.005f, 0.005f }, { 0.22f, 0.01f, 0.012f },
                                         { 0.38f, 0.03f, 0.03f }, { 0.5f, 0.06f, 0.05f });
    settings.liquid.threshold = 0.35f;
    settings.liquid.softness = 0.06f;
    settings.liquid.extinction = 70.0f;
    settings.liquid.specular = 0.8f;
    settings.liquid.gloss = 80.0f;
    settings.liquid.fresnelF0 = 0.03f;
    settings.raySteps = 192;
}

std::function<bool(const VolumeSourceSettings&)> AlwaysLoopable()
{
    return [](const VolumeSourceSettings&) { return true; };
}

std::function<float(const VolumeSourceSettings&)> StartAfter(float lifetime)
{
    return [lifetime](const VolumeSourceSettings&) { return lifetime; };
}

std::deque<VolumeSourceDesc> MakeBuiltinSources()
{
    std::deque<VolumeSourceDesc> sources;
    sources.push_back({ "Puff", "単体の煙が昇りながら回って膨らむ (一発もの)", BuildPuff, {}, {}, {} });
    sources.push_back({ "RisingPlume", "下から煙が湧き続け、熱い芯が冷えて煙になる (ループ可)",
        [](const VolumeSourceSettings& settings, const VolumeSourceRange& range) {
            std::vector<VolumePuff> puffs;
            // WHY 周期を割り切れる値へ寄せるか: duration が周期のちょうど M 倍なら、
            //     puff j と j+M が同じ見た目で duration だけずれて生まれる。場が厳密に元へ戻る。
            AppendPeriodicVolumePuffs(settings, range, kPlumeSpawnPeriod, kPlumeLifetime,
                [seed = settings.seed](std::uint32_t variant, float birthTime) {
                    return MakePlumePuff(seed, variant, birthTime);
                },
                puffs);
            return puffs;
        },
        // 湧き始めの 1 本目から焼くと «細い柱» から始まってしまう。寿命ぶん先へ進めて定常状態を 0 コマ目にする。
        StartAfter(kPlumeLifetime), AlwaysLoopable(), {} });
    sources.push_back({ "Fireball", "中心から放射状に弾けて急冷する (一発もの)", BuildFireball, {}, {}, {} });
    sources.push_back({ "Torch", "松明の炎。細い舌が昇って煤になる (ループ可)",
        [](const VolumeSourceSettings& settings, const VolumeSourceRange& range) {
            std::vector<VolumePuff> puffs;
            AppendPeriodicVolumePuffs(settings, range, kTorchSpawnPeriod, kTorchLifetime,
                [seed = settings.seed](std::uint32_t variant, float birthTime) {
                    return MakeTorchPuff(seed, variant, birthTime);
                },
                puffs);
            return puffs;
        },
        StartAfter(kTorchLifetime), AlwaysLoopable(), ApplyTorchLook });
    sources.push_back({ "Fountain", "噴水。水柱が上がって崩れ落ちる (ループ可・液体)",
        [](const VolumeSourceSettings& settings, const VolumeSourceRange& range) {
            std::vector<VolumePuff> puffs;
            AppendPeriodicVolumePuffs(settings, range, kFountainSpawnPeriod, kFountainLifetime,
                [seed = settings.seed](std::uint32_t variant, float birthTime) {
                    return MakeFountainPuff(seed, variant, birthTime);
                },
                puffs);
            return puffs;
        },
        StartAfter(kFountainLifetime), AlwaysLoopable(), ApplyWaterLook });
    sources.push_back({ "WaterSplash", "水面に物が落ちた跳ね。王冠・水柱・水煙 (一発もの・液体)",
                        BuildWaterSplash, {}, {}, ApplyWaterLook });
    sources.push_back({ "BloodSpray", "斬撃の血しぶき。右上へ飛んで落ちる (一発もの・液体)",
                        BuildBloodSpray, {}, {}, ApplyBloodLook });
    sources.push_back({ "BloodBurst", "着弾の血しぶき。上半球へ散る (一発もの・液体)",
                        BuildBloodBurst, {}, {}, ApplyBloodLook });
    sources.push_back({ kVolumeEmitterSourceName, "パネルの Emitter 設定だけで puff の出方を決める汎用ソース",
        BuildEmitter,
        [](const VolumeSourceSettings& settings) {
            return settings.emitter.burst ? 0.0f : EmitterMaxLifetime(settings.emitter);
        },
        [](const VolumeSourceSettings& settings) { return !settings.emitter.burst; },
        {} });
    return sources;
}

std::deque<VolumeSourceDesc>& Registry()
{
    static std::deque<VolumeSourceDesc> registry = MakeBuiltinSources();
    return registry;
}

} // namespace

void RegisterVolumeSource(VolumeSourceDesc desc)
{
    if (desc.name.empty() || !desc.build) return;
    std::deque<VolumeSourceDesc>& registry = Registry();
    for (VolumeSourceDesc& existing : registry) {
        if (existing.name == desc.name) {
            existing = std::move(desc);
            return;
        }
    }
    registry.push_back(std::move(desc));
}

const VolumeSourceDesc* FindVolumeSource(std::string_view name)
{
    for (const VolumeSourceDesc& desc : Registry())
        if (desc.name == name) return &desc;
    return nullptr;
}

const std::deque<VolumeSourceDesc>& VolumeSources()
{
    return Registry();
}

float DefaultVolumeStartTime(const VolumeSourceSettings& settings)
{
    const VolumeSourceDesc* desc = FindVolumeSource(settings.preset);
    return desc != nullptr && desc->defaultStartTime ? (std::max)(desc->defaultStartTime(settings), 0.0f) : 0.0f;
}

float ResolveVolumeStartTime(const VolumeSourceSettings& settings)
{
    return settings.startTime >= 0.0f ? settings.startTime : DefaultVolumeStartTime(settings);
}

bool VolumeSourceCanLoop(const VolumeSourceSettings& settings)
{
    const VolumeSourceDesc* desc = FindVolumeSource(settings.preset);
    return desc != nullptr && desc->canLoop && desc->canLoop(settings);
}

bool VolumeSourceLoops(const VolumeSourceSettings& settings)
{
    return settings.loop && VolumeSourceCanLoop(settings);
}

std::vector<VolumePuff> BuildVolumePuffs(const VolumeSourceSettings& settings)
{
    const VolumeSourceDesc* desc = FindVolumeSource(settings.preset);
    if (desc == nullptr || !desc->build) return {};
    const VolumeSourceRange range{ ResolveVolumeStartTime(settings), BakeDuration(settings) };
    return desc->build(settings, range);
}

void ApplyVolumeSourceLook(VolumeFlipbookBakeSettings& settings)
{
    const VolumeFlipbookBakeSettings defaults;
    settings.lightColor = defaults.lightColor;
    settings.ambient = defaults.ambient;
    settings.extinction = defaults.extinction;
    settings.albedoRamp = defaults.albedoRamp;
    settings.emissionRamp = defaults.emissionRamp;
    settings.liquid = defaults.liquid;
    settings.anisotropy = defaults.anisotropy;
    settings.emissionIntensity = defaults.emissionIntensity;
    settings.exposure = defaults.exposure;
    settings.raySteps = defaults.raySteps;
    settings.shadowSteps = defaults.shadowSteps;
    const VolumeSourceDesc* desc = FindVolumeSource(settings.source.preset);
    if (desc != nullptr && desc->applyLook) desc->applyLook(settings);
}

float VolumeHash01(std::uint32_t seed, std::uint32_t index, std::uint32_t channel)
{
    const std::uint32_t h = core::PcgHash(seed * 0x9E3779B9u ^ index * 0x85EBCA6Bu ^ channel * 0xC2B2AE35u);
    return static_cast<float>(h) / 4294967296.0f;
}

float VolumeHashSigned(std::uint32_t seed, std::uint32_t index, std::uint32_t channel)
{
    return VolumeHash01(seed, index, channel) * 2.0f - 1.0f;
}

math::Vector3 VolumeRandomDirection(std::uint32_t seed, std::uint32_t index, std::uint32_t channel)
{
    const float y = VolumeHashSigned(seed, index, channel);
    const float phi = 2.0f * kPi * VolumeHash01(seed, index, channel + 1);
    const float ring = std::sqrt((std::max)(0.0f, 1.0f - y * y));
    return { ring * std::cos(phi), y, ring * std::sin(phi) };
}

math::Vector3 VolumeRandomInCone(const math::Vector3& axis, float halfAngleRadians,
                                 std::uint32_t seed, std::uint32_t index, std::uint32_t channel)
{
    const math::Vector3 a = axis.NormalizedOr(math::Vector3::UP);
    // cosθ を [cos(half), 1] で一様にすると、円錐の中で立体角が一様になる。
    const float cosHalf = std::cos(std::clamp(halfAngleRadians, 0.0f, kPi));
    const float cosTheta = 1.0f - VolumeHash01(seed, index, channel) * (1.0f - cosHalf);
    const float sinTheta = std::sqrt((std::max)(0.0f, 1.0f - cosTheta * cosTheta));
    const float phi = 2.0f * kPi * VolumeHash01(seed, index, channel + 1);
    const math::Vector3 helper = std::fabs(a.y) < 0.99f ? math::Vector3::UP : math::Vector3::RIGHT;
    const math::Vector3 tangent = math::Vector3::Cross(helper, a).NormalizedOr(math::Vector3::RIGHT);
    const math::Vector3 bitangent = math::Vector3::Cross(a, tangent);
    return tangent * (sinTheta * std::cos(phi)) + a * cosTheta + bitangent * (sinTheta * std::sin(phi));
}

void AppendPeriodicVolumePuffs(const VolumeSourceSettings& settings, const VolumeSourceRange& range,
                               float period, float maxLifetime,
                               const std::function<VolumePuff(std::uint32_t variant, float birthTime)>& make,
                               std::vector<VolumePuff>& out)
{
    float step = (std::max)(period, 1.0e-3f);
    int variants = 0;
    if (settings.loop) {
        variants = (std::max)(1, static_cast<int>(std::lround(range.duration / step)));
        step = range.duration / static_cast<float>(variants);
    }
    const int first = static_cast<int>(std::floor((range.start - maxLifetime) / step)) - 1;
    const int last = static_cast<int>(std::ceil((range.start + range.duration + settings.frameDt) / step)) + 1;
    for (int j = first; j <= last; ++j) {
        const int variant = variants > 0 ? ((j % variants) + variants) % variants : j;
        out.push_back(make(static_cast<std::uint32_t>(variant), static_cast<float>(j) * step));
    }
}

} // namespace fbzz::asset
