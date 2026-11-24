/// @file    RenderMode.hpp
/// @brief   Raster 方式と独立した描画モードの要求。
/// @author  Hasegawa Jin
/// @date    2026-09-30
#pragma once

#include <cstdint>

namespace fbzz::renderer {

enum class RenderMode : uint8_t {
    RASTER,
    HYBRID,
    PATH_TRACING,
};

enum class PathTracingProfile : uint8_t {
    REFERENCE,
    GAME,
};

enum class PrimaryVisibility : uint8_t {
    RASTER,
    CAMERA_RAY,
};

enum class RayExecution : uint8_t {
    NONE,
    INLINE_RAY_QUERY,
};

/// @note 要求は解決時に書き換えない。効果の要求は HYBRID のときだけ有効。
struct RenderModeRequest {
    RenderMode mode = RenderMode::RASTER;
    PathTracingProfile pathProfile = PathTracingProfile::REFERENCE;
    bool rayShadow = false;
    bool rayReflection = false;
    bool rayDiffuseGi = false;
};

}
