/// @file    ColliderFit.hpp
/// @brief   GameObject のメッシュ bounds から Collider の寸法を自動計算するヘルパー。
/// @author  Hasegawa Jin
/// @date    2026-07-08
///
/// @note アタッチした瞬間にメッシュへフィットさせる (Unity と同じ)。Inspector の Add Component と
///       Hierarchy の Create の両方がここを使う。メッシュが無い GameObject は Unity 相当の既定値
///       (Box 1m³ / Sphere r0.5 / Capsule r0.5,h2 / Cylinder r0.5,h2)。
#pragma once
#include <Engine/Renderer/Mesh.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Components/ColliderComponent.hpp>
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include <Engine/Scene/Components/SkinnedMeshRenderer.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/Model.hpp>
#include <algorithm>

namespace fbzz::editor::colliderfit {

/// CPU 頂点 (静的 / スキンの両対応) でローカル AABB を広げる。頂点が 1 つでもあれば true。
/// @note 複数 submesh を合成できるよう、min/max を初期化せず「広げる」形にしている。
inline bool ExtendMeshLocalBounds(const renderer::Mesh& mesh,
                                  math::Vector3& outMin, math::Vector3& outMax)
{
    bool any = false;
    auto extend = [&](const math::Vector3& p) {
        outMin.x = std::min(outMin.x, p.x); outMax.x = std::max(outMax.x, p.x);
        outMin.y = std::min(outMin.y, p.y); outMax.y = std::max(outMax.y, p.y);
        outMin.z = std::min(outMin.z, p.z); outMax.z = std::max(outMax.z, p.z);
        any = true;
    };
    for (const auto& v : mesh.cpuVertices)        extend(v.position);
    for (const auto& v : mesh.cpuSkinnedVertices) extend(v.position);
    return any;
}

/// CPU 頂点からローカル AABB を求める。頂点が無ければ false。
inline bool MeshLocalBounds(const renderer::Mesh& mesh,
                            math::Vector3& outMin, math::Vector3& outMax)
{
    outMin = {  1e30f,  1e30f,  1e30f };
    outMax = { -1e30f, -1e30f, -1e30f };
    return ExtendMeshLocalBounds(mesh, outMin, outMax);
}

/// GameObject の描画メッシュ全体のローカル AABB。
/// @note SkinnedMeshRenderer は 1 GameObject = モデル全体を描くため、
///       コライダーも先頭 submesh ではなく全 submesh を包む寸法でなければならない。
inline bool LocalBoundsFromGameObject(scene::GameObject& go,
                                      math::Vector3& outMin, math::Vector3& outMax)
{
    outMin = {  1e30f,  1e30f,  1e30f };
    outMax = { -1e30f, -1e30f, -1e30f };

    if (auto* mr = go.GetComponent<scene::MeshRenderer>(); mr && mr->mesh)
        return ExtendMeshLocalBounds(*mr->mesh, outMin, outMax);

    if (auto* smr = go.GetComponent<scene::SkinnedMeshRenderer>()) {
        if (!smr->model && !smr->modelPath.empty())
            smr->model = asset::AssetManager::LoadAndGet<asset::Model>(smr->modelPath);
        if (!smr->model) return false;
        bool any = false;
        for (const auto& mesh : smr->model->meshes)
            if (mesh) any |= ExtendMeshLocalBounds(*mesh, outMin, outMax);
        return any;
    }
    return false;
}

/// bounds の half extents と中心。メッシュ無し / 退化時は false。
///
/// @note AABB は原点対称とは限らない (足元に原点があるキャラクター等)。寸法を合わせる処理と
///       位置を合わせる処理は必ず同じ bounds から出す。
inline bool FitFromGameObject(scene::GameObject& go, math::Vector3& outHalf, math::Vector3& outCenter)
{
    math::Vector3 mn, mx;
    if (!LocalBoundsFromGameObject(go, mn, mx)) return false;
    /// @note 平面等の薄いメッシュでも物理が安定するよう最小厚みを保証する
    constexpr float kMinHalf = 0.01f;
    outHalf = { std::max((mx.x - mn.x) * 0.5f, kMinHalf),
                std::max((mx.y - mn.y) * 0.5f, kMinHalf),
                std::max((mx.z - mn.z) * 0.5f, kMinHalf) };
    outCenter = (mn + mx) * 0.5f;
    return true;
}

inline bool HalfExtentsFromGameObject(scene::GameObject& go, math::Vector3& outHalf)
{
    math::Vector3 center;
    return FitFromGameObject(go, outHalf, center);
}

inline scene::BoxColliderComponent MakeFittedBoxCollider(scene::GameObject& go)
{
    scene::BoxColliderComponent col;
    math::Vector3 half, center;
    if (FitFromGameObject(go, half, center)) {
        col.SetSize(half * 2.0f);
        col.SetCenter(center);
    }
    /// @note メッシュ無しは既定 1m³
    return col;
}

inline scene::AabbColliderComponent MakeFittedAabbCollider(scene::GameObject& go)
{
    scene::AabbColliderComponent col;
    math::Vector3 half, center;
    if (FitFromGameObject(go, half, center)) {
        col.SetSize(half * 2.0f);
        col.SetCenter(center);
    }
    return col;
}

inline scene::SphereColliderComponent MakeFittedSphereCollider(scene::GameObject& go)
{
    scene::SphereColliderComponent col;
    math::Vector3 half, center;
    if (FitFromGameObject(go, half, center)) {
        /// @note bounds 外接 (Unity と同じ規則)
        col.SetRadius(std::max({ half.x, half.y, half.z }));
        col.SetCenter(center);
    }
    /// @note 既定 r=0.5
    return col;
}

inline scene::CapsuleColliderComponent MakeFittedCapsuleCollider(scene::GameObject& go)
{
    scene::CapsuleColliderComponent col;
    math::Vector3 half, center;
    if (FitFromGameObject(go, half, center)) {
        /// @note Y 軸カプセル: 半径は水平 extents、円柱半長は全高から両端の半球を引いた残り
        const float radius = std::max(half.x, half.z);
        const float halfHeight = std::max(half.y - radius, 0.01f);
        col.SetCapsule(radius, halfHeight);
        col.SetCenter(center);
    } else {
        /// @note 人間大 (全高 2m, Unity と同じ)
        col.SetCapsule(0.5f, 0.5f);
    }
    return col;
}

inline scene::CylinderColliderComponent MakeFittedCylinderCollider(scene::GameObject& go)
{
    scene::CylinderColliderComponent col;
    math::Vector3 half, center;
    if (FitFromGameObject(go, half, center)) {
        /// @note Y 軸円柱: カプセルと違い端が平らなので、半長は bounds の Y extents そのもの
        col.SetCylinder(std::max(half.x, half.z), half.y);
        col.SetCenter(center);
    }
    /// @note 既定 r=0.5 / 全高 2m
    return col;
}

} // namespace fbzz::editor::colliderfit
