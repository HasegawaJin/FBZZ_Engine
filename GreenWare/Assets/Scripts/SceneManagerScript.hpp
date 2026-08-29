/// @file    SceneManagerScript.hpp
/// @brief   ボタンクリック / 自動タイマーによるシーン遷移ユーティリティスクリプト。
/// @author  Hasegawa Jin
/// @date    2026-08-12
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
#include <Scripts/Game/ScreenEffectManagerComponent.hpp>
#include <Engine/Scene/Script.hpp>
#include <algorithm>
#include <string>

using namespace fbzz::scene;
using fbzz::Time;

namespace sandbox {

class SceneManagerScript : public Script {
    FBZZ_SCRIPT(SceneManagerScript)

    // ボタンモードでのみ UIButton を読む (autoTransition=true の Load.scene 用途では不要)。
    // 「どちらの使い方でも成立する」ので必須にはできない。付け忘れたときに
    // 気付けるよう、任意として名前だけ出しておく。
    FBZZ_OPTIONAL_COMPONENT(UIButton)

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
    // ボタンモードで、クリックの代わりにこのアクションでも遷移させる。
    //
    // WHY フォーカス移動ではなくボタンごとに割り当てるか:
    //   UIButton はマウス座標で判定するため、パッドだけでは 1 つも押せない。
    //   本作のメニューはタイトルに 1 つ、リザルトに 2 つしかないので、
    //   カーソル移動と選択状態を作り込むより「A で再挑戦 / B でタイトル」と
    //   ボタンへ直接割り当てる方が、実装も操作も短くなる。
    //   空欄ならこれまでどおりクリックのみ。
    FBZZ_FIELD(std::string, triggerAction, "", "Trigger Action")
    FBZZ_TOOLTIP("ProjectSettings/Input.inputactions のアクション名。"
                 "Submit / Cancel を割り当てるとパッドとキーボードで押せるようになる")

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
        }
        // WHY 0 でも ApplyFade を通すか: マネージャーが居るシーンでは postprocess を
        //     直接クリアしてはいけない。他の画面効果まで巻き添えで消える。
        //     解除の判断も含めて書き手へ一本化する。
        ApplyFade();
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

    const bool clicked   = m_btn && m_btn->onClick;
    const bool triggered = !triggerAction.empty() && input.GetActionDown(triggerAction);
    if (!clicked && !triggered) return;

    m_fired = true;
    BeginFadeOut();
}

inline void SceneManagerScript::ApplyFade()
{
    // WHY マネージャーがあればそちらへ渡すか:
    //   ランタイムの PostProcessSettings は「まるごと差し替える」器なので、
    //   フェードと被弾フラッシュが別々に書くとどちらかが必ず消える。
    //   ScreenEffectManagerComponent を置いたシーンでは書き手をそちらへ一本化する。
    if (auto* screen = ScreenEffectManagerComponent::Instance()) {
        if (m_fadeAlpha > 0.0f) screen->SetFade(m_fadeAlpha);
        else                    screen->ClearFade();
        return;
    }

    // マネージャーを置いていないシーン向けの直接書き込み。
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
    // 中継シーンを経由する場合: 目的地を静的変数に保存してから中継シーンへ
    const std::string destination = viaScene.empty() ? targetScene : viaScene;
    if (!viaScene.empty()) s_next = targetScene;

    if (scene.LoadScene(destination)) return;

    // WHY 失敗をここで拾うか: 遷移はフェードアウトの後に呼ばれる。要求が通らなかった場合、
    //     暗転したまま何も起きない画面が残り、操作も効かないので原因が読めない。
    //     入力の受付を戻し、暗転していたなら明転させてから、理由をログへ出す。
    debug.LogError("SceneManagerScript: could not switch to scene '" + destination
                   + "'. Staying here (the SceneManager error above has the reason).");
    s_next.clear();
    s_fadeIn = false;
    m_fired  = false;

    // 暗転済みのときだけ明転させる。フェード無しの経路で入れると余計な暗転が 1 回挟まる。
    if (m_fadeAlpha > 0.0f) {
        m_fadeState = FadeState::FadeIn;
    } else {
        m_fadeState = FadeState::Idle;
        ApplyFade();
    }
}

} // namespace sandbox
