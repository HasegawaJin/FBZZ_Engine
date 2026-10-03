/// @file    SkinnedMeshRenderer.hpp
/// @brief   Component that draws a skinned model and owns runtime bone entity links.
/// @author  Hasegawa Jin
/// @date    2026-05-24
#pragma once

#include <Engine/Asset/Model.hpp>
#include <Engine/Scene/Entity.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/Matrix4.hpp>
#include <cstdint>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

namespace fbzz::scene {

struct SkinnedMeshRenderer {
    bool enabled = true;
    /// @note LODSystem 専用のランタイム可視性。Scene には保存しない。
    bool lodVisible = true;
    /// @note LOD クロスフェード中のディザしきい値。MeshRenderer::lodDither と同じ意味。
    float lodDither = 0.0f;
    /// @note シャドウマップへ影を落とすか。MeshRenderer::castShadows と同じ意味。
    bool castShadows = true;
    std::string modelPath;

    asset::Model* model = nullptr;

    /// @brief この Renderer が描く submesh の添字列 (model->meshes の添字)。空ならモデル全体を描く。
    /// @note ローカルスロット i (= MaterialComponent のスロット i) が submeshIndices[i] を描く。
    ///       Assimp は 1 DCC メッシュをマテリアルごとに分割するため、配列にして「1 ノード =
    ///       1 GameObject、マテリアルはスロット配列」という Unity と同じ対応にしている。
    /// @note 配置時に決まる構造的な値。Inspector には出さず、Serializer と配置コードだけが書き込む。
    std::vector<uint32_t> submeshIndices;

    /// @brief 描画対象の submesh 数。submeshIndices が空ならモデル全体の submesh 数。
    [[nodiscard]] size_t SubmeshCount() const
    {
        if (!submeshIndices.empty()) return submeshIndices.size();
        return model ? model->meshes.size() : 0u;
    }

    /// @brief ローカルスロット番号 → model->meshes の添字。
    /// @return 範囲外は UINT32_MAX。
    /// @note 描画・影・スキニング・ピッキングが同じ対応をここへ集約する。各所で書き直すと
    ///       片方だけ直したときに部位ごとに食い違う壊れ方をする。
    [[nodiscard]] uint32_t SubmeshAt(size_t localIndex) const
    {
        if (!submeshIndices.empty())
            return localIndex < submeshIndices.size()
                ? submeshIndices[localIndex] : UINT32_MAX;
        const size_t count = model ? model->meshes.size() : 0u;
        return localIndex < count ? static_cast<uint32_t>(localIndex) : UINT32_MAX;
    }

    /// @brief 担当 submesh の renderer::Mesh。
    /// @return 未割当 / 範囲外は nullptr。
    [[nodiscard]] renderer::Mesh* SubmeshMesh(size_t localIndex) const
    {
        if (!model) return nullptr;
        const uint32_t meshIndex = SubmeshAt(localIndex);
        if (meshIndex == UINT32_MAX || meshIndex >= model->meshes.size()) return nullptr;
        return model->meshes[meshIndex].get();
    }

    /// @note SkeletonNode はアセット側の参照テーブルのまま。これらの EntityID はロード/実行時に
    ///       再構築され、実体である Bone GameObject を指す (Transform を編集・アニメ・リグの共通基盤にするため)。
    EntityID skeletonRootEntity = EntityID::INVALID;
    std::vector<EntityID> nodeEntities;
    /// @note Morph は共有 Mesh を変更せず、この Renderer インスタンス専用の頂点バッファへ反映する。
    std::unordered_map<std::string, float> morphWeights;
    std::unordered_map<std::string, float> appliedMorphWeights;
    std::vector<renderer::ResourceHandle<renderer::BufferTag>> morphVertexBuffers;

    /// @name コンピュートスキニングの出力
    /// @{
    /// @brief submesh ごとの「変形済み頂点」。レイアウトは静的メッシュの renderer::Vertex と同一で、
    ///        シャドウや GBuffer からは普通の静的メッシュとして読める。
    /// @note VS スキニングだとシャドウと画面で同じ変形を描くたび計算し直す。1 回だけ計算して共有する置き場。
    std::vector<renderer::ResourceHandle<renderer::BufferTag>> skinnedVertexBuffers;
    /// @brief skinnedVertexBuffers を確保したときのモデル。model が差し替わったら作り直す。
    /// @note バッファは submesh の頂点数に合わせて確保するため、古いバッファを使い回すと
    ///       頂点数が足りず描画が途中で切れる / 範囲外へ書き込む。
    const asset::Model* skinnedBufferModel = nullptr;
    /// @brief このフレームで GPU スキニングが成立したか。
    /// @note false のときは従来の VS スキニング経路を使う (Animator 未評価・モーフ有効・
    ///       バックエンド未対応などで成立しないことがある)。
    bool gpuSkinnedThisFrame = false;
    /// @brief 最後にポーズを GPU へ展開したエンジンフレーム。
    /// @note Scene View と Game View は同じ Time::frameCount 内で別々に RenderSystem を実行するため、
    ///       bool だけでは 2 回目のビューでも同じポーズを Dispatch してしまう。
    uint64_t gpuSkinningFrame = (std::numeric_limits<uint64_t>::max)();
    /// @brief 同フレームの別ビューでも同じパス統計を表示できるよう、実際に処理した仕事量を保持する。
    uint64_t gpuSkinningVertexCount = 0;
    uint32_t gpuSkinningDispatchCount = 0;
    /// @note 入力の完全比較で同じポーズの再 Dispatch を省く。保存せず、model / resource 世代が変われば棄却する。
    std::vector<uint32_t> gpuSkinningContent;
    /// @}

    /// @note VelocityPass だけが読み書きする。シーンへは保存しない。ボーンの動きは
    ///       AnimatorComponent::prevBoneMatrices が持つので、ここはキャラクター自身の
    ///       移動・回転ぶんだけ。2 枚持つのは MeshRenderer と同じ理由 (Scene View と Game View が
    ///       同じフレームで 2 回描く)。
    math::Matrix4 prevWorldMatrix     = math::Matrix4::Identity();
    math::Matrix4 worldMatrixSnapshot = math::Matrix4::Identity();
    uint64_t      prevWorldFrame      = (std::numeric_limits<uint64_t>::max)();

    /// @brief GPU スキニング済みなら変形済み頂点バッファを、そうでなければ無効ハンドルを返す。
    /// @note ResolveVertexBuffer と混同しないこと。あちらは「VS でスキニングする入力」を返す。
    ///       こちらを VS スキニング用シェーダーへ渡すと二重に変形がかかって崩れる。
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

    /// @name ローカルスロット番号で引く版
    /// @{
    /// @note morphVertexBuffers / skinnedVertexBuffers は model->meshes の添字で確保するが、
    ///       MaterialComponent のスロットとループ変数はこの Renderer 内のローカル番号。
    ///       submeshIndices が入ると両者が食い違い、誤った添字で引くと別部位の頂点で描かれる。
    ///       呼び出し側にはローカル番号だけを使わせ、変換をここへ閉じ込める。
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
    /// @}

    const char* GetTypeName() const { return "Skinned Mesh Renderer"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled",     enabled);
        r.Field("castShadows", castShadows);
        r.Field("modelPath",   modelPath);
    }
};

} // namespace fbzz::scene
