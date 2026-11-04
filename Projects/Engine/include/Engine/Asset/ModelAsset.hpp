/// @file    ModelAsset.hpp
/// @brief   .fzasset バイナリのランタイム表現。
/// @author  Hasegawa Jin
/// @date    2026-06-18

/// @note メッシュ複数 + LOD + スケルトンを保持する。マテリアル・アニメは別アセット。
/// @note 旧 Model はメッシュ/マテリアル/アニメを一括保持していたが、
/// @note 各アセットを独立させることで再利用・差し替えを容易にする。
#pragma once
#include <Engine/Api.hpp>
#include <Engine/Asset/ModelNode.hpp>
#include <Engine/Asset/Skeleton.hpp>
#include <Engine/Renderer/Mesh.hpp>
#include <memory>
#include <string>
#include <vector>

namespace fbzz::asset {

struct SubmeshEntry {
    uint32_t                        materialSlotIndex = 0;
    std::string                     name;
    std::unique_ptr<renderer::Mesh> mesh;
};

struct LodLevel {
    float                    screenSizeThreshold = 0.0f; ///< @note 0.0 = 常時使用
    std::vector<SubmeshEntry> submeshes;
};

struct ModelAsset {
    ModelAsset() = default;
    /// @note submesh の GPU バッファを返す。理由と注意点は `Model::~Model` と同じ ─
    /// @note `renderer::Mesh` は ResourceManager を知らないので、唯一の所有者が返す。
    FBZZ_ENGINE_API ~ModelAsset();

    ModelAsset(const ModelAsset&)            = delete;
    ModelAsset& operator=(const ModelAsset&) = delete;
    ModelAsset(ModelAsset&&)                 = default;
    ModelAsset& operator=(ModelAsset&&)      = default;

    /// @note lods[0] が最高品質。常に 1 以上存在する。
    std::vector<LodLevel>          lods;
    /// @note マテリアルスロット名。Scene/Prefab で実際の .mat に束縛する。
    std::vector<std::string>       materialSlotNames;
    std::unique_ptr<Skeleton>      skeleton; ///< @note nullptr = 静的メッシュ

    /// @note DCC のノード階層。meshIndices は lods[0].submeshes の添字を指す。
    /// @note 空 = ノード情報なし (v3 以前のベイク)。この場合は「全 submesh を 1 GameObject」
    /// @note という従来の解釈へフォールバックする。
    std::vector<ModelNode>         nodes;
    int                            rootNodeIndex = -1;
    /// @note nodes の localTranslation/Rotation/Scale が既に頂点へ焼き込まれているか。
    /// @note true の場合、配置側は Transform へ代入してはならない (二重変換になる)。
    /// @note 詳細は FzModelFormat.hpp の FZMODEL_FLAG_NODE_TRANSFORMS_BAKED を参照。
    bool                           nodeTransformsBaked = true;

    bool IsSkinned()   const { return skeleton != nullptr; }
    uint32_t LodCount() const { return static_cast<uint32_t>(lods.size()); }

    /// @brief メッシュが常駐している最も高品質な LOD。
    /// @note 非同期経路は品質段より高品質な LOD のメッシュを持たない (submesh の枠と名前だけ残る)。
    /// @note LOD を選ぶ側は、希望の LOD が未常駐ならこれへ倒す。全部未常駐なら LodCount() を返す。
    /// @see Docs/design/asset-streaming.md «常駐予算とストリーミング»
    [[nodiscard]] uint32_t FirstResidentLod() const
    {
        for (uint32_t i = 0; i < LodCount(); ++i)
            for (const SubmeshEntry& submesh : lods[i].submeshes)
                if (submesh.mesh) return i;
        return LodCount();
    }
};

} /// @note namespace fbzz::asset
