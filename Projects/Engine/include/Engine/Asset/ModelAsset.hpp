/// @file    ModelAsset.hpp
/// @brief   .fzasset バイナリのランタイム表現。
/// @author  Hasegawa Jin
/// @date    2026-06-18
///
/// メッシュ複数 + LOD + スケルトンを保持する。マテリアル・アニメは別アセット。
/// WHY: 旧 Model はメッシュ/マテリアル/アニメを一括保持していたが、
/// 各アセットを独立させることで再利用・差し替えを容易にする。
#pragma once
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
    float                    screenSizeThreshold = 0.0f; // 0.0 = 常時使用
    std::vector<SubmeshEntry> submeshes;
};

struct ModelAsset {
    // lods[0] が最高品質。常に 1 以上存在する。
    std::vector<LodLevel>          lods;
    // マテリアルスロット名。Scene/Prefab で実際の .mat に束縛する。
    std::vector<std::string>       materialSlotNames;
    std::unique_ptr<Skeleton>      skeleton; // nullptr = 静的メッシュ

    // DCC のノード階層。meshIndices は lods[0].submeshes の添字を指す。
    // 空 = ノード情報なし (v3 以前のベイク)。この場合は「全 submesh を 1 GameObject」
    // という従来の解釈へフォールバックする。
    std::vector<ModelNode>         nodes;
    int                            rootNodeIndex = -1;
    // nodes の localTranslation/Rotation/Scale が既に頂点へ焼き込まれているか。
    // true の場合、配置側は Transform へ代入してはならない (二重変換になる)。
    // 詳細は FzModelFormat.hpp の FZMODEL_FLAG_NODE_TRANSFORMS_BAKED を参照。
    bool                           nodeTransformsBaked = true;

    bool IsSkinned()   const { return skeleton != nullptr; }
    uint32_t LodCount() const { return static_cast<uint32_t>(lods.size()); }

};

} // namespace fbzz::asset
