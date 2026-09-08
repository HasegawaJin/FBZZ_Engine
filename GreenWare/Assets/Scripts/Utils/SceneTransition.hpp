/// @file    SceneTransition.hpp
/// @brief   シーン遷移の «扉»。マスクで塗って切り替え、次のシーンで剥がす
/// @author  Hasegawa Jin
/// @date    2026-09-07
///
/// 使い方:
///   transition::Begin(targetScene)          … 塗り (出) を始める。塗り切ったら LoadScene
///   transition::Tick(dt, scene)            … 毎フレーム 1 回。誰かが進める (下記)
///   transition::PushWipe(pp)               … 今フレームの覆いを PostProcessSettings へ載せる
///
/// WHY 静的な状態で持つか:
///   遷移は «シーンをまたぐ» 出来事で、塗った側のスクリプトは LoadScene で消える。
///   剥がすのは次のシーンの誰かで、その誰かが «剥がすべき覆いがある» ことを知るには、
///   シーンに属さない場所に置くしかない (SceneManagerScript の s_fadeIn と同じ理由)。
///
/// WHY 進める人と描く人を分けるか:
///   ランタイムの PostProcessSettings は «まるごと差し替え» の器で、書き手は 1 人でないと
///   互いを消す (ScreenEffectManagerComponent のヘッダー)。遊びのシーンでは
///   ScreenEffectManager が書き手なので、遷移はそこへ «載せてもらう» (PushWipe)。
///   マネージャーが居ないメニューでは SceneManagerScript が自分で書く。
///   どちらのシーンでも進めるのは Tick で、同じフレームに 2 人が呼んでも 1 回しか進まない。
///
/// WHY 黒の lerp (screenFadeAlpha) をやめたか: ScreenWipe.hlsl のヘッダー。
#pragma once

#include <Engine/Core/Time.hpp>
#include <Engine/Renderer/RenderSettings.hpp>
#include <Engine/Scene/ScriptProxy/ScriptPostProcessProxy.hpp>
#include <Engine/Scene/ScriptProxy/ScriptSceneProxy.hpp>
#include <Math/MathUtils.hpp>
#include <Math/Vector4.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace sandbox::transition {

inline constexpr const char* kWipeEffectName   = "SceneWipe";
inline constexpr const char* kWipeShaderPath   = "assets/shaders/PostProcess/Custom/ScreenWipe.hlsl";
inline constexpr const char* kWipeMaterialPath = "Assets/Materials/PostProcess/PP_ScreenWipe.mat";

/// 見え方。シーンごとに変えたければ Begin の前に Style() を書く。
struct Style {
    float   outSeconds = 0.55f;   ///< 塗り切るまで
    float   inSeconds  = 0.65f;   ///< 剥がし切るまで
    float   hold       = 0.08f;   ///< 塗り切ってから切り替えるまでの間 (真っ黒を 1 コマ見せる)
    float   softness   = 0.035f;  ///< 縁の柔らかさ (マスク値の幅)
    float   glowWidth  = 0.06f;   ///< 縁の光の太さ (マスク値の幅)
    float   glowGain   = 1.4f;    ///< 縁の光の強さ
    float   rotationIn = 3.14159f;///< 剥がすときのマスクの回転。出と逆向きに走らせる
    fbzz::math::Vector4 glow { 0.35f, 1.00f, 0.55f, 1.0f };    ///< 縁の光 (プレイヤー色)。塗る色はシェーダー側で固定
};

enum class Phase { Idle, Out, Hold, In };

struct State {
    Phase       phase   = Phase::Idle;
    float       cover   = 0.0f;   ///< 0..1。1 で全面
    float       elapsed = 0.0f;
    std::string target;
    /// 直前に LoadScene が失敗した行き先。同じ所へは «出来なかった» と返し続ける。
    ///
    /// WHY 覚えるか: 失敗しても扉は Idle へ戻るだけなので、呼ぶ側の
    ///     `if (transition::Active()) return;` が翌フレームには通り、また塗り始める。
    ///     Begin が true を返している限り呼ぶ側は «始まった» と思っているので、
    ///     **扉が延々と閉じては開くのに画面には何も出ない** ─ 一番追いにくい形になる。
    ///     ここで覚えて false を返せば、各所の «読み込めませんでした» の分岐が
    ///     書いた人の意図どおりに効く (それまでは 6 か所とも到達しない死んだ枝だった)。
    std::string failedTarget;
    Style       style;
    std::uint64_t tickedFrame = ~std::uint64_t{ 0 };
    bool        loadRequested = false;
};

inline State& Mutable()
{
    static State state;
    return state;
}

inline Style& StyleRef() { return Mutable().style; }

[[nodiscard]] inline bool  Active()   { return Mutable().phase != Phase::Idle; }
[[nodiscard]] inline float Coverage() { return Mutable().cover; }
/// 塗り切って切り替えを待っている / 剥がしている最中。入力を受けない目安。
[[nodiscard]] inline bool  Busy()     { return Mutable().phase == Phase::Out || Mutable().phase == Phase::Hold; }

/// 塗り始める。既に塗っている最中なら無視 (二重発火の防止はここが持つ)。
/// 直前に読み込めなかった行き先も false で返す (State::failedTarget の WHY)。
inline bool Begin(const std::string& targetScene)
{
    State& s = Mutable();
    if (targetScene.empty()) return false;
    if (s.phase == Phase::Out || s.phase == Phase::Hold) return false;
    if (!s.failedTarget.empty() && s.failedTarget == targetScene) return false;

    s.phase = Phase::Out;
    // 剥がしている途中 (Phase::In) から塗り直すことがある ─ Busy() は In では
    // false なので、扉が開きかけの間もメニューは押せる。elapsed だけ 0 に戻すと
    // 次の Tick で cover が ease(0) = 0 へ飛び、**扉が 1 コマ全開してから閉じ直す**。
    // 今の覆いに対応する時刻から始めれば、開きかけの位置からそのまま閉じる。
    const float cover = fbzz::math::Clamp01(s.cover);
    s.elapsed = (1.0f - std::sqrt(std::max(1.0f - cover, 0.0f)))
              * std::max(s.style.outSeconds, 0.01f);
    s.cover         = cover;
    s.target        = targetScene;
    s.loadRequested = false;
    return true;
}

/// 新しいシーンの頭で «剥がす» へ入る。LoadScene の直前に Tick が立てるので、
/// 呼ぶ側は何もしなくてよい (次のシーンの Tick が自然に剥がす)。
inline void BeginIn()
{
    State& s = Mutable();
    s.phase   = Phase::In;
    s.cover   = 1.0f;
    s.elapsed = 0.0f;
    s.failedTarget.clear();
}

/// 毎フレーム 1 回進める。同じフレームに何度呼ばれても 1 回しか進まない。
/// dt は実時間。塗り切った瞬間に LoadScene を呼ぶ。
inline void Tick(float dt, const fbzz::scene::ScriptSceneProxy& scene)
{
    State& s = Mutable();
    if (s.phase == Phase::Idle) return;
    if (s.tickedFrame == fbzz::Time::frameCount) return;
    s.tickedFrame = fbzz::Time::frameCount;

    dt = std::max(dt, 0.0f);
    const Style& st = s.style;

    // 進みは «速く入って緩く止まる»。等速だと機械が塗っているように見える。
    const auto ease = [](float t) { return 1.0f - (1.0f - t) * (1.0f - t); };

    switch (s.phase) {
    case Phase::Out: {
        s.elapsed += dt;
        const float t = fbzz::math::Clamp01(s.elapsed / std::max(st.outSeconds, 0.01f));
        s.cover = ease(t);
        if (t >= 1.0f) {
            s.cover   = 1.0f;
            s.phase   = Phase::Hold;
            s.elapsed = 0.0f;
        }
        break;
    }
    case Phase::Hold: {
        s.elapsed += dt;
        if (s.elapsed < st.hold) break;
        if (!s.loadRequested) {
            s.loadRequested = true;
            // 次のシーンの Tick が剥がす。LoadScene が失敗しても覆いを残さない。
            const std::string target = s.target;
            BeginIn();
            if (!scene.LoadScene(target)) {
                s.phase        = Phase::Idle;
                s.cover        = 0.0f;
                // 覚えておく。次に同じ所を頼まれたら Begin が false を返し、
                // 呼ぶ側の «読み込めませんでした» が出る (それまでは無言で回り続けた)。
                s.failedTarget = target;
            }
        }
        break;
    }
    case Phase::In: {
        s.elapsed += dt;
        const float t = fbzz::math::Clamp01(s.elapsed / std::max(st.inSeconds, 0.01f));
        s.cover = 1.0f - ease(t);
        if (t >= 1.0f) {
            s.cover = 0.0f;
            s.phase = Phase::Idle;
        }
        break;
    }
    default: break;
    }
}

/// 今フレームの覆いをカスタムパスとして載せる。覆いが 0 なら何もしない。
inline void PushWipe(fbzz::renderer::PostProcessSettings& pp)
{
    const State& s = Mutable();
    if (s.cover <= 0.0f) return;
    const Style& st = s.style;

    auto& custom = pp.customEffects.emplace_back();
    custom.name          = kWipeEffectName;
    custom.enabled       = true;
    custom.shaderPath    = kWipeShaderPath;
    custom.materialPath  = kWipeMaterialPath;
    custom.intensity     = 1.0f;
    custom.blend         = 1.0f;
    custom.parameters[0] = s.cover;
    custom.parameters[1] = st.softness;
    custom.parameters[2] = st.glowWidth;
    custom.parameters[3] = s.phase == Phase::In ? st.rotationIn : 0.0f;
    custom.parameters[4] = st.glow.x;
    custom.parameters[5] = st.glow.y;
    custom.parameters[6] = st.glow.z;
    custom.parameters[7] = st.glowGain;
}


/// マネージャー (ScreenEffectManagerComponent) の居ないシーンで、1 つのスクリプトが
/// «進めて描く» ための口。OnUpdate の頭で毎フレーム呼ぶ。
///
/// @param hasManager  マネージャーが居るなら true。描くのはあちらに任せ、進めるだけにする
/// @param writing     このスクリプトが postprocess を書いたかの札 (呼ぶ側が持つ)
/// @return 扉が動いていて、入力を受けるべきでないなら true
inline bool Drive(float unscaledDt, const fbzz::scene::ScriptSceneProxy& scene,
                  const fbzz::scene::ScriptPostProcessProxy& postprocess,
                  bool& writing, bool hasManager)
{
    Tick(unscaledDt, scene);
    if (!hasManager) {
        const float cover = Coverage();
        if (cover <= 0.0f) {
            if (writing) {
                postprocess.Clear();
                writing = false;
            }
        } else {
            // 既存のランタイム設定 (自分が書いたものでなければ) を引き継いで扉だけ載せる。
            fbzz::renderer::PostProcessSettings pp =
                (!writing && postprocess.TryGet()) ? *postprocess.TryGet()
                                                   : fbzz::renderer::PostProcessSettings{};
            // 前フレームに誰かが載せた扉は捨てて、今フレームの覆いで載せ直す。
            std::erase_if(pp.customEffects,
                          [](const fbzz::renderer::CustomPostProcessSettings& e) {
                              return e.name == kWipeEffectName;
                          });
            PushWipe(pp);
            postprocess.Set(pp);
            writing = true;
        }
    }
    return Busy();
}

} // namespace sandbox::transition
