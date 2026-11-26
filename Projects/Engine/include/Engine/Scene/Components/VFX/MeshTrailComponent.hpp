/// @file    MeshTrailComponent.hpp
/// @brief   MeshRenderer / SkinnedMeshRenderer の形状を過去姿勢で半透明描画する残像トレイル。
/// @author  Hasegawa Jin
/// @date    2026-06-06
#pragma once
#include <Engine/Scene/Components/Rendering/TransientGpuState.hpp>

#include <Graphics/Renderer/Resources/ResourceHandle.hpp>
#include <Engine/Reflection/Reflection.hpp>
#include <Math/LinearAlgebra/Matrix4.hpp>
#include <Math/LinearAlgebra/Vector3.hpp>
#include <Math/LinearAlgebra/Vector4.hpp>
#include <memory>
#include <string>
#include <vector>

namespace fbzz::scene {

/// @note MeshTrailSample — 1 回分の残像サンプル。
/// @note Static Mesh は world 行列だけで再描画できる。Skinned Mesh はその時点の boneMatrices も必要になる。
/// @note GPU 所有権と再確保用キャッシュは Component の値コピーへ含めない。
struct MeshTrailSampleGpuState {
    renderer::OwnedResource<renderer::ConstantBufferTag> skinningCB; ///< SkinnedMeshRenderer 用の過去ボーンパレット。保存対象ではなく、描画時に必要なら作成する。
    /// @note skinningCBDirty — サンプル時点で固定された boneMatrices を GPU へ初回だけ転送するためのフラグ。
    /// @note 残像サンプルのボーン姿勢は生成後に変化しないため、毎フレーム再アップロードする必要がない。
    bool skinningCBDirty = true; ///< 保存した骨行列を残像用定数バッファへ転送する必要がある。
};

struct MeshTrailSample : TransientGpuState<MeshTrailSampleGpuState> {
    float timestamp = 0.0f; ///< 残像の寿命を計算するために保持するサンプル取得時刻（秒）。
    math::Vector3 position = math::Vector3::ZERO; ///< 次回のサンプリングで移動距離を測る取得時点のワールド位置。
    math::Matrix4 world = math::Matrix4::Identity(); ///< 残像メッシュを描画するためサンプリング時に保存したワールド行列。
    std::vector<math::Matrix4> boneMatrices; ///< 取得時点のスキンドメッシュのボーン行列配列。

};

/// @note MeshTrailComponent — オブジェクト形状そのものを残像として描画する Component。
/// @note リボン Trail は点列から帯を生成するが、キャラクターや武器の形状残像はメッシュ再描画の方が破綻しにくい。
/// @note GPU 所有権と再確保用キャッシュは Component の値コピーへ含めない。
struct MeshTrailComponentGpuState {
    /// @note samples — 固定スロットのリングバッファ。論理順は MeshTrailRenderSystem の SampleAt() で扱う。
    /// @note 古いサンプル破棄のたびに vector::erase(begin) で全要素を移動すると、SkinnedMesh の
    /// @note boneMatrices も毎回移動するため避ける。
    std::vector<MeshTrailSample> samples; ///< 取得順を維持する固定容量の残像のリングバッファ。
    int sampleHead = 0; ///< 最も古いサンプルを示す samples 内の添字。
    int sampleCount = 0; ///< リングバッファで有効な残像の数。
    int allocatedMaxSamples = 0; ///< 現在のリングバッファに確保したサンプル枠の数。
    float lastSampleTime = -1.0f; ///< 最後に残像を取得した時刻（秒）。負値は取得なし。
    bool clearRequested = false; ///< 既存の残像を次の描画抽出の前に破棄する。

    renderer::OwnedResource<renderer::ConstantBufferTag> meshTrailCB; ///< 残像描画へ色や時間などを渡す定数バッファ。
    renderer::ResourceHandle<renderer::TextureTag> texture; ///< 解決済みの残像用アルベドテクスチャのハンドル。
    std::string loadedTexturePath; ///< 現在の残像テクスチャを解決した参照パス。
    std::string loadedMaterialPath; ///< materialPath の変更検出用。シーン保存対象外。

};

struct MeshTrailComponent : TransientGpuState<MeshTrailComponentGpuState> {
    bool enabled = true; ///< メッシュの過去姿勢を残像として取得する。

    float duration       = 0.5f; ///< 1 残像がサンプリング後に表示される時間（秒）。
    float sampleInterval = 1.0f / 15.0f; ///< 残像取得の最小間隔（秒）。
    float minVertexDist  = 0.02f; ///< 直近の取得位置から新しいサンプルを追加するための最小移動距離（ワールド m）。
    int   maxSamples     = 12; ///< 同時に保持し描画できるメッシュ残像の最大数。

    math::Vector4 colorStart = { 0.35f, 0.75f, 1.0f, 0.35f }; ///< 新しい残像に適用する RGBA 色。
    math::Vector4 colorEnd   = { 0.35f, 0.75f, 1.0f, 0.0f }; ///< 残像が寿命を迎えるときに適用する RGBA 色。
    bool doubleSided = true; ///< 残像メッシュの表面と裏面の両方を描く。
    /// @note clearOnDisable — enabled=false にした瞬間に残像を消すか、duration に任せて自然消滅させるか。
    /// @note 攻撃終了後だけサンプリングを止め、既存のメッシュ残像をフェードアウトさせる用途を Script API で表現する。
    bool clearOnDisable = true; ///< 無効化と同時に既存の残像を破棄する。false では自然にフェードする。
    /// @note .mat アセットへの参照。albedo テクスチャと doubleSided を .mat から解決する。
    /// @note テクスチャを .mat に集約し、複数の MeshTrail 間での共有と Editor ピッカーによる管理を可能にする。
    std::string materialPath; ///< 残像用のアルベドなどを解決する .mat の参照。
    /// @note excludedMeshIndices — SkinnedModel 内で残像を描かない submesh index。
    /// @note 顔や素体を除外し、武器・甲冑など演出対象だけにメッシュ残像を限定できるようにする。
    std::vector<int> excludedMeshIndices; ///< 残像を描画しないスキンドモデル内のサブメッシュの添字列。

    const char* GetTypeName() const { return "Mesh Trail"; }

    /// @note Reflect — Editor / Script から編集する外観とサンプリング設定。
    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        r.Field("duration", duration);
        r.Field("sampleInterval", sampleInterval);
        r.Field("minVertexDist", minVertexDist);
        r.Field("maxSamples", maxSamples);
        r.ColorField("colorStart", colorStart);
        r.ColorField("colorEnd", colorEnd);
        r.Field("doubleSided", doubleSided);
        r.Field("clearOnDisable", clearOnDisable);
        r.Field("materialPath", materialPath);
        // 保存はされていたのに Reflect に無く、AI バスと汎用 Inspector からだけ
        // 見えない状態だった。キー名は保存済みシーンに合わせる。
        r.ListField("excludedMeshIndices", excludedMeshIndices);
    }
};

}
