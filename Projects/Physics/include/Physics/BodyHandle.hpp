/// @file    BodyHandle.hpp
/// @brief   Physics World 内の pool 要素を世代付きで参照する軽量ハンドル。
/// @author  Hasegawa Jin
/// @date    2026-06-08
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
