/// @file    Layer.hpp
/// @brief   GameObject レイヤーマスクと衝突行列。
/// @author  Hasegawa Jin
/// @date    2026-05-23
#pragma once

#include <cstdint>

namespace fbzz {

using LayerMask = uint32_t;

// Unity 風の 0-31 レイヤー。Physics 側では BroadPhase のフィルタに使う。
struct Layer {
    static constexpr int Default       = 0;
    static constexpr int TransparentFX = 1;
    static constexpr int IgnoreRaycast = 2;
    static constexpr int Water         = 4;
    static constexpr int UI            = 5;
    static constexpr int Static        = 9;

    static constexpr LayerMask Everything = ~0u;
    static constexpr LayerMask Nothing    = 0u;

    static LayerMask Mask(int layer) { return 1u << (layer & 31); }
    static bool Contains(LayerMask mask, int layer) { return (mask & Mask(layer)) != 0; }
};

// 衝突可否は対称行列として保持し、片側の変更で逆方向も同時に更新する。
struct LayerCollisionMatrix {
    bool data[32][32];

    LayerCollisionMatrix()
    {
        for (int i = 0; i < 32; ++i)
            for (int j = 0; j < 32; ++j)
                data[i][j] = true;
    }

    bool CanCollide(int a, int b) const { return data[a & 31][b & 31]; }
    void Set(int a, int b, bool value)
    {
        data[a & 31][b & 31] = value;
        data[b & 31][a & 31] = value;
    }
};

} // namespace fbzz
