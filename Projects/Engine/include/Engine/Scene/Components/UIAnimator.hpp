/// @file    UIAnimator.hpp
/// @brief   UIImage 向け Tween アニメーションコンポーネント。
/// @author  Hasegawa Jin
/// @date    2026-05-23
///
/// 色と位置の変化を時間で補間し、UIAnimatorSystem が結果を書き込む。
/// UI 表示そのものは UISystem に委譲する。
#pragma once
#include <Engine/Scene/Script.hpp>
#include <Math/Vector2.hpp>
#include <Math/Vector4.hpp>
#include <cstdio>
#include <string>

namespace fbzz::scene {

enum class UIEasingType { Linear, EaseIn, EaseOut, EaseInOut };

struct UIColorTween {
    math::Vector4 from     = { 1, 1, 1, 1 };
    math::Vector4 to       = { 1, 1, 1, 0 };
    float         duration = 1.0f;
    float         elapsed  = 0.0f;
    UIEasingType  easing   = UIEasingType::Linear;
    bool          loop     = false;
    /// @brief ループごとに from/to を反転する。往復アニメーションに使う。
    /// @note loop との違い: loop は終端でリセット、pingPong は折り返す。
    bool          pingPong = false;
    bool          active   = false; ///< false のとき UIAnimatorSystem はこのトゥイーンをスキップする
};

struct UIPositionTween {
    math::Vector2 from     = { 0, 0 };
    math::Vector2 to       = { 100, 0 };
    float         duration = 1.0f;
    float         elapsed  = 0.0f;
    UIEasingType  easing   = UIEasingType::Linear;
    bool          loop     = false;
    bool          pingPong = false;
    bool          active   = false;
};

struct UIScaleTween {
    math::Vector2 from     = { 1, 1 };
    math::Vector2 to       = { 1.2f, 1.2f };
    float         duration = 0.3f;
    float         elapsed  = 0.0f;
    UIEasingType  easing   = UIEasingType::EaseOut;
    bool          loop     = false;
    bool          pingPong = false;
    bool          active   = false;
};

/// @brief 1 つの数値を動かすトゥイーン。回転・塗り潰し量・マテリアルの 1 パラメータが使う。
/// @note 既存 3 種は Reflect でフィールド名を平坦に並べており (colorFrom / posFrom …)、1 つの
///       テンプレートへまとめるとシーンに保存済みのキー名が変わって既存シーンが壊れるため、
///       型ごとに分けたまま数値版だけ追加している。
struct UIFloatTween {
    float        from     = 0.0f;
    float        to       = 1.0f;
    float        duration = 0.3f;
    float        elapsed  = 0.0f;
    UIEasingType easing   = UIEasingType::Linear;
    bool         loop     = false;
    bool         pingPong = false;
    bool         active   = false;
};

struct UIAnimator {
    UIColorTween    colorTween;
    UIPositionTween positionTween;
    UIScaleTween    scaleTween;
    /// @brief 度。UIAnimatorSystem が transform.rotation の Z へ書く。
    UIFloatTween    rotationTween;
    /// @brief UIImage.fillAmount [0,1]。クールダウンや充填の演出に使う。
    UIFloatTween    fillTween;
    /// @brief UIImage の要素ごとマテリアル上書き 1 つ。materialParam が空なら何もしない。
    /// @note 複数を同時に動かしたい場面は .mat 側で 1 つの進行度から派生させたほうが破綻しない
    ///       (色と縁と発光がばらばらに進むと調整が指数的に増える)。入口は 1 本に絞る。
    UIFloatTween    materialTween;
    std::string     materialParam;

    /// @brief 再生開始までの待ち (秒)。全トゥイーン共通。
    /// @note 連結ではなく待ちにする理由: 「0.1 秒後に出る」のように絶対時刻で指定したい場面が
    ///       多い。待ちだけあれば、同時再生も段差も作れる。
    float           delay   = 0.0f;
    /// @note ランタイム専用: delay の消化量。
    float           delayElapsed = 0.0f;

    bool            enabled = true;

    /// @brief Tween 再生 API。ScriptProxy / Inspector ボタンの両方から同じ入口を使う。
    /// @note elapsed と active の初期化規則を呼び出し側へ分散させると、途中再生・ループ・
    ///       pingPong の挙動が API ごとにずれるため、ここへ集約する。
    void PlayColor(const math::Vector4& from, const math::Vector4& to,
                   float duration, UIEasingType easing = UIEasingType::Linear,
                   bool loop = false, bool pingPong = false)
    {
        colorTween.from = from;
        colorTween.to = to;
        colorTween.duration = duration;
        colorTween.elapsed = 0.0f;
        colorTween.easing = easing;
        colorTween.loop = loop;
        colorTween.pingPong = pingPong;
        colorTween.active = true;
        enabled = true;
    }

    void PlayPosition(const math::Vector2& from, const math::Vector2& to,
                      float duration, UIEasingType easing = UIEasingType::Linear,
                      bool loop = false, bool pingPong = false)
    {
        positionTween.from = from;
        positionTween.to = to;
        positionTween.duration = duration;
        positionTween.elapsed = 0.0f;
        positionTween.easing = easing;
        positionTween.loop = loop;
        positionTween.pingPong = pingPong;
        positionTween.active = true;
        enabled = true;
    }

    void PlayScale(const math::Vector2& from, const math::Vector2& to,
                   float duration, UIEasingType easing = UIEasingType::Linear,
                   bool loop = false, bool pingPong = false)
    {
        scaleTween.from = from;
        scaleTween.to = to;
        scaleTween.duration = duration;
        scaleTween.elapsed = 0.0f;
        scaleTween.easing = easing;
        scaleTween.loop = loop;
        scaleTween.pingPong = pingPong;
        scaleTween.active = true;
        enabled = true;
    }

    /// @brief 数値トゥイーンの共通の起こし方。回転・塗り潰し・マテリアルが同じ規則で動く。
    static void StartFloat(UIFloatTween& tween, float from, float to, float duration,
                           UIEasingType easing, bool loop, bool pingPong)
    {
        tween.from     = from;
        tween.to       = to;
        tween.duration = duration;
        tween.elapsed  = 0.0f;
        tween.easing   = easing;
        tween.loop     = loop;
        tween.pingPong = pingPong;
        tween.active   = true;
    }

    void PlayRotation(float fromDegrees, float toDegrees, float duration,
                      UIEasingType easing = UIEasingType::Linear,
                      bool loop = false, bool pingPong = false)
    {
        StartFloat(rotationTween, fromDegrees, toDegrees, duration, easing, loop, pingPong);
        enabled = true;
    }

    void PlayFill(float from, float to, float duration,
                  UIEasingType easing = UIEasingType::Linear,
                  bool loop = false, bool pingPong = false)
    {
        StartFloat(fillTween, from, to, duration, easing, loop, pingPong);
        enabled = true;
    }

    void PlayMaterialFloat(const std::string& param, float from, float to, float duration,
                           UIEasingType easing = UIEasingType::Linear,
                           bool loop = false, bool pingPong = false)
    {
        materialParam = param;
        StartFloat(materialTween, from, to, duration, easing, loop, pingPong);
        enabled = true;
    }

    /// @brief 次に起こすトゥイーンの開始を遅らせる。Play* より先に呼ぶこと。
    void SetDelay(float seconds)
    {
        delay        = seconds;
        delayElapsed = 0.0f;
    }

    void StopColor() { colorTween.active = false; }
    void StopPosition() { positionTween.active = false; }
    void StopScale() { scaleTween.active = false; }
    void StopRotation() { rotationTween.active = false; }
    void StopFill() { fillTween.active = false; }
    void StopMaterial() { materialTween.active = false; }
    void StopAll()
    {
        StopColor();
        StopPosition();
        StopScale();
        StopRotation();
        StopFill();
        StopMaterial();
    }

    bool IsPlaying() const
    {
        return enabled && (colorTween.active || positionTween.active || scaleTween.active
                        || rotationTween.active || fillTween.active || materialTween.active);
    }

    const char* GetTypeName() const { return "UIAnimator"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        /// @note Inspector で編集する Tween パラメーター。
        static constexpr const char* kEasingLabels[] = { "Linear", "Ease In", "Ease Out", "Ease In Out" };

        r.Group("Color Tween");
        r.ColorField("colorFrom",        colorTween.from);
        r.ColorField("colorTo",          colorTween.to);
        r.FloatRange("colorDuration", colorTween.duration, 0.01f, 60.0f);
        int colorEasing = static_cast<int>(colorTween.easing);
        r.Enum("colorEasing", colorEasing, kEasingLabels);
        colorEasing = (colorEasing < 0 || colorEasing > 3) ? 0 : colorEasing;
        colorTween.easing = static_cast<UIEasingType>(colorEasing);
        r.Field("colorLoop",        colorTween.loop);
        r.Field("colorPingPong",    colorTween.pingPong);
        r.Field("colorActive",      colorTween.active);

        r.Group("Position Tween");
        r.Field("posFrom",          positionTween.from);
        r.Field("posTo",            positionTween.to);
        r.FloatRange("posDuration", positionTween.duration, 0.01f, 60.0f);
        int posEasing = static_cast<int>(positionTween.easing);
        r.Enum("posEasing", posEasing, kEasingLabels);
        posEasing = (posEasing < 0 || posEasing > 3) ? 0 : posEasing;
        positionTween.easing = static_cast<UIEasingType>(posEasing);
        r.Field("posLoop",          positionTween.loop);
        r.Field("posPingPong",      positionTween.pingPong);
        r.Field("posActive",        positionTween.active);

        r.Group("Scale Tween");
        r.Field("scaleFrom",        scaleTween.from);
        r.Field("scaleTo",          scaleTween.to);
        r.FloatRange("scaleDuration", scaleTween.duration, 0.01f, 60.0f);
        int scaleEasing = static_cast<int>(scaleTween.easing);
        r.Enum("scaleEasing", scaleEasing, kEasingLabels);
        scaleEasing = (scaleEasing < 0 || scaleEasing > 3) ? 0 : scaleEasing;
        scaleTween.easing = static_cast<UIEasingType>(scaleEasing);
        r.Field("scaleLoop",        scaleTween.loop);
        r.Field("scalePingPong",    scaleTween.pingPong);
        r.Field("scaleActive",      scaleTween.active);

        /// @note 数値トゥイーン 3 種は項目の並びを上の 3 つと揃える。既存 3 種がフィールド名を
        ///       平坦に並べているため、ここだけ入れ子にすると保存キーの付け方が 2 種類になる。
        const auto reflectFloatTween = [&](const char* prefix, UIFloatTween& tween,
                                           float minDuration, float maxDuration) {
            char key[48];
            const auto name = [&](const char* suffix) -> const char* {
                std::snprintf(key, sizeof(key), "%s%s", prefix, suffix);
                return key;
            };
            r.Field(name("From"), tween.from);
            r.Field(name("To"),   tween.to);
            r.FloatRange(name("Duration"), tween.duration, minDuration, maxDuration);
            int easing = static_cast<int>(tween.easing);
            r.Enum(name("Easing"), easing, kEasingLabels);
            easing = (easing < 0 || easing > 3) ? 0 : easing;
            tween.easing = static_cast<UIEasingType>(easing);
            r.Field(name("Loop"),     tween.loop);
            r.Field(name("PingPong"), tween.pingPong);
            r.Field(name("Active"),   tween.active);
        };

        r.Group("Rotation Tween");
        reflectFloatTween("rot", rotationTween, 0.01f, 60.0f);
        r.Tooltip("度。Z 軸まわりだけを回します");

        r.Group("Fill Tween");
        reflectFloatTween("fill", fillTween, 0.01f, 60.0f);
        r.Tooltip("同じ GameObject の UIImage.fillAmount を動かします");

        r.Group("Material Tween");
        r.Field("materialParam", materialParam);
        r.Tooltip("動かす .mat のパラメータ名。空欄なら何もしません。"
                  "値は要素ごとの上書きへ入るので、同じ .mat の他の要素には波及しません");
        reflectFloatTween("mat", materialTween, 0.01f, 60.0f);

        r.Group("Timing");
        r.FloatRange("delay", delay, 0.0f, 60.0f);
        r.Tooltip("再生開始までの待ち (秒)。全トゥイーン共通");
    }
};

} // namespace fbzz::scene
