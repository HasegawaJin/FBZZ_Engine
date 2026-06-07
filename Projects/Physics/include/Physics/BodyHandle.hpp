// FBZZ Engine
// BodyHandle.hpp | fbzz::physics
// Physics World 内の pool 要素を世代付きで参照する軽量ハンドル
#pragma once
#include <cstdint>

namespace fbzz::physics
{
    struct BodyHandle
    {
        uint32_t slot = 0;
        uint32_t generation = 0;
        bool IsValid() const { return slot != 0; }
    };

    struct ColliderHandle
    {
        uint32_t slot = 0;
        uint32_t generation = 0;
        bool IsValid() const { return slot != 0; }
    };

    struct VolumeHandle
    {
        uint32_t slot = 0;
        uint32_t generation = 0;
        bool IsValid() const { return slot != 0; }
    };
} // namespace fbzz::physics
