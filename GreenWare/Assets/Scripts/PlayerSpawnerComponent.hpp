// GreenWare
// PlayerSpawnerComponent.hpp | sandbox
// Player.scene 起動時に、カプセル形状のプレイヤーを実行時生成するテスト用スポーナー
#pragma once

#include <Engine/Scene/Components/CharacterControllerComponent.hpp>
#include <Engine/Scene/Components/ColliderComponent.hpp>
#include <Engine/Scene/Components/MaterialComponent.hpp>
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include <Engine/Scene/Components/RigidBodyComponent.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Physics/RigidBody.hpp>
#include "Scripts/PlayerControllerComponent.hpp"
#include <memory>

using namespace fbzz::scene;
using namespace fbzz::math;

namespace sandbox {

class PlayerSpawnerComponent : public Script {
    FBZZ_SCRIPT(PlayerSpawnerComponent)

public:
    void OnStart() override;

private:
    bool m_spawned = false;
};

FBZZ_REFLECT(PlayerSpawnerComponent)

inline void PlayerSpawnerComponent::OnStart()
{
    if (m_spawned || scene.FindWithTag("Player")) return;
    m_spawned = true;

    // WHY: primitive:capsule は既定で半径0.5・半円柱長0.5 (全高2m) で生成される。
    //      CapsuleColliderComponent の既定 halfHeight=1.0 (=中心から円柱端までの距離) は
    //      見た目と噛み合わない (全高3m相当になる) ため、radius==halfHeight のまま
    //      SetCapsule() で明示的に揃える。
    constexpr float kRadius = 0.5f;
    constexpr float kHalfHeight = 0.5f;
    constexpr float kRestY = kRadius + kHalfHeight; // カプセル下端が Y=0 に接する高さ

    GameObject& player = scene.Create("Player");
    player.tag = "Player";
    player.transform.position = { 0.0f, kRestY, 0.0f };

    auto& mesh = player.AddComponent<MeshRenderer>();
    mesh.meshPath = "primitive:capsule";

    // WHY: MeshRenderer だけでは描画されない (RenderSystem は MaterialComponent が無い GO を
    //      描画スキップする)。新規 .mat を作らず共有 Fallback.mat + albedo 上書きで単色を与える。
    auto& material = player.AddComponent<MaterialComponent>();
    material.materialPath = "Assets/Materials/Fallback/Fallback.mat";
    material.paramOverrides["albedo"] = { 0.25f, 0.55f, 1.0f, 1.0f };

    auto& collider = player.AddComponent<CapsuleColliderComponent>();
    collider.SetCapsule(kRadius, kHalfHeight);

    auto& body = player.AddComponent<RigidBodyComponent>();
    body.rigidBody = std::make_unique<fbzz::physics::RigidBody>();
    body.rigidBody->SetMass(70.0f);
    body.rigidBody->m_linearDrag = 0.05f;
    body.rigidBody->SetPosition(player.transform.position);

    player.AddComponent<CharacterControllerComponent>();
    player.AddScript<PlayerControllerComponent>();
}

} // namespace sandbox
