// FBZZ Engine
// Layer.hpp | fbzz
// GameObject layer masks and collision matrix
#pragma once

#include <cstdint>

namespace fbzz {

using LayerMask = uint32_t;

struct Layer {
    static constexpr int Default       = 0;
    static constexpr int TransparentFX = 1;
    static constexpr int IgnoreRaycast = 2;
    static constexpr int Water         = 4;
    static constexpr int UI            = 5;

    static constexpr LayerMask Everything = ~0u;
    static constexpr LayerMask Nothing    = 0u;

    static LayerMask Mask(int layer) { return 1u << (layer & 31); }
    static bool Contains(LayerMask mask, int layer) { return (mask & Mask(layer)) != 0; }
};

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
