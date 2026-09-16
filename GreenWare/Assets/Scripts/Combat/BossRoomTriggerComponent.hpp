/// @file    BossRoomTriggerComponent.hpp
/// @brief   «部屋へ入ったらボスが起きる» 登場条件。ボス自身が持つ
/// @author  Hasegawa Jin
/// @date    2026-08-29
///
/// WHY Wave 進行をやめてこちらにするか:
///   Wave 制は «盤面が空になった» を数えて次を出す作りで、進行の主導権が時間の側に
///   あった。プレイヤーは片付け終わるのを待つだけで、いつ次が来るかを選べない。
///   部屋で区切ると、戦いの開始をプレイヤーが踏み込んで決めることになる。
///   «まだ入らない» という選択が生まれる時点で、待ち時間が準備の時間に変わる。
///
/// WHY ボス自身が持つか (別の当たり判定オブジェクトではなく):
///   «いつ起きるか» はこのボスの性質で、部屋はその条件を書くための座標にすぎない。
///   トリガーを別オブジェクトへ出すと、ボスを別のシーンへ持っていくたびに
///   «起こす仕掛け» を作り直すことになる。ボス 1 体で完結させる。
///
/// WHY コライダーではなく距離で見るか:
///   ボスは既に胴体のカプセルを 1 つ持っていて、そこへ «部屋» のトリガーを重ねられない
///   (別 GameObject へ逃がすと上の WHY に反する)。加えてボスの判定は BossAiComponent の
///   冒頭のとおり、この盤面では一貫して «物理の接触» ではなく座標で書いてある。
///
/// WHY 眠っている間もボスを «消さない» か:
///   居ることが分かっていて、まだ起きていない — その状態こそが «入るかどうか» を
///   選ばせる。姿ごと消すと、部屋は空き部屋にしか見えず、踏み込む判断が生まれない。
///   止めるのは行動 (BossAiComponent) だけで、姿と音の土台はそのまま残す。
#pragma once

#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include <Math/Vector3.hpp>
#include <Scripts/Camera/BossCameraDirectorComponent.hpp>
#include <Scripts/Combat/BossAiComponent.hpp>
#include <Scripts/Combat/BossAudioComponent.hpp>
#include <Scripts/Combat/BossCoreComponent.hpp>
#include <algorithm>
#include <cmath>

using namespace fbzz::scene;
using namespace fbzz::math;
using fbzz::Time;

namespace sandbox {

class BossRoomTriggerComponent : public Script {
    FBZZ_SCRIPT(BossRoomTriggerComponent)

public:
    FBZZ_GROUP("Room")
    FBZZ_FIELD_TAG(playerTag, "Player", "プレイヤーのタグ")
    FBZZ_FIELD(Vector3, roomOffset, (Vector3::ZERO), "Offset")
    FBZZ_TOOLTIP("部屋の中心をボスからずらす [m]。ボスが部屋の中央に立っていないときだけ使う")
    FBZZ_FIELD_RANGE(float, roomRadius, 12.0f, "半径", 1.0f, 60.0f)
    FBZZ_TOOLTIP("この距離まで近づいたら起きる。踏み込んだことが分かる大きさにすること "
                 "(狭すぎると «殴れる距離» と区別が付かない)")
    FBZZ_FIELD_RANGE(float, roomHeight, 12.0f, "高さ", 0.0f, 60.0f)
    FBZZ_TOOLTIP("高さ方向の厚み [m]。上下に通路があるアリーナで «真上を通っただけ» で "
                 "起きないようにする。0 で高さを見ない")

    FBZZ_GROUP("航跡")
    FBZZ_FIELD_RANGE(float, wakeDelay, 1.0f, "遅延", 0.0f, 8.0f)
    FBZZ_TOOLTIP("踏み込んでからボスが動き出すまで [s]。0 だと入った瞬間に殴られる。"
                 "この間もバーと弾薬の供給は始まっているので、身構える時間になる")

    FBZZ_GROUP("デバッグ")
    // 既定は消しておく。他のデバッグ表示 (drawDebugRanges / drawDebugFeet …) はどれも
    // 既定 off なのに、ここだけ on だった ─ 半径を触った人が «見えるように» 立てたまま
    // 保存すると、ゲーム画面に黄色い球が出たまま配布される。
    FBZZ_FIELD(bool, drawRoom, false, "Draw Room")
    FBZZ_FIELD_READ_ONLY(bool, debugEngaged, false, "交戦中")
    FBZZ_FIELD_READ_ONLY(float, debugDistance, 0.0f, "距離")

    /// 部屋に入らずに始める。デバッグと、演出から直接始めたい場合の入口。
    void Engage();
    [[nodiscard]] bool IsEngaged() const { return m_engaged; }

    void OnStart() override;
    void OnUpdate() override;
    void OnDrawGizmos() override;

private:
    [[nodiscard]] Vector3 RoomCenter() const { return transform.worldPosition + roomOffset; }
    /// プレイヤーが部屋の中に居るか。居ない / 見つからないなら false。
    [[nodiscard]] bool PlayerInside();
    /// 行動を止める / 戻す。姿と音はどちらでも残す。
    void SetAiRunning(bool running) const;
    void PublishEngaged() const;

    float m_delayRemaining = 0.0f;
    bool  m_engaged = false;
};

FBZZ_REFLECT(BossRoomTriggerComponent)

// ── 実装 (inline) ─────────────────────────────────────────────────────────────

inline void BossRoomTriggerComponent::SetAiRunning(bool running) const
{
    if (auto* ai = scene.GetScript<BossAiComponent>()) ai->enabled = running;
}

inline void BossRoomTriggerComponent::PublishEngaged() const
{
    if (auto* core = scene.GetScript<BossCoreComponent>()) core->SetEngaged(m_engaged);
}

inline void BossRoomTriggerComponent::OnStart()
{
    m_engaged = false;
    m_delayRemaining = 0.0f;
    debugEngaged  = false;
    debugDistance = 0.0f;

    SetAiRunning(false);
    PublishEngaged();

    if (!scene.GetScript<BossCoreComponent>()) {
        // 交戦中かどうかを盤面へ公開する口が無いと、体力バーも弾薬の供給も
        // «まだ» のまま止まる。症状は «部屋に入っても何も始まらない» だけになる。
        debug.LogError("BossRoomTriggerComponent requires a BossCoreComponent on the "
                       "same object (it is what publishes the engaged state to the board).");
    }
}

inline bool BossRoomTriggerComponent::PlayerInside()
{
    GameObject* player = scene.FindWithTag(playerTag);
    if (!player) return false;

    const Vector3 delta = player->transform.worldPosition - RoomCenter();
    // 水平と垂直を分けて見る。球で見ると «真上の通路» と «部屋の縁» が同じ距離になり、
    // 上を通っただけで起きる部屋ができる。
    const float horizontal = Vector3{ delta.x, 0.0f, delta.z }.Length();
    debugDistance = horizontal;

    if (horizontal > std::max(roomRadius, 0.0f)) return false;
    return roomHeight <= 0.0f || std::abs(delta.y) <= roomHeight * 0.5f;
}

inline void BossRoomTriggerComponent::Engage()
{
    if (m_engaged) return;
    m_engaged = true;
    debugEngaged = true;
    m_delayRemaining = std::max(wakeDelay, 0.0f);

    // 交戦の «開始» はここで盤面へ通る。体力バーも弾薬の供給も、この 1 行から始まる。
    PublishEngaged();

    // 登場音はこの瞬間に鳴らす。開始と同時に鳴らしてしまうと、部屋の外に居るあいだに
    // 5 秒の登場が終わり、踏み込んだときには何も起きていないことになる
    // (BossAudioComponent の Play Appear は切っておくこと)。
    if (auto* sfx = scene.GetScript<BossAudioComponent>()) sfx->Appear();

    // 見上げてボスの全身を見せる。ここが «何と戦うか» を伝える唯一の機会で、
    // 戦いはまだ始まっていないので操作を取り上げても失うものが無い。
    if (auto* camera = BossCameraDirectorComponent::Instance())
        camera->Play(BossShot::Intro);
}

inline void BossRoomTriggerComponent::OnUpdate()
{
    if (!m_engaged) {
        // 交戦していないことは毎フレーム押し直す。他のスクリプトの OnStart 順に
        // 関係なく «まだ» が保たれる (BossCoreComponent の m_engaged の WHY)。
        PublishEngaged();
        if (PlayerInside()) Engage();
        return;
    }

    if (m_delayRemaining <= 0.0f) return;

    // 登場の画が流れているあいだは数えない。Delay は «画が終わってから身構える間»。
    // 画より先に数え切ると、見上げている最中に踏まれる。
    if (auto* camera = BossCameraDirectorComponent::Instance())
        if (camera->Current() == BossShot::Intro) return;

    // WHY 実時間ではなくスケール時間か: 身構える «間» は盤面の時間の一部で、
    //     踏み込んだ瞬間にヒットストップが入ればその間も一緒に止まってよい。
    m_delayRemaining = std::max(0.0f, m_delayRemaining - Time::deltaTime);
    if (m_delayRemaining <= 0.0f) SetAiRunning(true);
}

inline void BossRoomTriggerComponent::OnDrawGizmos()
{
    if (!drawRoom || m_engaged) return;

    const Vector3 center = RoomCenter();
    debug.DrawSphere(center, std::max(roomRadius, 0.0f), { 1.0f, 0.75f, 0.25f, 1.0f });
}

} // namespace sandbox
