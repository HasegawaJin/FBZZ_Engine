/// @file    WeatherSystem.cpp
/// @brief   降雨量 → 濡れ量の時間積分と、雨エミッターの発生量制御
/// @author  Hasegawa Jin
/// @date    2026-08-25
#include <Engine/Scene/Systems/WeatherSystem.hpp>
#include <Engine/Core/Scheduler/SystemContext.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Components/ParticleEmitter.hpp>
#include <Engine/Scene/Components/WeatherComponent.hpp>
#include <Engine/Scene/Systems/ParticleSimulationSystem.hpp>
#include <algorithm>

namespace fbzz::scene {
namespace {

/// @brief 雨エミッターは Weather と同じ GameObject か、その子孫に置く規約。
/// @note 雨は «降る範囲» を持つのでプレイヤーへ追従させたく別階層へ吊りたくなるが、
///       親子関係なら Hierarchy を見ればどれが雨か分かる。
void DriveRainEmitters(GameObject& go, float emitRate, bool raining)
{
    if (auto* emitter = go.GetComponent<ParticleEmitter>()) {
        emitter->settings.emitRate = emitRate;
        /// @note 雨量 0 で «出続けているが 0 個» にすると、既に飛んでいる粒が消えるまで
        ///       シミュレーションが走り続ける。止めるなら発生も止める。
        emitter->settings.playing = raining;
    }
    const int childCount = go.GetChildCount();
    for (int i = 0; i < childCount; ++i)
        if (GameObject* child = go.GetChild(i))
            DriveRainEmitters(*child, emitRate, raining);
}

} // namespace

ComponentAccess WeatherSystem::GetAccess() const
{
    return ComponentAccess{}.Writes<WeatherComponent, ParticleEmitter>();
}

OrderingHints WeatherSystem::GetOrder() const
{
    /// @note 同じフレームのうちに雨量を効かせる。後に回すと発生量が常に 1 フレーム遅れ、
    ///       雨脚を script で急に上げ下げしたときに一拍置いてから変わる。
    return OrderingHints{}.Before<ParticleSimulationSystem>();
}

void WeatherSystem::Update(SystemContext& ctx)
{
    /// @note Pause 中 (playing かつ !simulating) は «時間を止めている» だけなので何も進めない。
    ///       編集中 (!playing) は時間が無いので、積分ではなく目標値へ即座に合わせる。
    const bool editing = !ctx.playing;
    if (!editing && !ctx.simulating) return;

    for (auto& go : ctx.scene.GameObjects()) {
        /// @note 親ごと無効化された天候は進めない (描画側の FindActiveWeather も同じ規則で拾わない)。
        if (!go.activeInHierarchy()) continue;
        auto* weather = go.GetComponent<WeatherComponent>();
        if (!weather || !weather->enabled) continue;

        const float rain = std::clamp(weather->rainIntensity, 0.0f, 1.0f);

        if (weather->autoWetness) {
            if (editing) {
                /// @note 編集中は dt が «再生していない時間» で、ランプに乗せると雨量を上げても
                ///       絵が変わらない。ここは到達点そのものを入れる (手で置いた wetness を
                ///       残したいなら autoWetness を切る)。
                weather->wetness = rain;
            } else {
                const bool  wetting  = rain > weather->wetness;
                const float duration = (std::max)(
                    wetting ? weather->wetDuration : weather->dryDuration, 0.001f);

                /// @note 到達点まで duration 秒かかる線形移動。指数緩和にすると「いつまでも乾ききらない」
                ///       尻尾が残り、雨上がりの絵が止まったように見える。
                const float step = ctx.dt / duration;
                weather->wetness = wetting
                    ? (std::min)(weather->wetness + step, rain)
                    : (std::max)(weather->wetness - step, rain);
            }
        }

        DriveRainEmitters(go, (std::max)(weather->rainEmitRate, 0.0f) * rain, rain > 0.0f);
    }
}

} // namespace fbzz::scene
