/// @file    SceneManagerScript.hpp
/// @brief   ボタンクリック / 自動タイマーによるシーン遷移ユーティリティスクリプト。
/// @author  Hasegawa Jin
/// @date    2026-06-22
///
/// 使い方:
/// [ボタン GO に付ける場合]
/// targetScene : 遷移先シーン名
/// viaScene    : 中継シーン名 (例: "Load")。空の場合は直接遷移。
///
/// [Load.scene のコントローラー GO に付ける場合]
/// autoTransition = true
/// autoDelay      = 遷移までの秒数 (フェードイン完了後にカウント開始)
/// targetScene    は OnStart で s_next から自動セットされる。
///
/// フェード動作:
/// fadeEnabled=true のとき、遷移前に黒フェードアウト、遷移先シーンで黒フェードインを行う。
/// fadeDuration でフェードイン/アウト各々の秒数を制御する。
/// シーン間の状態受け渡しは s_fadeIn 静的変数で行う。
///
/// WHY: シーンごとに個別コントローラーを作らず、1 スクリプトで全遷移パターンを賄う。
/// ポストプロセスの screenFadeAlpha を使うことで UI に依存せず真の最終レイヤーでフェードできる。
#pragma once

#include <Engine/Renderer/RenderSettings.hpp>
#include <Engine/Scene/Components/UIButton.hpp>
#include <Engine/Scene/Script.hpp>
#include <algorithm>
#include <string>

using namespace fbzz::scene;
using fbzz::Time;

namespace sandbox {

class SceneManagerScript : public Script {
    FBZZ_SCRIPT(SceneManagerScript)
public:
    // 遷移先シーン名。autoTransition=true の場合は OnStart で s_next から上書きされる。
    FBZZ_FIELD(std::string, targetScene,    "", "Target Scene")
    // 中継シーン名。非空のとき s_next=targetScene を設定してからこのシーンをロードする。
    FBZZ_FIELD(std::string, viaScene,       "", "Via Scene")
    // true のとき autoDelay 秒後に targetScene へ自動遷移 (Load.scene 用)
    FBZZ_FIELD(bool,  autoTransition, false, "Auto Transition")
    FBZZ_FIELD(float, autoDelay,      1.5f,  "Auto Delay")
    // フェードの有効/無効とフェードイン・アウト各々の長さ (秒)
    FBZZ_FIELD(bool,  fadeEnabled,    true,  "Fade Enabled")
    FBZZ_FIELD(float, fadeDuration,   0.5f,  "Fade Duration")

    // シーンをまたいで「次シーンはフェードインで開始」を伝達する静的変数群。
    // WHY: LoadScene でシーンが切り替わると現スクリプトも破棄されるため、
    //      次シーンの OnStart が読める静的変数でフェード状態を引き継ぐ。
    static inline std::string s_next;
    static inline bool        s_fadeIn = false;

    void OnStart()  override;
    void OnUpdate() override;

private:
    // フェードアウト開始。fadeEnabled=false なら即 ExecuteLoad() へ。
    void BeginFadeOut();
    // LoadScene を実際に呼ぶ。viaScene がある場合は中継シーンを経由する。
    void ExecuteLoad();
    // 現在の m_fadeAlpha を postprocess に書き込む。
    void ApplyFade();

    enum class FadeState { Idle, FadeOut, FadeIn };

    UIButton*  m_btn       = nullptr;
    float      m_elapsed   = 0.0f;
    float      m_fadeAlpha = 0.0f;
    FadeState  m_fadeState = FadeState::Idle;
    bool       m_fired     = false; // ボタン / タイマー二重発火防止
};

FBZZ_REFLECT(SceneManagerScript)

// ── 実装 (inline) ─────────────────────────────────────────────────────────────
inline void SceneManagerScript::OnStart()
{
    // 前シーンがフェードアウトして遷移してきた場合、黒から始めてフェードインする。
    if (s_fadeIn && fadeEnabled) {
        m_fadeAlpha = 1.0f;
        m_fadeState = FadeState::FadeIn;
        s_fadeIn    = false;
        ApplyFade();
    }

    if (autoTransition) {
        // Load.scene モード: 静的変数から遷移先を受け取る
        if (!s_next.empty())
            targetScene = s_next;
    } else {
        // ボタンモード: 同 GO の UIButton をキャッシュ
        m_btn = scene.GetComponent<UIButton>();
    }
}

inline void SceneManagerScript::OnUpdate()
{
    // フェードイン: alpha 1→0
    if (m_fadeState == FadeState::FadeIn) {
        m_fadeAlpha -= Time::deltaTime / std::max(fadeDuration, 0.01f);
        if (m_fadeAlpha <= 0.0f) {
            m_fadeAlpha = 0.0f;
            m_fadeState = FadeState::Idle;
            postprocess.Clear();
        } else {
            ApplyFade();
        }
        return;
    }

    // フェードアウト: alpha 0→1, 完了後 LoadScene
    if (m_fadeState == FadeState::FadeOut) {
        m_fadeAlpha += Time::deltaTime / std::max(fadeDuration, 0.01f);
        if (m_fadeAlpha >= 1.0f) {
            m_fadeAlpha = 1.0f;
            ApplyFade();
            m_fadeState = FadeState::Idle; // 再入防止 (SceneManager は次フレームで切り替える)
            s_fadeIn    = fadeEnabled;
            ExecuteLoad();
        } else {
            ApplyFade();
        }
        return;
    }

    if (m_fired) return;

    if (autoTransition) {
        m_elapsed += Time::deltaTime;
        if (m_elapsed >= autoDelay && !targetScene.empty()) {
            m_fired = true;
            BeginFadeOut();
        }
        return;
    }

    if (!m_btn || !m_btn->onClick) return;
    m_fired = true;
    BeginFadeOut();
}

inline void SceneManagerScript::ApplyFade()
{
    // 既存のランタイム PostProcess 設定を引き継ぎ、screenFadeAlpha だけ上書きする。
    // WHY: ブルームや被写界深度など他のエフェクトを消さずにフェードだけを重ねるため。
    fbzz::renderer::PostProcessSettings pp =
        postprocess.TryGet() ? *postprocess.TryGet() : fbzz::renderer::PostProcessSettings{};
    pp.screenFadeAlpha    = m_fadeAlpha;
    pp.screenFadeColor[0] = 0.0f; // 黒フェード
    pp.screenFadeColor[1] = 0.0f;
    pp.screenFadeColor[2] = 0.0f;
    postprocess.Set(pp);
}

inline void SceneManagerScript::BeginFadeOut()
{
    if (!fadeEnabled) {
        // フェードなし: 即座に遷移
        s_fadeIn = false;
        ExecuteLoad();
        return;
    }
    m_fadeAlpha = 0.0f;
    m_fadeState = FadeState::FadeOut;
}

inline void SceneManagerScript::ExecuteLoad()
{
    if (!viaScene.empty()) {
        // 中継シーンを経由する場合: 目的地を静的変数に保存してから中継シーンへ
        s_next = targetScene;
        scene.LoadScene(viaScene);
    } else {
        scene.LoadScene(targetScene);
    }
}

} // namespace sandbox
