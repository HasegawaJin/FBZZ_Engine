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
/// 遷移の絵:
/// fadeEnabled=true のとき、マスクのワイプ (Utils/SceneTransition.hpp / ScreenWipe.hlsl) で
/// 塗ってから切り替え、遷移先で剥がす。fadeDuration は塗り (出) の秒数。
/// 状態はシーンをまたぐ静的な transition::State が持つ。
///
/// WHY: シーンごとに個別コントローラーを作らず、1 スクリプトで全遷移パターンを賄う。
/// 進めるのは transition::Tick で、ScreenEffectManager の居るシーンでは絵をそちらへ
/// 載せてもらい、居ないシーン (メニュー) では自分で PostProcessSettings を書く。
/// 2026-09-07 に黒の lerp (screenFadeAlpha) からマスクのワイプへ移した (ScreenWipe.hlsl の WHY)。
#pragma once

#include <Engine/Renderer/RenderSettings.hpp>
#include <Engine/Scene/Components/UIButton.hpp>
#include <Scripts/Game/ScreenEffectManagerComponent.hpp>
#include <Scripts/Utils/SceneTransition.hpp>
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
    // ワイプの有効/無効と塗り (出) の長さ (秒)。剥がし (入) は 1.2 倍
    FBZZ_FIELD(bool,  fadeEnabled,    true,  "Wipe Enabled")
    FBZZ_FIELD(float, fadeDuration,   0.55f, "Wipe Seconds")
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

    // 中継シーンへ «本当の目的地» を渡す静的変数。
    // WHY: LoadScene でシーンが切り替わると現スクリプトも破棄されるため、
    //      次シーンの OnStart が読める静的変数で引き継ぐ。ワイプの状態は
    //      transition::State (同じ理由で静的) が持つ。
    static inline std::string s_next;

    void OnStart()  override;
    void OnUpdate() override;

private:
    // 塗り始める。fadeEnabled=false なら即 ExecuteLoad() へ。
    void BeginFadeOut();
    // LoadScene を実際に呼ぶ (ワイプ無しの経路)。viaScene がある場合は中継シーンを経由する。
    void ExecuteLoad();
    // マネージャーが居ないシーンで、今フレームの覆いを PostProcessSettings へ書く。
    void ApplyWipe();
    // 遷移先。viaScene があればそちら (目的地は s_next へ)。
    [[nodiscard]] std::string Destination();

    UIButton*  m_btn       = nullptr;
    float      m_elapsed   = 0.0f;
    bool       m_fired     = false; // ボタン / タイマー二重発火防止
    bool       m_writing   = false; // 自分で postprocess を書いた (マネージャー無し)
};

FBZZ_REFLECT(SceneManagerScript)

// ── 実装 (inline) ─────────────────────────────────────────────────────────────
inline void SceneManagerScript::OnStart()
{
    if (autoTransition) {
        // Load.scene モード: 静的変数から遷移先を受け取る
        if (!s_next.empty())
            targetScene = s_next;
    } else {
        // ボタンモード: 同 GO の UIButton をキャッシュ
        m_btn = scene.GetComponent<UIButton>();
    }
    // 塗られたまま来た (前のシーンが塗った) なら、最初のフレームから覆いを出す。
    ApplyWipe();
}

inline void SceneManagerScript::OnUpdate()
{
    // 扉は実時間で進める。同じフレームに ScreenEffectManager も呼ぶが、進むのは 1 回。
    ApplyWipe();

    // 塗っている / 剥がしている最中は押せない (押した瞬間にもう 1 枚扉が開く)。
    if (transition::Busy()) return;
    if (m_fired) return;

    if (autoTransition) {
        // 剥がし切ってから数える。剥がしている最中に数えると «見えた瞬間に去る» になる。
        if (transition::Active()) return;
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

inline void SceneManagerScript::ApplyWipe()
{
    // WHY マネージャーがあれば描かないか:
    //   ランタイムの PostProcessSettings は「まるごと差し替える」器なので、書き手は
    //   1 人でないと互いを消す。ScreenEffectManagerComponent を置いたシーンでは
    //   あちらが transition::PushWipe で載せる。進めるのはどちらでも 1 回だけ。
    (void)transition::Drive(std::max(time.UnscaledDeltaTime(), 0.0f), scene, postprocess,
                            m_writing, ScreenEffectManagerComponent::Instance() != nullptr);
}

inline std::string SceneManagerScript::Destination()
{
    // 中継シーンを経由する場合: 目的地を静的変数に保存してから中継シーンへ
    if (!viaScene.empty()) s_next = targetScene;
    return viaScene.empty() ? targetScene : viaScene;
}

inline void SceneManagerScript::BeginFadeOut()
{
    if (!fadeEnabled) {
        // ワイプなし: 即座に遷移
        ExecuteLoad();
        return;
    }
    transition::Style& style = transition::StyleRef();
    style.outSeconds = std::max(fadeDuration, 0.05f);
    style.inSeconds  = std::max(fadeDuration, 0.05f) * 1.2f;
    if (!transition::Begin(Destination())) {
        // 既に扉が動いている。押した事実だけ戻して、次に任せる。
        m_fired = false;
    }
}

inline void SceneManagerScript::ExecuteLoad()
{
    const std::string destination = Destination();
    if (scene.LoadScene(destination)) return;

    // WHY 失敗をここで拾うか: 要求が通らなかった場合、何も起きない画面が残り、
    //     操作も効かないので原因が読めない。入力の受付を戻して理由をログへ出す。
    debug.LogError("SceneManagerScript: could not switch to scene '" + destination
                   + "'. Staying here (the SceneManager error above has the reason).");
    s_next.clear();
    m_fired = false;
}

} // namespace sandbox
