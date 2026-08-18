// FBZZ Engine
// SkinnedMeshRenderer.hpp | fbzz::scene
// Component that draws a skinned model and owns runtime bone entity links.
#pragma once

#include <Engine/Asset/Model.hpp>
#include <Engine/Scene/Entity.hpp>
#include <Engine/Scene/Script.hpp>
#include <cstdint>
#include <limits>
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

    asset::Model* model = nullptr;

    // この Renderer が描く submesh の添字列 (model->meshes の添字)。
    // ローカルスロット i (= MaterialComponent のスロット i) が submeshIndices[i] を描く。
    //
    // 空 = モデル全体を描く。単一 GameObject へモデルをまるごと載せる従来の構成と、
    // Unity のようにノードごとへ分けた構成の両方を、呼び出し側の分岐なしで扱うため。
    //
    // WHY 単一の meshIndex ではなく配列か:
    //   Assimp は 1 つの DCC メッシュをマテリアルごとに分割するため、DCC 上で 1 個の
    //   オブジェクト (= 1 GameObject) が複数 submesh を持つ。単一 index にすると
    //   マテリアル数ぶんの GameObject へ散らばり、階層から元の構造が読めなくなる。
    //   ここを配列にすることで「1 ノード = 1 GameObject、その中の複数マテリアルは
    //   スロット配列」という Unity と同じ対応になる。
    //
    // WHY 以前 meshIndex を捨てたのに戻すか:
    //   撤退の理由は Inspector に -1 を入れられない壊れた int フィールドを出していたこと。
    //   これは配置時に決まる構造的な事実で、ユーザーが手で打つ値ではない。
    //   Inspector へは露出せず、Serializer と配置コードだけが書き込む。
    std::vector<uint32_t> submeshIndices;

    // 描画対象の submesh 数。submeshIndices が空ならモデル全体の submesh 数。
    [[nodiscard]] size_t SubmeshCount() const
    {
        if (!submeshIndices.empty()) return submeshIndices.size();
        return model ? model->meshes.size() : 0u;
    }

    // ローカルスロット番号 → model->meshes の添字。範囲外は UINT32_MAX。
    // WHY アクセサへ寄せるか: 描画・影・スキニング・ピッキングが同じ対応を
    //     各所で書き直すと、片方だけ直したときに「影だけ別の部位が出る」という
    //     切り分けの難しい壊れ方をする。対応は 1 箇所だけが知っていればよい。
    [[nodiscard]] uint32_t SubmeshAt(size_t localIndex) const
    {
        if (!submeshIndices.empty())
            return localIndex < submeshIndices.size()
                ? submeshIndices[localIndex] : UINT32_MAX;
        const size_t count = model ? model->meshes.size() : 0u;
        return localIndex < count ? static_cast<uint32_t>(localIndex) : UINT32_MAX;
    }

    // 担当 submesh の renderer::Mesh。未割当 / 範囲外は nullptr。
    [[nodiscard]] renderer::Mesh* SubmeshMesh(size_t localIndex) const
    {
        if (!model) return nullptr;
        const uint32_t meshIndex = SubmeshAt(localIndex);
        if (meshIndex == UINT32_MAX || meshIndex >= model->meshes.size()) return nullptr;
        return model->meshes[meshIndex].get();
    }

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
    // 最後にポーズを GPU へ展開したエンジンフレーム。
    // WHY: Scene View と Game View は同じ Time::frameCount 内で別々に RenderSystem を実行するため、
    //      bool だけでは 2 回目のビューでも同じポーズを Dispatch してしまう。
    uint64_t gpuSkinningFrame = (std::numeric_limits<uint64_t>::max)();
    // 同フレームの別ビューでも同じパス統計を表示できるよう、実際に処理した仕事量を保持する。
    uint64_t gpuSkinningVertexCount = 0;
    uint32_t gpuSkinningDispatchCount = 0;

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

    // ── ローカルスロット番号で引く版 ───────────────────────────────────────
    // WHY 2 種類の添字を分けるか (重要):
    //   morphVertexBuffers / skinnedVertexBuffers は model->meshes と同じ添字で確保する。
    //   一方 MaterialComponent のスロットとループ変数はこの Renderer 内のローカル番号。
    //   submeshIndices が入ると両者が一致しなくなり、描画ループが片方の添字で
    //   もう片方の配列を引くと「別の部位の頂点で描かれる」という、絵を見ても
    //   原因が分からない壊れ方をする。呼び出し側にはローカル番号だけを使わせ、
    //   変換をここへ閉じ込める。
    [[nodiscard]] renderer::ResourceHandle<renderer::BufferTag> ResolveSlotVertexBuffer(
        size_t localIndex,
        renderer::ResourceHandle<renderer::BufferTag> fallback) const
    {
        const uint32_t meshIndex = SubmeshAt(localIndex);
        if (meshIndex == UINT32_MAX) return fallback;
        return ResolveVertexBuffer(static_cast<size_t>(meshIndex), fallback);
    }

    [[nodiscard]] renderer::ResourceHandle<renderer::BufferTag> ResolveSlotSkinnedVertexBuffer(
        size_t localIndex) const
    {
        const uint32_t meshIndex = SubmeshAt(localIndex);
        if (meshIndex == UINT32_MAX) return {};
        return ResolveSkinnedVertexBuffer(static_cast<size_t>(meshIndex));
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
