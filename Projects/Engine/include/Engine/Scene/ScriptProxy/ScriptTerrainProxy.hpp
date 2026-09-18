/// @file    ScriptTerrainProxy.hpp
/// @brief   Script から TerrainComponent を照会・更新するショートハンド。
/// @author  Hasegawa Jin
/// @date    2026-06-26
#pragma once

#include <Math/Vector3.hpp>
#include <string_view>

namespace fbzz::scene {

class Script;

/// @brief 自分の GameObject の TerrainComponent への窓口。
/// @note Terrain が無いときは何もせず、照会は既定値 (高さ 0 / 法線 UP / 重み 0) を返す。
/// @see Docs/design/terrain-layers.md
struct ScriptTerrainProxy {
    Script* script = nullptr;

    /// @return ローカル XZ [m] の地形高さ (ローカル Y [m])。
    float GetHeightLocal(float localX, float localZ) const;
    math::Vector3 GetNormalLocal(float localX, float localZ) const;
    /// @return ワールド XZ 直下の地形高さ (ワールド Y [m])。Terrain が無ければ worldPos.y。
    float GetHeightWorld(const math::Vector3& worldPos) const;
    math::Vector3 GetNormalWorld(const math::Vector3& worldPos) const;

    bool SetHeightAtGrid(int x, int z, float worldHeight) const;
    /// @brief 頂点 (x, z) の layer の重みを weight [0, 1] に置く。
    bool PaintLayerAtGrid(int x, int z, int layer, float weight) const;
    /// @brief 層の .mat を差し替える。layer == GetLayerCount() なら末尾に追加する。
    /// @return 範囲外・層数上限なら false。
    bool SetLayerMaterial(int layer, std::string_view materialPath) const;
    [[nodiscard]] int GetLayerCount() const;
    /// @return 頂点 (x, z) の layer の重み [0, 1]。範囲外や上位 4 層に無い層は 0。
    [[nodiscard]] float GetLayerWeightAtGrid(int x, int z, int layer) const;
    /// @brief セル (cellX, cellZ) の穴を立てる / 消す。変化したときだけメッシュとコライダーの再構築を要求する。
    /// @return セルが範囲外なら false。
    bool SetHoleAtGrid(int cellX, int cellZ, bool hole) const;
    void RequestRebuild() const;
};

} // namespace fbzz::scene
