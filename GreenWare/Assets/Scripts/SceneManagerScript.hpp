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
/// @note シーンごとに個別コントローラーを作らず、1 スクリプトで全遷移パターンを賄う。進めるのは
///       transition::Tick で、ScreenEffectManager が居るシーンでは絵をそちらへ載せてもらい、
///       居ないシーン (メニュー) では自分で PostProcessSettings を書く。
#pragma once

#include <Engine/Renderer/RenderSettings.hpp>
#include <Engine/Scene/Components/PresentationComponents.hpp>
#include <Engine/Scene/Components/UIButton.hpp>
#include <Scripts/Game/ScreenEffectManagerComponent.hpp>
#include <Scripts/Title/ElectrodeRig.hpp>
#include <Scripts/Utils/SceneTransition.hpp>
#include <Engine/Scene/Script.hpp>
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using namespace fbzz::scene;
using fbzz::Time;
using fbzz::math::Vector3;
using fbzz::math::Vector4;

namespace sandbox {

class SceneManagerScript : public Script {
    FBZZ_SCRIPT(SceneManagerScript)

    /// ボタンモードでのみ UIButton を読む (autoTransition=true の Load.scene 用途では不要)。
    /// 「どちらの使い方でも成立する」ので必須にはできない。付け忘れたときに
    /// 気付けるよう、任意として名前だけ出しておく。
    FBZZ_OPTIONAL_COMPONENT(UIButton)

public:
    /// 遷移先シーン名。autoTransition=true の場合は OnStart で s_next から上書きされる。
    FBZZ_FIELD(std::string, targetScene,    "", "Target Scene")
    /// 中継シーン名。非空のとき s_next=targetScene を設定してからこのシーンをロードする。
    FBZZ_FIELD(std::string, viaScene,       "", "Via Scene")
    /// true のとき autoDelay 秒後に targetScene へ自動遷移 (Load.scene 用)
    FBZZ_FIELD(bool,  autoTransition, false, "Auto Transition")
    FBZZ_FIELD(float, autoDelay,      1.5f,  "Auto Delay")
    /// ワイプの有効/無効と塗り (出) の長さ (秒)。剥がし (入) は 1.2 倍
    FBZZ_FIELD(bool,  fadeEnabled,    true,  "Wipe Enabled")
    FBZZ_FIELD(float, fadeDuration,   0.55f, "Wipe Seconds")
    /// ボタンモードで、クリックの代わりにこのアクションでも遷移させる。空欄ならクリックのみ。
    /// @note UIButton はマウス座標で判定するためパッドだけでは押せない。本作のメニューはタイトルに
    ///       1 つ、リザルトに 2 つしかないため、カーソル移動を作り込むよりボタンへ直接割り当てる。
    FBZZ_FIELD(std::string, triggerAction, "", "Trigger Action")
    FBZZ_TOOLTIP("ProjectSettings/Input.inputactions のアクション名。"
                 "Submit / Cancel を割り当てるとパッドとキーボードで押せるようになる")

    /// 中継シーンへ «本当の目的地» を渡す静的変数。
    /// @note LoadScene でシーンが切り替わると現スクリプトも破棄されるため、次シーンの OnStart が
    ///       読める静的変数で引き継ぐ。ワイプの状態は transition::State (同じ理由で静的) が持つ。
    static inline std::string s_next;

    void OnStart()  override;
    void OnUpdate() override;

private:
    /// 塗り始める。fadeEnabled=false なら即 ExecuteLoad() へ。
    void BeginFadeOut();
    /// LoadScene を実際に呼ぶ (ワイプ無しの経路)。viaScene がある場合は中継シーンを経由する。
    void ExecuteLoad();
    /// マネージャーが居ないシーンで、今フレームの覆いを PostProcessSettings へ書く。
    void ApplyWipe();
    void TickFusion();
    void StartFusion(const Vector3& position);
    void UpdateFusionVisual();
    /// 遷移先。viaScene があればそちら (目的地は s_next へ)。
    [[nodiscard]] std::string Destination();

    UIButton*  m_btn       = nullptr;
    float      m_elapsed   = 0.0f;
    bool       m_fired     = false; ///< ボタン / タイマー二重発火防止
    bool       m_writing   = false; ///< 自分で postprocess を書いた (マネージャー無し)
    float      m_loadingElapsed = 0.0f;
    int        m_loadingDots = -1;
    bool       m_fusionStarted = false;
    float      m_fusionElapsed = 0.0f;
    Vector3    m_fusionPosition = Vector3::ZERO;
    std::vector<EntityID> m_fusionParts;
};

FBZZ_REFLECT(SceneManagerScript)

inline void SceneManagerScript::OnStart()
{
    if (autoTransition) {
        const std::string next = transition::TakeNextScene();
        if (!next.empty()) targetScene = next;
        /// @note Load.scene モード: 静的変数から遷移先を受け取る
        else if (!s_next.empty())
            targetScene = s_next;
        s_next.clear();
        if (targetScene.empty() || targetScene == "Load") targetScene = "Title";
        ui.SetText(scene.Find("LoadDestination", true), targetScene);
        m_loadingElapsed = 0.0f;
        m_loadingDots = -1;
        m_fusionStarted = false;
        m_fusionElapsed = 0.0f;
        m_fusionParts.clear();
    } else {
        /// @note ボタンモード: 同 GO の UIButton をキャッシュ
        m_btn = scene.GetComponent<UIButton>();
    }
    /// @note 塗られたまま来た (前のシーンが塗った) なら、最初のフレームから覆いを出す。
    ApplyWipe();
}

inline void SceneManagerScript::OnUpdate()
{
    /// @note 扉は実時間で進める。同じフレームに ScreenEffectManager も呼ぶが、進むのは 1 回。
    ApplyWipe();

    if (autoTransition) TickFusion();

    if (autoTransition && !m_fired) {
        m_loadingElapsed += std::max(time.UnscaledDeltaTime(), 0.0f);
        const int dots = static_cast<int>(m_loadingElapsed / 0.32f) % 4;
        if (dots != m_loadingDots) {
            std::string label = "NOW LOADING";
            label.append(static_cast<std::size_t>(dots), '.');
            ui.SetText(scene.Find("LoadingText", true), label);
            m_loadingDots = dots;
        }
    }

    /// @note 塗っている / 剥がしている最中は押せない (押した瞬間にもう 1 枚扉が開く)。
    if (transition::Busy()) return;
    if (m_fired) {
        if (autoTransition && !transition::Active()
            && transition::Mutable().failedTarget == targetScene) {
            ui.SetText(scene.Find("LoadingText", true), "LOAD FAILED");
            ui.SetText(scene.Find("LoadStatus", true), "Unable to open scene. Press Cancel to return.");
            if (input.GetActionDown("Cancel")) {
                targetScene = "Title";
                m_fired = false;
                m_elapsed = autoDelay;
            }
        }
        return;
    }

    if (autoTransition) {
        /// @note 剥がし切ってから数える。剥がしている最中に数えると «見えた瞬間に去る» になる。
        if (transition::Active()) return;
        m_elapsed += std::max(time.UnscaledDeltaTime(), 0.0f);
        const bool fusionSettled = !ElectrodeRig::IsFusionApproaching()
            && (!m_fusionStarted || m_fusionElapsed >= 1.1f);
        if (m_elapsed >= autoDelay && fusionSettled && !targetScene.empty()) {
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

inline void SceneManagerScript::TickFusion()
{
    if (!m_fusionStarted) {
        if (ElectrodeRig::IsFusionActive()) {
            StartFusion(ElectrodeRig::FusionPosition());
        }
    }

    if (m_fusionStarted) {
        m_fusionElapsed += std::max(time.UnscaledDeltaTime(), 0.0f);
        UpdateFusionVisual();
    }
}

inline void SceneManagerScript::StartFusion(const Vector3& position)
{
    if (m_fusionStarted) return;
    m_fusionStarted = true;
    m_fusionPosition = position;
    m_fusionElapsed = 0.0f;
    ElectrodeRig::BeginFusion(position);

    constexpr int kParts = 26;
    m_fusionParts.reserve(kParts);
    for (int index = 0; index < kParts; ++index) {
        GameObject& part = scene.Create("LoadFusionCore");
        part.runtimeGenerated = true;
        part.transform.position = Vector3::ZERO;
        auto& line = part.AddComponent<LineRendererComponent>();
        line.materialPath = "Assets/Materials/Effects/ElectricArc.mat";
        line.space = LineSpace::World;
        line.billboard = true;
        line.orderInLayer = 60;
        line.points.reserve(49);
        m_fusionParts.push_back(part.GetID());
    }
}

inline void SceneManagerScript::UpdateFusionVisual()
{
    constexpr float kTwoPi = 6.28318530718f;
    const float burst = fbzz::math::Clamp01(m_fusionElapsed / 0.65f);
    const float travel = 1.0f - std::pow(1.0f - burst, 3.0f);
    const float impact = std::exp(-m_fusionElapsed * 9.0f);
    const float settle = 1.0f - 0.38f * std::exp(-m_fusionElapsed * 7.0f)
        * std::cos(m_fusionElapsed * 19.0f);
    const float pulse = settle * (1.0f + std::sin(m_fusionElapsed * 3.2f) * 0.025f);
    const Vector4 green{ 0.12f, 1.0f, 0.38f, 1.0f };
    const Vector4 glow{ 0.22f + impact * 0.42f, 1.0f, 0.45f + impact * 0.3f, 1.0f };

    for (std::size_t index = 0; index < m_fusionParts.size(); ++index) {
        GameObject* object = scene.GetGameObject(m_fusionParts[index]);
        if (!object) continue;
        auto* line = object->GetComponent<LineRendererComponent>();
        if (!line) continue;

        const MaterialInstance instance = material.Instance(EntityRef{ m_fusionParts[index] });
        if (instance.HasProperty(MaterialPropertyId("coreColor"))) {
            instance.SetVector4(MaterialPropertyId("tipColor"), green);
            instance.SetVector4(MaterialPropertyId("coreColor"), { 0.6f, 1.6f, 0.85f, 1.0f });
            instance.SetFloat(MaterialPropertyId("coreTint"), 0.85f);
            instance.SetFloat(MaterialPropertyId("breakup"), index >= 12 ? 0.08f : 0.3f);
            instance.SetFloat(MaterialPropertyId("phase"), m_fusionElapsed);
            instance.SetFloat(MaterialPropertyId("travel"), index >= 12 ? 0.0f : 2.0f);
        }

        if (index >= 12) {
            const float afterglow = std::max(m_fusionElapsed - 0.45f, 0.0f);
            const float reveal = fbzz::math::Clamp01(afterglow / 0.4f);
            line->enabled = reveal > 0.0f;
            if (!line->enabled) continue;

            line->points.clear();
            if (index < 14) {
                const float cycle = afterglow / 1.8f + static_cast<float>(index - 12) * 0.5f;
                const float phase = cycle - std::floor(cycle);
                const float radius = 0.5f + phase * 0.85f;
                const float envelope = std::sin(phase * kTwoPi * 0.5f);
                line->startWidth = 0.025f * (1.0f - phase * 0.65f);
                line->endWidth = line->startWidth;
                line->startColor = { green.x, green.y, green.z,
                    reveal * envelope * envelope * 0.3f };
                line->endColor = line->startColor;
                for (int point = 0; point <= 48; ++point) {
                    const float angle = kTwoPi * static_cast<float>(point) / 48.0f;
                    line->points.push_back(m_fusionPosition
                        + Vector3::RIGHT * std::cos(angle) * radius
                        + Vector3::UP * std::sin(angle) * radius);
                }
            } else {
                const float seed = static_cast<float>(index - 14);
                const float cycle = afterglow / (1.5f + seed * 0.045f) + seed / 12.0f;
                const float phase = cycle - std::floor(cycle);
                const float envelope = std::sin(phase * kTwoPi * 0.5f);
                const float radius = 0.36f + (1.0f - phase) * (1.0f - phase) * 0.95f;
                const float angle = seed * 2.399963f + afterglow * 1.4f + phase * 3.2f;
                const float alpha = reveal * envelope * envelope * 0.75f;
                line->startWidth = 0.007f;
                line->endWidth = 0.035f + envelope * 0.015f;
                /// @note ElectricArc は頂点色でなく startColor のアルファを帯全体へ使う。
                line->startColor = { green.x, green.y, green.z, alpha };
                line->endColor = { 0.3f, 1.0f, 0.55f, alpha };
                for (int point = 0; point <= 6; ++point) {
                    const float tail = 1.0f - static_cast<float>(point) / 6.0f;
                    const float arc = angle - tail * 0.22f;
                    const float distance = radius + tail * 0.08f;
                    line->points.push_back(m_fusionPosition
                        + Vector3::RIGHT * std::cos(arc) * distance
                        + Vector3::UP * std::sin(arc) * distance);
                }
            }
            continue;
        }

        if (index >= 3 && burst >= 1.0f) {
            line->enabled = false;
            continue;
        }

        const bool ring = index <= 3;
        const float radius = index == 0 ? 0.48f * pulse
            : index == 1 ? 0.13f * pulse
            : index == 2 ? 0.29f * pulse
            : 0.48f + travel * 1.5f;
        line->enabled = true;
        line->startWidth = index == 0 ? 0.065f + impact * 0.045f
            : index == 1 ? 0.23f * pulse
            : index == 2 ? 0.045f
            : 0.085f * (1.0f - burst);
        line->endWidth = line->startWidth;
        line->startColor = index == 1 ? glow : green;
        if (index >= 3) line->startColor.w = (1.0f - burst) * (1.0f - burst);
        else if (index == 2) line->startColor.w = 0.45f;
        line->endColor = line->startColor;

        if (ring) {
            line->points.clear();
            for (int point = 0; point <= 48; ++point) {
                const float angle = kTwoPi * static_cast<float>(point) / 48.0f;
                line->points.push_back(m_fusionPosition
                    + Vector3::RIGHT * std::cos(angle) * radius
                    + Vector3::UP * std::sin(angle) * radius);
            }
        } else {
            const int ray = static_cast<int>(index) - 4;
            const float angle = kTwoPi * static_cast<float>(ray) / 8.0f;
            const Vector3 direction = Vector3::RIGHT * std::cos(angle)
                                    + Vector3::UP * std::sin(angle);
            line->points = { m_fusionPosition + direction * (radius - 0.3f * (1.0f - burst)),
                             m_fusionPosition + direction * radius };
        }
    }
}

inline void SceneManagerScript::ApplyWipe()
{
    /// @note ランタイムの PostProcessSettings は「まるごと差し替える」器なので書き手は 1 人でないと
    ///       互いを消す。ScreenEffectManagerComponent を置いたシーンでは transition::PushWipe で
    ///       あちらが載せる。進めるのはどちらでも 1 回だけ。
    (void)transition::Drive(std::max(time.UnscaledDeltaTime(), 0.0f), scene, postprocess,
                            m_writing, ScreenEffectManagerComponent::Instance() != nullptr);
}

inline std::string SceneManagerScript::Destination()
{
    /// @note 中継シーンを経由する場合: 目的地を静的変数に保存してから中継シーンへ
    if (!viaScene.empty()) s_next = targetScene;
    return viaScene.empty() ? targetScene : viaScene;
}

inline void SceneManagerScript::BeginFadeOut()
{
    if (!fadeEnabled) {
        /// @note ワイプなし: 即座に遷移
        ExecuteLoad();
        return;
    }
    transition::Style& style = transition::StyleRef();
    style.outSeconds = std::max(fadeDuration, 0.05f);
    style.inSeconds  = std::max(fadeDuration, 0.05f) * 1.2f;
    if (!transition::Begin(Destination())) {
        /// @note 既に扉が動いている。押した事実だけ戻して、次に任せる。
        m_fired = false;
    }
}

inline void SceneManagerScript::ExecuteLoad()
{
    const std::string destination = Destination();
    if (scene.LoadScene(destination)) return;

    /// @note 要求が通らなかった場合、何も起きない画面が残り操作も効かず原因が読めないため、
    ///       入力の受付を戻して理由をログへ出す。
    debug.LogError("SceneManagerScript: could not switch to scene '" + destination
                   + "'. Staying here (the SceneManager error above has the reason).");
    s_next.clear();
    m_fired = false;
}

} // namespace sandbox
