// FBZZ Engine
// PlayerWorldSpaceUIComponent.hpp | sandbox
// WorldSpace UI テスト用: 指定した名前の GO 頭上にネームプレートを表示するスクリプト
#pragma once

#include <Engine/Scene/Components/UICanvas.hpp>
#include <Engine/Scene/Components/UIImage.hpp>
#include <Engine/Scene/Components/UIText.hpp>
#include <Engine/Scene/Script.hpp>
#include <string>

using namespace fbzz::scene;
using namespace fbzz::math;

namespace sandbox {

class PlayerWorldSpaceUIComponent : public Script {
    FBZZ_SCRIPT(PlayerWorldSpaceUIComponent)

public:
    FBZZ_FIELD(std::string, targetName,  "Player", "Target Name")
    FBZZ_FIELD(std::string, displayName, "Player", "Display Name")
    FBZZ_FIELD(float,       headOffset,   2.2f,    "Head Offset")

    void OnStart()           override;
    void OnLateUpdate() override;
    void OnDestroy()         override;

private:
    EntityID    m_canvasID = EntityID::INVALID;
    GameObject* m_targetGO = nullptr;

    void SyncPosition();
};

} // namespace sandbox

#include "PlayerWorldSpaceUIComponent.generated.hpp"

// ── 実装 ────────────────────────────────────────────────────────────────────
#ifndef PLAYER_WORLDSPACE_UI_IMPL
#define PLAYER_WORLDSPACE_UI_IMPL

namespace sandbox {

inline void PlayerWorldSpaceUIComponent::OnStart()
{
    m_targetGO = scene.Find(targetName);
    if (!m_targetGO) m_targetGO = scene.Self();

    // WHY: CollectCanvases() はルート GO のみ UICanvas を探索するため親なし root として配置。
    auto& canvasGO = scene.Create("__NamePlate_Canvas__");
    m_canvasID = canvasGO.GetID();

    UICanvas canvas;
    canvas.renderMode   = UIRenderMode::WorldSpace;
    canvas.canvasWidth  = 300.0f;
    canvas.canvasHeight = 60.0f;
    canvas.worldScale   = 0.003f;
    canvas.sortOrder    = 10;
    canvasGO.AddComponent(canvas);

    auto& bgGO = scene.Create("__NamePlate_BG__");
    bgGO.SetParent(canvasGO);
    bgGO.transform.position = {};
    bgGO.transform.scale    = { 300.0f, 60.0f, 1.0f };
    UIImage bg;
    bg.color = { 0.0f, 0.0f, 0.0f, 0.65f };
    bgGO.AddComponent(bg);

    auto& textGO = scene.Create("__NamePlate_Text__");
    textGO.SetParent(canvasGO);
    textGO.transform.position = { 20.0f, 14.0f, 0.0f };
    UIText label;
    label.text     = displayName;
    label.fontSize = 32.0f;
    label.color    = { 1.0f, 1.0f, 1.0f, 1.0f };
    textGO.AddComponent(label);

    SyncPosition();
}

inline void PlayerWorldSpaceUIComponent::OnLateUpdate() { SyncPosition(); }

inline void PlayerWorldSpaceUIComponent::OnDestroy()
{
    if (auto* go = scene.GetGameObject(m_canvasID))
        scene.Destroy(*go);
}

inline void PlayerWorldSpaceUIComponent::SyncPosition()
{
    if (!m_targetGO) return;
    auto* canvasGO = scene.GetGameObject(m_canvasID);
    if (!canvasGO) return;
    const Vector3 headPos = m_targetGO->transform.worldPosition + Vector3{ 0.0f, headOffset, 0.0f };
    canvasGO->transform.position = headPos;
}

} // namespace sandbox
#endif
