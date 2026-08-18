// FBZZ Engine
// Model.hpp | fbzz::asset
// FBX / OBJ などから得たメッシュ・マテリアル・アニメーション一式
// AssetManager が唯一の所有者。Scene 側のコンポーネントは Mesh* / Material* の非所有参照を使う。
// メッシュとマテリアルの対応は配列インデックスで揃える前提にする。
#pragma once
#include <memory>
#include <vector>
#include "AnimationClip.hpp"
#include "ModelNode.hpp"
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

    // DCC のノード階層。meshes をどう GameObject へ配るかはここだけが知っている。
    // 空の場合は「ノード情報なし」= 呼び出し側が meshes を平坦に扱ってよい (旧経路)。
    std::vector<ModelNode> nodes;
    int rootNodeIndex = -1;
    // nodes の TRS が既に頂点へ焼き込まれているか。true なら配置側は Transform へ
    // 代入してはならない (二重変換)。FzModelFormat.hpp の
    // FZMODEL_FLAG_NODE_TRANSFORMS_BAKED を参照。
    bool nodeTransformsBaked = true;

    // meshIndex を担当するノードを引く。見つからなければ -1。
    // WHY: Renderer から「自分はモデルのどの部位か」を逆引きしたい箇所があるため、
    //      線形探索を各所へ書かせず 1 箇所へ寄せる。ノード数は数十のオーダー。
    [[nodiscard]] int FindNodeForMesh(uint32_t meshIndex) const
    {
        for (size_t i = 0; i < nodes.size(); ++i)
            for (const uint32_t candidate : nodes[i].meshIndices)
                if (candidate == meshIndex) return static_cast<int>(i);
        return -1;
    }

    // skeleton->referencePose を載せた定数バッファ。AnimatorComponent を持たない
    // SkinnedMeshRenderer の描画で使う既定パレット。
    // WHY Model が持つ: リファレンスポーズはスケルトン固有でインスタンス非依存。
    //     同じモデルの全インスタンスで 1 本を共有できる。
    //     RenderSystem が初回描画時に遅延生成する。
    renderer::ResourceHandle<renderer::ConstantBufferTag> referencePoseCB;
};

} // namespace fbzz::asset
