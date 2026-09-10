/// @file    MeshTrailComponent.hpp
/// @brief   MeshRenderer / SkinnedMeshRenderer の形状を過去姿勢で半透明描画する残像トレイル。
/// @author  Hasegawa Jin
/// @date    2026-06-06
#pragma once

#include <Engine/Renderer/ResourceHandle.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/Matrix4.hpp>
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>
#include <memory>
#include <string>
#include <vector>

namespace fbzz::scene {

// MeshTrailSample — 1 回分の残像サンプル。
// WHAT: Static Mesh は world 行列だけで再描画できる。Skinned Mesh はその時点の boneMatrices も必要になる。
struct MeshTrailSample {
    float timestamp = 0.0f;
    math::Vector3 position = math::Vector3::ZERO;
    math::Matrix4 world = math::Matrix4::Identity();
    std::vector<math::Matrix4> boneMatrices;

    // SkinnedMeshRenderer 用の過去ボーンパレット。保存対象ではなく、描画時に必要なら作成する。
    renderer::ResourceHandle<renderer::ConstantBufferTag> skinningCB;
    // skinningCBDirty — サンプル時点で固定された boneMatrices を GPU へ初回だけ転送するためのフラグ。
    // WHY: 残像サンプルのボーン姿勢は生成後に変化しないため、毎フレーム再アップロードする必要がない。
    bool skinningCBDirty = true;
};

// MeshTrailComponent — オブジェクト形状そのものを残像として描画する Component。
// WHY: リボン Trail は点列から帯を生成するが、キャラクターや武器の形状残像はメッシュ再描画の方が破綻しにくい。
struct MeshTrailComponent {
    MeshTrailComponent() = default;

    MeshTrailComponent(const MeshTrailComponent& other)
        : enabled(other.enabled)
        , duration(other.duration)
        , sampleInterval(other.sampleInterval)
        , minVertexDist(other.minVertexDist)
        , maxSamples(other.maxSamples)
        , colorStart(other.colorStart)
        , colorEnd(other.colorEnd)
        , doubleSided(other.doubleSided)
        , clearOnDisable(other.clearOnDisable)
        , materialPath(other.materialPath)
        , excludedMeshIndices(other.excludedMeshIndices)
    {
    }

    MeshTrailComponent& operator=(const MeshTrailComponent& other)
    {
        if (this == &other)
            return *this;
        enabled = other.enabled;
        duration = other.duration;
        sampleInterval = other.sampleInterval;
        minVertexDist = other.minVertexDist;
        maxSamples = other.maxSamples;
        colorStart = other.colorStart;
        colorEnd = other.colorEnd;
        doubleSided = other.doubleSided;
        clearOnDisable = other.clearOnDisable;
        materialPath = other.materialPath;
        excludedMeshIndices = other.excludedMeshIndices;
        samples.clear();
        sampleHead = 0;
        sampleCount = 0;
        allocatedMaxSamples = 0;
        lastSampleTime = -1.0f;
        clearRequested = false;
        meshTrailCB = {};
        texture = {};
        loadedTexturePath.clear();
        loadedMaterialPath.clear();
        return *this;
    }

    MeshTrailComponent(MeshTrailComponent&&) noexcept = default;
    MeshTrailComponent& operator=(MeshTrailComponent&&) noexcept = default;

    bool enabled = true;

    float duration       = 0.5f;
    float sampleInterval = 1.0f / 15.0f;
    float minVertexDist  = 0.02f;
    int   maxSamples     = 12;

    math::Vector4 colorStart = { 0.35f, 0.75f, 1.0f, 0.35f };
    math::Vector4 colorEnd   = { 0.35f, 0.75f, 1.0f, 0.0f };
    bool doubleSided = true;
    // clearOnDisable — enabled=false にした瞬間に残像を消すか、duration に任せて自然消滅させるか。
    // WHY: 攻撃終了後だけサンプリングを止め、既存のメッシュ残像をフェードアウトさせる用途を Script API で表現する。
    bool clearOnDisable = true;
    // .mat アセットへの参照。albedo テクスチャと doubleSided を .mat から解決する。
    // WHY: テクスチャを .mat に集約することで複数の MeshTrail 間での共有と Editor ピッカーを使ったアセット管理を可能にする。
    std::string materialPath;
    // excludedMeshIndices — SkinnedModel 内で残像を描かない submesh index。
    // WHY: 顔や素体を除外し、武器・甲冑など演出対象だけにメッシュ残像を限定できるようにする。
    std::vector<int> excludedMeshIndices;

    // samples — 固定スロットのリングバッファ。論理順は MeshTrailRenderSystem の SampleAt() で扱う。
    // WHY: 古いサンプル破棄のたびに vector::erase(begin) で全要素を移動すると、SkinnedMesh の boneMatrices も毎回移動する。
    std::vector<MeshTrailSample> samples;
    int sampleHead = 0;
    int sampleCount = 0;
    int allocatedMaxSamples = 0;
    float lastSampleTime = -1.0f;
    bool clearRequested = false;

    renderer::ResourceHandle<renderer::ConstantBufferTag> meshTrailCB;
    renderer::ResourceHandle<renderer::TextureTag> texture;
    std::string loadedTexturePath;
    std::string loadedMaterialPath; // materialPath の変更検出用。シーン保存対象外。

    const char* GetTypeName() const { return "Mesh Trail"; }

    // Reflect — Editor / Script から編集する外観とサンプリング設定。
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

} // namespace fbzz::scene
