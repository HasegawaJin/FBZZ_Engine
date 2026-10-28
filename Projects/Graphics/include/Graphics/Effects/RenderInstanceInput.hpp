/// @file    RenderInstanceInput.hpp
/// @brief   Immutable placement data and resolved mesh/LOD parts shared by trees and details.
/// @author  Hasegawa Jin
/// @date    2026-10-10
#pragma once
#include <Math/Vector3.hpp>
#include <Math/Matrix4.hpp>
#include <cstdint>
#include <memory>
#include <vector>

namespace fbzz::renderer {
struct InstancePlacement {
    uint32_t id = 0;
    math::Vector3 position{};
    math::Vector3 eulerRadians{};
    math::Vector3 scale{1,1,1};
    bool active = true;
};
/// @note Parent revision and dirty slots allow a GPU cache to apply edits without copying unchanged placements.
struct InstanceSetData {
    uint64_t revision = 1, parentRevision = 0;
    std::vector<InstancePlacement> placements;
    std::vector<uint32_t> dirtySlots;
};
} /// @note namespace fbzz::renderer
