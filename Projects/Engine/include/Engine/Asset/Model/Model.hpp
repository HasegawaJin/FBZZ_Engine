/// @file    Model.hpp
/// @brief   FBX / OBJ などから得たメッシュ・マテリアル・アニメーション一式。
/// @author  Hasegawa Jin
/// @date    2026-05-21

/// @note AssetManager が唯一の所有者。Scene 側のコンポーネントは Mesh* / Material* の非所有参照を使う。
/// @note メッシュとマテリアルの対応は配列インデックスで揃える前提にする。
#pragma once
#include <Engine/Api.hpp>
#include <memory>
#include <vector>
#include "Engine/Asset/Animation/Clips/AnimationClip.hpp"
#include "ModelNode.hpp"
#include "Engine/Asset/Animation/Rig/Skeleton.hpp"
#include <Graphics/Renderer/Geometry/Mesh.hpp>
#include <Graphics/Renderer/Materials/Material.hpp>
#include <Graphics/Renderer/Resources/ResourceHandle.hpp>

namespace fbzz::asset {

struct Model {
    Model() = default;
    /// @brief メッシュの GPU バッファを返す。
    /// @note `renderer::Mesh` は ResourceManager を知らないため唯一の所有者である Model が返す。
    /// @note これが無いとモデルを読み直すたび (ホットリロード / プロジェクト切り替え / UnloadAll)
    /// @note に頂点・インデックスバッファが GPU へ残り続けた。Model はアセットの静的キャッシュに
    /// @note 載っており ResourceManager より後に消えることがあるため、Active() が空なら
    /// @note 「返す相手が居ない」と判断し何もしない (プロセス終了時なので実害はない)。
    FBZZ_ENGINE_API ~Model();

    Model(const Model&)            = delete;
    Model& operator=(const Model&) = delete;
    Model(Model&&)                 = default;
    Model& operator=(Model&&)      = default;

    /// @note Model が meshes / materials の唯一の所有者。
    /// @note MeshRenderer / MaterialComponent は Mesh* / Material* (非所有) を保持する。
    std::vector<std::unique_ptr<renderer::Mesh>>     meshes; ///< モデルが所有する描画メッシュの配列。ノードが添字で参照する。
    std::vector<std::unique_ptr<renderer::Material>> materials; ///< meshes[i] に対応する material は materials[i]
    std::unique_ptr<Skeleton> skeleton; ///< スキニングに使う骨格の所有ポインター。骨格のないモデルでは null。
    std::vector<AnimationClip> clips; ///< このモデルへ読み込まれ、Animator が名前・番号で引くアニメーション列。

    /// @note DCC のノード階層。meshes をどう GameObject へ配るかはここだけが知っている。
    /// @note 空の場合は「ノード情報なし」= 呼び出し側が meshes を平坦に扱ってよい (旧経路)。
    std::vector<ModelNode> nodes; ///< 親子関係とローカル姿勢を保持するモデルノードの配列。
    int rootNodeIndex = -1; ///< nodes 内のルートノードの添字。-1 は階層なし。
    /// @note nodes の TRS が既に頂点へ焼き込まれているか。true なら配置側は Transform へ
    /// @note 代入してはならない (二重変換)。FzModelFormat.hpp の
    /// @note FZMODEL_FLAG_NODE_TRANSFORMS_BAKED を参照。
    bool nodeTransformsBaked = true; ///< ノード変換をメッシュ頂点へ焼き込み済みか。

    /// @note meshIndex を担当するノードを引く。見つからなければ -1。
    /// @note Renderer から「自分はモデルのどの部位か」を逆引きしたい箇所があるため、
    /// @note 線形探索を各所へ書かせず 1 箇所へ寄せる。ノード数は数十のオーダー。
    [[nodiscard]] int FindNodeForMesh(uint32_t meshIndex) const
    {
        for (size_t i = 0; i < nodes.size(); ++i)
            for (const uint32_t candidate : nodes[i].meshIndices)
                if (candidate == meshIndex) {
                    return static_cast<int>(i);
                }
        return -1;
    }

    /// @note skeleton->referencePose を載せた定数バッファ。AnimatorComponent を持たない
    /// @note SkinnedMeshRenderer の描画で使う既定パレット。
    /// @note リファレンスポーズはスケルトン固有でインスタンス非依存のため、Model が持ち
    /// @note 同じモデルの全インスタンスで 1 本を共有する。RenderSystem が初回描画時に遅延生成する。
    renderer::ResourceHandle<renderer::ConstantBufferTag> referencePoseCB; ///< Animator が未評価のときに使う基準姿勢の GPU 定数バッファ。
};

} // namespace fbzz::asset
