// FBZZ Engine
// SkinnedMeshRenderer.hpp | fbzz::scene
// Component that draws a skinned model and owns runtime bone entity links.
#pragma once

#include <Engine/Asset/Model.hpp>
#include <Engine/Scene/Entity.hpp>
#include <Engine/Scene/Script.hpp>
#include <string>
#include <unordered_map>
#include <vector>

namespace fbzz::scene {

struct SkinnedMeshRenderer {
    bool enabled = true;
    // LODSystem 専用のランタイム可視性。Scene には保存しない。
    bool lodVisible = true;
    // シャドウマップへ影を落とすか (MeshRenderer::castShadows と同じ意味)。
    bool castShadows = true;
    std::string modelPath;

    // WHY: 以前は meshIndex で「この GO が担当する submesh」を指していたが、
    //      submesh ごとに子 GameObject を生やす構成になり階層が読みにくく、
    //      Inspector の Mesh Index も -1 (全 submesh) を設定できない壊れた UI だった。
    //      現在は 1 GameObject = モデル全体を描画し、submesh ごとのマテリアルは
    //      MaterialComponent のスロット配列 (submesh i → SlotAt(i)) で表現する。
    asset::Model* model = nullptr;

    // WHY: Transform is the common base for scene editing, animation and rigging.
    // SkeletonNode remains the asset-side lookup table; these EntityID values are
    // rebuilt at load/runtime and point at the visible Bone GameObjects.
    EntityID skeletonRootEntity = EntityID::INVALID;
    std::vector<EntityID> nodeEntities;
    // Morph は共有 Mesh を変更せず、この Renderer インスタンス専用の頂点バッファへ反映する。
    std::unordered_map<std::string, float> morphWeights;
    std::unordered_map<std::string, float> appliedMorphWeights;
    std::vector<renderer::ResourceHandle<renderer::BufferTag>> morphVertexBuffers;

    // ── コンピュートスキニングの出力 ──────────────────────────────────────────
    // submesh ごとの「変形済み頂点」。レイアウトは静的メッシュの renderer::Vertex と同一で、
    // シャドウや GBuffer からは普通の静的メッシュとして読める。
    // WHY: スキニングを VS で行うと、同じキャラをシャドウと画面へ描くたびに同じ変形を
    //      計算し直すことになる。1 回だけ計算して共有するための置き場。
    std::vector<renderer::ResourceHandle<renderer::BufferTag>> skinnedVertexBuffers;
    // skinnedVertexBuffers を確保したときのモデル。model が差し替わったら作り直す。
    // WHY: バッファは submesh の頂点数に合わせて確保する。モデルを変えたのに古いバッファを
    //      使い回すと、頂点数が足りずに描画が途中で切れる / 範囲外へ書き込む。
    const asset::Model* skinnedBufferModel = nullptr;
    // このフレームで GPU スキニングが成立したか。false のときは従来の VS スキニング経路を使う
    // (Animator 未評価・モーフ有効・バックエンド未対応などで成立しないことがある)。
    bool gpuSkinnedThisFrame = false;

    // GPU スキニング済みなら変形済み頂点バッファを、そうでなければ無効ハンドルを返す。
    // NOTE: ResolveVertexBuffer と混同しないこと。あちらは「VS でスキニングする入力」を返す。
    //       こちらを VS スキニング用シェーダーへ渡すと二重に変形がかかって崩れる。
    renderer::ResourceHandle<renderer::BufferTag> ResolveSkinnedVertexBuffer(size_t index) const
    {
        if (!gpuSkinnedThisFrame || index >= skinnedVertexBuffers.size()) return {};
        return skinnedVertexBuffers[index];
    }

    renderer::ResourceHandle<renderer::BufferTag> ResolveVertexBuffer(
        size_t index,
        renderer::ResourceHandle<renderer::BufferTag> fallback) const
    {
        return index < morphVertexBuffers.size() && morphVertexBuffers[index].IsValid()
            ? morphVertexBuffers[index] : fallback;
    }

    const char* GetTypeName() const { return "Skinned Mesh Renderer"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled",     enabled);
        r.Field("castShadows", castShadows);
        r.Field("modelPath",   modelPath);
    }
};

} // namespace fbzz::scene
