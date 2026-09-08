/// @file    BossHatchComponent.hpp
/// @brief   背面の装甲カバーの開閉と、コアが «斬れる» 時間の管理
/// @author  Hasegawa Jin
/// @date    2026-09-08
///
/// WHY 弱点にカバーを付けるか:
///   弱点は «守られている状態との対比» でしか読めない。常時露出しているものは、
///   どれだけ光らせても装飾に見える。閉＝ただの装甲 / 開＝弱点、という対比を
///   モデルの側に持たせることで、Docs/climb-core.md の契約
///   「隙のあいだだけ露出する急所」がそのまま絵になる。
///
/// WHY 専用レイヤーへ逃がすか (加算ではなく Override + マスク):
///   蓋のボーン (Hatch_*) と昇降 (Core_Lift) は、ベースのどのクリップもキーしていない。
///   加算の利点は «土台の動きを保ったまま差分を乗せる» ことなので、土台が無いここでは
///   利点がゼロのまま «基準ポーズ» という壊れやすい設定だけが増える。
///   マスクを 5 本へ絞った Override なら、このレイヤーは残りのボーンへ物理的に触れない。
///
/// WHY «開いている» と «斬れる» を別に持つか:
///   蓋は 0.63 秒かけて開き、コアはそこから遅れてせり上がる。開き始めた瞬間に
///   当たり判定を出すと、まだ装甲の下にあるコアへ刃が通る。逆に閉じ始めてから
///   最後まで当たると、目の前で閉じた蓋の中を斬れてしまう。
///   絵と判定のどちらを正にするかは «絵» で、判定をそこへ寄せる。
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
    ///
    /// WHY 判定側に «露出しているか» を聞かせないか: 斬撃・とどめ・HUD の 3 か所が
    ///     それぞれ蓋の状態を引くと、1 か所書き忘れたときに «閉じているのに斬れる»
    ///     が生まれる。オブジェクトごと畳めば、当たりを探す側は何も知らなくてよい
    ///     (BladeComponent の扇は activeInHierarchy で弾いている)。
    void ApplyHitbox();
    [[nodiscard]] static GameObject* FindInSubtree(GameObject& root, const std::string& name);

    bool      m_open    = false;
    bool      m_exposed = false;
    float     m_timer   = 0.0f;   // 現在の状態になってからの経過
    bool      m_applied = false;  // 直近で当たりへ書いた値
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
    m_applied    = true;   // 閉じた状態を 1 度書かせる
    m_started    = false;  // ここで書く «閉じた» は開閉ではないので鳴らさない
    m_probeCooldown = 0.0f;

    // Play をまたぐと Animator は Controller の初期値へ戻る。こちらの真偽値と
    // 食い違ったまま始まると «開いているのに斬れない» が残る。
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

    // WHY 開閉で音を鳴らすか: コアが斬れるのは «蓋が開いている間» だけで、その窓は
    //     地上からは見えない (Docs/climb-core.md「シルエットで «無防備» が読める」は
    //     花弁の輪郭の話で、闘技場の反対側からでは間に合わない)。窓の開閉は
    //     画面に出ていないところでも起きるので、耳で分かる必要がある。
    //
    // WHY 閉じる音を «開く音の逆» にしないか: 閉じは «間に合わなかった» の合図で、
    //     開きと同じ形だと «また開いた» と取り違える。掛け金が噛む 1 発で終わらせる。
    if (!m_started) return;   // OnStart の初期化で «閉じた» を書くときは鳴らさない
    se::Play(audio, open ? se::kBossHatchOpen : se::kBossHatchClose, hatchVolume);
}

inline void BossHatchComponent::OnUpdate()
{
    // 開ける条件は «転倒中» ただ 1 つ。踏みつけを弾いた 0.9 秒の硬直では
    // 開き 0.63s + 閉じ 0.50s が収まらず、開いている時間が実質残らない
    // (Docs/climb-core.md「蓋を開ける条件」)。
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
    if (root.name == name) return &root;
    const int count = root.GetChildCount();
    for (int i = 0; i < count; ++i)
        if (GameObject* child = root.GetChild(i))
            if (GameObject* found = FindInSubtree(*child, name)) return found;
    return nullptr;
}

inline void BossHatchComponent::ApplyHitbox()
{
    if (coreHitboxName.empty()) return;

    GameObject* hitbox = m_coreHitbox.Resolve(scene);
    if (!hitbox) {
        // 当たりは Play のたびに BossHitboxRigComponent が作り直す runtimeGenerated な
        // オブジェクトで、こちらの OnStart より後に出来ることがある。見つかるまで諦めない。
        //
        // WHY 毎フレーム探さないか (2026-09-08): FindInSubtree はボスの全サブツリーを
        //     再帰で歩く。ボーンが実行時生成になってからは 100 ノードを超えていて、
        //     見つからない間ずっと毎フレーム歩き続けると、それだけで目に見えて重くなる。
        //     «見つからない» は数百 ms 遅れても誰も困らないので、間隔を空ける。
        m_probeCooldown -= std::max(Time::deltaTime, 0.0f);
        if (m_probeCooldown > 0.0f) return;
        m_probeCooldown = kProbeInterval;

        GameObject* self = scene.Self();
        hitbox = self ? FindInSubtree(*self, coreHitboxName) : nullptr;
        if (!hitbox) return;
        m_coreHitbox = EntityRef{ hitbox->GetID() };
        m_applied    = !m_exposed;   // 初回は必ず書く
    }

    if (m_applied == m_exposed) return;
    hitbox->SetActive(m_exposed);
    m_applied = m_exposed;
}

} // namespace sandbox
