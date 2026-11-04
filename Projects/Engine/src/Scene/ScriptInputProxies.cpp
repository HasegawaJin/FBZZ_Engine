/// @file    ScriptInputProxies.cpp
/// @brief   Input 領域の Script 転送と関連する内部ヘルパー。
/// @author  Hasegawa Jin
/// @date    2026-10-31
#include <Engine/Scene/Script.hpp>
#include <Engine/Core/Cursor.hpp>
#include <Engine/Input/Input.hpp>
#include <Engine/Input/Gamepad.hpp>
#include <Engine/Input/InputActionMap.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <limits>
#include <type_traits>
#include <string_view>
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cmath>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include "../Input/GameInputConsumption.hpp"
namespace fbzz::scene {
namespace {

/// @brief pad = -1 を「接続中の最初のパッド」へ解決する。
/// @brief 1 台も接続されていない場合は 0 を返す (未接続スロットへの問い合わせは false / 0)。
int ResolveScriptPad(int pad)
{
    if (pad >= 0) return pad;
    const int first = input::Gamepad::GetFirstConnectedPad();
    return first >= 0 ? first : 0;
}

} /// @note namespace
bool ScriptInputProxy::GetKey(input::KeyCode key) const { return input::Input::GameKeyHeld(key); }

bool ScriptInputProxy::GetKeyDown(input::KeyCode key) const { return input::Input::GameKeyDown(key); }

bool ScriptInputProxy::GetKeyUp(input::KeyCode key) const { return input::Input::GameKeyUp(key); }


math::Vector2 ScriptInputProxy::GetMouseDelta() const { return input::Input::GameMouseDelta(); }

math::Vector2 ScriptInputProxy::GetMousePosition() const { return input::Input::GameMousePosition(); }

float ScriptInputProxy::GetMouseScrollDelta() const { return input::Input::GameMouseScrollDelta(); }

bool ScriptInputProxy::MouseButton(MouseBtn btn) const { return input::Input::GameMouseButton(static_cast<int>(btn)); }

bool ScriptInputProxy::MouseButtonDown(MouseBtn btn) const { return input::Input::GameMouseButtonDown(static_cast<int>(btn)); }

bool ScriptInputProxy::MouseButtonUp(MouseBtn btn) const { return input::Input::GameMouseButtonUp(static_cast<int>(btn)); }

/// @name アクション層


bool ScriptInputProxy::GetAction(std::string_view name) const
{
    return input::InputActionMap::GetAction(name);
}

bool ScriptInputProxy::GetActionDown(std::string_view name) const
{
    return input::InputActionMap::GetActionDown(name);
}

bool ScriptInputProxy::GetActionUp(std::string_view name) const
{
    return input::InputActionMap::GetActionUp(name);
}

float ScriptInputProxy::GetActionAxis(std::string_view name) const
{
    return input::InputActionMap::GetAxis(name);
}


math::Vector2 ScriptInputProxy::GetMoveAxis() const
{
    return input::InputActionMap::GetAxis2D("MoveX", "MoveY");
}

math::Vector2 ScriptInputProxy::GetLookAxis() const
{
    return input::InputActionMap::GetAxis2D("LookX", "LookY");
}

/// @name ゲームパッド直接アクセス


bool ScriptInputProxy::GetPadButton(input::GamepadButton button, int pad) const
{
    return core::Cursor::CanReadGameInput() && !input::input_detail::GameInputConsumptionState().PadButtonBlocked(ResolveScriptPad(pad), static_cast<std::size_t>(button)) && input::Gamepad::ButtonHeld(button, ResolveScriptPad(pad));
}

bool ScriptInputProxy::GetPadButtonDown(input::GamepadButton button, int pad) const
{
    return core::Cursor::CanReadGameInput() && !input::input_detail::GameInputConsumptionState().PadButtonBlocked(ResolveScriptPad(pad), static_cast<std::size_t>(button)) && input::Gamepad::ButtonDown(button, ResolveScriptPad(pad));
}

bool ScriptInputProxy::GetPadButtonUp(input::GamepadButton button, int pad) const
{
    return core::Cursor::CanReadGameInput() && !input::input_detail::GameInputConsumptionState().PadButtonBlocked(ResolveScriptPad(pad), static_cast<std::size_t>(button)) && input::Gamepad::ButtonUp(button, ResolveScriptPad(pad));
}

float ScriptInputProxy::GetPadAxis(input::GamepadAxis axis, int pad) const
{
    return core::Cursor::CanReadGameInput() && !input::input_detail::GameInputConsumptionState().PadAxisBlocked(ResolveScriptPad(pad), static_cast<std::size_t>(axis)) ? input::Gamepad::Axis(axis, ResolveScriptPad(pad)) : 0.0f;
}

bool ScriptInputProxy::IsPadConnected(int pad) const
{
    if (pad >= 0) return input::Gamepad::IsConnected(pad);
    return input::Gamepad::GetFirstConnectedPad() >= 0;
}


void ScriptInputProxy::SetVibration(float lowFrequency, float highFrequency,
                                    float durationSeconds, int pad) const
{
    if (!core::Cursor::CanReadGameInput()) return;
    input::Gamepad::SetVibration(lowFrequency, highFrequency, durationSeconds,
                                 ResolveScriptPad(pad));
}

void ScriptInputProxy::StopVibration(int pad) const
{
    input::Gamepad::StopVibration(ResolveScriptPad(pad));
}

/// @name キーコンフィグ


int ScriptInputProxy::GetActionBindingCount(std::string_view action) const
{
    const auto* found = input::InputActionMap::FindAction(action);
    return found ? static_cast<int>(found->bindings.size()) : 0;
}


ScriptInputBinding ScriptInputProxy::GetActionBinding(std::string_view action, int index) const
{
    const auto* found = input::InputActionMap::FindAction(action);
    if (!found || index < 0 || index >= static_cast<int>(found->bindings.size())) return {};

    const input::InputBinding& binding = found->bindings[static_cast<size_t>(index)];
    return { static_cast<int>(binding.source), binding.code };
}


bool ScriptInputProxy::SetActionBinding(std::string_view action, int index,
                                        ScriptInputBinding binding) const
{
    auto* found = input::InputActionMap::FindAction(action);
    if (!found || !binding.IsValid()) return false;

    /// @note scale / padIndex / buttonThreshold は既存のバインドのものを引き継ぐ。閾値 0.5 のボタン扱い等の
    /// @note 設定は割り当てを差し替えても効いてほしく、値まで初期化するとリバインドした軸だけ既定へ戻る。
    if (index >= 0 && index < static_cast<int>(found->bindings.size())) {
        input::InputBinding& slot = found->bindings[static_cast<size_t>(index)];
        slot.source = static_cast<input::BindingSource>(binding.source);
        slot.code   = binding.code;
        return true;
    }

    input::InputBinding added{};
    added.source = static_cast<input::BindingSource>(binding.source);
    added.code   = binding.code;
    found->bindings.push_back(added);
    return true;
}


std::string ScriptInputProxy::DescribeActionBinding(std::string_view action, int index) const
{
    const auto* found = input::InputActionMap::FindAction(action);
    if (!found || index < 0 || index >= static_cast<int>(found->bindings.size())) return {};
    return input::InputActionMap::DescribeBinding(found->bindings[static_cast<size_t>(index)]);
}


void ScriptInputProxy::BeginRebindAction(std::string_view action, int index) const
{
    input::InputActionMap::BeginRebindAction(action, index);
}


bool ScriptInputProxy::IsRebinding() const
{
    return input::InputActionMap::IsRebinding();
}


bool ScriptInputProxy::ConsumeRebindCompleted() const
{
    return input::InputActionMap::ConsumeRebindCompleted();
}


void ScriptInputProxy::CancelRebind() const
{
    input::InputActionMap::CancelRebind();
}


} /// @note namespace fbzz::scene
