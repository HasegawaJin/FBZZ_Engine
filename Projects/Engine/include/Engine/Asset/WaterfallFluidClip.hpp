/// @file    WaterfallFluidClip.hpp
/// @brief   Local liquid surface baking and frame streaming for connected waterfalls.
/// @author  Hasegawa Jin
/// @date    2026-11-11
#pragma once
#include <Engine/Api.hpp>
#include <Math/Quaternion.hpp>
#include <Math/Vector3.hpp>
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace fbzz::asset {

struct WaterfallFluidObstacle {
    math::Vector3 center;
    math::Vector3 halfSize;
    math::Quaternion rotation;
    float sphereRadius = 0.0f;
};

/// @note Positions are normalized to the bake cube [-1,1]. Gravity and velocity are in cube units per second squared and per second.
struct WaterfallFluidBakeInput {
    math::Vector3 inlet;
    math::Vector3 inletVelocity;
    math::Vector3 inletAxis;
    float inletHalfWidth = 0.0f;
    float receiverHeight = 0.0f;
    float gravity = 9.81f;
    int grid = 40;
    int particlesPerSecond = 850;
    int frameCount = 48;
    int framesPerSecond = 24;
    std::uint64_t stamp = 0;
    /// @note grid × grid height samples across XZ from -1 to 1, X varies fastest.
    std::vector<float> ground;
    std::vector<WaterfallFluidObstacle> obstacles;
};

struct WaterfallFluidFrame {
    struct Vertex {
        math::Vector3 position;
        math::Vector3 normal;
        float coverage;
    };
    std::vector<Vertex> vertices;
    std::vector<std::uint16_t> indices;
    int frame = -1;
    int frameCount = 0;
    int framesPerSecond = 0;
};

/// @return Empty on success. Failure and cancellation leave an existing file intact.
[[nodiscard]] FBZZ_ENGINE_API std::string BakeWaterfallFluidClip(const WaterfallFluidBakeInput& input,
    const std::string& path, const std::atomic<bool>& cancel, std::atomic<int>& progress);

/// @note Reads the index and one checksummed frame. The stamp identifies the placement and bake settings.
[[nodiscard]] FBZZ_ENGINE_API bool LoadWaterfallFluidFrame(const std::string& path, std::uint64_t stamp,
    int requestedFrame, WaterfallFluidFrame& out);

} /// @note namespace fbzz::asset
