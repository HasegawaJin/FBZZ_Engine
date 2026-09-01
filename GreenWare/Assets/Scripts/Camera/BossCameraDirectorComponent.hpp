/// @file    BossCameraDirectorComponent.hpp
/// @brief   ボス戦の決め所だけカメラを預かる。登場・転倒・撃破の 3 つ。
/// @author  Hasegawa Jin
/// @date    2026-08-31
///
/// WHY 遊びのカメラと別に持つか:
///   TpsCameraComponent は «プレイヤーを追う» ことだけを仕事にしていて、そこへ
///   «たまにボスを見せる» を足すと、追従の式に演出用の分岐が混ざる。カメラが
///   おかしいときに «追従が悪いのか演出が悪いのか» を切り分けられなくなる。
///   預かっている間は追従を止め、返すときに位置と向きを引き継ぐ、と境界を 1 本引く。
///
/// WHY VirtualCamera の仕組みに乗せないか:
///   エンジンには優先度で選ぶ仮想カメラ (CameraRigComponents) があるが、この作品の
///   遊びのカメラはスクリプト (TpsCamera) が transform を直接書く作りで、リグの
///   選択には参加していない。片方だけリグへ載せると «どちらが最後に書いたか» で
///   絵が決まる状態になる。書き手は 1 人、という今の形を保つ。
///
/// WHY 3 つに絞るか:
///   カメラを取り上げられるのはプレイヤーにとって «操作できない時間» で、多用すると
///   ボス戦がムービーの連続になる。取り上げてよいのは «操作しても意味が無い瞬間» ─
///   まだ戦いが始まっていない (登場)、ボスが倒れて手出しが要らない (転倒・撃破) ─
///   の 3 つだけに限る。
///
/// WHY 撃破だけ操作を返さないか:
///   撃破の後はリザルトへ行くだけで、プレイヤーに returns する遊びが無い。
///   そこだけは最後まで演出が持ったまま画面を渡す。
#pragma once

#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include <Math/Quaternion.hpp>
#include <Math/Vector3.hpp>
#include <Scripts/Camera/TpsCameraComponent.hpp>
#include <algorithm>
#include <cmath>
#include <string>

using namespace fbzz::scene;
using namespace fbzz::math;
using fbzz::Time;

namespace sandbox {

/// 演出の種類。値は Inspector のデバッグ再生でも使う。
enum class BossShot : int {
    None   = 0,
    Intro  = 1,   ///< 部屋へ踏み込んだ瞬間。見上げて全身を見せる
    Topple = 2,   ///< 異極の対で転倒した瞬間。寄って «効いた» を返す
    Death  = 3,   ///< 撃破。引きながら回り込む
};

class BossCameraDirectorComponent : public Script {
    FBZZ_SCRIPT(BossCameraDirectorComponent)

public:
    FBZZ_GROUP("Binding")
    FBZZ_FIELD(std::string, bossName, "Boss", "Boss")
    FBZZ_TOOLTIP("見せる相手。空ならタグ / 型では探さず、演出そのものを行わない")
    FBZZ_FIELD_RANGE(float, focusHeight, 3.2f, "Focus Height", 0.0f, 12.0f)
    FBZZ_TOOLTIP("ボスの足元から何 m 上を画面の中心に置くか。全高 6m の胴の中心あたり")

    // WHY 登場だけ長いか: ここは «これから何と戦うか» を伝える唯一の機会で、
    //     全高 6m を下から上まで見せるには時間が要る。転倒と撃破は «今起きたこと» の
    //     確認なので、長いと «見せられている» に変わる。
    FBZZ_GROUP("Intro")
    FBZZ_FIELD(bool, introEnabled, true, "Enabled")
    FBZZ_FIELD_RANGE(float, introSeconds, 2.6f, "Seconds", 0.2f, 10.0f)
    FBZZ_FIELD_RANGE(float, introDistance, 13.0f, "Distance", 2.0f, 40.0f)
    FBZZ_TOOLTIP("ボスからの水平距離 [m]。全高 6m が画角 74 度に収まるのは 12m 前後")
    FBZZ_FIELD_RANGE(float, introPitch, -12.0f, "Pitch", -80.0f, 80.0f)
    FBZZ_TOOLTIP("負で見上げる。ボスを大きく見せるのは角度の仕事で、寄りではない")
    FBZZ_FIELD_RANGE(float, introRise, 2.4f, "Rise", -8.0f, 8.0f)
    FBZZ_TOOLTIP("演出のあいだにカメラが昇る高さ [m]。足元から見上げて胴の高さへ")
    FBZZ_FIELD_RANGE(float, introOrbit, 18.0f, "Orbit", -180.0f, 180.0f)
    FBZZ_TOOLTIP("演出のあいだに回り込む角 [度]。0 で正面から動かない")

    FBZZ_GROUP("Topple")
    FBZZ_FIELD(bool, toppleEnabled, true, "Enabled")
    FBZZ_FIELD_RANGE(float, toppleSeconds, 1.15f, "Seconds", 0.2f, 6.0f)
    FBZZ_FIELD_RANGE(float, toppleDistance, 7.5f, "Distance", 2.0f, 30.0f)
    FBZZ_FIELD_RANGE(float, topplePitch, 6.0f, "Pitch", -80.0f, 80.0f)
    FBZZ_FIELD_RANGE(float, toppleRise, -0.6f, "Rise", -8.0f, 8.0f)
    FBZZ_FIELD_RANGE(float, toppleOrbit, -8.0f, "Orbit", -180.0f, 180.0f)

    FBZZ_GROUP("Death")
    FBZZ_FIELD(bool, deathEnabled, true, "Enabled")
    FBZZ_FIELD_RANGE(float, deathSeconds, 4.4f, "Seconds", 0.2f, 12.0f)
    FBZZ_TOOLTIP("GameFlow の Boss End Delay (既定 5.0) より短くすること。"
                 "長いとリザルトへ切り替わった後もカメラだけ動き続ける")
    FBZZ_FIELD_RANGE(float, deathDistance, 9.0f, "Distance", 2.0f, 40.0f)
    FBZZ_FIELD_RANGE(float, deathPitch, 10.0f, "Pitch", -80.0f, 80.0f)
    FBZZ_FIELD_RANGE(float, deathRise, 3.0f, "Rise", -8.0f, 8.0f)
    FBZZ_FIELD_RANGE(float, deathOrbit, 26.0f, "Orbit", -180.0f, 180.0f)
    FBZZ_FIELD_RANGE(float, deathPullBack, 5.0f, "Pull Back", -10.0f, 20.0f)
    FBZZ_TOOLTIP("演出のあいだに離れる距離 [m]。引くほど «終わった» の間が出る")

    FBZZ_GROUP("Blend")
    FBZZ_FIELD_RANGE(float, blendIn, 0.35f, "In", 0.0f, 3.0f)
    FBZZ_TOOLTIP("遊びの画から演出の画へ寄せる秒数。0 で切り替え (カット)")
    // NOTE: 返す側の «出» は持たない。ResumeFrom で演出の姿勢を追従へ渡すので、
    //       そこから先は追従自身の Follow Speed が遊びの画へ寄せていく。
    //       ここに 2 つ目の時定数を置くと、同じ «戻り» を 2 か所で調整することになる。

    FBZZ_GROUP("Debug")
    FBZZ_FIELD_ENUM(BossShot, previewShot, BossShot::None, "Preview",
                    "None", "Intro", "Topple", "Death")
    FBZZ_TOOLTIP("Play 中にここを選ぶとその演出を 1 回流す。確認したら None へ戻すこと")
    FBZZ_FIELD_READ_ONLY(std::string, debugShot, "-", "Shot")
    FBZZ_FIELD_READ_ONLY(float, debugElapsed, 0.0f, "Elapsed")

    [[nodiscard]] static BossCameraDirectorComponent* Instance() { return s_instance; }

    /// 演出を 1 つ流す。既に流れているものがあれば置き換える。
    ///
    /// WHY 置き換えるか (待たせないか): 転倒中に撃破が決まることがある。待たせると
    ///     «倒れたのに寄りの画のまま数秒» になり、決着の瞬間を見逃す。後から来た
    ///     出来事の方が必ず新しいので、そちらを見せる。
    void Play(BossShot shot);
    /// 今すぐ遊びのカメラへ返す。
    void Release();

    [[nodiscard]] bool IsPlaying() const { return m_shot != BossShot::None; }

    void OnStart()      override;
    void OnLateUpdate() override;
    void OnDestroy()    override { if (s_instance == this) s_instance = nullptr; }

private:
    static inline BossCameraDirectorComponent* s_instance = nullptr;

    /// 1 つの演出の «形»。フィールドから組んで、あとはこれだけを見て動かす。
    struct Shot {
        float seconds  = 1.0f;
        float distance = 8.0f;
        float pitch    = 0.0f;
        float rise     = 0.0f;
        float orbit    = 0.0f;
        float pullBack = 0.0f;
        bool  release  = true;   ///< 終わりに遊びへ返すか
    };

    [[nodiscard]] Shot Build(BossShot shot) const;
    /// 進み 0..1 での «あるべきカメラ姿勢»。
    void Pose(float t, Vector3& position, Quaternion& rotation) const;
    [[nodiscard]] GameObject* Boss() const;
    /// 見つめる点。ボスの足元から focusHeight だけ上。
    [[nodiscard]] Vector3 Focus(const GameObject& boss) const;

    BossShot m_shot    = BossShot::None;
    Shot     m_form;
    float    m_elapsed = 0.0f;
    /// 演出を始めた時点のカメラ姿勢。ここから blendIn で寄せる。
    Vector3    m_fromPos{};
    Quaternion m_fromRot = Quaternion::Identity();
    /// 演出を始めた時点の «ボスから見た方角» [度]。回り込みはここを起点にする。
    ///
    /// WHY 開始時に固定するか: 毎フレーム «今の» 方角から組むと、ボスが動くたびに
    ///     カメラが回り込みの角ぶん飛ぶ。演出中の構図は始まった瞬間に決まる。
    float m_baseYaw = 0.0f;

    TpsCameraComponent* m_tps = nullptr;
    /// 演出前に追従が有効だったか。切っていた人が居るなら勝手に戻さない。
    bool m_tpsWasEnabled = true;
};

FBZZ_REFLECT(BossCameraDirectorComponent)

inline void BossCameraDirectorComponent::OnStart()
{
    if (s_instance && s_instance != this) {
        debug.LogWarning("BossCameraDirectorComponent: 2 つ目が起動した。"
                         "後から始まった方が窓口になる");
    }
    s_instance = this;

    m_shot    = BossShot::None;
    m_elapsed = 0.0f;
    debugShot = "-";

    // 追従は同じ GameObject に居る前提 (どちらもカメラの振る舞い)。
    m_tps = scene.GetScript<TpsCameraComponent>();
    if (!m_tps) {
        debug.LogWarning("BossCameraDirectorComponent: 同じ GameObject に "
                         "TpsCameraComponent が無い。演出から遊びへ返せない");
    }
}

inline GameObject* BossCameraDirectorComponent::Boss() const
{
    return bossName.empty() ? nullptr : scene.Find(bossName);
}

inline Vector3 BossCameraDirectorComponent::Focus(const GameObject& boss) const
{
    return boss.transform.worldPosition + Vector3::UP * std::max(focusHeight, 0.0f);
}

inline BossCameraDirectorComponent::Shot
BossCameraDirectorComponent::Build(BossShot shot) const
{
    switch (shot) {
    case BossShot::Intro:
        return { introSeconds, introDistance, introPitch, introRise, introOrbit, 0.0f, true };
    case BossShot::Topple:
        return { toppleSeconds, toppleDistance, topplePitch, toppleRise, toppleOrbit, 0.0f, true };
    case BossShot::Death:
        // 撃破だけ返さない (ヘッダーの WHY)。
        return { deathSeconds, deathDistance, deathPitch, deathRise, deathOrbit,
                 deathPullBack, false };
    default:
        return {};
    }
}

inline void BossCameraDirectorComponent::Play(BossShot shot)
{
    if (shot == BossShot::None) { Release(); return; }

    const bool allowed = shot == BossShot::Intro  ? introEnabled
                       : shot == BossShot::Topple ? toppleEnabled
                                                  : deathEnabled;
    if (!allowed) return;

    GameObject* boss = Boss();
    if (!boss || !transform) return;

    m_shot    = shot;
    m_form    = Build(shot);
    m_elapsed = 0.0f;
    debugShot = shot == BossShot::Intro ? "Intro" : shot == BossShot::Topple ? "Topple" : "Death";

    // 始まりの画は «今映っている画»。ここを控えずに組むと、どの演出も
    // 決まった位置から始まることになり、直前の状況と繋がらない。
    m_fromPos = transform.worldPosition;
    m_fromRot = transform.worldRotation;

    // 構図の起点。ボスから «今カメラが居る方角» を採り、そこから orbit ぶん回す。
    const Vector3 focus = Focus(*boss);
    const Vector3 away  = Vector3{ m_fromPos.x - focus.x, 0.0f, m_fromPos.z - focus.z };
    m_baseYaw = (away.x * away.x + away.z * away.z) < 0.0001f
              ? 0.0f
              : ToDeg(std::atan2(away.x, away.z));

    if (m_tps) {
        m_tpsWasEnabled = m_tps->enabled;
        m_tps->enabled  = false;
    }
}

inline void BossCameraDirectorComponent::Release()
{
    if (m_shot == BossShot::None) return;
    m_shot    = BossShot::None;
    debugShot = "-";

    // 位置と向きを引き継いでから返す。これが無いと演出の画から 1 フレームで飛ぶ。
    if (m_tps && transform) {
        m_tps->ResumeFrom(transform.worldPosition, transform.worldRotation);
        m_tps->enabled = m_tpsWasEnabled;
    }
}

inline void BossCameraDirectorComponent::Pose(float t, Vector3& position,
                                              Quaternion& rotation) const
{
    GameObject* boss = Boss();
    if (!boss) return;

    const Vector3 focus = Focus(*boss);

    // 回り込みと昇りは «演出のあいだに» 進む。始点で 0、終点で満額。
    const float yaw   = m_baseYaw + m_form.orbit * t;
    const float dist  = std::max(m_form.distance + m_form.pullBack * t, 0.5f);
    const float rise  = m_form.rise * t;

    const Quaternion spin = Quaternion::FromAxisAngle(Vector3::UP, ToRad(yaw));
    const Vector3    dir  = spin * Vector3::FORWARD;

    position = Vector3{ focus.x + dir.x * dist,
                        focus.y + rise,
                        focus.z + dir.z * dist };

    // 向きは «見つめる点» から作る。距離や昇りを変えても中心が外れない。
    //
    // WHY 符号を反転しないか: カメラは focus から dir 方向へ dist だけ離れた所に居るので、
    //     見るべき向きは «focus へ戻る» 側 = toFocus そのもの。ここを -toFocus で
    //     組むと、カメラは正しい位置に居るまま «ボスに背を向ける» ことになり、
    //     «演出が始まっているのにボスが映らない» という形でしか症状が出ない。
    //
    // WHY atan2(x, z) の順か: yaw は UP まわりの回転で、FORWARD(0,0,1) を回すと
    //     (sin, 0, cos) になる。x が第 1 引数でなければ 90 度ずれる。
    const Vector3 toFocus = focus - position;
    const float   flat    = std::sqrt(toFocus.x * toFocus.x + toFocus.z * toFocus.z);
    const float   look    = ToDeg(std::atan2(toFocus.x, toFocus.z));
    // pitch は «見下ろし正» の慣習に揃える (TpsCamera と同じ)。
    // カメラが上に居れば toFocus.y は負で、tilt は正 = 見下ろしになる。
    const float   tilt    = ToDeg(std::atan2(-toFocus.y, std::max(flat, 0.001f)));

    const Quaternion yawRot   = Quaternion::FromAxisAngle(Vector3::UP,    ToRad(look));
    const Quaternion pitchRot = Quaternion::FromAxisAngle(Vector3::RIGHT,
                                                          ToRad(tilt + m_form.pitch));
    rotation = (yawRot * pitchRot).Normalized();
}

inline void BossCameraDirectorComponent::OnLateUpdate()
{
    // Inspector からの確認。Play 中に選んだら 1 回流して None へ戻す。
    if (previewShot != BossShot::None) {
        const BossShot requested = previewShot;
        previewShot = BossShot::None;
        Play(requested);
    }

    if (m_shot == BossShot::None) return;
    if (!transform) return;

    // WHY スケール時間か: 転倒も撃破もヒットストップと同じ «盤面の出来事» で、
    //     止まっている画面の上でカメラだけ動くと、止めが効いていないように見える。
    m_elapsed += std::max(Time::deltaTime, 0.0f);
    debugElapsed = m_elapsed;

    const float total = std::max(m_form.seconds, 0.05f);
    const float t     = Clamp01(m_elapsed / total);
    // 動きは «速く入って緩く止まる»。等速だと機械が回しているように見える。
    const float ease  = 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);

    Vector3    position = transform.worldPosition;
    Quaternion rotation = transform.worldRotation;
    Pose(ease, position, rotation);

    // 入りだけ «直前の画» から寄せる。カットしたいときは Blend In を 0 にする。
    if (blendIn > 0.0f && m_elapsed < blendIn) {
        const float k = Clamp01(m_elapsed / blendIn);
        const float s = k * k * (3.0f - 2.0f * k);
        position = Vector3::Lerp(m_fromPos, position, s);
        rotation = Quaternion::Slerp(m_fromRot, rotation, s).Normalized();
    }

    // WHY ローカルへ書くか: worldRotation は proxy が put を持たない (world は
    //     TransformSystem が local から作る派生値)。カメラはルートに置いてあるので
    //     ローカル = ワールドで、書き先は TpsCamera と同じ 2 つで足りる。
    transform.position = position;
    transform.rotation = rotation;

    if (t < 1.0f) return;

    // 終わり。返さない演出 (撃破) は姿勢を保ったまま居座る。
    if (m_form.release) Release();
}

} // namespace sandbox
