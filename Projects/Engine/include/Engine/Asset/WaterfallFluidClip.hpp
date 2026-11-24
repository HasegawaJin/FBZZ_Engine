/// @file    WaterfallFluidClip.hpp
/// @brief   連結された滝のローカル液面をベイクし、フレーム単位でストリーミングする。
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

/// @note 位置はベイク立方体の [-1,1] に正規化する。重力加速度は立方体の単位毎秒毎秒、速度は立方体の単位毎秒。
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
    /// @note XZ の [-1,1] を grid × grid でサンプルした高さ。X の添字が最も速く変化する。
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

/// @return 成功時は空文字列。失敗やキャンセルでは既存ファイルを変更しない。
[[nodiscard]] FBZZ_ENGINE_API std::string BakeWaterfallFluidClip(const WaterfallFluidBakeInput& input,
    const std::string& path, const std::atomic<bool>& cancel, std::atomic<int>& progress);

/// @note インデックスと、チェックサムを検証した 1 フレームを読む。スタンプは配置とベイク設定を識別する。
[[nodiscard]] FBZZ_ENGINE_API bool LoadWaterfallFluidFrame(const std::string& path, std::uint64_t stamp,
    int requestedFrame, WaterfallFluidFrame& out);

} /// @note namespace fbzz::asset
