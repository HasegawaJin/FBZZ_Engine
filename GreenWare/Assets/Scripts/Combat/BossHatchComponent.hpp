/// @file    BossHatchComponent.hpp
/// @brief   背面の装甲カバーの開閉と、コアが «斬れる» 時間の管理
/// @author  Hasegawa Jin
/// @date    2026-09-08
///
/// @note «開いている» (蓋) と «斬れる» (コア露出) の当たりは別。蓋 0.63 秒 / 閉 0.50 秒の
///       アニメ尺に対し exposeDelay/sealLead で遅らせて合わせる (絵が正、判定を寄せる)。
/// @note レイヤーは加算でなく Override+マスクにしてある。蓋 (Hatch_*) と昇降 (Core_Lift) の
///       ボーンはベースクリップがキーしていないので、加算では基準ポーズが無く壊れやすい。
#pragma once

#include <Engine/Scene/EntityRef.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include <Scripts/Combat/IBoss.hpp>
#include <Scripts/Utils/SeLibrary.hpp>
#include <algorithm>
#include <string>

using namespace fbzz::scene;
using namespace fbzz::math;
using fbzz::Time;

namespace sandbox {

class BossHatchComponent : public Script {
    FBZZ_SCRIPT(BossHatchComponent)

public:
    FBZZ_GROUP("レイヤー")
    FBZZ_FIELD(std::string, layerName, "Hatch", "レイヤー名")
    FBZZ_TOOLTIP("Boss.animcontroller の蓋レイヤー。M_Boss_Hatch.mask で "
                 "Hatch_R/F/L/B と Core_Lift の 5 本だけに効く")
    FBZZ_FIELD(std::string, openParam, "HatchOpen", "パラメーター")
    FBZZ_TOOLTIP("bool。true で Open ステートへ、false で Closing → Sealed へ戻る")

    FBZZ_GROUP("間")
    FBZZ_FIELD_RANGE(float, exposeDelay, 0.57f, "露出まで", 0.0f, 3.0f)
    FBZZ_TOOLTIP("開き始めてからコアが斬れるようになるまで [s]。"
                 "Hatch_Open は 20F(0.63s) で、コアがせり上がり切る手前に置く")
    FBZZ_FIELD_RANGE(float, sealLead, 0.16f, "遮蔽まで", 0.0f, 3.0f)
    FBZZ_TOOLTIP("閉じ始めてから斬れなくなるまで [s]。Hatch_Close は 16F(0.50s) で、"
                 "コアが沈み始めた時点で切る")

    FBZZ_GROUP("当たり")
    FBZZ_FIELD(std::string, coreHitboxName, "HB_Core", "コアの当たり")
    FBZZ_FIELD_RANGE(float, hatchVolume, 0.9f, "開閉の音量", 0.0f, 2.0f)
    FBZZ_TOOLTIP("蓋の開閉音。窓が開いた/閉じたを知らせる合図なので、演出として下げると «いつ斬れるのか» が耳から消える")
    FBZZ_TOOLTIP("BossHitboxRigComponent が作る球の名前。露出しているあいだだけ有効にする")

    FBZZ_GROUP("デバッグ")
    FBZZ_FIELD_READ_ONLY(bool, debugOpen, false, "開いている")
    FBZZ_FIELD_READ_ONLY(bool, debugExposed, false, "コア露出")
    FBZZ_FIELD_READ_ONLY(float, debugTimer, 0.0f, "経過 [s]")

    /// 今このフレーム、コアへ刃が通るか。斬撃・当たり判定・HUD がここだけを見る。
    [[nodiscard]] bool IsCoreExposed() const { return m_exposed; }
    /// 蓋が開く指示が出ているか (絵はまだ動いている途中かもしれない)。
    [[nodiscard]] bool IsOpening() const { return m_open; }

    /// 転倒とは無関係に開け閉めする。演出とデバッグの入口。
    void SetOpen(bool open);

    void OnStart() override;
    void OnUpdate() override;

private:
    /// コアの当たり (HB_Core) を露出に合わせて出し入れする。
    /// @note 斬撃・とどめ・HUD に «露出しているか» を個別に聞かせない。書き忘れた 1 か所が
    ///       «閉じているのに斬れる» になるため、当たりごと出し入れして判定側を無知にする。
    void ApplyHitbox();
    [[nodiscard]] static GameObject* FindInSubtree(GameObject& root, const std::string& name);

    bool      m_open    = false;
    bool      m_exposed = false;
    float     m_timer   = 0.0f;   ///< 現在の状態になってからの経過
    bool      m_applied = false;  ///< 直近で当たりへ書いた値
    /// OnStart を抜けたか。初期化で書く «閉じた» を開閉と取り違えないため。
    bool      m_started = false;
    /// 当たりを探し直すまでの残り [秒]。全サブツリー探索を毎フレーム回さないため。
    float     m_probeCooldown = 0.0f;
    static constexpr float kProbeInterval = 0.25f;
    EntityRef m_coreHitbox;
};

FBZZ_REFLECT(BossHatchComponent)

inline void BossHatchComponent::OnStart()
{
    m_open       = false;
    m_exposed    = false;
    m_timer      = 0.0f;
    debugOpen    = false;
    debugExposed = false;
    debugTimer   = 0.0f;
    m_coreHitbox = {};
    /// @note 閉じた状態を 1 度書かせる
    m_applied    = true;
    /// @note ここで書く «閉じた» は開閉ではないので鳴らさない
    m_started    = false;
    m_probeCooldown = 0.0f;

    /// @note Play をまたぐと Animator は Controller の初期値へ戻る。こちらの真偽値と
    ///       食い違ったまま始まると «開いているのに斬れない» が残る。
    if (!openParam.empty()) animator.SetBool(openParam, false);
    if (!layerName.empty()) animator.SetLayerWeight(layerName, 1.0f);
    m_started = true;
}

inline void BossHatchComponent::SetOpen(bool open)
{
    if (m_open == open) return;
    m_open  = open;
    m_timer = 0.0f;
    if (!openParam.empty()) animator.SetBool(openParam, open);

    /// @note 窓の開閉は画面外でも起きるため音で知らせる。閉じ音は開き音の逆再生にしない ─
    ///       同じ形だと «また開いた» と取り違えるため、専用の 1 発で終わらせる。
    /// @note OnStart の初期化で «閉じた» を書くときは鳴らさない。
    if (!m_started) return;
    se::Play(audio, open ? se::kBossHatchOpen : se::kBossHatchClose, hatchVolume);
}

inline void BossHatchComponent::OnUpdate()
{
    /// @note 開ける条件は «転倒中» ただ 1 つ。踏みつけを弾いた 0.9 秒の硬直では
    ///       開き 0.63s + 閉じ 0.50s が収まらず、開いている時間が実質残らない
    ///       (Docs/climb-core.md「蓋を開ける条件」)。
    const IBoss* boss = IBoss::Of(scene.Self());
    SetOpen(boss != nullptr && boss->IsToppled());

    m_timer += std::max(Time::deltaTime, 0.0f);
    m_exposed = m_open ? (m_timer >= exposeDelay)
                       : (m_timer <  sealLead && m_exposed);

    ApplyHitbox();

    debugOpen    = m_open;
    debugExposed = m_exposed;
    debugTimer   = m_timer;
}

inline GameObject* BossHatchComponent::FindInSubtree(GameObject& root, const std::string& name)
{
    return root.FindInSubtree(name);
}

inline void BossHatchComponent::ApplyHitbox()
{
    if (coreHitboxName.empty()) return;

    GameObject* hitbox = m_coreHitbox.Resolve(scene);
    if (!hitbox) {
        /// @note 当たりは Play のたびに BossHitboxRigComponent が作り直す runtimeGenerated な
        ///       オブジェクトで、OnStart より後に出来ることがある。見つかるまで諦めない。
        /// @note 毎フレーム探さない。100 ノード超のサブツリーを再帰で歩くと重く、
        ///       «見つからない» は数百 ms 遅れても困らないので間隔を空ける。
        m_probeCooldown -= std::max(Time::deltaTime, 0.0f);
        if (m_probeCooldown > 0.0f) return;
        m_probeCooldown = kProbeInterval;

        GameObject* self = scene.Self();
        hitbox = self ? FindInSubtree(*self, coreHitboxName) : nullptr;
        if (!hitbox) return;
        m_coreHitbox = EntityRef{ hitbox->GetID() };
        /// @note 初回は必ず書く
        m_applied    = !m_exposed;
    }

    if (m_applied == m_exposed) return;
    hitbox->SetActive(m_exposed);
    m_applied = m_exposed;
}

} // namespace sandbox
