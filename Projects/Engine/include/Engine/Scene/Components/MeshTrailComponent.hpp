// FBZZ Engine
// MeshTrailComponent.hpp | fbzz::scene
// MeshRenderer / SkinnedMeshRenderer の形状を過去姿勢で半透明描画する残像トレイル
#pragma once

#include <Engine/Renderer/ResourceHandle.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/Matrix4.hpp>
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>
#include <memory>
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
        samples.clear();
        lastSampleTime = -1.0f;
        clearRequested = false;
        meshTrailCB = {};
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

    // samples はリングバッファではなく小さな配列として管理する。
    // WHY: maxSamples は十数個程度を想定しており、古いサンプル削除と描画順ソートを単純に保つ。
    std::vector<MeshTrailSample> samples;
    float lastSampleTime = -1.0f;
    bool clearRequested = false;

    renderer::ResourceHandle<renderer::ConstantBufferTag> meshTrailCB;

    const char* GetTypeName() const { return "Mesh Trail"; }

    // Reflect — Editor / Script から編集する外観とサンプリング設定。
    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        r.Field("duration", duration);
        r.Field("sampleInterval", sampleInterval);
        r.Field("minVertexDist", minVertexDist);
        r.Field("maxSamples", maxSamples);
        r.Field("colorStart", colorStart);
        r.Field("colorEnd", colorEnd);
        r.Field("doubleSided", doubleSided);
    }
};

} // namespace fbzz::scene
