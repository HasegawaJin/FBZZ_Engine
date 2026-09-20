/// @file    BossCameraDirectorComponent.hpp
/// @brief   ボス戦の決め所だけカメラを預かる。登場・転倒・撃破の 3 つ。
/// @author  Hasegawa Jin
/// @date    2026-08-31
///
/// @note 遊びの TpsCameraComponent とは分離する。追従の式へ演出分岐を混ぜると「追従が悪いのか演出が悪いのか」を切り分けられなくなるため、預かる間は追従を止め、返すときに位置・向きだけ引き継ぐ。
/// @note VirtualCamera (優先度制) には乗せない。遊びのカメラは TpsCamera がスクリプトで transform を直書きする作りで、片方だけリグに載せると「最後に書いた方」で絵が決まる状態になる。
/// @note 演出は Intro/Topple/Death/Execute の 3 種+とどめに限る。操作を取り上げてよいのは「操作しても意味が無い瞬間」だけ。
/// @note 撃破だけは操作を返さない。リザルトへ直行するだけで、その先に遊びが無いため。
/// @note 演出中に盤面を止める相手は演出ごとに違う (登場は両方・転倒はどちらも止めない・撃破ととどめはプレイヤーのみ)。申告は cutscene::Publish 経由でボス AI と入力側が読み、カメラは直接触らない (「誰が止めたか」を 1 箇所に置くため)。
/// @note とどめ (Execute) は脚が千切れる瞬間だけ切断面へ 0.58 秒寄る。プレイヤー視点だと脚が体の陰になるため。倒れたボスは動かないので取り上げても失うものが無い。
#pragma once

#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include <Math/Quaternion.hpp>
#include <Math/Vector3.hpp>
#include <Engine/Scene/Components/CameraComponent.hpp>
#include <Scripts/Camera/TpsCameraComponent.hpp>
#include <Scripts/Combat/IBoss.hpp>
#include <Scripts/Game/GameSettingsComponent.hpp>
#include <Scripts/Game/TimeManagerComponent.hpp>
#include <Scripts/Utils/PlayerActionState.hpp>
#include <algorithm>
#include <cmath>
#include <string>

using namespace fbzz::scene;
using namespace fbzz::math;
using fbzz::Time;

namespace sandbox {

/// 演出の種類。値は Inspector のデバッグ再生でも使う。
enum class BossShot : int {
    None    = 0,
    Intro   = 1,   ///< 部屋へ踏み込んだ瞬間。見上げて全身を見せる
    Topple  = 2,   ///< 崩しが満ちて転倒した瞬間。寄って «効いた» を返す
    Death   = 3,   ///< 撃破。引きながら回り込む
    Execute = 4,   ///< とどめ。切断面へ寄る短いカットイン
};

class BossCameraDirectorComponent : public Script {
    FBZZ_SCRIPT(BossCameraDirectorComponent)

public:
    FBZZ_GROUP("Binding")
    FBZZ_FIELD(std::string, bossName, "Boss", "ボス")
    FBZZ_TOOLTIP("見せる相手。空ならタグ / 型では探さず、演出そのものを行わない")
    FBZZ_FIELD_RANGE(float, focusHeight, 3.2f, "注視の高さ", 0.0f, 12.0f)
    FBZZ_TOOLTIP("ボスの足元から何 m 上を画面の中心に置くか。全高 6m の胴の中心あたり")

    /// @note 登場だけ長い。「これから何と戦うか」を伝える唯一の機会で全高 6m を見せるには時間が要る。転倒・撃破は「今起きたこと」の確認なので長いと冗長になる。
    FBZZ_GROUP("登場タイトル")
    FBZZ_FIELD(std::string, introTitle, "IRON WARDEN", "タイトル")
    FBZZ_FIELD(std::string, introSubtitle, "鉄骸の番人", "異名")
    FBZZ_FIELD_COLOR(introColor, Vector4(0.9f, 0.65f, 0.3f, 1.0f), "アクセント")
    FBZZ_FIELD(bool, introFixedFocus, false, "登場の注視点を固定")
    FBZZ_FIELD(Vector3, introFocus, Vector3::ZERO, "登場の注視点")

    FBZZ_GROUP("導入")
    FBZZ_FIELD(bool, introEnabled, true, "有効")
    FBZZ_FIELD_RANGE(float, introSeconds, 2.6f, "秒数", 0.2f, 10.0f)
    FBZZ_FIELD_RANGE(float, introDistance, 13.0f, "距離", 2.0f, 40.0f)
    FBZZ_TOOLTIP("ボスからの水平距離 [m]。全高 6m が画角 74 度に収まるのは 12m 前後")
    FBZZ_FIELD_RANGE(float, introPitch, -12.0f, "ピッチ", -80.0f, 80.0f)
    FBZZ_TOOLTIP("負で見上げる。ボスを大きく見せるのは角度の仕事で、寄りではない")
    FBZZ_FIELD_RANGE(float, introRise, 2.4f, "立ち上がり", -8.0f, 8.0f)
    FBZZ_TOOLTIP("演出のあいだにカメラが昇る高さ [m]。足元から見上げて胴の高さへ")
    FBZZ_FIELD_RANGE(float, introOrbit, 18.0f, "旋回", -180.0f, 180.0f)
    FBZZ_TOOLTIP("演出のあいだに回り込む角 [度]。0 で正面から動かない")
    FBZZ_FIELD_RANGE(float, introFov, -6.0f, "FOV のオフセット", -30.0f, 30.0f)
    FBZZ_TOOLTIP("設定の視野角へ足す量 [度]。負で広角 ─ 6m を見上げるときは少し広げる")
    FBZZ_FIELD(bool, introHoldBoss, true, "Hold Boss")
    FBZZ_TOOLTIP("演出のあいだボスの手を止める。切ると 2.6 秒の登場中に踏まれる")
    FBZZ_FIELD(bool, introHoldPlayer, true, "プレイヤーを固める")

    FBZZ_GROUP("Topple")
    FBZZ_FIELD(bool, toppleEnabled, true, "有効")
    FBZZ_FIELD_RANGE(float, toppleSeconds, 1.15f, "秒数", 0.2f, 6.0f)
    FBZZ_FIELD_RANGE(float, toppleDistance, 7.5f, "距離", 2.0f, 30.0f)
    FBZZ_FIELD_RANGE(float, topplePitch, 6.0f, "ピッチ", -80.0f, 80.0f)
    FBZZ_FIELD_RANGE(float, toppleRise, -0.6f, "立ち上がり", -8.0f, 8.0f)
    FBZZ_FIELD_RANGE(float, toppleOrbit, -8.0f, "旋回", -180.0f, 180.0f)
    FBZZ_FIELD_RANGE(float, toppleFov, 0.0f, "FOV のオフセット", -30.0f, 30.0f)
    /// 転倒は 5 秒の «隙» そのもの。カメラを預かる 1.15 秒も入力は生かす ─ 寄っている
    /// 間に走り寄れないと、隙の 1/4 を演出が食う。
    FBZZ_FIELD(bool, toppleHoldPlayer, false, "プレイヤーを固める")

    FBZZ_GROUP("撃破")
    FBZZ_FIELD(bool, deathEnabled, true, "有効")
    FBZZ_FIELD_RANGE(float, deathSeconds, 4.4f, "秒数", 0.2f, 12.0f)
    FBZZ_TOOLTIP("GameFlow の Boss End Delay (既定 5.0) より短くすること。"
                 "長いとリザルトへ切り替わった後もカメラだけ動き続ける")
    FBZZ_FIELD_RANGE(float, deathDistance, 9.0f, "距離", 2.0f, 40.0f)
    FBZZ_FIELD_RANGE(float, deathPitch, 10.0f, "ピッチ", -80.0f, 80.0f)
    FBZZ_FIELD_RANGE(float, deathRise, 3.0f, "立ち上がり", -8.0f, 8.0f)
    FBZZ_FIELD_RANGE(float, deathOrbit, 26.0f, "旋回", -180.0f, 180.0f)
    FBZZ_FIELD_RANGE(float, deathPullBack, 5.0f, "Pull Back", -10.0f, 20.0f)
    FBZZ_TOOLTIP("演出のあいだに離れる距離 [m]。引くほど «終わった» の間が出る")
    FBZZ_FIELD_RANGE(float, deathFov, -4.0f, "FOV のオフセット", -30.0f, 30.0f)
    FBZZ_FIELD(bool, deathHoldPlayer, true, "プレイヤーを固める")

    /// @note 短い。居合は 0.58 秒で寄って戻すだけ。長いとボスが起き上がる動きまで見せることになり、次の手が読めないまま操作が返る。
    FBZZ_GROUP("とどめ")
    FBZZ_FIELD(bool, executeEnabled, true, "有効")
    FBZZ_FIELD_RANGE(float, executeSeconds, 0.62f, "秒数", 0.1f, 3.0f)
    FBZZ_FIELD_RANGE(float, executeDistance, 4.2f, "距離", 1.0f, 20.0f)
    FBZZ_TOOLTIP("切断面からの水平距離 [m]。脚 1 本が画面の半分を占めるのは 4m 前後")
    FBZZ_FIELD_RANGE(float, executePitch, 4.0f, "ピッチ", -80.0f, 80.0f)
    FBZZ_FIELD_RANGE(float, executeRise, 0.9f, "立ち上がり", -8.0f, 8.0f)
    FBZZ_FIELD_RANGE(float, executeOrbit, 34.0f, "旋回", -180.0f, 180.0f)
    FBZZ_TOOLTIP("刀と脚が重ならない側へ回り込む角。0 だとプレイヤーの背中で切断面が隠れる")
    FBZZ_FIELD_RANGE(float, executeFocusHeight, 0.6f, "注視の高さ", -2.0f, 6.0f)
    FBZZ_TOOLTIP("切断面 (呼ぶ側が渡した点) から何 m 上を中心に置くか")
    FBZZ_FIELD_RANGE(float, executeFov, 6.0f, "FOV のオフセット", -30.0f, 30.0f)
    FBZZ_TOOLTIP("正で望遠。寄りは距離ではなく画角で作ると、背景が圧縮されて «寄った» が強く出る")
    FBZZ_FIELD_RANGE(float, executeSlow, 0.45f, "Slow Scale", 0.05f, 1.0f)
    FBZZ_FIELD_RANGE(float, executeSlowSeconds, 0.28f, "スロー時間 [秒]", 0.0f, 1.5f)
    FBZZ_TOOLTIP("居合の入りだけ遅くする実時間 [秒]。0 でスロー無し。斬った瞬間には戻っていること")
    FBZZ_FIELD(bool, executeHoldPlayer, true, "プレイヤーを固める")

    FBZZ_GROUP("ブレンド")
    FBZZ_FIELD_RANGE(float, blendIn, 0.35f, "In", 0.0f, 3.0f)
    FBZZ_TOOLTIP("遊びの画から演出の画へ寄せる秒数。0 で切り替え (カット)")
    FBZZ_FIELD_RANGE(float, executeBlendIn, 0.10f, "In (Execute)", 0.0f, 1.0f)
    FBZZ_TOOLTIP("とどめだけ速く寄せる。0.35 では寄り切る前に居合が終わる")
    /// @note 返す側の «出» は持たない。ResumeFrom で演出の姿勢を追従へ渡すので、
    ///       そこから先は追従自身の Follow Speed が遊びの画へ寄せていく。
    ///       ここに 2 つ目の時定数を置くと、同じ «戻り» を 2 か所で調整することになる。

    FBZZ_GROUP("デバッグ")
    FBZZ_FIELD_ENUM(BossShot, previewShot, BossShot::None, "プレビュー",
                    "None", "Intro", "Topple", "Death", "Execute")
    FBZZ_TOOLTIP("Play 中にここを選ぶとその演出を 1 回流す。確認したら None へ戻すこと")
    FBZZ_FIELD_READ_ONLY(std::string, debugShot, "-", "Shot")
    FBZZ_FIELD_READ_ONLY(float, debugElapsed, 0.0f, "経過")

    [[nodiscard]] static BossCameraDirectorComponent* Instance() { return s_instance; }

    /// @brief 演出を 1 つ流す。既に流れているものがあれば置き換える。
    /// @note 待たせない。転倒中に撃破が決まることがあり、後から来た出来事の方が必ず新しいので、そちらを優先して見せる。
    void Play(BossShot shot);
    /// @brief 見つめる点を指定して流す (とどめ = 切断面、蛇 = 頭)。点は演出のあいだ動かない。
    /// @note 点で受け取る。蛇は根 (Boss02) が中心に居て頭がそこに無いため、ボスごとの規則より呼ぶ側が点を渡す方が短い。
    void PlayAt(BossShot shot, const Vector3& focus);
    /// 今すぐ遊びのカメラへ返す。
    void Release();

    [[nodiscard]] bool IsPlaying() const { return m_shot != BossShot::None; }
    [[nodiscard]] BossShot Current() const { return m_shot; }
    /// 今流れている演出がボスの手を止めているか。
    [[nodiscard]] bool HoldsBoss() const { return IsPlaying() && m_form.holdBoss; }
    [[nodiscard]] bool HoldsPlayer() const { return IsPlaying() && m_form.holdPlayer; }

    void OnStart()      override;
    void OnLateUpdate() override;
    void OnDisable() override { Release(); UpdateTitle(0.0f); }
    void OnDestroy() override { Release(); UpdateTitle(0.0f); if (s_instance == this) s_instance = nullptr; }

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
        float fov      = 0.0f;   ///< 設定の視野角へ足す量 [度]
        float blendIn  = 0.35f;  ///< 直前の画から寄せる秒数
        bool  holdBoss   = false;
        bool  holdPlayer = false;
        /// 焦点を «ボスの足元 + focusHeight» ではなく呼ぶ側の点に置く。
        bool    focusFixed  = false;
        Vector3 focusPoint  = Vector3::ZERO;
        /// 固定した点から何 m 上を見るか (とどめは切断面の少し上)。
        float   focusLift   = 0.0f;
    };

    [[nodiscard]] Shot Build(BossShot shot) const;
    void Begin(BossShot shot, const Shot& form);
    /// 演出の画角を書く。返すときは設定の値へ戻す (追従は次のフレームから自分で書く)。
    void ApplyFov(float offset) const;
    /// 進み 0..1 での «あるべきカメラ姿勢»。
    void Pose(float t, Vector3& position, Quaternion& rotation) const;
    [[nodiscard]] GameObject* Boss() const;
    /// 見つめる点。ボスの足元から focusHeight だけ上。
    [[nodiscard]] Vector3 Focus(const GameObject& boss) const;
    [[nodiscard]] static const char* NameOf(BossShot shot);

    void UpdateTitle(float alpha);
    bool m_introPlayed = false;
    BossShot m_shot    = BossShot::None;
    Shot     m_form;
    float    m_elapsed = 0.0f;
    /// 演出を始めた時点のカメラ姿勢。ここから blendIn で寄せる。
    Vector3    m_fromPos{};
    Quaternion m_fromRot = Quaternion::Identity();
    /// @note 演出を始めた時点の「ボスから見た方角」[度] に固定する (回り込みの起点)。毎フレーム今の方角から組むとボスが動くたびカメラが飛ぶため。
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
    m_introPlayed = false;
    UpdateTitle(0.0f);

    m_shot    = BossShot::None;
    m_elapsed = 0.0f;
    debugShot = "-";

    /// @note 追従は同じ GameObject に居る前提 (どちらもカメラの振る舞い)。
    m_tps = scene.GetScript<TpsCameraComponent>();
    if (!m_tps) {
        debug.LogWarning("BossCameraDirectorComponent: 同じ GameObject に "
                         "TpsCameraComponent が無い。演出から遊びへ返せない");
    }
}

inline GameObject* BossCameraDirectorComponent::Boss() const
{
    return bossName.empty() ? nullptr : scene.Find(bossName, true);
}

inline Vector3 BossCameraDirectorComponent::Focus(const GameObject& boss) const
{
    return boss.transform.worldPosition + Vector3::UP * std::max(focusHeight, 0.0f);
}

inline BossCameraDirectorComponent::Shot
BossCameraDirectorComponent::Build(BossShot shot) const
{
    Shot form;
    switch (shot) {
    case BossShot::Intro:
        form = { introSeconds, introDistance, introPitch, introRise, introOrbit, 0.0f, true,
                 introFov, blendIn, introHoldBoss, introHoldPlayer };
        break;
    case BossShot::Topple:
        form = { toppleSeconds, toppleDistance, topplePitch, toppleRise, toppleOrbit, 0.0f, true,
                 toppleFov, blendIn, false, toppleHoldPlayer };
        break;
    case BossShot::Death:
        /// @note 撃破だけ操作を返さない (release=false)。リザルトへ直行し、その先に遊びが無いため。
        form = { deathSeconds, deathDistance, deathPitch, deathRise, deathOrbit,
                 deathPullBack, false, deathFov, blendIn, false, deathHoldPlayer };
        break;
    case BossShot::Execute:
        form = { executeSeconds, executeDistance, executePitch, executeRise, executeOrbit, 0.0f,
                 true, executeFov, executeBlendIn, false, executeHoldPlayer };
        form.focusLift = executeFocusHeight;
        break;
    default:
        break;
    }
    return form;
}

inline const char* BossCameraDirectorComponent::NameOf(BossShot shot)
{
    switch (shot) {
    case BossShot::Intro:   return "Intro";
    case BossShot::Topple:  return "Topple";
    case BossShot::Death:   return "Death";
    case BossShot::Execute: return "Execute";
    default:                return "-";
    }
}

inline void BossCameraDirectorComponent::ApplyFov(float offset) const
{
    if (auto* camera = scene.GetComponent<CameraComponent>())
        camera->fovY = GameSettingsComponent::GameOrDefault().fov + offset;
}

inline void BossCameraDirectorComponent::Play(BossShot shot)
{
    if (shot == BossShot::None) { Release(); return; }
    Begin(shot, Build(shot));
}

inline void BossCameraDirectorComponent::PlayAt(BossShot shot, const Vector3& focus)
{
    if (shot == BossShot::None) { Release(); return; }
    Shot form = Build(shot);
    form.focusFixed = true;
    form.focusPoint = focus;
    Begin(shot, form);
}

inline void BossCameraDirectorComponent::Begin(BossShot shot, const Shot& formIn)
{
    const bool allowed = shot == BossShot::Intro   ? introEnabled
                       : shot == BossShot::Topple  ? toppleEnabled
                       : shot == BossShot::Death   ? deathEnabled
                                                   : executeEnabled;
    if (!allowed || (shot == BossShot::Intro && m_introPlayed)) return;

    /// @note 撃破は最後まで持つ。その上に とどめ や転倒が来ても (同フレームの順序次第で
    ///       起きうる) 決着の画を捨てない。
    if (m_shot == BossShot::Death && shot != BossShot::Death) return;

    GameObject* boss = Boss();
    if (!boss || !transform) return;

    const bool wasPlaying = m_shot != BossShot::None;
    m_shot    = shot;
    m_form    = formIn;
    if (shot == BossShot::Intro) {
        m_introPlayed = true;
        if (introFixedFocus) {
            m_form.focusFixed = true;
            m_form.focusPoint = introFocus;
            m_form.focusLift = 0.0f;
        }
    }
    UpdateTitle(0.0f);
    m_elapsed = 0.0f;
    debugShot = NameOf(shot);

    /// @note とどめの入りだけ遅くする。実時間で数えるので、演出の尺 (スケール時間) より
    ///       短く置かないと、戻る前に居合が終わる。
    if (shot == BossShot::Execute && executeSlowSeconds > 0.0f && executeSlow < 1.0f)
        if (auto* timeManager = TimeManagerComponent::Instance())
            timeManager->SlowFor(Clamp01(executeSlow), executeSlowSeconds, 0.0f);

    /// @note 始まりの画は «今映っている画»。ここを控えずに組むと、どの演出も
    ///       決まった位置から始まることになり、直前の状況と繋がらない。
    m_fromPos = transform.worldPosition;
    m_fromRot = transform.worldRotation;

    /// @note 構図の起点。ボスから «今カメラが居る方角» を採り、そこから orbit ぶん回す。
    const Vector3 focus = m_form.focusFixed
        ? m_form.focusPoint + Vector3::UP * m_form.focusLift
        : Focus(*boss);
    const Vector3 away  = Vector3{ m_fromPos.x - focus.x, 0.0f, m_fromPos.z - focus.z };
    m_baseYaw = (away.x * away.x + away.z * away.z) < 0.0001f
              ? 0.0f
              : ToDeg(std::atan2(away.x, away.z));

    /// @note 追従を切るのは演出の «入り» の 1 度だけ。演出の途中で別の演出へ置き換わったときに
    ///       «切った時点の enabled» をもう一度読むと、false を元の値として覚えてしまう。
    if (m_tps && !wasPlaying) {
        m_tpsWasEnabled = m_tps->enabled;
        m_tps->enabled  = false;
    }

    cutscene::Publish(m_form.holdBoss, m_form.holdPlayer, Time::unscaledTime);
}

inline void BossCameraDirectorComponent::Release()
{
    if (m_shot == BossShot::None) return;
    m_shot    = BossShot::None;
    UpdateTitle(0.0f);
    debugShot = "-";

    /// @note 画角を設定の値へ戻す。追従は次のフレームから自分で書くが、その 1 フレームだけ
    ///       演出の画角が残ると «一瞬ズームが跳ねた» に見える。
    ApplyFov(0.0f);
    cutscene::Publish(false, false, Time::unscaledTime);

    /// @note 位置と向きを引き継いでから返す。これが無いと演出の画から 1 フレームで飛ぶ。
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

    const Vector3 focus = m_form.focusFixed
        ? m_form.focusPoint + Vector3::UP * m_form.focusLift
        : Focus(*boss);

    /// @note 回り込みと昇りは «演出のあいだに» 進む。始点で 0、終点で満額。
    const float yaw   = m_baseYaw + m_form.orbit * t;
    const float dist  = std::max(m_form.distance + m_form.pullBack * t, 0.5f);
    const float rise  = m_form.rise * t;

    const Quaternion spin = Quaternion::FromAxisAngle(Vector3::UP, ToRad(yaw));
    const Vector3    dir  = spin * Vector3::FORWARD;

    position = Vector3{ focus.x + dir.x * dist,
                        focus.y + rise,
                        focus.z + dir.z * dist };

    /// @note 向きは見つめる点から作る (距離・昇りを変えても中心が外れない)。符号は反転しない: 見るべき向きは focus へ戻る側 = toFocus そのもの。-toFocus にするとボスに背を向けたまま位置だけ正しくなり、症状は「演出中なのにボスが映らない」としてしか出ない。
    /// @note atan2(x, z) の順を守る。yaw は UP まわりの回転で FORWARD(0,0,1) を回すと (sin, 0, cos) になるため、x を第 1 引数にしないと 90 度ずれる。
    const Vector3 toFocus = focus - position;
    const float   flat    = std::sqrt(toFocus.x * toFocus.x + toFocus.z * toFocus.z);
    const float   look    = ToDeg(std::atan2(toFocus.x, toFocus.z));
    /// @note pitch は «見下ろし正» の慣習に揃える (TpsCamera と同じ)。
    ///       カメラが上に居れば toFocus.y は負で、tilt は正 = 見下ろしになる。
    const float   tilt    = ToDeg(std::atan2(-toFocus.y, std::max(flat, 0.001f)));

    const Quaternion yawRot   = Quaternion::FromAxisAngle(Vector3::UP,    ToRad(look));
    const Quaternion pitchRot = Quaternion::FromAxisAngle(Vector3::RIGHT,
                                                          ToRad(tilt + m_form.pitch));
    rotation = (yawRot * pitchRot).Normalized();
}

inline void BossCameraDirectorComponent::UpdateTitle(float alpha)
{
    const bool visible = alpha > 0.001f;
    if (auto* label = scene.Find("HUD_BossIntroTitle", true)) {
        ui.SetTextEnabled(label, visible);
        ui.SetText(label, introTitle);
        ui.SetTextColor(label, { 0.96f, 0.95f, 0.9f, alpha });
    }
    if (auto* label = scene.Find("HUD_BossIntroSubtitle", true)) {
        ui.SetTextEnabled(label, visible);
        ui.SetText(label, introSubtitle);
        ui.SetTextColor(label, { introColor.x, introColor.y, introColor.z, alpha });
    }
    if (auto* line = scene.Find("HUD_BossIntroLine", true)) {
        ui.SetImageEnabled(line, visible);
        ui.SetImageColor(line, { introColor.x, introColor.y, introColor.z, alpha });
        ui.SetImageFillAmount(line, alpha);
    }
}

inline void BossCameraDirectorComponent::OnLateUpdate()
{
    /// @note Inspector からの確認。Play 中に選んだら 1 回流して None へ戻す。
    if (previewShot != BossShot::None) {
        const BossShot requested = previewShot;
        previewShot = BossShot::None;
        Play(requested);
    }

    /// @note 入室トリガーを持たないボスも、交戦公開後に1度だけ登場させる。
    if (!m_introPlayed && m_shot == BossShot::None)
        if (auto* owner = Boss())
            if (auto* boss = IBoss::Of(owner); boss && boss->IsEngaged()) Play(BossShot::Intro);
    if (m_shot == BossShot::None) return;
    if (!transform) return;

    /// @note 止めているものは毎フレーム申告し直す (読む側は古い申告を捨てる)。
    cutscene::Publish(m_form.holdBoss, m_form.holdPlayer, Time::unscaledTime);

    /// @note スケール時間を使う。転倒・撃破はヒットストップと同じ「盤面の出来事」で、止まっている画面の上でカメラだけ動くと止めが効いていないように見える。
    if (auto* manager = TimeManagerComponent::Instance(); manager && manager->IsPaused()) {
        UpdateTitle(0.0f);
        return;
    }
    m_elapsed += std::max(Time::deltaTime, 0.0f);
    debugElapsed = m_elapsed;

    const float total = std::max(m_form.seconds, 0.05f);
    const float t     = Clamp01(m_elapsed / total);
    /// @note 動きは «速く入って緩く止まる»。等速だと機械が回しているように見える。
    const float ease  = 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);

    Vector3    position = transform.worldPosition;
    Quaternion rotation = transform.worldRotation;
    Pose(ease, position, rotation);

    /// @note 入りだけ «直前の画» から寄せる。カットしたいときは Blend In を 0 にする。
    const float blend = std::max(m_form.blendIn, 0.0f);
    if (blend > 0.0f && m_elapsed < blend) {
        const float k = Clamp01(m_elapsed / blend);
        const float s = k * k * (3.0f - 2.0f * k);
        position = Vector3::Lerp(m_fromPos, position, s);
        rotation = Quaternion::Slerp(m_fromRot, rotation, s).Normalized();
    }

    float returnBlend = 0.0f;
    if (m_shot == BossShot::Intro) {
        const float outSeconds = std::min(0.65f, total * 0.25f);
        const float k = Clamp01((m_elapsed - total + outSeconds) / outSeconds);
        returnBlend = k * k * (3.0f - 2.0f * k);
        position = Vector3::Lerp(position, m_fromPos, returnBlend);
        rotation = Quaternion::Slerp(rotation, m_fromRot, returnBlend).Normalized();
        const float alpha = Clamp01((m_elapsed - 0.35f) / 0.35f) * (1.0f - returnBlend);
        UpdateTitle(alpha);
    }

    /// @note ローカルへ書く。worldRotation は proxy に setter が無く (world は TransformSystem が local から作る派生値)。カメラはルート直下でローカル=ワールドなので、TpsCamera と同じ 2 つの書き先で足りる。
    transform.position = position;
    transform.rotation = rotation;

    /// @note 画角も «寄せ» と同じ坂で入れる。位置だけ寄って画角が一瞬で変わると、
    ///       寄りの途中でカットが 1 回入ったように見える。
    const float fovIn = blend > 0.0f ? Clamp01(m_elapsed / blend) : 1.0f;
    ApplyFov(m_form.fov * fovIn * (2.0f - fovIn) * (1.0f - returnBlend));

    if (t < 1.0f) return;

    /// @note 終わり。返さない演出 (撃破) は姿勢を保ったまま居座る。
    if (m_form.release) Release();
}

} // namespace sandbox
