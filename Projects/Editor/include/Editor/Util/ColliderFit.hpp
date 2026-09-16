/// @file    ColliderFit.hpp
/// @brief   GameObject のメッシュ bounds から Collider の寸法を自動計算するヘルパー。
/// @author  Hasegawa Jin
/// @date    2026-07-08
///
/// WHY: Collider を固定既定値 (1m³ 等) で生成すると、モデルやプリミティブの実寸と
/// 合わず毎回手調整になる。Unity と同じく「アタッチした瞬間にメッシュへフィット」
/// させるため、Inspector の Add Component と Hierarchy の Create の両方がここを使う。
/// メッシュが無い GameObject には Unity 相当の既定値
/// (Box 1m³ / Sphere r0.5 / Capsule r0.5,h2 / Cylinder r0.5,h2)。
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

// CPU 頂点 (静的 / スキンの両対応) でローカル AABB を広げる。頂点が 1 つでもあれば true。
// WHY: 複数 submesh を合成できるよう、min/max を初期化せず「広げる」形にしている。
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

// CPU 頂点からローカル AABB を求める。頂点が無ければ false。
inline bool MeshLocalBounds(const renderer::Mesh& mesh,
                            math::Vector3& outMin, math::Vector3& outMax)
{
    outMin = {  1e30f,  1e30f,  1e30f };
    outMax = { -1e30f, -1e30f, -1e30f };
    return ExtendMeshLocalBounds(mesh, outMin, outMax);
}

// GameObject の描画メッシュ全体のローカル AABB。
// WHY: SkinnedMeshRenderer は 1 GameObject = モデル全体を描くため、
//      コライダーも先頭 submesh ではなく全 submesh を包む寸法でなければならない。
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

// bounds の half extents と中心。メッシュ無し / 退化時は false。
//
// WHY 中心も返すか:
//   AABB が原点対称とは限らない。原点が足元にあるキャラクターや、床から生えた柱を
//   原点対称とみなすと、寸法だけ合っていて位置が半分ずれたコライダーになる。
//   寸法を合わせる処理と位置を合わせる処理は必ず同じ bounds から出す。
inline bool FitFromGameObject(scene::GameObject& go, math::Vector3& outHalf, math::Vector3& outCenter)
{
    math::Vector3 mn, mx;
    if (!LocalBoundsFromGameObject(go, mn, mx)) return false;
    // 平面等の薄いメッシュでも物理が安定するよう最小厚みを保証する
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
    return col; // メッシュ無しは既定 1m³
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
        col.SetRadius(std::max({ half.x, half.y, half.z })); // bounds 外接 (Unity と同じ規則)
        col.SetCenter(center);
    }
    return col; // 既定 r=0.5
}

inline scene::CapsuleColliderComponent MakeFittedCapsuleCollider(scene::GameObject& go)
{
    scene::CapsuleColliderComponent col;
    math::Vector3 half, center;
    if (FitFromGameObject(go, half, center)) {
        // Y 軸カプセル: 半径は水平 extents、円柱半長は全高から両端の半球を引いた残り
        const float radius = std::max(half.x, half.z);
        const float halfHeight = std::max(half.y - radius, 0.01f);
        col.SetCapsule(radius, halfHeight);
        col.SetCenter(center);
    } else {
        col.SetCapsule(0.5f, 0.5f); // 人間大 (全高 2m, Unity と同じ)
    }
    return col;
}

inline scene::CylinderColliderComponent MakeFittedCylinderCollider(scene::GameObject& go)
{
    scene::CylinderColliderComponent col;
    math::Vector3 half, center;
    if (FitFromGameObject(go, half, center)) {
        // Y 軸円柱: カプセルと違い端が平らなので、半長は bounds の Y extents そのもの
        col.SetCylinder(std::max(half.x, half.z), half.y);
        col.SetCenter(center);
    }
    return col; // 既定 r=0.5 / 全高 2m
}

} // namespace fbzz::editor::colliderfit
