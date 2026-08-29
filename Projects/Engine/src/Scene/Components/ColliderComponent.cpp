/// @file    ColliderComponent.cpp
/// @brief   共有 .physmat の解決。
/// @author  Hasegawa Jin
/// @date    2026-08-16
///
/// WHY ヘッダに置かないか: ColliderComponent.hpp は Script.hpp 経由で全ユーザースクリプトへ
/// 取り込まれる。AssetManager (と、それが引き連れる MaterialAsset / ResourceManager) を
/// そこへ持ち込むと、スクリプトのビルド時間とインクルード依存が一段深くなる。
#include <Engine/Scene/Components/ColliderComponent.hpp>

#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/PhysicsMaterialAsset.hpp>

namespace fbzz::scene {

bool ColliderComponent::ResolvePhysicsMaterial()
{
    if (physicsMaterialPath.empty()) return false;

    // AssetManager がパス単位でキャッシュするため、毎フレーム呼んでもファイル I/O は起きない。
    const auto handle = asset::AssetManager::Load<asset::PhysicsMaterialAsset>(physicsMaterialPath);
    const auto* physicsMaterial = asset::AssetManager::Get<asset::PhysicsMaterialAsset>(handle);
    if (!physicsMaterial) return false;

    material = physicsMaterial->material;
    return true;
}

} // namespace fbzz::scene
