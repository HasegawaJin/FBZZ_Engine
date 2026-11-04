/// @file    ScriptUIProxies.cpp
/// @brief   UI 領域の Script 転送と関連する内部ヘルパー。
/// @author  Hasegawa Jin
/// @date    2026-10-31
#include "ScriptProxyCommon.hpp"
#include <Engine/Scene/Script.hpp>
#include <Engine/Scene/ScriptRuntime.hpp>
#include <Engine/Scene/UIPointer.hpp>
#include <Engine/Input/Input.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Components/UIButton.hpp>
#include <Engine/Scene/Components/UICanvas.hpp>
#include <Engine/Scene/Components/UIImage.hpp>
#include <Engine/Scene/Components/UIText.hpp>
#include <Engine/Scene/Components/UIAnimator.hpp>
#include <Engine/Scene/Components/UIControls.hpp>
#include <Engine/Scene/Components/UICanvasGroup.hpp>
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
#include <Engine/Scene/Systems/UIInteraction.hpp>
namespace fbzz::scene {
using namespace script_proxy_detail;
namespace {


UIButtonPhase ToButtonPhase(const UIButton* button)
{
    if (!button || !button->enabled || !button->isInteractable)
        return UIButtonPhase::Disabled;
    switch (button->state) {
    case UIButtonState::HOVERED: return UIButtonPhase::Hovered;
    case UIButtonState::PRESSED: return UIButtonPhase::Pressed;
    default:                     return UIButtonPhase::Normal;
    }
}




/// @brief 上書きを「その場で」書き換える。
/// @note 代入 (`map[key] = {...}`) は初期化子リストから毎回 vector を作って move するため、
/// @note 要素ごと毎フレーム呼ばれるこの Setter では確保・解放が定常的に走る。既存エントリの
/// @note 容量へ assign すれば 2 フレーム目以降の確保は 0 (キーの std::string も短ければ SSO で済む)。
void AssignUIMaterialOverride(UIImage& image, std::string_view param,
                              const float* values, std::size_t count)
{
    auto& target = image.materialParamOverrides[std::string(param)];
    target.assign(values, values + count);
}




/// @brief go が属する Canvas を親方向に辿って探す。
/// @note フォーカスは Canvas が持つ。要素側から辿らないと、複数 Canvas がある画面でどれの
/// @note フォーカスを触るのか決まらない。
UICanvas* OwningCanvas(GameObject* go)
{
    for (GameObject* node = go; node != nullptr; node = node->GetParent()) {
        if (auto* canvas = node->GetComponent<UICanvas>()) return canvas;
    }
    return nullptr;
}


} /// @note namespace
bool ScriptUIProxy::WasClicked() const
{
    const auto* b = SelfComponent<UIButton>(script);
    return b && b->onClick;
}


bool ScriptUIProxy::WasHoverEnter() const
{
    const auto* b = SelfComponent<UIButton>(script);
    return b && b->onEnter;
}


bool ScriptUIProxy::WasHoverExit() const
{
    const auto* b = SelfComponent<UIButton>(script);
    return b && b->onExit;
}


bool ScriptUIProxy::IsHovered() const
{
    return GetButtonPhase() == UIButtonPhase::Hovered;
}


bool ScriptUIProxy::IsPressed() const
{
    return GetButtonPhase() == UIButtonPhase::Pressed;
}


bool ScriptUIProxy::IsInteractable() const
{
    const auto* b = SelfComponent<UIButton>(script);
    return b && b->enabled && b->isInteractable;
}


UIButtonPhase ScriptUIProxy::GetButtonPhase() const
{
    return ToButtonPhase(SelfComponent<UIButton>(script));
}


bool ScriptUIProxy::WasClicked(GameObject* go) const
{
    const auto* b = ObjectComponent<UIButton>(go);
    return b && b->onClick;
}


bool ScriptUIProxy::WasHoverEnter(GameObject* go) const
{
    const auto* b = ObjectComponent<UIButton>(go);
    return b && b->onEnter;
}


bool ScriptUIProxy::WasHoverExit(GameObject* go) const
{
    const auto* b = ObjectComponent<UIButton>(go);
    return b && b->onExit;
}


bool ScriptUIProxy::IsHovered(GameObject* go) const
{
    return GetButtonPhase(go) == UIButtonPhase::Hovered;
}


bool ScriptUIProxy::IsPressed(GameObject* go) const
{
    return GetButtonPhase(go) == UIButtonPhase::Pressed;
}


bool ScriptUIProxy::IsInteractable(GameObject* go) const
{
    const auto* b = ObjectComponent<UIButton>(go);
    return b && b->enabled && b->isInteractable;
}


UIButtonPhase ScriptUIProxy::GetButtonPhase(GameObject* go) const
{
    return ToButtonPhase(ObjectComponent<UIButton>(go));
}


void ScriptUIProxy::SetButtonInteractable(bool v) const
{
    if (auto* b = SelfComponent<UIButton>(script)) b->isInteractable = v;
}


void ScriptUIProxy::SetButtonInteractable(GameObject* go, bool v) const
{
    if (auto* b = ObjectComponent<UIButton>(go)) b->isInteractable = v;
}


void ScriptUIProxy::SetImageColor(const math::Vector4& color) const
{
    if (auto* image = SelfComponent<UIImage>(script)) image->color = color;
}


void ScriptUIProxy::SetText(std::string_view text) const
{
    if (auto* t = SelfComponent<UIText>(script)) t->text = std::string(text);
}


void ScriptUIProxy::SetTextColor(const math::Vector4& color) const
{
    if (auto* t = SelfComponent<UIText>(script)) t->color = color;
}


void ScriptUIProxy::SetCanvasSortOrder(int order) const
{
    if (auto* canvas = SelfComponent<UICanvas>(script)) canvas->sortOrder = order;
}


void ScriptUIProxy::SetPointer(const math::Vector2& canvasPosition, bool pressed) const
{
    UIPointer::Set(canvasPosition, pressed);
}


void ScriptUIProxy::ClearPointer() const
{
    UIPointer::Clear();
}


bool ScriptUIProxy::IsPointerOverridden() const
{
    return UIPointer::IsActive();
}


bool ScriptUIProxy::TryGetCanvasSize(math::Vector2& outSize, GameObject* canvasObject) const
{
    if (!canvasObject) {
        canvasObject = script ? script->m_gameObject : nullptr;
        if (!ObjectComponent<UICanvas>(canvasObject))
            canvasObject = ScriptSceneProxy{script}.FindObjectOfType<UICanvas>();
    }
    const auto* canvas = ObjectComponent<UICanvas>(canvasObject);
    const auto runtime = ScriptRuntime::GetCurrent();
    if (!canvas || runtime.viewportWidth == 0 || runtime.viewportHeight == 0) return false;
    const auto size = GetCanvasRectSize(*canvas, static_cast<float>(runtime.viewportWidth),
                                       static_cast<float>(runtime.viewportHeight));
    if (!std::isfinite(size.x) || !std::isfinite(size.y) || size.x <= 0.0f || size.y <= 0.0f) return false;
    outSize = size;
    return true;
}


math::Vector2 ScriptUIProxy::GetCanvasMousePosition() const
{
    /// @note 自 GO が Canvas ならそれを、そうでなければシーンの最初の Canvas を使う。
    if (auto* self = SelfComponent<UICanvas>(script)) return self->resolvedMousePosition;
    const ScriptSceneProxy sceneProxy{ script };
    if (GameObject* go = sceneProxy.FindObjectOfType<UICanvas>())
        if (auto* canvas = ObjectComponent<UICanvas>(go)) return canvas->resolvedMousePosition;
    return {};
}


void ScriptUIProxy::SetImageSpriteRect(float x, float y, float w, float h, float texW, float texH) const
{
    if (auto* img = SelfComponent<UIImage>(script)) {
        const auto [uvMin, uvMax] = UIImage::PixelRectToUV(x, y, w, h, texW, texH);
        img->uvMin = uvMin;
        img->uvMax = uvMax;
    }
}


void ScriptUIProxy::SetImageTexture(std::string_view path) const
{
    if (auto* img = SelfComponent<UIImage>(script)) img->texturePath = std::string(path);
}


void ScriptUIProxy::SetImageFillAmount(float amount) const
{
    if (auto* img = SelfComponent<UIImage>(script))
        img->fillAmount = std::clamp(amount, 0.0f, 1.0f);
}


void ScriptUIProxy::SetImageColor(GameObject* go, const math::Vector4& color) const
{
    if (auto* image = ObjectComponent<UIImage>(go)) image->color = color;
}


void ScriptUIProxy::SetImageFillAmount(GameObject* go, float amount) const
{
    if (auto* img = ObjectComponent<UIImage>(go))
        img->fillAmount = std::clamp(amount, 0.0f, 1.0f);
}


void ScriptUIProxy::SetImageTexture(GameObject* go, std::string_view path) const
{
    if (auto* img = ObjectComponent<UIImage>(go)) img->texturePath = std::string(path);
}


void ScriptUIProxy::SetText(GameObject* go, std::string_view text) const
{
    if (auto* t = ObjectComponent<UIText>(go)) t->text = std::string(text);
}


void ScriptUIProxy::SetTextColor(GameObject* go, const math::Vector4& color) const
{
    if (auto* t = ObjectComponent<UIText>(go)) t->color = color;
}


void ScriptUIProxy::SetImageEnabled(GameObject* go, bool enabled) const
{
    if (auto* image = ObjectComponent<UIImage>(go)) image->enabled = enabled;
}


void ScriptUIProxy::SetTextEnabled(GameObject* go, bool enabled) const
{
    if (auto* t = ObjectComponent<UIText>(go)) t->enabled = enabled;
}


float ScriptUIProxy::GetImageFillAmount() const
{
    const auto* img = SelfComponent<UIImage>(script);
    return img ? img->fillAmount : 0.0f;
}


math::Vector4 ScriptUIProxy::GetImageColor() const
{
    const auto* img = SelfComponent<UIImage>(script);
    return img ? img->color : math::Vector4{ 1.0f, 1.0f, 1.0f, 1.0f };
}


std::string ScriptUIProxy::GetText() const
{
    const auto* t = SelfComponent<UIText>(script);
    return t ? t->text : std::string{};
}


math::Vector4 ScriptUIProxy::GetTextColor() const
{
    const auto* t = SelfComponent<UIText>(script);
    return t ? t->color : math::Vector4{ 1.0f, 1.0f, 1.0f, 1.0f };
}


bool ScriptUIProxy::IsImageEnabled() const
{
    const auto* img = SelfComponent<UIImage>(script);
    return img && img->enabled;
}


bool ScriptUIProxy::IsTextEnabled() const
{
    const auto* t = SelfComponent<UIText>(script);
    return t && t->enabled;
}


float ScriptUIProxy::GetImageFillAmount(GameObject* go) const
{
    const auto* img = ObjectComponent<UIImage>(go);
    return img ? img->fillAmount : 0.0f;
}


math::Vector4 ScriptUIProxy::GetImageColor(GameObject* go) const
{
    const auto* img = ObjectComponent<UIImage>(go);
    return img ? img->color : math::Vector4{ 1.0f, 1.0f, 1.0f, 1.0f };
}


std::string ScriptUIProxy::GetText(GameObject* go) const
{
    const auto* t = ObjectComponent<UIText>(go);
    return t ? t->text : std::string{};
}


math::Vector4 ScriptUIProxy::GetTextColor(GameObject* go) const
{
    const auto* t = ObjectComponent<UIText>(go);
    return t ? t->color : math::Vector4{ 1.0f, 1.0f, 1.0f, 1.0f };
}


bool ScriptUIProxy::IsImageEnabled(GameObject* go) const
{
    const auto* img = ObjectComponent<UIImage>(go);
    return img && img->enabled;
}


bool ScriptUIProxy::IsTextEnabled(GameObject* go) const
{
    const auto* t = ObjectComponent<UIText>(go);
    return t && t->enabled;
}


void ScriptUIProxy::SetMaterial(GameObject* go, std::string_view materialPath) const
{
    auto* image = ObjectComponent<UIImage>(go);
    if (!image) return;
    const std::string next(materialPath);
    if (image->materialPath == next) return;
    image->materialPath = next;
    /// @note 別のマテリアルへ移ると、前のシェーダーに合わせた上書きは意味を失う。
    /// @note 残すと「効かない上書き」が黙って積まれ、綴り間違いと区別が付かなくなる。
    image->materialParamOverrides.clear();
    image->materialTextureOverrides.clear();
}


void ScriptUIProxy::SetMaterialFloat(GameObject* go, std::string_view param, float value) const
{
    if (auto* image = ObjectComponent<UIImage>(go))
        AssignUIMaterialOverride(*image, param, &value, 1);
}


void ScriptUIProxy::SetMaterialVector2(GameObject* go, std::string_view param,
                                       float x, float y) const
{
    const float values[2] = { x, y };
    if (auto* image = ObjectComponent<UIImage>(go))
        AssignUIMaterialOverride(*image, param, values, 2);
}


void ScriptUIProxy::SetMaterialVector4(GameObject* go, std::string_view param,
                                       const math::Vector4& value) const
{
    const float values[4] = { value.x, value.y, value.z, value.w };
    if (auto* image = ObjectComponent<UIImage>(go))
        AssignUIMaterialOverride(*image, param, values, 4);
}


void ScriptUIProxy::SetMaterialColor(GameObject* go, std::string_view param,
                                     const math::Vector4& color) const
{
    SetMaterialVector4(go, param, color);
}


void ScriptUIProxy::SetMaterialTexture(GameObject* go, std::string_view slot,
                                       std::string_view texturePath) const
{
    if (auto* image = ObjectComponent<UIImage>(go)) {
        /// @note 値側も assign で容量を使い回す。テクスチャパスは SSO に収まらない長さになる。
        image->materialTextureOverrides[std::string(slot)].assign(
            texturePath.data(), texturePath.size());
    }
}


void ScriptUIProxy::ClearMaterialOverride(GameObject* go, std::string_view param) const
{
    if (auto* image = ObjectComponent<UIImage>(go)) {
        const std::string key(param);
        image->materialParamOverrides.erase(key);
        image->materialTextureOverrides.erase(key);
    }
}


void ScriptUIProxy::ClearMaterialOverrides(GameObject* go) const
{
    if (auto* image = ObjectComponent<UIImage>(go)) {
        image->materialParamOverrides.clear();
        image->materialTextureOverrides.clear();
    }
}


bool ScriptUIProxy::HasMaterialOverride(GameObject* go, std::string_view param) const
{
    const auto* image = ObjectComponent<UIImage>(go);
    if (!image) return false;
    const std::string key(param);
    return image->materialParamOverrides.count(key) > 0
        || image->materialTextureOverrides.count(key) > 0;
}

/// @name ウィジェットの値
/// @brief 子オブジェクトを名前で探す。
/// @note スクリプトが Engine の実装型に直接依存しないための層 (AGENTS.md)。無いと
/// @note オプション画面が GetComponent<UISlider>() を直に呼ぶ形になる。

GameObject* ScriptUIProxy::Find(GameObject* parent, std::string_view childName) const
{
    if (!parent || !parent->IsValid()) return nullptr;
    for (int i = 0; i < parent->GetChildCount(); ++i) {
        GameObject* child = parent->GetChild(i);
        if (child && child->name == childName) return child;
    }
    return nullptr;
}


bool ScriptUIProxy::IsPointerOverUI() const
{
    const auto* self = script ? script->scene.Self() : nullptr;
    const auto* owner = self ? self->GetScene() : nullptr;
    return owner && ScriptRuntime::IsPointerOverUI(owner->GetRenderSceneGeneration());
}


float ScriptUIProxy::GetSliderValue(GameObject* go) const
{
    const auto* s = ObjectComponent<UISlider>(go);
    return s ? s->value : 0.0f;
}


void ScriptUIProxy::SetSliderValue(GameObject* go, float value) const
{
    auto* s = ObjectComponent<UISlider>(go);
    if (!s) return;
    const float low  = (std::min)(s->minimum, s->maximum);
    const float high = (std::max)(s->minimum, s->maximum);
    s->value = std::clamp(s->wholeNumbers ? std::round(value) : value, low, high);
    /// @note 見た目の追従は UISystem がドラッグ中にしかやらない。外から入れた値でも
    /// @note ゲージが動くよう、同じ規則をここでも当てる。
    if (auto* image = ObjectComponent<UIImage>(go)) {
        const float range = s->maximum - s->minimum;
        image->fillAmount = std::abs(range) > 0.000001f
            ? std::clamp((s->value - s->minimum) / range, 0.0f, 1.0f) : 0.0f;
    }
}


bool ScriptUIProxy::WasSliderChanged(GameObject* go) const
{
    const auto* s = ObjectComponent<UISlider>(go);
    return s && s->onValueChanged;
}


void ScriptUIProxy::SetSliderRange(GameObject* go, float minimum, float maximum) const
{
    if (auto* s = ObjectComponent<UISlider>(go)) {
        s->minimum = minimum;
        s->maximum = maximum;
    }
}


void ScriptUIProxy::SetSliderInteractable(GameObject* go, bool interactable) const
{
    if (auto* s = ObjectComponent<UISlider>(go)) s->interactable = interactable;
}


bool ScriptUIProxy::IsToggleOn(GameObject* go) const
{
    const auto* t = ObjectComponent<UIToggle>(go);
    return t && t->isOn;
}


void ScriptUIProxy::SetToggleOn(GameObject* go, bool isOn) const
{
    if (auto* t = ObjectComponent<UIToggle>(go)) t->isOn = isOn;
}


bool ScriptUIProxy::WasToggleChanged(GameObject* go) const
{
    const auto* t = ObjectComponent<UIToggle>(go);
    return t && t->onValueChanged;
}


void ScriptUIProxy::SetToggleInteractable(GameObject* go, bool interactable) const
{
    if (auto* t = ObjectComponent<UIToggle>(go)) t->interactable = interactable;
}


math::Vector2 ScriptUIProxy::GetScrollPosition(GameObject* go) const
{
    const auto* s = ObjectComponent<UIScrollView>(go);
    return s ? s->scrollPosition : math::Vector2::ZERO;
}


void ScriptUIProxy::SetScrollPosition(GameObject* go, const math::Vector2& position) const
{
    if (auto* s = ObjectComponent<UIScrollView>(go)) s->scrollPosition = position;
}


void ScriptUIProxy::SetScrollContentSize(GameObject* go, const math::Vector2& size) const
{
    if (auto* s = ObjectComponent<UIScrollView>(go)) s->contentSize = size;
}


bool ScriptUIProxy::WasScrollChanged(GameObject* go) const
{
    const auto* s = ObjectComponent<UIScrollView>(go);
    return s && s->onValueChanged;
}


std::string ScriptUIProxy::GetFieldText(GameObject* go) const
{
    const auto* f = ObjectComponent<UIInputField>(go);
    return f ? f->text : std::string{};
}


void ScriptUIProxy::SetFieldText(GameObject* go, std::string_view text) const
{
    if (auto* f = ObjectComponent<UIInputField>(go)) {
        f->text.assign(text.data(), text.size());
        f->caretPosition = f->text.size();
    }
}


bool ScriptUIProxy::WasFieldChanged(GameObject* go) const
{
    const auto* f = ObjectComponent<UIInputField>(go);
    return f && f->onValueChanged;
}


bool ScriptUIProxy::WasSubmitted(GameObject* go) const
{
    const auto* f = ObjectComponent<UIInputField>(go);
    return f && f->onSubmit;
}


bool ScriptUIProxy::IsFieldFocused(GameObject* go) const
{
    const auto* f = ObjectComponent<UIInputField>(go);
    return f && f->focused;
}


void ScriptUIProxy::SetFieldFocused(GameObject* go, bool focused) const
{
    if (auto* f = ObjectComponent<UIInputField>(go)) f->focused = focused;
}


float ScriptUIProxy::GetGroupAlpha(GameObject* go) const
{
    const auto* g = ObjectComponent<UICanvasGroup>(go);
    return g ? g->alpha : 1.0f;
}


void ScriptUIProxy::SetGroupAlpha(GameObject* go, float alpha) const
{
    if (auto* g = ObjectComponent<UICanvasGroup>(go))
        g->alpha = std::clamp(alpha, 0.0f, 1.0f);
}


void ScriptUIProxy::SetGroupInteractable(GameObject* go, bool interactable) const
{
    if (auto* g = ObjectComponent<UICanvasGroup>(go)) g->interactable = interactable;
}


void ScriptUIProxy::SetGroupBlocksRaycasts(GameObject* go, bool blocks) const
{
    if (auto* g = ObjectComponent<UICanvasGroup>(go)) g->blocksRaycasts = blocks;
}


void ScriptUIProxy::SetFocus(GameObject* go) const
{
    if (!go || !go->IsValid()) return;
    if (auto* canvas = OwningCanvas(go))
        canvas->focusedObject = { go->GetID() };
}


GameObject* ScriptUIProxy::GetFocus() const
{
    const ScriptSceneProxy sceneProxy{ script };
    GameObject* canvasGO = sceneProxy.FindObjectOfType<UICanvas>();
    const auto* canvas = ObjectComponent<UICanvas>(canvasGO);
    if (!canvas || !canvas->focusedObject.IsValid()) return nullptr;
    return canvas->focusedObject.Resolve(sceneProxy);
}


bool ScriptUIProxy::IsFocused(GameObject* go) const
{
    const auto* nav = ObjectComponent<UINavigation>(go);
    return nav && nav->focused;
}


void ScriptUIProxy::ClearFocus(GameObject* go) const
{
    if (!go || !go->IsValid()) return;
    if (auto* canvas = OwningCanvas(go)) canvas->focusedObject = {};
}


bool ScriptUIProxy::IsDragging(GameObject* go) const
{
    const auto* d = ObjectComponent<UIDragSource>(go);
    return d && d->dragging;
}


bool ScriptUIProxy::WasDropped(GameObject* go) const
{
    const auto* d = ObjectComponent<UIDragSource>(go);
    return d && d->dropped;
}


GameObject* ScriptUIProxy::GetDropTarget(GameObject* go) const
{
    const auto* d = ObjectComponent<UIDragSource>(go);
    if (!d || !d->droppedOn.IsValid()) return nullptr;
    const ScriptSceneProxy sceneProxy{ script };
    return d->droppedOn.Resolve(sceneProxy);
}


bool ScriptUIProxy::WasReceived(GameObject* go) const
{
    const auto* t = ObjectComponent<UIDropTarget>(go);
    return t && t->received;
}


GameObject* ScriptUIProxy::GetReceivedFrom(GameObject* go) const
{
    const auto* t = ObjectComponent<UIDropTarget>(go);
    if (!t || !t->receivedFrom.IsValid()) return nullptr;
    const ScriptSceneProxy sceneProxy{ script };
    return t->receivedFrom.Resolve(sceneProxy);
}


std::string ScriptUIProxy::GetReceivedPayload(GameObject* go) const
{
    const auto* t = ObjectComponent<UIDropTarget>(go);
    return t ? t->receivedPayload : std::string{};
}


bool ScriptUIProxy::IsDropHovered(GameObject* go) const
{
    const auto* t = ObjectComponent<UIDropTarget>(go);
    return t && t->hovered;
}


void ScriptUIProxy::SetVisibleCharacters(GameObject* go, int count) const
{
    if (auto* t = ObjectComponent<UIText>(go)) t->visibleCharacters = count;
}


int ScriptUIProxy::GetVisibleCharacters(GameObject* go) const
{
    const auto* t = ObjectComponent<UIText>(go);
    return t ? t->visibleCharacters : -1;
}


int ScriptUIProxy::GetCharacterCount(GameObject* go) const
{
    const auto* t = ObjectComponent<UIText>(go);
    if (!t) return 0;
    return static_cast<int>(UITextVisibleLength(t->text, t->richText));
}


void ScriptUIAnimatorProxy::PlayColor(const math::Vector4& from, const math::Vector4& to, float duration) const
{
    PlayColor(from, to, duration, UIEasingType::Linear);
}


void ScriptUIAnimatorProxy::PlayColor(const math::Vector4& from, const math::Vector4& to,
                                      float duration, UIEasingType easing, bool loop, bool pingPong) const
{
    if (auto* anim = SelfComponent<UIAnimator>(script))
        anim->PlayColor(from, to, duration, easing, loop, pingPong);
}


void ScriptUIAnimatorProxy::PlayPosition(const math::Vector2& from, const math::Vector2& to, float duration) const
{
    PlayPosition(from, to, duration, UIEasingType::Linear);
}


void ScriptUIAnimatorProxy::PlayPosition(const math::Vector2& from, const math::Vector2& to,
                                         float duration, UIEasingType easing, bool loop, bool pingPong) const
{
    if (auto* anim = SelfComponent<UIAnimator>(script))
        anim->PlayPosition(from, to, duration, easing, loop, pingPong);
}


void ScriptUIAnimatorProxy::PlayScale(const math::Vector2& from, const math::Vector2& to, float duration) const
{
    PlayScale(from, to, duration, UIEasingType::Linear);
}


void ScriptUIAnimatorProxy::PlayScale(const math::Vector2& from, const math::Vector2& to,
                                      float duration, UIEasingType easing, bool loop, bool pingPong) const
{
    if (auto* anim = SelfComponent<UIAnimator>(script))
        anim->PlayScale(from, to, duration, easing, loop, pingPong);
}


void ScriptUIAnimatorProxy::StopColor() const
{
    if (auto* anim = SelfComponent<UIAnimator>(script)) anim->StopColor();
}


void ScriptUIAnimatorProxy::StopPosition() const
{
    if (auto* anim = SelfComponent<UIAnimator>(script)) anim->StopPosition();
}


void ScriptUIAnimatorProxy::StopScale() const
{
    if (auto* anim = SelfComponent<UIAnimator>(script)) anim->StopScale();
}


void ScriptUIAnimatorProxy::StopAll() const
{
    if (auto* anim = SelfComponent<UIAnimator>(script)) anim->StopAll();
}


bool ScriptUIAnimatorProxy::IsPlaying() const
{
    if (auto* anim = SelfComponent<UIAnimator>(script)) return anim->IsPlaying();
    return false;
}


} /// @note namespace fbzz::scene
