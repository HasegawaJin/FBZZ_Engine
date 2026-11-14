/// @file    WaterfallComponent.hpp
/// @brief   Persistent waterfall connections and their disposable render parts.
/// @author  Hasegawa Jin
/// @date    2026-11-11
#pragma once
#include <Engine/Asset/WaterfallFluidClip.hpp>
#include <Engine/Scene/Components/TransientGpuState.hpp>
#include <Engine/Scene/EntityRef.hpp>
#include <Engine/Scene/Reflection.hpp>
#include <Engine/Scene/Waterfall.hpp>
#include <Math/Vector4.hpp>
#include <array>
#include <future>
#include <string>

namespace fbzz::scene {
struct WaterfallRuntime {
    std::array<EntityID, 6> parts{};
    WaterfallGeometry geometry;
    math::Vector3 midFall;
    std::uint64_t terrainRevision = 0;
    math::Vector4 groundProbeBounds{};
    bool hasGroundProbes = false;
    bool groundProbesFinite = true;
    float spray = -1.0f;
    float mist = -1.0f;
    bool prepared = false;
    bool valid = false;
    std::string status;
    std::string fluidStatus;
    std::string playingFluidClip;
    std::string resolvedFluidPath;
    std::uint64_t fluidStamp = 0;
    std::uint64_t fluidInputRevision = 0;
    std::uint64_t fluidAssetRevision = 0;
    float fluidTime = 0.0f;
    int fluidFrame = -1;
    int fluidFrameCount = 0;
    int fluidFps = 24;
    bool fluidLoadFailed = false;
    bool fluidViewDemand = false;
    std::future<asset::WaterfallFluidFrame> fluidFrameJob;
};

struct WaterfallComponent : TransientGpuState<WaterfallRuntime> {
    bool enabled = true;
    EntityRef upstream;
    EntityRef downstream;
    bool upstreamAtStart = false;
    bool downstreamAtStart = true;
    bool rockAttached = false;
    float breakupAmount = 1.0f;
    float sprayAmount = 1.0f;
    float mistAmount = 1.0f;
    std::string fluidClip;
    float fluidSize = 8.0f;
    int fluidGrid = 40;
    int fluidEmission = 850;
    int fluidFrames = 48;
    std::vector<EntityRef> fluidRocks;

    const char* GetTypeName() const { return "Waterfall"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        r.RefField("upstream", upstream, "WaterComponent");
        r.Field("upstreamAtStart", upstreamAtStart);
        r.RefField("downstream", downstream, "WaterComponent");
        r.Tooltip("For a lake or rectangular pool, place the Waterfall object at the intended impact point.");
        r.Field("downstreamAtStart", downstreamAtStart);
        r.Field("rockAttached", rockAttached);
        r.FloatRange("breakupAmount", breakupAmount, 0.0f, 2.0f);
        r.FloatRange("sprayAmount", sprayAmount, 0.0f, 2.0f);
        r.FloatRange("mistAmount", mistAmount, 0.0f, 2.0f);
        r.FileField("fluidClip", fluidClip, ".wfall", "Generated local liquid surface clip.");
        r.Tooltip("Width of the simulated impact region in meters.");
        r.FloatRange("fluidSize", fluidSize, 1.0f, 48.0f);
        r.IntRange("fluidGrid", fluidGrid, 24, 64);
        r.Tooltip("Liquid particles emitted per second across the lower curtain.");
        r.IntRange("fluidEmission", fluidEmission, 100, 3000);
        r.Tooltip("Number of frames in the 24 FPS surface clip.");
        r.IntRange("fluidFrames", fluidFrames, 8, 120);
        r.RefListField("fluidRocks", fluidRocks, "");
        r.Readonly("status", status);
        r.Readonly("fluidStatus", fluidStatus);
    }
};

struct WaterfallPart {
    std::size_t index = 0;
    std::uint32_t farIndexStart = 0;
    std::uint32_t pendingFarIndexStart = 0;
};
} /// @note namespace fbzz::scene
