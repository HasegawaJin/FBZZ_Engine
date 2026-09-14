/// @file    ColliderSync.hpp
/// @brief   ColliderComponent → physics::Collider の遅延構築・形状同期・ワールド姿勢反映。
/// @author  Hasegawa Jin
/// @date    2026-08-14
///
/// WHY: これらの処理は元々 PhysicsSystem.cpp の無名名前空間に閉じていた。
/// しかし PhysicsSystem は RunMode::SimOnly のため、エディタ停止中は一度も走らない。
/// その結果 MeshCollider / ConvexHullCollider / TerrainCollider は
/// collider が nullptr のままとなり、コライダー可視化 (DebugCollidersPass) が
/// 「一部のオブジェクトだけ描かれない」状態になっていた。
/// また Inspector の Reflect 経由で size / radius を編集しても
/// SetSize() を通らないため、停止中は形状が古いまま表示されていた。
/// 同期処理を独立モジュールへ切り出し、Physics と可視化の双方から同じ手順を呼ぶ。
#pragma once

#include <Engine/Scene/Components/ColliderComponent.hpp>
#include <Math/Vector3.hpp>
#include <type_traits>

namespace fbzz::scene {

class Scene;
class GameObject;

// GameObject の Transform ワールドスケール。
// WHY 関数にするか: PrepareCollider はテンプレートのためこのヘッダーに実体が要るが、
//      ここでは GameObject が前方宣言しかされていない (依存を増やさないための意図的な形)。
//      メンバーへ触る部分だけ .cpp 側の関数へ逃がす。
[[nodiscard]] math::Vector3 ColliderTransformScale(const GameObject& go);

// Collider のローカル center を GameObject の worldScale でスケールしたオフセット。
[[nodiscard]] math::Vector3 ColliderCenterOffset(const GameObject& go, const ColliderComponent& col);

// Collider 中心のワールド座標 (worldPosition + worldRotation * scaledCenter)。
[[nodiscard]] math::Vector3 ColliderWorldCenter(const GameObject& go, const ColliderComponent& col);

/// プリミティブの現在の設定とワールド姿勢から境界を測る。物理実体を変更しない。
/// 非アクティブ・無効の形状も含む。複数形状は統合し、未対応形状のみなら false。
/// 失敗時 outBounds は未変更。Transform の world 値が更新済みであること。
[[nodiscard]] bool TryGetPrimitiveColliderBounds(GameObject& go, physics::AABB& outBounds);

// コンポーネントの形状パラメータ (size / radius / halfHeight) を physics::Collider へ反映する。
// 実体の型が食い違っている場合は作り直す。基底版は「形状パラメータを持たない」ため何もしない。
//
// WHY worldScale を受け取るか:
//   Inspector の size / radius は「スケールを掛ける前の寸法」で、メッシュのローカル bounds と
//   同じ空間にある。スケールを掛けずに physics へ渡すと、Transform を 2 倍にした
//   オブジェクトのコライダーだけが等倍のまま残り、見た目より小さい当たり判定になる。
//   中心オフセット (ColliderWorldCenter) は既にスケールを掛けているので、
//   寸法だけ掛けていないのは単純に片手落ちだった。
//   メッシュ系コライダーは UpdateColliderPose 側で UpdateWithScale を通るため、ここには来ない。
void SyncColliderShape(ColliderComponent& col, const math::Vector3& worldScale);
void SyncColliderShape(AabbColliderComponent& col, const math::Vector3& worldScale);
void SyncColliderShape(BoxColliderComponent& col, const math::Vector3& worldScale);
void SyncColliderShape(SphereColliderComponent& col, const math::Vector3& worldScale);
void SyncColliderShape(CapsuleColliderComponent& col, const math::Vector3& worldScale);
void SyncColliderShape(CylinderColliderComponent& col, const math::Vector3& worldScale);

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
        SyncColliderShape(col, ColliderTransformScale(go));
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
