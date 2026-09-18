/// @file    LightFlickerSystem.cpp
/// @brief   明滅の波形生成と intensity への適用・復元
/// @author  Hasegawa Jin
/// @date    2026-09-12
#include <Engine/Scene/Systems/LightFlickerSystem.hpp>

#include <Engine/Core/Logger.hpp>
#include <Engine/Core/Scheduler/SystemContext.hpp>
#include <Engine/Scene/Components/LightComponent.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Systems/VFXSystem.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace fbzz::scene {
namespace {

constexpr float kTwoPi = 6.28318530717958647692f;

/// 整数ハッシュ (lowbias32)。同じ入力からは常に同じ値しか出ない —— «同じ seed なら
/// 同じ結果» を成立させるのに、状態を持つ乱数生成器を使ってはいけない。
/// フレームを飛ばしても巻き戻しても、時刻と seed だけで揺れが決まる。
[[nodiscard]] std::uint32_t HashU32(std::uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

[[nodiscard]] float Hash01(std::uint32_t x)
{
    return static_cast<float>(HashU32(x)) * (1.0f / 4294967296.0f);
}

/// @brief 整数格子の乱数を smoothstep で繋いだ value noise [0, 1]。
/// @note 白色雑音は毎フレーム独立の乱数でフレームレートが上がるほど速く震え、60fps と
///       144fps で別物の絵になる。時刻を格子で区切れば、揺れの速さは flickerFrequency だけが決める。
[[nodiscard]] float ValueNoise01(float t, std::uint32_t seed)
{
    const float floored = std::floor(t);
    const auto  cell    = static_cast<std::uint32_t>(static_cast<std::int32_t>(floored));
    const float frac    = t - floored;
    const std::uint32_t salt = HashU32(seed ^ 0x9e3779b9u);
    const float a = Hash01(cell ^ salt);
    const float b = Hash01((cell + 1u) ^ salt);
    const float w = frac * frac * (3.0f - 2.0f * frac);
    return a + (b - a) * w;
}

/// 波形 [0, 1] 目安。Curve モードだけはカーブの値そのままなので 1 を超えうる。
[[nodiscard]] float FlickerWave(const LightComponent& light)
{
    const float cycles = light.flickerTime * (std::max)(light.flickerFrequency, 0.0f)
                       + light.flickerPhase;

    if (light.flickerMode == LightComponent::FlickerMode::Noise)
        return ValueNoise01(cycles, light.flickerSeed);

    float wave = 1.0f;
    if (light.flickerMode == LightComponent::FlickerMode::Sine) {
        wave = 0.5f + 0.5f * std::sin(cycles * kTwoPi);
    } else if (light.flickerMode == LightComponent::FlickerMode::Curve) {
        wave = light.flickerCurve.Evaluate(cycles - std::floor(cycles));
    }

    const float noiseAmount = std::clamp(light.flickerNoise, 0.0f, 1.0f);
    if (noiseAmount <= 0.0f) return wave;
    const float noise = ValueNoise01(cycles, light.flickerSeed);
    return wave + (noise - wave) * noiseAmount;
}

/// @brief Play セッション中か。Pause 中も true で、編集中だけ false になる。
/// @note SetPlaying() を呼ばない Standalone のテンプレート/プレビューでは playing が false の
///       まま simulating だけ true になる。ScriptSystem::InPlayMode と同じ判定を使う。
[[nodiscard]] bool InPlayMode(const SystemContext& ctx)
{
    return ctx.simulating || ctx.playing;
}

[[nodiscard]] bool FlickerActive(const LightComponent& light)
{
    return light.enabled
        && light.flickerMode != LightComponent::FlickerMode::Off
        && light.flickerAmplitude > 0.0f;
}

/// 捕獲した値へ戻す。捕獲していなければ何もしない。
void Restore(LightComponent& light)
{
    if (!light.flickerCaptured) return;
    light.intensity         = light.flickerBaseIntensity;
    light.flickerCaptured   = false;
    light.flickerTime       = 0.0f;
}

} // namespace

ComponentAccess LightFlickerSystem::GetAccess() const
{
    return ComponentAccess{}.Writes<LightComponent>();
}

OrderingHints LightFlickerSystem::GetOrder() const
{
    /// @note VFXLightEnvelope も intensity を捕まえて掛け直す。先に VFX を通しておかないと、
    ///       捕獲の順番がフレームごとに入れ替わりうる (同じ光源に両方付けるのは非推奨)。
    return OrderingHints{}.After<VFXSystem>();
}

void LightFlickerSystem::Update(SystemContext& ctx)
{
    const float dt      = (std::max)(ctx.dt, 0.0f);
    const bool  playing = InPlayMode(ctx);

    for (const EntityID id : ctx.scene.GetEntities<LightComponent>()) {
        LightComponent* lightPtr = ctx.scene.GetComponent<LightComponent>(id);
        if (lightPtr == nullptr) continue;
        LightComponent& light = *lightPtr;

        /// @note 編集中は書かない。intensity はシーンへ保存されるフィールドなので、
        ///       揺れている途中の値が «オーサリング値» として焼き付く。
        if (!playing || !FlickerActive(light)) {
            Restore(light);
            continue;
        }

        if (!light.flickerCaptured) {
            /// @note VFXLightEnvelope も «捕まえて掛け直す» ので、同じ光源に両方付けると互いの出力を
            ///       基準として掴み合い、繰り返すたびに暗くなる。絵だけ見て原因に辿り着けない。
            if (ctx.scene.GetComponent<VFXLightEnvelope>(id) != nullptr) {
                const GameObject* owner = ctx.scene.GetGameObject(id);
                FBZZ_LOG_WARN("LightFlicker: '%s' は VFXLightEnvelope と同居しています。"
                              "どちらも intensity を掛け直すため、繰り返すたびに暗くなります",
                              owner != nullptr ? owner->name.c_str() : "(unnamed)");
            }
            light.flickerBaseIntensity = light.intensity;
            light.flickerCaptured      = true;
            light.flickerTime          = 0.0f;
        }
        /// @note Pause 中 (playing && !simulating) は時刻を進めず、今の明るさを保つ。
        if (ctx.simulating) light.flickerTime += dt;

        const float amplitude = std::clamp(light.flickerAmplitude, 0.0f, 1.0f);
        const float scale     = (std::max)(1.0f - amplitude * (1.0f - FlickerWave(light)), 0.0f);
        light.intensity = light.flickerBaseIntensity * scale;
    }
}

} // namespace fbzz::scene
