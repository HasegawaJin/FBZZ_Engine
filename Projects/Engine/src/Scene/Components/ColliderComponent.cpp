/// @file    ColliderComponent.cpp
/// @brief   共有 .physmat の解決。
/// @author  Hasegawa Jin
/// @date    2026-08-16
///
/// ColliderComponent.hpp は Script.hpp 経由で全ユーザースクリプトへ取り込まれる。AssetManager
/// (と、それが引き連れる MaterialAsset / ResourceManager) をそこへ持ち込むと、スクリプトの
/// ビルド時間とインクルード依存が一段深くなるため、この実装は .cpp に置く。
#include <Engine/Scene/Components/ColliderComponent.hpp>

#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/PhysicsMaterialAsset.hpp>

namespace fbzz::scene {

bool ColliderComponent::ResolvePhysicsMaterial()
{
    if (physicsMaterialPath.empty()) return false;

    /// @note AssetManager がパス単位でキャッシュするため、毎フレーム呼んでもファイル I/O は起きない。
    const auto handle = asset::AssetManager::Load<asset::PhysicsMaterialAsset>(physicsMaterialPath);
    const auto* physicsMaterial = asset::AssetManager::Get<asset::PhysicsMaterialAsset>(handle);
    if (!physicsMaterial) return false;

    material = physicsMaterial->material;
    return true;
}

} // namespace fbzz::scene
