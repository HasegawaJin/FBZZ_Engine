// FBZZ Engine
// UIAnimatorSystem.cpp | fbzz::scene
// UIAnimator の Tween 評価
// 時間経過に応じて UIImage の色と Transform の位置を更新する。
// UISystem より前に呼ぶことで描画へ反映される。
#include "Engine/Scene/Systems/UIAnimatorSystem.hpp"
#include "Engine/Scene/Scene.hpp"
#include "Engine/Scene/GameObject.hpp"
#include "Engine/Scene/Components/UIAnimator.hpp"
#include "Engine/Scene/Components/UIImage.hpp"
#include "Engine/Scene/Components/UIText.hpp"
#include "Engine/Core/Logger.hpp"
#include <algorithm>
#include <cmath>

namespace fbzz::scene {

namespace {

float ApplyEasing(float t, UIEasingType easing)
{
    switch (easing) {
    case UIEasingType::EaseIn:    return t * t;
    case UIEasingType::EaseOut:   return 1.0f - (1.0f - t) * (1.0f - t);
    case UIEasingType::EaseInOut: return t < 0.5f ? 2.0f * t * t : 1.0f - 2.0f * (1.0f - t) * (1.0f - t);
    default: return t; // 線形
    }
}

math::Vector4 LerpV4(const math::Vector4& a, const math::Vector4& b, float t)
{
    return { a.x + (b.x - a.x) * t,
             a.y + (b.y - a.y) * t,
             a.z + (b.z - a.z) * t,
             a.w + (b.w - a.w) * t };
}

math::Vector2 LerpV2(const math::Vector2& a, const math::Vector2& b, float t)
{
    return { a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t };
}

// active のまま elapsed をラップしたとき didWrap = true を返す。
// WHY: fmod の結果が厳密に 0.0f にならない浮動小数点誤差を回避するため、
//      pingPong の折り返し検出を elapsed の値比較ではなくフラグで行う。
void AdvanceTween(float& elapsed, float duration, bool loop, float dt, bool& active, bool& didWrap)
{
    didWrap = false;
    if (!active) return;
    elapsed += dt;
    if (elapsed >= duration) {
        if (loop) {
            elapsed = std::fmod(elapsed, duration);
            didWrap = true;
        } else {
            elapsed = duration;
            active  = false;
        }
    }
}

void ProcessGO(GameObject& go, float dt)
{
    if (!go.activeInHierarchy()) return;

    auto* anim  = go.GetComponent<UIAnimator>();
    auto* image = go.GetComponent<UIImage>();
    auto* text  = go.GetComponent<UIText>();

    if (anim && anim->enabled) {
        // 色 Tween: UIImage または UIText が必要
        UIColorTween& ct = anim->colorTween;
        if (ct.active) {
            if (!image && !text) {
                static bool s_warnedColor = false;
                if (!s_warnedColor) {
                    FBZZ_LOG_WARN("UIAnimatorSystem: colorTween is active but no UIImage/UIText found on '%s'",
                                  go.name.c_str());
                    s_warnedColor = true;
                }
            } else {
                bool didWrap = false;
                AdvanceTween(ct.elapsed, ct.duration, ct.loop, dt, ct.active, didWrap);
                float t = (ct.duration > 0.0f) ? std::clamp(ct.elapsed / ct.duration, 0.0f, 1.0f) : 1.0f;
                t = ApplyEasing(t, ct.easing);
                const math::Vector4 col = LerpV4(ct.from, ct.to, t);
                if (image && image->enabled) image->color = col;
                if (text  && text->enabled)  text->color  = col;
                if (ct.loop && ct.pingPong && didWrap)
                    std::swap(ct.from, ct.to);
            }
        }

        // 位置 Tween: UIImage の有無に依存しない
        UIPositionTween& pt = anim->positionTween;
        if (pt.active) {
            bool didWrap = false;
            AdvanceTween(pt.elapsed, pt.duration, pt.loop, dt, pt.active, didWrap);
            float t = (pt.duration > 0.0f) ? std::clamp(pt.elapsed / pt.duration, 0.0f, 1.0f) : 1.0f;
            t = ApplyEasing(t, pt.easing);
            const math::Vector2 posVal = LerpV2(pt.from, pt.to, t);
            go.transform.position.x = posVal.x;
            go.transform.position.y = posVal.y;
            if (pt.loop && pt.pingPong && didWrap)
                std::swap(pt.from, pt.to);
        }

        // スケール Tween
        UIScaleTween& st = anim->scaleTween;
        if (st.active) {
            bool didWrap = false;
            AdvanceTween(st.elapsed, st.duration, st.loop, dt, st.active, didWrap);
            float t = (st.duration > 0.0f) ? std::clamp(st.elapsed / st.duration, 0.0f, 1.0f) : 1.0f;
            t = ApplyEasing(t, st.easing);
            const math::Vector2 scaleVal = LerpV2(st.from, st.to, t);
            go.transform.scale.x = scaleVal.x;
            go.transform.scale.y = scaleVal.y;
            if (st.loop && st.pingPong && didWrap)
                std::swap(st.from, st.to);
        }
    }

    for (int i = 0; i < go.GetChildCount(); ++i) {
        if (GameObject* child = go.GetChild(i))
            ProcessGO(*child, dt);
    }
}

} // namespace

void UIAnimatorSystem(Scene& scene, float deltaTime)
{
    for (GameObject* root : scene.GetRootGameObjects()) {
        if (root) ProcessGO(*root, deltaTime);
    }
}

} // namespace fbzz::scene
