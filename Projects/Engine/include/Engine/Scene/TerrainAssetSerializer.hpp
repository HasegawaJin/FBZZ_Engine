/// @file    TerrainAssetSerializer.hpp
/// @brief   TerrainComponent の重い編集データを .terrain として保存・復元する。
/// @author  Hasegawa Jin
/// @date    2026-06-01
#pragma once

#include <string>

namespace fbzz::scene {

struct TerrainComponent;

// TerrainAssetSerializer — Terrain の height/splat/layer データを外部アセット化する。
// WHY: Scene / Prefab に巨大な配列を直接持たせると差分確認と読み込みが重くなるため、
//      地形データだけを Assets/Terrain/*.terrain に分離して参照保存できるようにする。
class TerrainAssetSerializer {
public:
    // component の現在の地形編集データを path に保存する。失敗時は false。
    static bool Save(const TerrainComponent& component, const std::string& path);

    // path から component に地形編集データを読み込む。読み込み後は再構築 dirty を立てる。
    static bool Load(const std::string& path, TerrainComponent& component);
};

} // namespace fbzz::scene
