/// @file    ManagedServiceUi.cpp
/// @brief   UI コンポーネントとポインター操作のサービス経路。
/// @author  Hasegawa Jin
/// @date    2026-11-21
#include "ManagedServiceRoutes.hpp"

#include <Engine/Scene/Components/UIButton.hpp>
#include <Engine/Scene/Components/UICanvas.hpp>
#include <Engine/Scene/Components/UICanvasGroup.hpp>
#include <Engine/Scene/Components/UIControls.hpp>
#include <Engine/Scene/Components/UIImage.hpp>
#include <Engine/Scene/Components/UIText.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <cmath>

namespace fbzz::scene::managed_detail {

ManagedBridgeStatus RunManagedUiService(Script& view, GameObject& target, const ManagedBridgeRequest& request,
    const ManagedServiceArguments& arguments, ManagedServiceResult& output)
{
    const auto& p = arguments.values;
    const auto& t = arguments.text;
    const int64_t integer = arguments.integer;
    {
        bool present = false;
        if (request.index <= 5 || request.index == 10 || request.index == 17) present = target.GetComponent<UIButton>() != nullptr;
        else if (request.index == 6 || request.index == 12 || request.index == 13 || request.index == 18 || (request.index >= 24 && request.index <= 27)) present = target.GetComponent<UIImage>() != nullptr;
        else if (request.index == 7 || request.index == 14 || request.index == 15 || request.index == 19 || request.index == 20 || request.index == 28 || (request.index >= 62 && request.index <= 64)) present = target.GetComponent<UIText>() != nullptr;
        else if (request.index == 21 || (request.index >= 76 && request.index <= 79)) present = target.GetComponent<UICanvas>() != nullptr;
        else if (request.index >= 29 && request.index <= 33) present = target.GetComponent<UISlider>() != nullptr;
        else if (request.index >= 34 && request.index <= 37) present = target.GetComponent<UIToggle>() != nullptr;
        else if (request.index >= 38 && request.index <= 41) present = target.GetComponent<UIScrollView>() != nullptr;
        else if (request.index >= 42 && request.index <= 47) present = target.GetComponent<UIInputField>() != nullptr;
        else if (request.index >= 48 && request.index <= 51) present = target.GetComponent<UICanvasGroup>() != nullptr;
        else if (request.index >= 55 && request.index <= 57) present = target.GetComponent<UIDragSource>() != nullptr;
        else if (request.index >= 58 && request.index <= 61) present = target.GetComponent<UIDropTarget>() != nullptr;
        else present = target.GetComponent<UIButton>() || target.GetComponent<UIImage>() || target.GetComponent<UIText>()
            || target.GetComponent<UICanvas>() || target.GetComponent<UISlider>() || target.GetComponent<UIToggle>()
            || target.GetComponent<UIScrollView>() || target.GetComponent<UIInputField>() || target.GetComponent<UICanvasGroup>()
            || target.GetComponent<UIDragSource>() || target.GetComponent<UIDropTarget>();
        if (!present) return ManagedBridgeStatus::NOT_FOUND;
    }
    switch (request.index) {
    case 0: {
        if (!MatchesServiceArguments(request, arguments, 1u, 0, 0)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        output.integer = static_cast<int64_t>(view.ui.WasClicked());
        break;
    }
    case 1: {
        if (!MatchesServiceArguments(request, arguments, 1u, 0, 0)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        output.integer = static_cast<int64_t>(view.ui.WasHoverEnter());
        break;
    }
    case 2: {
        if (!MatchesServiceArguments(request, arguments, 1u, 0, 0)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        output.integer = static_cast<int64_t>(view.ui.WasHoverExit());
        break;
    }
    case 3: {
        if (!MatchesServiceArguments(request, arguments, 1u, 0, 0)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        output.integer = static_cast<int64_t>(view.ui.IsHovered());
        break;
    }
    case 4: {
        if (!MatchesServiceArguments(request, arguments, 1u, 0, 0)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        output.integer = static_cast<int64_t>(view.ui.IsPressed());
        break;
    }
    case 5: {
        if (!MatchesServiceArguments(request, arguments, 1u, 0, 0)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        output.integer = static_cast<int64_t>(view.ui.IsInteractable());
        break;
    }
    case 6: {
        if (!MatchesServiceArguments(request, arguments, 1u, 0, 0)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        output.integer = static_cast<int64_t>(view.ui.IsImageEnabled());
        break;
    }
    case 7: {
        if (!MatchesServiceArguments(request, arguments, 1u, 0, 0)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        output.integer = static_cast<int64_t>(view.ui.IsTextEnabled());
        break;
    }
    case 8: {
        if (!MatchesServiceArguments(request, arguments, 1u, 0, 0)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        output.integer = static_cast<int64_t>(view.ui.IsPointerOverridden());
        break;
    }
    case 9: {
        if (!MatchesServiceArguments(request, arguments, 1u, 0, 0)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        output.integer = static_cast<int64_t>(view.ui.IsPointerOverUI());
        break;
    }
    case 10: {
        if (!MatchesServiceArguments(request, arguments, 1u, 0, 0)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        output.integer = static_cast<int64_t>(view.ui.GetButtonPhase());
        break;
    }
    case 11: {
        if (!MatchesServiceArguments(request, arguments, 1u, 0, 0)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        const auto value = view.ui.GetCanvasMousePosition(); output.values = {value.x, value.y};
        break;
    }
    case 12: {
        if (!MatchesServiceArguments(request, arguments, 1u, 0, 0)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        output.values = {view.ui.GetImageFillAmount()};
        break;
    }
    case 13: {
        if (!MatchesServiceArguments(request, arguments, 1u, 0, 0)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        const auto value = view.ui.GetImageColor(); output.values = {value.x, value.y, value.z, value.w};
        break;
    }
    case 14: {
        if (!MatchesServiceArguments(request, arguments, 1u, 0, 0)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        output.text.emplace_back(view.ui.GetText());
        break;
    }
    case 15: {
        if (!MatchesServiceArguments(request, arguments, 1u, 0, 0)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        const auto value = view.ui.GetTextColor(); output.values = {value.x, value.y, value.z, value.w};
        break;
    }
    case 16: {
        if (!MatchesServiceArguments(request, arguments, 1u, 0, 0)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        output.object = view.ui.GetFocus();
        break;
    }
    case 17: {
        if (request.flags != 0u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
        if (integer != 0 && integer != 1) return ManagedBridgeStatus::INVALID_ARGUMENT;
        if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
        view.ui.SetButtonInteractable(integer != 0);
        break;
    }
    case 18: {
        if (!MatchesServiceArguments(request, arguments, 0u, 4, 0)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        view.ui.SetImageColor(math::Vector4{p[0], p[1], p[2], p[3]});
        break;
    }
    case 19: {
        if (!MatchesServiceArguments(request, arguments, 0u, 0, 1)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        view.ui.SetText(t[0]);
        break;
    }
    case 20: {
        if (!MatchesServiceArguments(request, arguments, 0u, 4, 0)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        view.ui.SetTextColor(math::Vector4{p[0], p[1], p[2], p[3]});
        break;
    }
    case 21: {
        if (request.flags != 0u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
        if (!FitsInt(integer)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
        view.ui.SetCanvasSortOrder(static_cast<int>(integer));
        break;
    }
    case 22: {
        if (request.flags != 0u || p.size() != 2 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
        if (integer != 0 && integer != 1) return ManagedBridgeStatus::INVALID_ARGUMENT;
        if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
        view.ui.SetPointer(math::Vector2{p[0], p[1]}, integer != 0);
        break;
    }
    case 23: {
        if (!MatchesServiceArguments(request, arguments, 0u, 0, 0)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        view.ui.ClearPointer();
        break;
    }
    case 24: {
        if (!MatchesServiceArguments(request, arguments, 0u, 6, 0)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        view.ui.SetImageSpriteRect(p[0], p[1], p[2], p[3], p[4], p[5]);
        break;
    }
    case 25: {
        if (!MatchesServiceArguments(request, arguments, 0u, 0, 1)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        view.ui.SetImageTexture(t[0]);
        break;
    }
    case 26: {
        if (!MatchesServiceArguments(request, arguments, 0u, 1, 0)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        view.ui.SetImageFillAmount(p[0]);
        break;
    }
    case 27: {
        if (request.flags != 0u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
        if (integer != 0 && integer != 1) return ManagedBridgeStatus::INVALID_ARGUMENT;
        if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
        view.ui.SetImageEnabled(&target, integer != 0);
        break;
    }
    case 28: {
        if (request.flags != 0u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
        if (integer != 0 && integer != 1) return ManagedBridgeStatus::INVALID_ARGUMENT;
        if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
        view.ui.SetTextEnabled(&target, integer != 0);
        break;
    }
    case 29: {
        if (!MatchesServiceArguments(request, arguments, 1u, 0, 0)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        output.values = {view.ui.GetSliderValue(&target)};
        break;
    }
    case 30: {
        if (!MatchesServiceArguments(request, arguments, 0u, 1, 0)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        view.ui.SetSliderValue(&target, p[0]);
        break;
    }
    case 31: {
        if (!MatchesServiceArguments(request, arguments, 1u, 0, 0)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        output.integer = static_cast<int64_t>(view.ui.WasSliderChanged(&target));
        break;
    }
    case 32: {
        if (!MatchesServiceArguments(request, arguments, 0u, 2, 0)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        view.ui.SetSliderRange(&target, p[0], p[1]);
        break;
    }
    case 33: {
        if (request.flags != 0u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
        if (integer != 0 && integer != 1) return ManagedBridgeStatus::INVALID_ARGUMENT;
        if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
        view.ui.SetSliderInteractable(&target, integer != 0);
        break;
    }
    case 34: {
        if (!MatchesServiceArguments(request, arguments, 1u, 0, 0)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        output.integer = static_cast<int64_t>(view.ui.IsToggleOn(&target));
        break;
    }
    case 35: {
        if (request.flags != 0u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
        if (integer != 0 && integer != 1) return ManagedBridgeStatus::INVALID_ARGUMENT;
        if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
        view.ui.SetToggleOn(&target, integer != 0);
        break;
    }
    case 36: {
        if (!MatchesServiceArguments(request, arguments, 1u, 0, 0)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        output.integer = static_cast<int64_t>(view.ui.WasToggleChanged(&target));
        break;
    }
    case 37: {
        if (request.flags != 0u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
        if (integer != 0 && integer != 1) return ManagedBridgeStatus::INVALID_ARGUMENT;
        if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
        view.ui.SetToggleInteractable(&target, integer != 0);
        break;
    }
    case 38: {
        if (!MatchesServiceArguments(request, arguments, 1u, 0, 0)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        const auto value = view.ui.GetScrollPosition(&target); output.values = {value.x, value.y};
        break;
    }
    case 39: {
        if (!MatchesServiceArguments(request, arguments, 0u, 2, 0)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        view.ui.SetScrollPosition(&target, math::Vector2{p[0], p[1]});
        break;
    }
    case 40: {
        if (!MatchesServiceArguments(request, arguments, 0u, 2, 0)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        view.ui.SetScrollContentSize(&target, math::Vector2{p[0], p[1]});
        break;
    }
    case 41: {
        if (!MatchesServiceArguments(request, arguments, 1u, 0, 0)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        output.integer = static_cast<int64_t>(view.ui.WasScrollChanged(&target));
        break;
    }
    case 42: {
        if (!MatchesServiceArguments(request, arguments, 1u, 0, 0)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        output.text.emplace_back(view.ui.GetFieldText(&target));
        break;
    }
    case 43: {
        if (!MatchesServiceArguments(request, arguments, 0u, 0, 1)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        view.ui.SetFieldText(&target, t[0]);
        break;
    }
    case 44: {
        if (!MatchesServiceArguments(request, arguments, 1u, 0, 0)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        output.integer = static_cast<int64_t>(view.ui.WasFieldChanged(&target));
        break;
    }
    case 45: {
        if (!MatchesServiceArguments(request, arguments, 1u, 0, 0)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        output.integer = static_cast<int64_t>(view.ui.WasSubmitted(&target));
        break;
    }
    case 46: {
        if (!MatchesServiceArguments(request, arguments, 1u, 0, 0)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        output.integer = static_cast<int64_t>(view.ui.IsFieldFocused(&target));
        break;
    }
    case 47: {
        if (request.flags != 0u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
        if (integer != 0 && integer != 1) return ManagedBridgeStatus::INVALID_ARGUMENT;
        if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
        view.ui.SetFieldFocused(&target, integer != 0);
        break;
    }
    case 48: {
        if (!MatchesServiceArguments(request, arguments, 1u, 0, 0)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        output.values = {view.ui.GetGroupAlpha(&target)};
        break;
    }
    case 49: {
        if (!MatchesServiceArguments(request, arguments, 0u, 1, 0)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        view.ui.SetGroupAlpha(&target, p[0]);
        break;
    }
    case 50: {
        if (request.flags != 0u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
        if (integer != 0 && integer != 1) return ManagedBridgeStatus::INVALID_ARGUMENT;
        if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
        view.ui.SetGroupInteractable(&target, integer != 0);
        break;
    }
    case 51: {
        if (request.flags != 0u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
        if (integer != 0 && integer != 1) return ManagedBridgeStatus::INVALID_ARGUMENT;
        if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
        view.ui.SetGroupBlocksRaycasts(&target, integer != 0);
        break;
    }
    case 52: {
        if (!MatchesServiceArguments(request, arguments, 0u, 0, 0)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        view.ui.SetFocus(&target);
        break;
    }
    case 53: {
        if (!MatchesServiceArguments(request, arguments, 1u, 0, 0)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        output.integer = static_cast<int64_t>(view.ui.IsFocused(&target));
        break;
    }
    case 54: {
        if (!MatchesServiceArguments(request, arguments, 0u, 0, 0)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        view.ui.ClearFocus(&target);
        break;
    }
    case 55: {
        if (!MatchesServiceArguments(request, arguments, 1u, 0, 0)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        output.integer = static_cast<int64_t>(view.ui.IsDragging(&target));
        break;
    }
    case 56: {
        if (!MatchesServiceArguments(request, arguments, 1u, 0, 0)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        output.integer = static_cast<int64_t>(view.ui.WasDropped(&target));
        break;
    }
    case 57: {
        if (!MatchesServiceArguments(request, arguments, 1u, 0, 0)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        output.object = view.ui.GetDropTarget(&target);
        break;
    }
    case 58: {
        if (!MatchesServiceArguments(request, arguments, 1u, 0, 0)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        output.integer = static_cast<int64_t>(view.ui.WasReceived(&target));
        break;
    }
    case 59: {
        if (!MatchesServiceArguments(request, arguments, 1u, 0, 0)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        output.object = view.ui.GetReceivedFrom(&target);
        break;
    }
    case 60: {
        if (!MatchesServiceArguments(request, arguments, 1u, 0, 0)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        output.text.emplace_back(view.ui.GetReceivedPayload(&target));
        break;
    }
    case 61: {
        if (!MatchesServiceArguments(request, arguments, 1u, 0, 0)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        output.integer = static_cast<int64_t>(view.ui.IsDropHovered(&target));
        break;
    }
    case 62: {
        if (request.flags != 0u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
        if (!FitsInt(integer)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
        view.ui.SetVisibleCharacters(&target, static_cast<int>(integer));
        break;
    }
    case 63: {
        if (!MatchesServiceArguments(request, arguments, 1u, 0, 0)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        output.integer = static_cast<int64_t>(view.ui.GetVisibleCharacters(&target));
        break;
    }
    case 64: {
        if (!MatchesServiceArguments(request, arguments, 1u, 0, 0)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        output.integer = static_cast<int64_t>(view.ui.GetCharacterCount(&target));
        break;
    }
    case 65: {
        if (!MatchesServiceArguments(request, arguments, 0u, 0, 1)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        view.ui.SetMaterial(&target, t[0]);
        break;
    }
    case 66: {
        if (!MatchesServiceArguments(request, arguments, 0u, 1, 1)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        view.ui.SetMaterialFloat(&target, t[0], p[0]);
        break;
    }
    case 67: {
        if (!MatchesServiceArguments(request, arguments, 0u, 2, 1)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        view.ui.SetMaterialVector2(&target, t[0], p[0], p[1]);
        break;
    }
    case 68: {
        if (!MatchesServiceArguments(request, arguments, 0u, 4, 1)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        view.ui.SetMaterialVector4(&target, t[0], math::Vector4{p[0], p[1], p[2], p[3]});
        break;
    }
    case 69: {
        if (!MatchesServiceArguments(request, arguments, 0u, 4, 1)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        view.ui.SetMaterialColor(&target, t[0], math::Vector4{p[0], p[1], p[2], p[3]});
        break;
    }
    case 70: {
        if (!MatchesServiceArguments(request, arguments, 0u, 0, 2)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        view.ui.SetMaterialTexture(&target, t[0], t[1]);
        break;
    }
    case 71: {
        if (!MatchesServiceArguments(request, arguments, 0u, 0, 1)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        view.ui.ClearMaterialOverride(&target, t[0]);
        break;
    }
    case 72: {
        if (!MatchesServiceArguments(request, arguments, 0u, 0, 0)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        view.ui.ClearMaterialOverrides(&target);
        break;
    }
    case 73: {
        if (!MatchesServiceArguments(request, arguments, 1u, 0, 1)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        output.integer = static_cast<int64_t>(view.ui.HasMaterialOverride(&target, t[0]));
        break;
    }
    case 74: {
        if (!MatchesServiceArguments(request, arguments, 1u, 0, 1)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        output.object = view.ui.Find(&target, t[0]);
        break;
    }
    case 75: {
        if (!MatchesServiceArguments(request, arguments, 1u, 0, 0)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        math::Vector2 value;
        output.integer = view.ui.TryGetCanvasSize(value, &target) ? 1 : 0;
        if (output.integer) output.values = {value.x, value.y};
        break;
    }
    case 76: {
        if (request.flags != 1u || integer || !p.empty() || !t.empty() || arguments.relatedId)
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        output.integer = static_cast<int64_t>(target.GetComponent<UICanvas>()->depthMode);
        break;
    }
    case 77: {
        if (request.flags || integer < 0 || integer > 1 || !p.empty() || !t.empty() || arguments.relatedId)
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        target.GetComponent<UICanvas>()->depthMode = static_cast<UICanvasDepthMode>(integer);
        break;
    }
    case 78: {
        if (request.flags != 1u || integer || !p.empty() || !t.empty() || arguments.relatedId)
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        output.values = {target.GetComponent<UICanvas>()->surfaceOffset};
        break;
    }
    case 79: {
        if (request.flags || integer || p.size() != 1 || !t.empty() || arguments.relatedId
            || !std::isfinite(p[0]) || p[0] < -1.0f || p[0] > 1.0f)
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        target.GetComponent<UICanvas>()->surfaceOffset = p[0];
        break;
    }
    default: return ManagedBridgeStatus::UNSUPPORTED;
    }
    return ManagedBridgeStatus::OK;
}

}
