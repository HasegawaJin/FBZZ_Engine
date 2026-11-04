/// @file    ScriptTerrainProxy.hpp
/// @brief   Script から TerrainComponent を照会・更新するショートハンド。
/// @author  Hasegawa Jin
/// @date    2026-06-26
#pragma once
#include <Engine/Api.hpp>

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
    FBZZ_ENGINE_API float GetHeightLocal(float localX, float localZ) const;
    FBZZ_ENGINE_API math::Vector3 GetNormalLocal(float localX, float localZ) const;
    /// @return worldXZ 鉛直線と地形三角形の最上交点Y [m]。欠損・穴・範囲外・無効姿勢では worldPos.y。
    FBZZ_ENGINE_API float GetHeightWorld(const math::Vector3& worldPos) const;
    /// @return worldXZ 鉛直線と地形三角形の最上面。欠損・穴・範囲外・無効姿勢では false、両 out 未変更。
    /// @note 非一様/負 scale と任意回転を含む world TRS を用い、初期化済み Physics collider を要求しない。
    FBZZ_ENGINE_API bool TryGetSurfaceWorld(float worldX, float worldZ, math::Vector3& position, math::Vector3& normal) const;
    /// @return worldPos を Terrain local XZ へ逆変換して標本化した単位ワールド法線。欠損・非有限入力・非可逆 scale は UP。
    /// @note 法線は world TRS の逆転置で変換し、負の scale と非一様 scale を含む。
    /// @see https://pbr-book.org/4ed/Geometry_and_Transformations/Applying_Transformations#Normals PBRT 3.10.3 Normals。
    FBZZ_ENGINE_API math::Vector3 GetNormalWorld(const math::Vector3& worldPos) const;

    /// @return Nearest floor below reference in world metres. False leaves outs unchanged.
    FBZZ_ENGINE_API bool TryGetFloorWorld(const math::Vector3& reference, float maxDistance, math::Vector3& position, math::Vector3& normal) const;
    /// @return Nearest ceiling above reference in world metres. False leaves outs unchanged.
    FBZZ_ENGINE_API bool TryGetCeilingWorld(const math::Vector3& reference, float maxDistance, math::Vector3& position, math::Vector3& normal) const;

    FBZZ_ENGINE_API bool SetHeightAtGrid(int x, int z, float worldHeight) const;
    /// @brief 頂点 (x, z) の layer の重みを weight [0, 1] に置く。
    FBZZ_ENGINE_API bool PaintLayerAtGrid(int x, int z, int layer, float weight) const;
    /// @brief 層の .mat を差し替える。layer == GetLayerCount() なら末尾に追加する。
    /// @return 範囲外・層数上限なら false。
    FBZZ_ENGINE_API bool SetLayerMaterial(int layer, std::string_view materialPath) const;
    [[nodiscard]] FBZZ_ENGINE_API int GetLayerCount() const;
    /// @return 頂点 (x, z) の layer の重み [0, 1]。範囲外や上位 4 層に無い層は 0。
    [[nodiscard]] FBZZ_ENGINE_API float GetLayerWeightAtGrid(int x, int z, int layer) const;
    /// @brief セル (cellX, cellZ) の穴を立てる / 消す。変化したときだけメッシュとコライダーの再構築を要求する。
    /// @return セルが範囲外なら false。
    FBZZ_ENGINE_API bool SetHoleAtGrid(int cellX, int cellZ, bool hole) const;
    FBZZ_ENGINE_API void RequestRebuild() const;
};

} /// @note namespace fbzz::scene
