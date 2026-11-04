/// @file    SplineComponents.hpp
/// @brief   Catmull-Rom曲線と曲線追従のオーサリング／ランタイム状態。
/// @author  Hasegawa Jin
/// @date    2026-08-12
#pragma once

#include <Engine/Scene/EntityRef.hpp>
#include <Engine/Scene/Reflection.hpp>
#include <Engine/Scene/ControlPointAdapters.hpp>
#include <Math/Vector3.hpp>
#include <vector>

namespace fbzz::scene {

struct SplineComponent {
    bool enabled = true;
    std::vector<math::Vector3> points;
    ControlPointSourceMode pointSourceMode = ControlPointSourceMode::INLINE;
    ControlPointRuntimeMode pointRuntimeMode = ControlPointRuntimeMode::LIVE;
    std::vector<SplineControlPointBinding> pointBindings;
    std::vector<SplineControlPointValue> bakedPoints;
    std::uint64_t bakedInputSignature = 0;
    ControlPointSourceMode bakedSourceMode = ControlPointSourceMode::INLINE;
    std::uint32_t bakedAdapterVersion = 0;
    /// @brief Whether the retained point sequence closes its last segment back to the first.
    bool bakedClosed = false;
    bool bakeValid = false;
    bool closed = false;

    const char* GetTypeName() const { return "Spline"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        ReflectControlPointSourceMode(r, "pointSourceMode", pointSourceMode);
        ReflectControlPointRuntimeMode(r, "pointRuntimeMode", pointRuntimeMode);
        if (pointSourceMode == ControlPointSourceMode::INLINE)
            r.ListField("points", points);
        ReflectSplineControlPointBindings(r, pointBindings);
        ReflectSplineControlPointValues(r, "bakedPoints", bakedPoints);
        ReflectControlPointSignature(r, "bakedInputSignature", bakedInputSignature);
        ReflectControlPointSourceMode(r, "bakedSourceMode", bakedSourceMode);
        ReflectControlPointVersion(r, "bakedAdapterVersion", bakedAdapterVersion);
        r.Field("bakedClosed", bakedClosed);
        r.Field("bakeValid", bakeValid);
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

} /// @note namespace fbzz::scene
