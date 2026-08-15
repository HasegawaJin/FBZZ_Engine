// FBZZ Engine
// Model.hpp | fbzz::asset
// FBX / OBJ などから得たメッシュ・マテリアル・アニメーション一式
// AssetManager が唯一の所有者。Scene 側のコンポーネントは Mesh* / Material* の非所有参照を使う。
// メッシュとマテリアルの対応は配列インデックスで揃える前提にする。
#pragma once
#include <memory>
#include <vector>
#include "AnimationClip.hpp"
#include "Skeleton.hpp"
#include <Engine/Renderer/Mesh.hpp>
#include <Engine/Renderer/Material.hpp>
#include <Engine/Renderer/ResourceHandle.hpp>

namespace fbzz::asset {

struct Model {
    // WHY: Model が meshes / materials の唯一の所有者。
    //      MeshRenderer / MaterialComponent は Mesh* / Material* (非所有) を保持する。
    std::vector<std::unique_ptr<renderer::Mesh>>     meshes;
    std::vector<std::unique_ptr<renderer::Material>> materials;  // meshes[i] に対応する material は materials[i]
    std::unique_ptr<Skeleton> skeleton;
    std::vector<AnimationClip> clips;

    // skeleton->referencePose を載せた定数バッファ。AnimatorComponent を持たない
    // SkinnedMeshRenderer の描画で使う既定パレット。
    // WHY Model が持つ: リファレンスポーズはスケルトン固有でインスタンス非依存。
    //     同じモデルの全インスタンスで 1 本を共有できる。
    //     RenderSystem が初回描画時に遅延生成する。
    renderer::ResourceHandle<renderer::ConstantBufferTag> referencePoseCB;
};

} // namespace fbzz::asset
