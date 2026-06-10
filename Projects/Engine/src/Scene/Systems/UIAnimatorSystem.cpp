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

void AdvanceTween(float& elapsed, float duration, bool loop, bool pingPong, float dt, bool& active)
{
    if (!active) return;
    elapsed += dt;
    if (elapsed >= duration) {
        if (loop) {
            elapsed = std::fmod(elapsed, duration);
            if (pingPong) {
                // from / to の入れ替えは呼び出し側で行う
            }
        } else {
            elapsed = duration;
            active = false;
        }
    }
}

void ProcessGO(GameObject& go, float dt)
{
    if (!go.activeSelf()) return;

    auto* anim  = go.GetComponent<UIAnimator>();
    auto* image = go.GetComponent<UIImage>();

    if (anim && anim->enabled && image && image->enabled) {
        // 色 Tween
        UIColorTween& ct = anim->colorTween;
        if (ct.active) {
            AdvanceTween(ct.elapsed, ct.duration, ct.loop, ct.pingPong, dt, ct.active);
            float t = (ct.duration > 0.0f) ? std::clamp(ct.elapsed / ct.duration, 0.0f, 1.0f) : 1.0f;
            t = ApplyEasing(t, ct.easing);
            image->color = LerpV4(ct.from, ct.to, t);
            if (ct.loop && ct.pingPong && ct.elapsed == 0.0f)
                std::swap(ct.from, ct.to); // 次のループで方向を反転する
        }

        // 位置 Tween
        UIPositionTween& pt = anim->positionTween;
        if (pt.active) {
            AdvanceTween(pt.elapsed, pt.duration, pt.loop, pt.pingPong, dt, pt.active);
            float t = (pt.duration > 0.0f) ? std::clamp(pt.elapsed / pt.duration, 0.0f, 1.0f) : 1.0f;
            t = ApplyEasing(t, pt.easing);
            auto posVal = LerpV2(pt.from, pt.to, t);
            go.transform.position.x = posVal.x;
            go.transform.position.y = posVal.y;
            if (pt.loop && pt.pingPong && pt.elapsed == 0.0f)
                std::swap(pt.from, pt.to);
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
