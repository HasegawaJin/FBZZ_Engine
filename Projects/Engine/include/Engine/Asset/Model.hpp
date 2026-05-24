// FBZZ Engine
// Model.hpp | fbzz::asset
// FBX / OBJ などから得たメッシュ・マテリアル・アニメーション一式
// AssetManager が共有所有し、Scene 側のコンポーネントは参照として使う。
// メッシュとマテリアルの対応は配列インデックスで揃える前提にする。
#pragma once
#include <memory>
#include <vector>
#include "AnimationClip.hpp"
#include "Skeleton.hpp"

namespace fbzz::renderer { struct Mesh; class Material; }

namespace fbzz::asset {

struct Model {
    std::vector<std::shared_ptr<renderer::Mesh>>     meshes;
    std::vector<std::shared_ptr<renderer::Material>> materials;  // meshes[i] に対応する material は materials[i]
    std::shared_ptr<Skeleton> skeleton;
    std::vector<AnimationClip> clips;
};

} // namespace fbzz::asset
