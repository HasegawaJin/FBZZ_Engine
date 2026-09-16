/// @file    UIAnimatorSystem.cpp
/// @brief   UIAnimator の Tween 評価。
/// @author  Hasegawa Jin
/// @date    2026-05-23
///
/// 時間経過に応じて UIImage の色と Transform の位置を更新する。
/// UISystem より前に呼ぶことで描画へ反映される。
#include "Engine/Scene/Systems/UIAnimatorSystem.hpp"
#include "Engine/Core/Scheduler/SystemContext.hpp"
#include "Engine/Scene/Systems/TransformSystem.hpp"
#include "Engine/Scene/Scene.hpp"
#include "Engine/Scene/GameObject.hpp"
#include "Engine/Scene/Components/UIAnimator.hpp"
#include "Engine/Scene/Components/UIImage.hpp"
#include "Engine/Scene/Components/UIText.hpp"
#include "Engine/Core/Logger.hpp"
#include "Math/Quaternion.hpp"
#include "Math/Vector3.hpp"
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

// 数値トゥイーンを 1 段進め、この瞬間の値を返す。active でなければ書かない。
bool EvaluateFloatTween(UIFloatTween& tween, float dt, float& out)
{
    if (!tween.active) return false;
    bool didWrap = false;
    AdvanceTween(tween.elapsed, tween.duration, tween.loop, dt, tween.active, didWrap);
    float t = (tween.duration > 0.0f)
        ? std::clamp(tween.elapsed / tween.duration, 0.0f, 1.0f) : 1.0f;
    t = ApplyEasing(t, tween.easing);
    out = tween.from + (tween.to - tween.from) * t;
    if (tween.loop && tween.pingPong && didWrap)
        std::swap(tween.from, tween.to);
    return true;
}

void ProcessGO(GameObject& go, float dt)
{
    if (!go.activeInHierarchy()) return;

    auto* anim  = go.GetComponent<UIAnimator>();
    auto* image = go.GetComponent<UIImage>();
    auto* text  = go.GetComponent<UIText>();

    if (anim && anim->enabled) {
        // 待ちは全トゥイーン共通の前段。待っている間は 1 つも進めない。
        //
        // WHY 個別に持たせないか: 「0.1 秒後に色と位置を同時に動かす」が普通の
        //     使い方で、トゥイーンごとに待ちを入れると必ずどれかがずれる。
        if (anim->delayElapsed < anim->delay) {
            anim->delayElapsed += dt;
            if (anim->delayElapsed < anim->delay) {
                for (int i = 0; i < go.GetChildCount(); ++i)
                    if (GameObject* child = go.GetChild(i)) ProcessGO(*child, dt);
                return;
            }
        }
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

        // 回転 Tween: 度で持ち、Z 軸だけを回す。
        // WHY クォータニオンで持たないか: UI の回転は必ず画面に平行な 1 軸で、
        //     オイラー角の曖昧さが起きない。度のまま補間するほうが往復も素直。
        if (float degrees = 0.0f; EvaluateFloatTween(anim->rotationTween, dt, degrees)) {
            constexpr float kDegToRad = 3.14159265358979323846f / 180.0f;
            go.transform.rotation =
                math::Quaternion::FromAxisAngle(math::Vector3{ 0.0f, 0.0f, 1.0f },
                                                degrees * kDegToRad);
        }

        // 塗り潰し Tween: クールダウンや充填の演出。
        if (float fill = 0.0f; EvaluateFloatTween(anim->fillTween, dt, fill)) {
            if (image) image->fillAmount = std::clamp(fill, 0.0f, 1.0f);
        }

        // マテリアルの 1 パラメータ。要素ごとの上書きへ書くので、同じ .mat を
        // 使う他の要素へは波及しない (UIImage.hpp の materialParamOverrides を参照)。
        if (float param = 0.0f; EvaluateFloatTween(anim->materialTween, dt, param)) {
            if (image && !anim->materialParam.empty()) {
                auto& values = image->materialParamOverrides[anim->materialParam];
                values.assign(1, param);
            }
        }
    }

    for (int i = 0; i < go.GetChildCount(); ++i) {
        if (GameObject* child = go.GetChild(i))
            ProcessGO(*child, dt);
    }
}

} // namespace

ComponentAccess UIAnimatorSystem::GetAccess() const
{
    return ComponentAccess{}
        .Reads<UIAnimator>()
        .Writes<UIAnimator, UIImage, UIText>();
}

OrderingHints UIAnimatorSystem::GetOrder() const
{
    return OrderingHints{}.Before<TransformLateUpdate>();
}

void UIAnimatorSystem::Update(SystemContext& ctx)
{
    for (GameObject* root : ctx.scene.GetRootGameObjects()) {
        if (root) ProcessGO(*root, ctx.dt);
    }
}

} // namespace fbzz::scene
