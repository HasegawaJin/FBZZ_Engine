/// @file    ColliderSync.hpp
/// @brief   ColliderComponent → physics::Collider の遅延構築・形状同期・ワールド姿勢反映。
/// @author  Hasegawa Jin
/// @date    2026-08-14
///
/// @note 元は PhysicsSystem.cpp (RunMode::SimOnly) に閉じていたため、エディタ停止中は
///       collider が nullptr のままで可視化・Inspector 編集が反映されなかった。
///       独立モジュールへ切り出し、Physics と可視化の双方から同じ手順で呼ぶ。
#pragma once

#include <Engine/Scene/Components/ColliderComponent.hpp>
#include <Math/Vector3.hpp>
#include <type_traits>

namespace fbzz::scene {

class Scene;
class GameObject;

/// GameObject の Transform ワールドスケール。
/// @note PrepareCollider はテンプレートで実体がここに要るが、GameObject は前方宣言のみ。
///       メンバーに触る部分だけ .cpp 側の関数へ逃がしている。
[[nodiscard]] math::Vector3 ColliderTransformScale(const GameObject& go);

/// Collider のローカル center を GameObject の worldScale でスケールしたオフセット。
[[nodiscard]] math::Vector3 ColliderCenterOffset(const GameObject& go, const ColliderComponent& col);

/// Collider 中心のワールド座標 (worldPosition + worldRotation * scaledCenter)。
[[nodiscard]] math::Vector3 ColliderWorldCenter(const GameObject& go, const ColliderComponent& col);

/// プリミティブの現在の設定とワールド姿勢から境界を測る。物理実体を変更しない。
/// 非アクティブ・無効の形状も含む。複数形状は統合し、未対応形状のみなら false。
/// 失敗時 outBounds は未変更。Transform の world 値が更新済みであること。
[[nodiscard]] bool TryGetPrimitiveColliderBounds(GameObject& go, physics::AABB& outBounds);

/// コンポーネントの形状パラメータ (size / radius / halfHeight) を physics::Collider へ反映する。
/// 実体の型が食い違っていれば作り直す。基底版は何もしない。
/// @note worldScale は Inspector 表示の寸法 (スケール前) に掛けて渡す。中心オフセットは既に
///       スケール済みのため、寸法側を省くと Transform 倍率ぶん当たり判定が小さくなる。
///       メッシュ系コライダーは UpdateColliderPose の UpdateWithScale 経由のためここには来ない。
void SyncColliderShape(ColliderComponent& col, const math::Vector3& worldScale);
void SyncColliderShape(AabbColliderComponent& col, const math::Vector3& worldScale);
void SyncColliderShape(BoxColliderComponent& col, const math::Vector3& worldScale);
void SyncColliderShape(SphereColliderComponent& col, const math::Vector3& worldScale);
void SyncColliderShape(CapsuleColliderComponent& col, const math::Vector3& worldScale);
void SyncColliderShape(CylinderColliderComponent& col, const math::Vector3& worldScale);

/// メッシュ由来コライダーの遅延構築。MeshRenderer / SkinnedMeshRenderer / meshPath の
/// 順にソースメッシュを解決し、CPU 頂点が揃っていなければ何もしない (次フレーム再試行)。
void EnsureMeshCollider(GameObject& go, MeshColliderComponent& col);
void EnsureConvexHullCollider(GameObject& go, ConvexHullColliderComponent& col);

/// 同一 GameObject の TerrainComponent から HeightFieldCollider を構築 / 再構築する。
/// 隣接タイルとの境界高さは平均化して繋ぎ、描画メッシュと段差が出ないようにする。
void SyncTerrainCollider(Scene& scene, GameObject& go, TerrainColliderComponent& col);

/// Transform のワールド姿勢を physics::Collider へ書き込む。
/// useTransformScale=false のときはスケールを適用せず、元のメッシュ寸法を保つ。
void UpdateColliderPose(const GameObject& go, ColliderComponent& col, bool useTransformScale);

/// PrepareCollider — 「構築 → 形状同期 → 姿勢反映」を型ごとに正しい順で 1 回にまとめる。
/// @return physics::Collider が利用可能になったか。false なら描画も物理登録もしない。
/// @note PhysicsSystem と DebugCollidersPass が別々に手順を組み立てると、Ensure 忘れや
///       スケール規約のずれといった差異が必ず生まれる。
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
