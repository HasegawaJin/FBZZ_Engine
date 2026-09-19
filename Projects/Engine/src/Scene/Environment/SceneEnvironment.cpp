/// @file    SceneEnvironment.cpp
/// @brief   環境流の直列化と、1 本ぶんの要約への解決。
/// @author  Hasegawa Jin
/// @date    2026-09-16
#include <Engine/Scene/Environment/SceneEnvironment.hpp>

#include <Engine/Scene/Fields/FlowFieldEval.hpp>
#include <Engine/Scene/Fields/FlowFieldFrame.hpp>
#include <algorithm>

namespace fbzz::scene {

void SceneEnvironment::Reflect(IReflector& r)
{
    r.Field("windEnabled", enabled);
    r.Field("windDirection", direction);
    r.Field("windSpeed", speed);
    r.Field("windTurbulence", turbulence);
    r.Field("windPulseFrequency", pulseFrequency);
}

AmbientWind ResolveAmbientWind(const SceneEnvironment& environment)
{
    AmbientWind wind;
    wind.active = environment.enabled;
    if (!wind.active) return wind;

    const float length = environment.direction.Length();
    /// @note @note 長さ 0 の指定は «向きが無い» ではなく «書き忘れ»。読む側 (雲・水面) は
    ///       正規化済みの向きを前提にしているので、ここで既知の値へ倒しきる。
    wind.direction = length > 1.0e-4f ? environment.direction * (1.0f / length)
                                      : math::Vector3{ 0.0f, 1.0f, 0.0f };
    wind.speed          = environment.speed;
    wind.turbulence     = (std::max)(environment.turbulence, 0.0f);
    wind.pulseFrequency = environment.pulseFrequency;
    return wind;
}

void AppendEnvironmentFlow(const AmbientWind& wind, std::vector<ActiveFlowField>& out)
{
    if (!wind.active) return;

    ActiveFlowField uniform{};
    uniform.position       = math::Vector3::ZERO;
    /// @note 減衰なしでシーン全体へ
    uniform.radius         = 0.0f;
    uniform.direction      = wind.direction;
    uniform.strength       = wind.speed;
    uniform.type           = FlowFieldType::Uniform;
    uniform.falloffPower   = 1.0f;
    uniform.noiseFrequency = 0.5f;
    uniform.noiseSpeed     = wind.pulseFrequency;
    uniform.channels       = 0xFFFFFFFFu;
    out.push_back(uniform);

    if (wind.turbulence <= 0.0f) return;
    ActiveFlowField curl = uniform;
    curl.type     = FlowFieldType::Curl;
    curl.strength = wind.turbulence;
    out.push_back(curl);
}

} // namespace fbzz::scene
