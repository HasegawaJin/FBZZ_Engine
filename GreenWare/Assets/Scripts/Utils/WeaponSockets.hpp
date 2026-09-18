/// @file    WeaponSockets.hpp
/// @brief   両手剣のオブジェクト名と装着点を共有する。
/// @author  Hasegawa Jin
/// @date    2026-08-22
#pragma once

#include <Engine/Scene/GameObject.hpp>
#include <Scripts/Utils/BladeColors.hpp>
#include <string_view>

namespace sandbox {

inline constexpr const char* kSwordObject = "Sword";
inline constexpr const char* kSocketWeapon = "Socket_Weapon_R";
inline constexpr const char* kSocketGrip = "Attach_Grip";
inline constexpr const char* kSocketTip = "SOCKET_Tip";
inline constexpr const char* kSocketTrailBase = "SOCKET_Trail_Base";
inline constexpr const char* kSocketTrailTip = "SOCKET_Trail_Tip";

/// VFXの汎用チャンネルAPIを保つ。Playerは右手で一本の剣を保持する。
enum class HandSide : int { Left = 2, Right = 1 };
[[nodiscard]] inline HandSide HandOf(BladeSide) { return HandSide::Right; }
[[nodiscard]] inline const char* SwordObjectName(HandSide) { return kSwordObject; }

[[nodiscard]] inline fbzz::scene::GameObject*
FindInSubtree(fbzz::scene::GameObject& root, std::string_view name)
{
    return root.FindInSubtree(name);
}

} // namespace sandbox
