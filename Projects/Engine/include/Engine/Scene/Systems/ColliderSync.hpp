// FBZZ Engine
// ColliderSync.hpp | fbzz::scene
// ColliderComponent → physics::Collider の遅延構築・形状同期・ワールド姿勢反映
//
// WHY: これらの処理は元々 PhysicsSystem.cpp の無名名前空間に閉じていた。
//      しかし PhysicsSystem は RunMode::SimOnly のため、エディタ停止中は一度も走らない。
//      その結果 MeshCollider / ConvexHullCollider / TerrainCollider は
//      collider が nullptr のままとなり、コライダー可視化 (DebugCollidersPass) が
//      「一部のオブジェクトだけ描かれない」状態になっていた。
//      また Inspector の Reflect 経由で size / radius を編集しても
//      SetSize() を通らないため、停止中は形状が古いまま表示されていた。
//      同期処理を独立モジュールへ切り出し、Physics と可視化の双方から同じ手順を呼ぶ。
#pragma once

#include <Engine/Scene/Components/ColliderComponent.hpp>
#include <Math/Vector3.hpp>
#include <type_traits>

namespace fbzz::scene {

class Scene;
class GameObject;

// Collider のローカル center を GameObject の worldScale でスケールしたオフセット。
[[nodiscard]] math::Vector3 ColliderCenterOffset(const GameObject& go, const ColliderComponent& col);

// Collider 中心のワールド座標 (worldPosition + worldRotation * scaledCenter)。
[[nodiscard]] math::Vector3 ColliderWorldCenter(const GameObject& go, const ColliderComponent& col);

// コンポーネントの形状パラメータ (size / radius / halfHeight) を physics::Collider へ反映する。
// 実体の型が食い違っている場合は作り直す。基底版は「形状パラメータを持たない」ため何もしない。
void SyncColliderShape(ColliderComponent& col);
void SyncColliderShape(AabbColliderComponent& col);
void SyncColliderShape(BoxColliderComponent& col);
void SyncColliderShape(SphereColliderComponent& col);
void SyncColliderShape(CapsuleColliderComponent& col);

// メッシュ由来コライダーの遅延構築。MeshRenderer / SkinnedMeshRenderer / meshPath の
// 順にソースメッシュを解決し、CPU 頂点が揃っていなければ何もしない (次フレーム再試行)。
void EnsureMeshCollider(GameObject& go, MeshColliderComponent& col);
void EnsureConvexHullCollider(GameObject& go, ConvexHullColliderComponent& col);

// 同一 GameObject の TerrainComponent から HeightFieldCollider を構築 / 再構築する。
// 隣接タイルとの境界高さは平均化して繋ぎ、描画メッシュと段差が出ないようにする。
void SyncTerrainCollider(Scene& scene, GameObject& go, TerrainColliderComponent& col);

// Transform のワールド姿勢を physics::Collider へ書き込む。
// useTransformScale=false のときはスケールを適用せず、元のメッシュ寸法を保つ。
void UpdateColliderPose(const GameObject& go, ColliderComponent& col, bool useTransformScale);

// PrepareCollider — 「構築 → 形状同期 → 姿勢反映」を型ごとに正しい順で 1 回にまとめる。
// 戻り値: physics::Collider が利用可能になったか (false のときは描画も物理登録もしない)。
// WHY: PhysicsSystem と DebugCollidersPass が別々に手順を組み立てると、
//      片方だけ Ensure を忘れる / スケール規約がずれる、といった差異が必ず生まれる。
template<typename T>
bool PrepareCollider(Scene& scene, GameObject& go, T& col)
{
    if constexpr (std::is_same_v<T, MeshColliderComponent>) {
        EnsureMeshCollider(go, col);
    } else if constexpr (std::is_same_v<T, ConvexHullColliderComponent>) {
        EnsureConvexHullCollider(go, col);
    } else if constexpr (std::is_same_v<T, TerrainColliderComponent>) {
        SyncTerrainCollider(scene, go, col);
    } else {
        SyncColliderShape(col);
    }

    if (!col.collider) return false;

    bool useTransformScale = true;
    if constexpr (std::is_same_v<T, MeshColliderComponent> ||
                  std::is_same_v<T, ConvexHullColliderComponent>) {
        useTransformScale = col.useTransformScale;
    }
    UpdateColliderPose(go, col, useTransformScale);
    return true;
}

} // namespace fbzz::scene
