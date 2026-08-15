// FBZZ Engine
// SplineComponents.hpp | fbzz::scene
// Catmull-Rom曲線と曲線追従のオーサリング／ランタイム状態
#pragma once

#include <Engine/Scene/EntityRef.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/Vector3.hpp>
#include <vector>

namespace fbzz::scene {

struct SplineComponent {
    bool enabled = true;
    std::vector<math::Vector3> points;
    bool closed = false;

    const char* GetTypeName() const { return "Spline"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        r.ListField("points", points);
        r.Field("closed", closed);
    }
};

enum class SplineWrapMode : int { Once = 0, Loop = 1, PingPong = 2 };

struct SplineFollowerComponent {
    bool enabled = true;
    EntityRef spline;
    float normalizedPosition = 0.0f;
    float speed = 1.0f;
    SplineWrapMode wrapMode = SplineWrapMode::Loop;
    bool playOnAwake = true;
    bool orientToPath = true;
    bool playing = true;
    bool reverse = false;

    const char* GetTypeName() const { return "Spline Follower"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        r.Field("spline", spline);
        r.FloatRange("normalizedPosition", normalizedPosition, 0.0f, 1.0f);
        r.Field("speed", speed);
        int value = static_cast<int>(wrapMode);
        static constexpr const char* MODES[] = { "Once", "Loop", "Ping Pong" };
        r.Enum("wrapMode", value, MODES);
        wrapMode = static_cast<SplineWrapMode>(value < 0 || value > 2 ? 0 : value);
        r.Field("playOnAwake", playOnAwake);
        r.Field("orientToPath", orientToPath);
        r.Field("playing", playing);
    }
};

} // namespace fbzz::scene
