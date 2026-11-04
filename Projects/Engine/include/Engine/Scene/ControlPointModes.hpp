/// @file    ControlPointModes.hpp
/// @brief   ControlPoint authoring source and runtime policies shared by domain adapters.
/// @author  Hasegawa Jin
/// @date    2026-10-08
#pragma once
#include <Engine/Api.hpp>

#include <cstdint>

namespace fbzz::scene {

struct IReflector;

/// @brief Position source used by ControlPoint-backed components.
enum class ControlPointSourceMode : std::uint8_t {
    INLINE = 0,
    POINT_OBJECTS = 1,
};

/// @brief Runtime policy for following live targets or retaining a numeric bake.
enum class ControlPointRuntimeMode : std::uint8_t {
    LIVE = 0,
    BAKED_STATIC = 1,
};

/// @brief Context that selects authoring validation or previously validated runtime data.
enum class ControlPointReadContext : std::uint8_t {
    AUTHORING,
    RUNTIME,
};

/// @brief Reflect a source mode while preserving unsupported serialized values.
FBZZ_ENGINE_API void ReflectControlPointSourceMode(IReflector& reflector, const char* fieldName,
    ControlPointSourceMode& mode);

/// @brief Reflect a runtime mode while preserving unsupported serialized values.
FBZZ_ENGINE_API void ReflectControlPointRuntimeMode(IReflector& reflector, const char* fieldName,
    ControlPointRuntimeMode& mode);

/// @brief Reflect a 64-bit canonical input signature through its supported string codec.
FBZZ_ENGINE_API void ReflectControlPointSignature(IReflector& reflector, const char* fieldName,
    std::uint64_t& signature);

/// @brief Reflect a nonnegative adapter version through its supported integer codec.
FBZZ_ENGINE_API void ReflectControlPointVersion(IReflector& reflector, const char* fieldName,
    std::uint32_t& version);

} /// @note namespace fbzz::scene
