/// @file    ScriptTerrainProxy.hpp
/// @brief   Script から TerrainComponent を照会・更新するショートハンド。
/// @author  Hasegawa Jin
/// @date    2026-06-26
#pragma once

#include <Math/Vector3.hpp>
#include <string_view>

namespace fbzz::scene {

class Script;

struct ScriptTerrainProxy {
    Script* script = nullptr;

    float GetHeightLocal(float localX, float localZ) const;
    math::Vector3 GetNormalLocal(float localX, float localZ) const;
    float GetHeightWorld(const math::Vector3& worldPos) const;
    math::Vector3 GetNormalWorld(const math::Vector3& worldPos) const;

    bool SetHeightAtGrid(int x, int z, float worldHeight) const;
    bool PaintLayerAtGrid(int x, int z, int layer, float weight) const;
    bool SetLayerMaterial(int layer, std::string_view materialPath) const;
    void RequestRebuild() const;
};

} // namespace fbzz::scene
