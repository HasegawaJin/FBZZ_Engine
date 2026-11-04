/// @file    TerrainAssetSerializer.hpp
/// @brief   TerrainComponent の重い編集データを .terrain として保存・復元する。
/// @author  Hasegawa Jin
/// @date    2026-06-01
#pragma once
#include <Engine/Api.hpp>

#include <string>
#include <string_view>

namespace fbzz::scene {

struct TerrainComponent;

/// @brief Terrain の height/splat/layer データを外部アセット化する。
/// @note Scene/Prefab に巨大な配列を直接持たせると差分確認と読み込みが重くなるため、地形データだけを `Assets/Terrain/*.terrain` へ分離して参照保存する。
class TerrainAssetSerializer {
public:
    /// @note Same data contract for external assets and inline Scene/Prefab persistence.
    FBZZ_ENGINE_API static bool Serialize(const TerrainComponent& component, std::string& out);
    /// @return Invalid geometry leaves component unchanged.
    FBZZ_ENGINE_API static bool Deserialize(std::string_view text, TerrainComponent& component);
    /// @brief component の現在の地形編集データを path に保存する。
    /// @return 失敗時は false。
    FBZZ_ENGINE_API static bool Save(const TerrainComponent& component, const std::string& path);

    /// @brief path から component に地形編集データを読み込む。
    /// @note 読み込み後は再構築 dirty を立てる。
    FBZZ_ENGINE_API static bool Load(const std::string& path, TerrainComponent& component);
};

} /// @note namespace fbzz::scene
