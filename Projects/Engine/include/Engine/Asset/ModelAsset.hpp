// FBZZ Engine
// ModelAsset.hpp | fbzz::asset
// .model バイナリのランタイム表現
// メッシュ複数 + LOD + スケルトンを保持する。マテリアル・アニメは別アセット。
// WHY: Model(.asset)はメッシュ/マテリアル/アニメを一括保持していたが、
//      各アセットを独立させることで再利用・差し替えを容易にする。
#pragma once
#include <Engine/Asset/Skeleton.hpp>
#include <Engine/Renderer/Mesh.hpp>
#include <memory>
#include <string>
#include <vector>

namespace fbzz::asset {

struct SubmeshEntry {
    uint32_t                        materialSlotIndex = 0;
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

    bool IsSkinned()   const { return skeleton != nullptr; }
    uint32_t LodCount() const { return static_cast<uint32_t>(lods.size()); }

    // 後方互換: LOD0 の全サブメッシュを返す
    const std::vector<SubmeshEntry>& GetSubmeshes() const {
        static const std::vector<SubmeshEntry> empty;
        return lods.empty() ? empty : lods[0].submeshes;
    }
};

} // namespace fbzz::asset
