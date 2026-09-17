/// @file    PlayerParryComponent.hpp
/// @brief   弾き (Parry) と、倒れたボスへの «とどめ» (Execute)。同じボタンの 2 つの顔
/// @author  Hasegawa Jin
/// @date    2026-09-04
///
/// @note 弾きととどめは同じボタン。ボスが立っている間は「受ける」、倒れている間は「仕留める」で同時に要ることが無く、押した瞬間のボスの状態で振り分ける方が速い。
/// @note 窓は「押した瞬間」から開く (クリップの受け姿勢完成を待たない)。待つと «押したのに弾けない» 0.1 秒が生まれるため、クリップは速く流して絵を判定に合わせる (斬撃と同じ判断)。
/// @note クリップは ToBlocking/GuardHit/BlockingToIdle の 3 本 (2026-09-14)。両手剣モーションセットが元々この 3 相で分かれているため、弾きの相をそのままクリップの境に合わせる。速度は各クリップの尺÷その相の秒数で決まる。
/// @note 空振りに硬直を置く。無いと弾き連打が最適になり予兆を読む理由が消えるため。硬直は回避で打ち切れる (2026-09-11) ようにし、0.55 秒から 0.40 秒へ短縮した。
/// @note 優先順位は 回避 > 弾き > 斬撃。3 つとも上半身の同じ Slot を使うため、優先が無いと弾きの裏で斬撃判定が出るなどの事故が起きる。
#pragma once

#include <Engine/Scene/EntityRef.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include <Scripts/Camera/BossCameraDirectorComponent.hpp>
#include <Scripts/Combat/BossBreakComponent.hpp>
#include <Scripts/Combat/BossHitboxRigComponent.hpp>
#include <Scripts/Combat/BossPartComponent.hpp>
#include <Scripts/Combat/IBoss.hpp>
#include <Scripts/Combat/PlayerHit.hpp>
#include <Scripts/Combat/SerpentHitboxRigComponent.hpp>
#include <Scripts/Game/CameraFollowManagerComponent.hpp>
#include <Scripts/Game/CameraShakeManagerComponent.hpp>
#include <Scripts/Game/HitstopManagerComponent.hpp>
#include <Scripts/Game/RumbleManagerComponent.hpp>
#include <Scripts/Game/ScreenEffectManagerComponent.hpp>
#include <Scripts/Game/TimeManagerComponent.hpp>
#include <Scripts/Game/VfxManagerComponent.hpp>
#include <Scripts/Player/PlayerBreathComponent.hpp>
#include <Scripts/Player/PlayerControllerComponent.hpp>
#include <Scripts/Utils/InputActions.hpp>
#include <Scripts/Utils/BladeColors.hpp>
#include <Scripts/Utils/MotionTempo.hpp>
#include <Scripts/Utils/SeLibrary.hpp>
#include <algorithm>
#include <cmath>
#include <string>

using namespace fbzz::scene;
using namespace fbzz::math;
using fbzz::Time;

namespace sandbox {

class PlayerParryComponent : public Script {
    FBZZ_SCRIPT(PlayerParryComponent)

public:
    /// 斬撃と同じ Override レイヤーへ差し込む。レイヤーの重みは BladeComponent が
    /// Slot の重みから毎フレーム流しているので、ここは Slot を鳴らすだけでよい。
    FBZZ_GROUP("動き")
    FBZZ_FIELD(std::string, layerName, "Attack", "Layer")
    /// @brief 埋めると相の繋ぎを 2 枚でクロスフェードする (既定は空 = 1 枚)。
    /// @note 弾きの 3 本は 1 つの連続動作の途中経過で継ぎ目のポーズが一致するため、ハードカットでも見えない (2026-09-14)。2 枚でクロスフェードすると同じ動作の 2 時点の平均を通り «カクッとズレる»。
    /// @note 「ガードが 2 回出る」不具合の原因は枚数でなく、一致しない時点で切り替えていたことだった (構えの途中から完成形へ飛んでいた)。
    FBZZ_FIELD(std::string, layerNameB, "", "Layer (B)")
    FBZZ_TOOLTIP("埋めると相の繋ぎを 2 枚でクロスフェードする。**通常は空のまま** ─ "
                 "弾きは 1 続きの動作なので、継ぎ目で混ぜると姿勢がズレる")
    /// 構え。押した瞬間に出て、窓のあいだ受けの姿勢を保つ。
    FBZZ_FIELD_FILE(parryClipFile,
        "guid:e868e45cccf53747bb47478207a97465|Library/Baked/57911fd51ce74fac920b7120996b5003/anims/ToBlocking.anim",
        "Guard Clip", ".anim,.fbx")
    FBZZ_FIELD(std::string, parryClipName, "ToBlocking", "Guard Clip Name")
    /// 弾けた瞬間。«パキッ» の絵はこれ 1 本が持つ。
    FBZZ_FIELD_FILE(guardHitClipFile,
        "guid:5ef8954912369033016d77197f2c9aed|Library/Baked/237865fc4ebd44ceafdf2dcaed33b848/anims/GuardHit.anim",
        "Riposte Clip", ".anim,.fbx")
    FBZZ_FIELD(std::string, guardHitClipName, "GuardHit", "Riposte Clip Name")
    /// 構えたまま保つところ。窓を過ぎても押している間はこれをループする。
    FBZZ_FIELD_FILE(holdClipFile,
        "guid:5c39623af4ff4c868a59f410d205f78c|Library/Baked/8d8448298b4942609c57470778c13c9a/anims/Blocking.anim",
        "Guard Hold Clip", ".anim,.fbx")
    FBZZ_FIELD(std::string, holdClipName, "Blocking", "Guard Hold Clip Name")
    /// 外して構えを解くところ。空振りの硬直はこれで埋める。
    FBZZ_FIELD_FILE(releaseClipFile,
        "guid:c55e17d2f4e684fe09033647649cdf2c|Library/Baked/f4205f13cfbc4cf1bb7fe14e63d366ad/anims/BlockingToIdle.anim",
        "Release Clip", ".anim,.fbx")
    FBZZ_FIELD(std::string, releaseClipName, "BlockingToIdle", "Release Clip Name")
    /// BlockingToIdle は 0.50s。空振りの硬直 0.40 秒へ収めるので 0.50 / 0.40 = 1.25。
    FBZZ_FIELD_RANGE(float, parrySpeed, 1.25f, "Release Speed", 0.5f, 3.0f)
    FBZZ_TOOLTIP("空振りの硬直の再生速度。構え・弾き返しは «緩急» の値で流れる")
    FBZZ_FIELD_FILE(executeClipFile,
        "guid:f318903764723f31bc36144d69bd49cf|Library/Baked/5c122d77aced43388c7a0712f05e5587/anims/SlideAttack.anim",
        "Execute Clip", ".anim,.fbx")
    FBZZ_FIELD(std::string, executeClipName, "SlideAttack", "Execute Clip Name")
    /// SlideAttack は 2.13s。踏み込んで斬るので、とどめ (膝下へ寄って斬る) の形に一番近い。
    FBZZ_FIELD_RANGE(float, executeHitTime, 0.90f, "Execute Hit Time", 0.05f, 3.0f)
    FBZZ_TOOLTIP("踏み込んで斬り抜けるクリップ秒数。ここに ResolveExecute の時刻を合わせる")
    FBZZ_FIELD_RANGE(float, executeSpeed, 1.5f, "Execute Speed", 0.5f, 3.0f)
    FBZZ_TOOLTIP("とどめの再生速度。1 だと斬り抜けまで 0.90 秒 ─ 倒れている 5 秒に対して重すぎる")
    FBZZ_FIELD_RANGE(float, fadeIn,  0.03f, "フェードイン",  0.0f, 0.5f)
    /// @note 0.06 まで詰める (2026-09-14)。ループしない Slot はクリップ末尾の `fadeOut` 秒に達した瞬間に自動でフェードアウトへ入るため (AnimatorSystem)、大きいと構えが完成する前に抜け始め「同じ動きが 2 回」見える。フェードの長さ = クリップ末尾から捨てる量。
    FBZZ_FIELD_RANGE(float, fadeOut, 0.06f, "フェードアウト", 0.0f, 0.5f)
    FBZZ_TOOLTIP("ワンショットのクリップは末尾この秒数ぶんが «抜ける時間» に使われる。"
                 "長くすると振り終わりが出ないまま消える")

    /// @note 構えを等速で流さない。ToBlocking は 0.50 秒、窓は 0.22 秒しか無いため、等速だと窓が閉じるまでに構えが出来上がらない。構えまでは一気に、出来上がったら窓の終わりまで留める。
    FBZZ_GROUP("緩急")
    /// @note 既定はクリップの終端に置く (2026-09-14)。構えが何秒目で出来上がるかはクリップの中を見ないと分からず、途中の秒数を当て推量で置くと窓の間ずっと振り上げ途中の姿勢が出る。終端まで駆け抜けて留めれば、クリップを差し替えても壊れない。
    FBZZ_FIELD_RANGE(float, parryGuardTime, 0.50f, "Guard Pose At [s]", 0.0f, 1.0f)
    FBZZ_TOOLTIP("構えが出来上がるクリップ秒数。既定はクリップの終端 (ToBlocking = 0.50s)。"
                 "ここまでを Guard Snap で駆け抜け、以降を Guard Hold で留める")
    FBZZ_FIELD_RANGE(float, parrySnapSpeed, 4.0f, "Guard Snap", 0.5f, 8.0f)
    FBZZ_TOOLTIP("押してから受けの姿勢までの再生速度。押した瞬間に «構えた» が見える速さ")
    FBZZ_FIELD_RANGE(float, parryHoldSpeed, 0.50f, "Guard Hold", 0.05f, 2.0f)
    FBZZ_TOOLTIP("受付時間のあいだの再生速度。出来上がった構えを窓の終わりまで保つ")
    /// GuardHit は 0.50s。弾けた後の硬直 0.22 秒へ収めるので 0.50 / 0.22 = 2.3。
    FBZZ_FIELD_RANGE(float, parryRiposteSpeed, 2.3f, "Riposte Speed", 0.5f, 4.0f)
    FBZZ_TOOLTIP("弾き返し (GuardHit) の再生速度。Hit Recovery に収まる速さ")
    /// @note とどめは「溜めて一閃」にする。仕留める一撃は踏み込むまでの静と斬り抜ける瞬間の動の落差そのもので、等速だとただ滑って斬る絵になる。溜めるのは踏み込みまでで、斬り抜けの時刻 (Execute Hit Time) は変えない。
    FBZZ_FIELD_RANGE(float, executeWindup, 0.55f, "Execute Windup", 0.1f, 1.0f)
    FBZZ_FIELD_RANGE(float, executeWindupPower, 3.0f, "Execute Strike Curve", 0.5f, 6.0f)
    FBZZ_FIELD_RANGE(float, executeFollowEnd, 0.4f, "Execute Zanshin Speed", 0.05f, 2.0f)
    FBZZ_TOOLTIP("斬り下ろした後の残心の再生速度 [Execute Speed に対する比]。0.35 秒かけてここまで落とす")

    /// @note ガードを再導入 (2026-09-14)。「押した 0.22 秒だけが受け」だと外した瞬間に無防備な 0.40 秒が来て、押すこと自体が損になる。押し続ければ止まる床を敷いた上で頭 0.22 秒だけを弾きにすると、«とりあえず構える» から «引き付けて合わせる» へ地続きに上がれる。
    /// @note ガードでは崩しを 1 も溜めない。溜まると Docs/break-parry.md の芯 (崩すのは弾きだけ) が壊れる。見返りは «止まる» ことだけで、止めている間は斬れず足も鈍り、崩しは減衰する。
    /// @note 削り (chip) は入れない。HP は 5 しかないので 1 削ると «ガードはほぼ被弾» になり床の意味が消える。押しっぱなしを咎めるのは Unblockable な手 (ビーム・パルス・扇) の役目。
    FBZZ_GROUP("ガード")
    FBZZ_FIELD(bool, holdToGuard, true, "押しっぱなしで防ぐ")
    FBZZ_TOOLTIP("切ると «窓 0.22 秒だけ» の弾き専用へ戻る")
    FBZZ_FIELD_RANGE(float, guardHitSeconds, 0.30f, "受け止めの間 [s]", 0.05f, 1.0f)
    FBZZ_TOOLTIP("ガードで受け止めた絵 (GuardHit) を見せる長さ。過ぎたら構えへ戻る")
    FBZZ_FIELD_RANGE(float, guardShake, 0.22f, "揺れ", 0.0f, 1.0f)
    FBZZ_TOOLTIP("受け止めた瞬間の揺れ。弾き (0.50) より弱くして «止めただけ» を伝える")
    FBZZ_FIELD_RANGE(float, guardHitStop, 0.35f, "ヒットストップ", 0.0f, 1.0f)
    /// @note 削りの代わりに息で咎める (2026-09-14)。押しっぱなしのコストが「止まっていること」だけだと弾けない手が来ない局面で構え続けられてしまうため、息 (Breath) が尽きると構えが割れる (痛みは無いが守り続けられなくなる)。
    FBZZ_FIELD_RANGE(float, guardBreakRecovery, 0.90f, "割れた硬直 [s]", 0.05f, 3.0f)
    FBZZ_TOOLTIP("息が尽きて構えが割れたときの硬直。外した硬直 (0.40) より長い ─ "
                 "«間に合わなかった» より «押し切られた» の方が重い")

    FBZZ_GROUP("タイミング")
    FBZZ_FIELD_RANGE(float, windowSeconds, 0.22f, "受付時間", 0.05f, 1.0f)
    FBZZ_TOOLTIP("押してから弾ける時間。踏みつけの叩きつけは 0.10 秒なので、"
                 "予兆 (静止 0.13 秒) の途中で押せば必ず入る長さ")
    /// @note Just は窓の «頭» 側で判定する。窓は押した瞬間から開くため、攻撃直前に押すほど窓の早い時刻で当たり «ぎりぎりまで引き付けた» 弾きになる。早押しして窓の後半で拾う弾きは保険を掛けた弾きで、読み切りではない。
    FBZZ_FIELD_RANGE(float, justParrySeconds, 0.08f, "Just Window", 0.0f, 0.3f)
    FBZZ_TOOLTIP("押してからこの秒数以内に受けた弾きは «Just»。崩しが多く溜まり、"
                 "スローと閃光が深くなる。0 で無効")
    FBZZ_FIELD_RANGE(float, recoverySeconds, 0.40f, "Whiff Recovery", 0.05f, 2.0f)
    FBZZ_TOOLTIP("外したときの硬直。連打を最適にしないための重さ。回避で打ち切れる")
    FBZZ_FIELD_RANGE(float, successRecovery, 0.22f, "Hit Recovery", 0.0f, 1.0f)
    FBZZ_TOOLTIP("弾けたときの硬直。攻撃ボタンで打ち切って斬り返せる")
    /// @note 押下を預かる (バッファ)。弾きは一番タイミングを狙って押すボタンなのに硬直中・回避中の押下を捨てていた。明ける直前に押して何も出ないのが手触りを一番損ねるため。
    FBZZ_FIELD_RANGE(float, parryBufferSeconds, 0.12f, "Parry Buffer", 0.0f, 0.4f)
    FBZZ_TOOLTIP("硬直中・回避中に押した弾きを預かる秒数。明けた瞬間に構える。"
                 "長くすると «早押しの保険» になるので短く")
    FBZZ_FIELD_RANGE(float, executeLock, 1.05f, "Execute Lock", 0.2f, 3.0f)
    FBZZ_TOOLTIP("とどめの間、他の入力を受け付けない時間。斬り下ろしより少し長く")
    FBZZ_FIELD_RANGE(float, executeRange, 3.4f, "Execute Range", 0.5f, 10.0f)
    FBZZ_TOOLTIP("倒れたボスの部位からこの距離以内なら、押した瞬間にとどめへ入る")
    /// @note 高さの帯が要る (2026-09-10)。届くかは水平距離だけで測るため、背のコアが的になったことで地上に立っていても «真上 5m のコア» が水平 0m として届いてしまい、何も無い空へ居合が出ていた。
    FBZZ_FIELD_RANGE(float, executeHeight, 3.0f, "Execute Height", 0.5f, 20.0f)
    FBZZ_TOOLTIP("足元からこの高さの帯にある部位だけが とどめ の的になる。"
                 "甲板 (足元から +5m 上) のコアと地上の脚を混ぜないための仕切り")
    FBZZ_FIELD_RANGE(float, moveScale, 0.35f, "Move Scale (busy)", 0.05f, 1.0f)

    /// «パキッ» の配分。止め・閃光・破片は一瞬で、余韻を残さない。
    /// ビネットや収差 (Distort) は使わない ─ 画面の縁が暗くなるのは «受けた» の語。
    FBZZ_GROUP("Parry Feel")
    FBZZ_FIELD_RANGE(float, parryHitStop, 0.85f, "ヒットストップ", 0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, parryShake, 0.50f, "揺れ", 0.0f, 1.0f)
    FBZZ_FIELD_COLOR(parryFlashColor, (Vector4{ 0.85f, 0.95f, 1.0f, 1.0f }), "Flash Color")
    FBZZ_FIELD_RANGE(float, parryFlash, 0.55f, "閃光", 0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, parryFlashSeconds, 0.07f, "閃光の長さ [秒]", 0.0f, 0.5f)
    FBZZ_FIELD_RANGE(float, parrySlowScale, 0.30f, "スロー", 0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, parrySlowSeconds, 0.12f, "スロー時間 [秒]", 0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, parryRumble, 0.95f, "振動", 0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, parryFov, 0.35f, "FOV の跳ね", 0.0f, 1.0f)
    FBZZ_TOOLTIP("弾いた瞬間に画角を開く強さ。重い手はこの 1.6 倍まで開く")
    FBZZ_FIELD_RANGE(float, parryVfxHeight, 1.35f, "VFX Height", 0.0f, 3.0f)
    FBZZ_FIELD_RANGE(float, parryVfxForward, 1.1f, "VFX Forward", 0.0f, 3.0f)
    FBZZ_FIELD_RANGE_INT(int, heavyDamageAt, 3, "Heavy At Damage", 1, 100)
    FBZZ_TOOLTIP("弾いた一撃のダメージがこれ以上なら «重い手» として扱う (突進 = 3)")

    FBZZ_GROUP("Execute Feel")
    FBZZ_FIELD_RANGE(float, executeHitStop, 1.0f, "ヒットストップ", 0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, executeShake, 0.9f, "揺れ", 0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, executeSlowScale, 0.25f, "スロー", 0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, executeSlowSeconds, 0.35f, "スロー時間 [秒]", 0.0f, 2.0f)
    FBZZ_FIELD_RANGE(float, executeFov, 1.0f, "FOV の跳ね", 0.0f, 1.0f)
    FBZZ_TOOLTIP("とどめが入った瞬間に画角を開く強さ。盤面で一番大きい一撃なので最大")

    FBZZ_GROUP("デバッグ")
    FBZZ_FIELD_READ_ONLY(std::string, debugPhase, "Idle", "位相")
    FBZZ_FIELD_READ_ONLY(int, debugParries, 0, "Parries")
    FBZZ_FIELD_READ_ONLY(int, debugJustParries, 0, "Just Parries")
    FBZZ_FIELD_READ_ONLY(int, debugExecutions, 0, "Executions")
    FBZZ_FIELD_READ_ONLY(int, debugGuards, 0, "Guards")
    FBZZ_TOOLTIP("受け止めた回数。**ここだけが伸びて Parries が伸びないなら、"
                 "«押しっぱなしで防いでいるだけ» で盤面は進んでいない**")

    /// 弾きの窓が開いているか。PlayerComponent が一撃を受けた瞬間に読む。
    [[nodiscard]] bool IsParryActive() const { return m_phase == Phase::Window; }
    /// 構えたまま «止められる» 状態か。窓を過ぎた後の押しっぱなしがここ。
    [[nodiscard]] bool IsGuardActive() const { return m_phase == Phase::Guard; }
    /// 受け止めた (崩しは溜まらない)。PlayerComponent が Guarded を返す前に呼ぶ。
    void OnGuarded(int amount, const Vector3* from);
    /// 弾きかとどめの最中か。剣はこの間、攻撃を受け付けない。
    [[nodiscard]] bool IsBusy() const { return m_phase != Phase::Idle; }
    /// 弾けた後の硬直中か。この間は攻撃ボタンで硬直を打ち切って斬り返せる。
    [[nodiscard]] bool CanAttackCancel() const
    { return m_phase == Phase::Recovery && m_succeeded; }
    /// 構え・硬直を打ち切り、上半身のクリップも畳む。とどめは演出ごと預かっているので切らない。
    void Cancel();

    /// @name 出来事の回数
    /// @{
    /// @note 通知でなく回数で返す。弾きは 1 フレームの出来事で受け手 (刃の焼き・HUD) は自分の更新順で読むため、通知だと取り逃し・二重受けが起きる。回数なら前フレームとの差で今フレームの発生回数が誰にでも出る。
    [[nodiscard]] int ParryCount() const { return m_parries; }
    /// 読み切って弾いた回数。ParryCount の内数。
    [[nodiscard]] int JustParryCount() const { return m_justParries; }
    [[nodiscard]] int ExecuteCount() const { return m_executions; }

    /// 弾けた。手触りを返し、崩しを溜め、ボスへ «弾かれた» を伝える。
    /// @param amount 弾いた一撃のダメージ (重さの判定に使う)
    /// @param from   攻撃してきた物の位置。nullptr なら正面とみなす
    void OnParried(int amount, const Vector3* from);

    /// 倒れているボスの部位が届く距離に居れば、とどめを始めて true。
    /// 攻撃ボタンからも呼ばれる (倒れている間はどのボタンでも仕留められる)。
    bool TryExecute();

    void SetController(PlayerControllerComponent* controller) { m_controller = controller; }
    /// 構えを保つ息の出どころ。未設定なら息を見ずに構え続けられる。
    void SetBreath(PlayerBreathComponent* breath) { m_breath = breath; }

    void OnStart()  override;
    FBZZ_GROUP("連撃チャンス")
    FBZZ_FIELD_RANGE(float, rushSeconds, 2.0f, "通常 [秒]", 0.0f, 5.0f)
    FBZZ_FIELD_RANGE(float, rushJustSeconds, 2.6f, "Just [秒]", 0.0f, 5.0f)
    FBZZ_FIELD_RANGE(float, rushWorldScale, 0.25f, "世界の速度", 0.125f, 1.0f)
    FBZZ_FIELD_RANGE(float, rushAttackSpeed, 1.65f, "連撃速度", 1.0f, 3.0f)
    void OnUpdate() override;
    /// @}

private:
    /// Window は «押した頭の 0.22 秒» で弾ける。そこを過ぎて押し続けていると Guard へ
    /// 落ちる ── 止められるが崩しは溜まらない。離すと Recovery (構えを解く)。
    enum class Phase : int { Idle = 0, Window, Guard, Recovery, Execute };

    void BeginParry();
    void DriveRushHud();
    int m_rushResumeFrames = 0;
    float m_rushHudPulse = 0.0f;
    /// 窓を過ぎても押し続けている ─ 構えを保つ層へ移る。
    void BeginGuard();
    /// 構えが出来上がるまでの秒数。末尾 fadeOut ぶんは自動フェードに食われるので、そこへ届く前にループへ渡す (fadeOut フィールドの @note 参照)。
    [[nodiscard]] float HandoffSeconds() const
    {
        const float usable = std::max(parryGuardTime - fadeOut, 0.02f);
        return usable / std::max(parrySnapSpeed, 0.1f);
    }
    /// 構えのループへ渡す。以後は姿勢が動かないので «壊れる» 余地が無くなる。
    void PlayHoldLoop()
    {
        PlayClip(holdClipFile, holdClipName, 1.0f, /*loop=*/true);
        m_holdPlaying = !holdClipFile.empty() && !holdClipName.empty();
    }
    /// 構えを解いて硬直へ。外したときも、ガードを離したときもここを通る。
    void BeginRelease();
    /// 息が尽きて構えが割れた。硬直は外したときより長く、崩しには何も起きない。
    void BreakGuard();
    /// 相ごとのクリップを Slot へ差す。空の割り当ては «絵を出さない» の意味。
    /// 差すたびに乗せる先を A / B で入れ替え、前のクリップは fadeOut で抜けさせる。
    void PlayClip(const std::string& file, const std::string& name, float speed,
                  bool loop = false);
    /// 今クリップが乗っているレイヤー。
    [[nodiscard]] const std::string& ActiveLayer() const
    { return m_flip && !layerNameB.empty() ? layerNameB : layerName; }
    /// 両方のレイヤーの Slot を畳む。
    void StopBoth(float fade);
    void ResolveExecute();
    /// とどめが届く部位と、その持ち主。無ければ nullptr。
    [[nodiscard]] GameObject* FindExecutablePart(GameObject*& outRoot) const;
    /// 部位からボス本体を引く。4 足と蛇でリグが違うので両方試す。
    [[nodiscard]] static GameObject* RootOf(GameObject* part);
    /// @brief 今戦っているボスの本体。居なければ nullptr。
    /// @note 一番近い相手を選ぶ。相手が 2 体立っている盤面 (Stage_02) で名簿のどれか 1 体を引くと、構えの向きも止めも毎回別の相手に掛かるため、近さで選び «弾いた相手» と一致させる。
    [[nodiscard]] GameObject* Boss() const
    {
        return FindNearestBossOnBoard(scene, transform.worldPosition);
    }
    void SetPhase(Phase phase, float seconds);
    /// 弾き・とどめのクリップの再生速度を位相ごとに決める (緩急)。
    void DriveTempo(float dt);

    PlayerControllerComponent* m_controller = nullptr;
    PlayerBreathComponent*     m_breath     = nullptr;

    /// 次にクリップを差す先が B 側か。相が変わるたびに反転する。
    bool  m_flip  = false;
    Phase m_phase = Phase::Idle;
    float m_timer = 0.0f;
    /// 預かっている弾き入力の残り [秒]。
    float m_buffer = 0.0f;
    /// 今の硬直が «弾けた» 後か (空振りの硬直と分ける)。
    bool  m_succeeded = false;
    /// とどめの斬り下ろしを既に判定したか。1 回のとどめで 1 度だけ。
    bool      m_executeResolved = false;
    bool      m_executeSwingPlayed = false;
    EntityRef m_executePart;
    EntityRef m_executeRoot;
    /// 受け止めた絵を見せている残り [秒]。0 になったら構えのループへ戻す。
    float m_guardHit = 0.0f;
    /// 構えのループへ渡すまでの残り [秒] と、渡し終えたかの札。
    float m_toHold      = 0.0f;
    bool  m_holdPlaying = false;
    int m_parries     = 0;
    int m_justParries = 0;
    int m_executions  = 0;
    int m_guards      = 0;
};

FBZZ_REFLECT(PlayerParryComponent)

inline void PlayerParryComponent::OnStart()
{
    m_rushResumeFrames = 0;
    m_rushHudPulse = 0.0f;
    m_phase = Phase::Idle;
    m_timer = 0.0f;
    m_flip      = false;
    m_buffer    = 0.0f;
    m_succeeded = false;
    m_executeResolved = false;
    m_executeSwingPlayed = false;
    m_executePart = {};
    m_executeRoot = {};
    m_parries     = 0;
    m_justParries = 0;
    m_executions  = 0;
    m_guards      = 0;
    m_guardHit    = 0.0f;
    m_toHold      = 0.0f;
    m_holdPlaying = false;
    debugPhase       = "Idle";
    debugParries     = 0;
    debugJustParries = 0;
    debugExecutions  = 0;
    debugGuards      = 0;
    se::EnsureSource(scene);
}

inline void PlayerParryComponent::SetPhase(Phase phase, float seconds)
{
    m_phase = phase;
    m_timer = std::max(seconds, 0.0f);
    switch (phase) {
    case Phase::Idle:     debugPhase = "Idle";     break;
    case Phase::Window:   debugPhase = "Window";   break;
    case Phase::Guard:    debugPhase = "Guard";    break;
    case Phase::Recovery: debugPhase = "Recovery"; break;
    case Phase::Execute:  debugPhase = "Execute";  break;
    }
}

inline GameObject* PlayerParryComponent::RootOf(GameObject* part)
{
    if (part)
        if (const auto* hitbox = part->GetScript<BossPartComponent>())
            if (GameObject* owner = hitbox->BossRoot()) return owner;
    if (GameObject* root = BossHitboxRigComponent::BossRootOf(part)) return root;
    return SerpentHitboxRigComponent::SerpentRootOf(part);
}

inline GameObject* PlayerParryComponent::FindExecutablePart(GameObject*& outRoot) const
{
    outRoot = nullptr;

    /// @note とどめが通るのは «倒れている» 相手だけ。2 体立っている盤面で代表の 1 体を
    ///       見ると、倒れているのがもう片方のときにとどめが一切通らなくなる ─
    ///       部位ごとに持ち主を引いて、その持ち主が倒れているかで決める。
    ///
    ///       部位の当たり判定 (脚の膝下・蛇の節) の中で一番近いもの。部位はレンダラーを
    ///       持たないので、太さは部位が申告している hitRadius を足す。
    const Vector3 origin = transform.worldPosition;
    const float   reach  = std::max(executeRange, 0.1f);

    GameObject* best     = nullptr;
    GameObject* bestRoot = nullptr;
    float       bestDist = 0.0f;
    for (GameObject* object : scene.FindObjectsOfType<BossPartComponent>()) {
        if (!object || !object->activeInHierarchy()) continue;
        const auto* part = scene.GetScript<BossPartComponent>(object);
        if (!part || part->IsBroken() || !part->IsExecutable()) continue;
        GameObject*  root  = RootOf(object);
        const IBoss* owner = IBoss::Of(root);
        if (!owner || !owner->CanExecute()) continue;

        const Vector3 delta  = object->transform.worldPosition - origin;
        const float   radius = std::max(part->hitRadius, 0.0f);
        if (std::abs(delta.y) > std::max(executeHeight, 0.1f) + radius) continue;

        Vector3 flat = delta;
        flat.y = 0.0f;
        if (flat.Length() - radius > reach) continue;

        /// @note 選ぶのは 3 次元の近さ (2026-09-10)。届くかは水平で測る (脚の膝下は胸の高さにあり 3D だと立ち位置で届かなくなるため) が、それだけで «どれを斬るか» を決めると甲板の真下 5m の脚と目の前のコアが同じ «0m» になるため、届く的の中から 3D で一番近いものを選ぶ。
        const float distance = delta.Length() - radius;
        if (!best || distance < bestDist) {
            best     = object;
            bestRoot = root;
            bestDist = distance;
        }
    }
    outRoot = bestRoot;
    return best;
}

inline bool PlayerParryComponent::TryExecute()
{
    /// @note 構えている最中は破って出す。とどめは «倒れている 9 秒» にしか存在しない機会で、そこを «まずガードを離す» から始めさせない (BladeComponent の呼び出し側と同じ理由)。
    if (IsBusy() && m_phase != Phase::Guard) return false;

    GameObject* root = nullptr;
    GameObject* part = FindExecutablePart(root);
    if (!part || !root) return false;

    m_executePart     = EntityRef{ part->GetID() };
    m_executeRoot     = EntityRef{ root->GetID() };
    m_executeResolved = false;
    m_executeSwingPlayed = false;
    m_holdPlaying     = false;
    m_guardHit        = 0.0f;
    SetPhase(Phase::Execute, std::max(executeLock, 0.2f));
    if (auto* manager = TimeManagerComponent::Instance()) manager->EndParryRush();

    /// @note 斬る相手を向く。居合は正面へ振り下ろすので、向いていないと空を斬る。
    if (m_controller) {
        Vector3 to = part->transform.worldPosition - transform.worldPosition;
        to.y = 0.0f;
        if (to.LengthSq() > EPSILON) m_controller->RequestFacing(to);
    }

    PlayClip(executeClipFile, executeClipName, executeSpeed);

    /// @note 切断面へ寄るカットイン。居合の尺だけ預かり、もげたら返る。
    if (auto* camera = BossCameraDirectorComponent::Instance())
        camera->PlayAt(BossShot::Execute, part->transform.worldPosition);

    se::Play(audio, se::kSwordReady);
    if (auto* pad = RumbleManagerComponent::Instance()) pad->Rumble(0.4f, 0.2f, 0.10f);
    return true;
}

inline void PlayerParryComponent::StopBoth(float fade)
{
    if (!layerName.empty())  animator.StopSlot(layerName, fade);
    if (!layerNameB.empty() && layerNameB != layerName) animator.StopSlot(layerNameB, fade);
}

inline void PlayerParryComponent::PlayClip(const std::string& file, const std::string& name,
                                           float speed, bool loop)
{
    if (file.empty() || name.empty()) return;

    /// @note 乗せる先を入れ替える。1 枚に重ねるとハードカットになる (layerNameB フィールドの @note 参照)。
    if (!layerNameB.empty() && layerNameB != layerName) m_flip = !m_flip;

    const std::string& layer = ActiveLayer();
    if (layer.empty()) return;

    /// @note 前の相は «weight 0 で消す» のではなく fadeOut で抜けさせる。0 にすると、
    ///       2 枚に分けた意味が無くなって繋ぎがハードカットへ戻る。
    if (!layerName.empty()  && layerName  != layer) animator.StopSlot(layerName,  fadeOut);
    if (!layerNameB.empty() && layerNameB != layer) animator.StopSlot(layerNameB, fadeOut);

    animator.PlaySlot(layer, file, name, fadeIn, fadeOut, std::max(speed, 0.1f), loop);
}

inline void PlayerParryComponent::BeginGuard()
{
    /// @note 構えのループは «出来上がった時点» で既に回っている (OnUpdate の受け渡し)。
    ///       ここで鳴らし直さないのが肝 ─ 鳴らし直すと、そこでまた立ち上がりが 1 回出る。
    SetPhase(Phase::Guard, 0.0f);
    m_succeeded = false;
    m_guardHit  = 0.0f;
    if (!m_holdPlaying) PlayHoldLoop();
}

inline void PlayerParryComponent::BeginRelease()
{
    SetPhase(Phase::Recovery, recoverySeconds);
    m_guardHit    = 0.0f;
    m_holdPlaying = false;
    /// @note 構えを解くところまで見せる ─ 硬直を «止まっているだけ» にすると、
    ///       押した本人には «入力が消えた» としか映らない。
    PlayClip(releaseClipFile, releaseClipName, parrySpeed);
}

inline void PlayerParryComponent::BreakGuard()
{
    SetPhase(Phase::Recovery, std::max(guardBreakRecovery, 0.05f));
    m_succeeded   = false;
    m_guardHit    = 0.0f;
    m_holdPlaying = false;
    PlayClip(releaseClipFile, releaseClipName, parrySpeed);

    /// @note 受け止めた «ゴッ» と同じ音で、閃光も画角も付けない ── 押し負けた側の出来事なので、
    ///       手応えは «重い» だけでよい。崩しゲージは当然どちらにも動かない。
    if (auto* shake = CameraShakeManagerComponent::Instance())
        shake->Shake(Clamp01(guardShake) * 1.4f);
    if (auto* pad = RumbleManagerComponent::Instance()) pad->Rumble(0.7f, 0.4f, 0.18f);
    se::Play(audio, se::kSwordGuard, 0.7f);
}

inline void PlayerParryComponent::BeginParry()
{
    SetPhase(Phase::Window, std::max(windowSeconds, 0.05f));
    m_succeeded   = false;
    m_guardHit    = 0.0f;
    m_holdPlaying = false;
    m_toHold      = HandoffSeconds();

    PlayClip(parryClipFile, parryClipName, parrySnapSpeed);

    /// @note 構える «シャッ»。当たったかどうかとは別に、構えたこと自体を返す。
    se::Play(audio, se::kSwordReady);
    if (auto* pad = RumbleManagerComponent::Instance()) pad->Rumble(0.0f, 0.18f, 0.05f);
}

inline void PlayerParryComponent::OnParried(int amount, const Vector3* from)
{
    ++m_parries;
    debugParries = m_parries;

    const bool heavy = amount >= std::max(heavyDamageAt, 1);
    /// @note 窓の頭で受けたか。m_timer は窓の残りなので、経過 = 窓 − 残り。
    const float elapsed = std::max(windowSeconds, 0.05f) - std::max(m_timer, 0.0f);
    const bool  just    = justParrySeconds > 0.0f && elapsed <= justParrySeconds;
    if (just) {
        ++m_justParries;
        debugJustParries = m_justParries;
    }
    /// @note Just は手触りも一段深い。止めは同じ (これ以上は操作が重い) で、
    ///       戻りのスローと閃光と画角で «読み切った» を言う。
    const float feel = just ? 1.6f : 1.0f;

    /// @note 弾いた «場所»。刃は正面にあるので、自分から攻撃してきた側へ少し出た所。
    Vector3 toward = transform.worldRotation * Vector3::FORWARD;
    if (from) {
        Vector3 d = *from - transform.worldPosition;
        d.y = 0.0f;
        if (d.LengthSq() > EPSILON) toward = d.Normalized();
    }
    toward.y = 0.0f;
    toward = toward.NormalizedOr(Vector3::FORWARD);
    const Vector3 point = transform.worldPosition + toward * std::max(parryVfxForward, 0.0f)
                        + Vector3{ 0.0f, std::max(parryVfxHeight, 0.0f), 0.0f };

    GameObject* boss = Boss();
    if (auto* follow = CameraFollowManagerComponent::Instance()) follow->FrameParry(boss);

    /// @note 止め。世界を一瞬固め、当事者 2 体の芝居も固める ─ «噛み合った» が出るのは
    ///       刃と脚が同じ 1 コマで止まるからで、片方だけだと «すり抜けた» に見える。
    if (auto* stop = HitstopManagerComponent::Instance()) {
        stop->Hit(Clamp01(parryHitStop));
        stop->FreezeAnimation(scene.Self(), Clamp01(parryHitStop));
        if (boss) stop->FreezeAnimation(boss, Clamp01(parryHitStop));
    }
    /// @note 成功の停止が解けてから連撃時間を数える。演出スローの要求とは別枠で保持する。
    if (auto* timeManager = TimeManagerComponent::Instance()) {
        const float seconds = just ? rushJustSeconds : rushSeconds;
        if (seconds > 0.0f)
            timeManager->BeginParryRush(seconds, rushWorldScale, rushAttackSpeed);
        else if (parrySlowSeconds > 0.0f && parrySlowScale < 1.0f)
            timeManager->SlowFor(parrySlowScale, parrySlowSeconds * feel, 0.0f);
    }
    /// @note 閃光は白。画面全体を一瞬だけ持ち上げる。縁を暗くする Distort / Surge は使わない
    ///       (ビネットは «受けた» の語で、弾きは «防いだ» の語)。
    if (auto* screen = ScreenEffectManagerComponent::Instance())
        screen->Flash(parryFlashColor, Clamp01(parryFlash * feel),
                      std::max(parryFlashSeconds, 0.01f) * feel);
    if (auto* shake = CameraShakeManagerComponent::Instance())
        shake->Shake(Clamp01(parryShake) * (heavy ? 1.3f : 1.0f));
    /// @note 画角を弾いた側へ «開く»。刃と攻撃が触れた瞬間に画面が一瞬広がると、
    ///       止めの後に世界が戻る速さが «弾き返した» 勢いとして読める。
    if (parryFov > 0.0f)
        if (auto* follow = CameraFollowManagerComponent::Instance())
            follow->PunchFov(Clamp01(parryFov) * (heavy ? 1.6f : 1.0f) * feel);
    if (auto* pad = RumbleManagerComponent::Instance())
        pad->Rumble(heavy ? 0.6f : 0.3f, Clamp01(parryRumble), heavy ? 0.2f : 0.13f);
    if (auto* vfx = VfxManagerComponent::Instance())
        vfx->PlayParry(point, -toward, just ? 1.0f : (heavy ? 1.0f : 0.5f), just);

    /// @note 接触と澄んだ響きを専用素材にまとめ、Justでも斬撃音を重ねない。
    se::Play(audio, just ? se::kSwordParryJust : se::kSwordParry,
             heavy ? 1.0f : 0.85f);

    /// @note 息が戻る。読み切った弾きほど厚い ─ 上手いほど長く構えていられる。
    if (m_breath) m_breath->GainParry(just);

    /// @note 崩しを溜め、ボスへ «弾かれた» を渡す。脚が跳ね上がる・突進が転ぶのはボスの側。
    if (boss) {
        if (auto* brk = scene.GetScript<BossBreakComponent>(boss)) brk->AddParry(heavy, just);
        if (auto* iboss = IBoss::Of(boss)) iboss->OnParried(point);
    }

    /// @note 弾けたら短い硬直で次へ。窓の残りは捨てる (1 回の構えで 2 発は受けない)。
    SetPhase(Phase::Recovery, successRecovery);
    m_succeeded = true;
    /// @note 受け止めた «パキッ» はここで初めて絵になる。構えのまま硬直へ入ると、
    ///       止めも閃光も出ているのに体だけ何も起きていないように見える。
    m_holdPlaying = false;
    PlayClip(guardHitClipFile, guardHitClipName, parryRiposteSpeed);
}

inline void PlayerParryComponent::OnGuarded(int amount, const Vector3* from)
{
    ++m_guards;
    debugGuards = m_guards;

    /// @note 受け止めた «ゴッ»。弾きの «パキッ» とは別物として鳴らす (同じ手応えだと崩しが溜まっていないことに気付けないまま押しっぱなしが最適解になる)。
    /// @note 閃光・画角は出さない。あれは «読み切った» の語で、止めただけの一撃に乗せると弾きの報酬が «いつもの音» に薄まる。
    if (auto* stop = HitstopManagerComponent::Instance()) {
        stop->Hit(Clamp01(guardHitStop));
        stop->FreezeAnimation(scene.Self(), Clamp01(guardHitStop));
    }
    if (auto* shake = CameraShakeManagerComponent::Instance()) shake->Shake(Clamp01(guardShake));
    if (auto* pad = RumbleManagerComponent::Instance()) pad->Rumble(0.5f, 0.35f, 0.10f);
    se::Play(audio, se::kSwordGuard);

    (void)amount;
    (void)from;

    /// @note 崩しゲージは触らない。**ここが弾きとの唯一で最大の違い。**
    ///       受け止めた絵だけ差して、構えはそのまま続く。
    m_guardHit    = std::max(guardHitSeconds, 0.05f);
    m_holdPlaying = false;
    PlayClip(guardHitClipFile, guardHitClipName, parryRiposteSpeed);
}

inline void PlayerParryComponent::Cancel()
{
    if (m_phase != Phase::Window && m_phase != Phase::Guard && m_phase != Phase::Recovery)
        return;
    SetPhase(Phase::Idle, 0.0f);
    m_succeeded   = false;
    m_guardHit    = 0.0f;
    m_holdPlaying = false;
    /// @note 2 枚とも畳む。乗っている側だけ止めると、抜けかけのもう 1 枚が残って被さる。
    StopBoth(fadeOut);
}

inline void PlayerParryComponent::ResolveExecute()
{
    m_executeResolved = true;

    GameObject* part = m_executePart.Resolve(scene);
    GameObject* root = m_executeRoot.Resolve(scene);
    IBoss*      boss = root ? IBoss::Of(root) : nullptr;

    /// @note 起き上がられていたら空振り。斬り下ろしの絵は最後まで流す。
    if (!part || !boss || !boss->IsToppled() || !boss->Execute(part, transform.worldPosition)) {
        return;
    }

    ++m_executions;
    debugExecutions = m_executions;

    if (auto* stop = HitstopManagerComponent::Instance()) {
        stop->Hit(Clamp01(executeHitStop));
        stop->FreezeAnimation(scene.Self(), Clamp01(executeHitStop));
        stop->FreezeAnimation(root, Clamp01(executeHitStop));
    }
    if (executeSlowSeconds > 0.0f && executeSlowScale < 1.0f)
        if (auto* timeManager = TimeManagerComponent::Instance())
            timeManager->SlowFor(executeSlowScale, executeSlowSeconds, 0.02f);
    if (auto* shake = CameraShakeManagerComponent::Instance()) shake->Shake(Clamp01(executeShake));
    if (executeFov > 0.0f)
        if (auto* follow = CameraFollowManagerComponent::Instance())
            follow->PunchFov(Clamp01(executeFov));
    if (auto* pad = RumbleManagerComponent::Instance()) pad->Rumble(1.0f, 0.7f, 0.35f);
    if (auto* screen = ScreenEffectManagerComponent::Instance())
        screen->Flash(Vector4{ 1.0f, 1.0f, 1.0f, 1.0f }, 0.4f, 0.08f);
    se::Play(audio, se::kSwordExecute);
}

inline void PlayerParryComponent::DriveTempo(float dt)
{
    /// @note 速さを掛けるのは «今乗っている» 1 枚だけ。抜けかけのもう 1 枚は fadeOut に
    ///       任せる ─ 両方に掛けると、抜けていく側が速度を変えながら残って二重に見える。
    const std::string& layer = ActiveLayer();
    if (layer.empty() || !animator.IsSlotPlaying(layer)) return;

    switch (m_phase) {
    case Phase::Window:
        /// @note 構えへ駆け上がる間だけ速さを掛ける。ループへ渡した後に掛けると
        ///       «構えが揺れる» ─ 止まっているべき姿勢が回り続けることになる。
        if (!m_holdPlaying) animator.SetSlotSpeed(layer, parrySnapSpeed);
        break;

    case Phase::Guard:
        /// @note 構えのループは等速。受け止めの絵 (GuardHit) を出している間だけ、
        ///       その速さを保つ。ここで速度を掛け直すと «構えが揺れる» に見える。
        break;

    case Phase::Recovery:
        animator.SetSlotSpeed(layer, m_succeeded ? parryRiposteSpeed : parrySpeed);
        break;

    case Phase::Execute: {
        const float lock    = std::max(executeLock, 0.2f);
        const float speed   = std::max(executeSpeed, 0.1f);
        const float hitAt   = std::max(executeHitTime, 0.01f) / speed;
        const float elapsed = lock - m_timer;
        if (elapsed < hitAt) {
            /// @note 斬り下ろしの時刻 (ResolveExecute) にクリップの斬り下ろしがちょうど来る。
            const float target = executeHitTime
                * tempo::WindupProgress(elapsed / hitAt, executeWindup, executeWindupPower);
            animator.SetSlotSpeed(layer, tempo::SpeedToReach(
                target, animator.GetSlotTime(layer), dt, speed));
        } else {
            animator.SetSlotSpeed(layer, speed * tempo::FollowThrough(
                elapsed - hitAt, 1.0f, executeFollowEnd, 0.35f));
        }
        break;
    }

    case Phase::Idle:
        break;
    }
}

inline void PlayerParryComponent::DriveRushHud()
{
    auto* manager = TimeManagerComponent::Instance();
    const bool show = manager && manager->IsParryRush() && !manager->IsPaused();
    GameObject* label = scene.Find("HUD_ParryRush");
    GameObject* fill = scene.Find("HUD_ParryRushFill");
    if (label) label->SetActive(show);
    if (fill) fill->SetActive(show);
    if (!show) { m_rushHudPulse = 0.0f; return; }

    std::string binding;
    const auto* settings = GameSettingsComponent::Instance();
    const bool pad = settings ? settings->Input().device == 1 : input.IsPadConnected();
    for (int i = 0; i < input.GetActionBindingCount(actions::kAttack); ++i) {
        const auto candidate = input.GetActionBinding(actions::kAttack, i);
        if (candidate.IsValid() && (candidate.source == 2) == pad) {
            binding = input.DescribeActionBinding(actions::kAttack, i);
            break;
        }
    }
    m_rushHudPulse = input.GetActionDown(actions::kAttack) ? 0.12f
        : std::max(m_rushHudPulse - time.UnscaledDeltaTime(), 0.0f);
    const float pulse = 0.82f + 0.18f * Clamp01(m_rushHudPulse / 0.12f);
    if (binding.empty()) binding = "攻撃";
    if (label) {
        ui.SetText(label, "連撃チャンス  /  攻撃 連打  [ " + binding + " ]");
        ui.SetTextColor(label, {0.55f * pulse, 1.0f, 0.80f * pulse, 1.0f});
    }
    if (fill) ui.SetImageFillAmount(fill, manager->ParryRush01());
}

inline void PlayerParryComponent::OnUpdate()
{
    if (!enabled) return;
    if (auto* manager = TimeManagerComponent::Instance(); manager && manager->IsPaused()) {
        /// @note Controller が前フレームの拘束要求を消費し終えるまで、解除扱いにしない。
        m_rushResumeFrames = 2;
        DriveRushHud();
        return;
    }
    const float dt = TimeManagerComponent::PlayerDeltaTime();

    /// @note 登攀のように数秒またぐ拘束は cutscene には乗らない (BladeComponent と同じ理由)。
    const bool held = cutscene::HoldsPlayer(Time::unscaledTime)
                   || (m_controller && m_controller->IsInputLocked());
    const bool dodging = m_controller && m_controller->IsDodging();
    const bool resuming = m_rushResumeFrames > 0;
    if (resuming) --m_rushResumeFrames;
    if ((held && !resuming) || m_phase == Phase::Execute || !Boss())
        if (auto* manager = TimeManagerComponent::Instance()) manager->EndParryRush();
    DriveRushHud();

    /// @note 回避は弾きより優先する。構えていても硬直中でも、転がった瞬間に畳む。
    if (dodging) Cancel();

    if (held) m_buffer = 0.0f;
    else if (input.GetActionDown(actions::kParry))
        m_buffer = std::max(parryBufferSeconds, 1.0e-4f);

    if (m_buffer > 0.0f && !IsBusy() && !dodging) {
        m_buffer = 0.0f;
        /// @note 倒れている相手が届く所に居れば とどめ。居なければ弾き。
        /// @note とどめには息が要らない。転倒の 9 秒は見返りの側にある機会で、«息が足りなくて逃した» にすると攻めた結果が攻める資源に阻まれる。息が縛るのは «守り» だけ。
        if (!TryExecute()) {
            if (m_breath && !m_breath->CanAct()) m_breath->Deny();
            else                                 BeginParry();
        }
    }
    m_buffer = std::max(0.0f, m_buffer - dt);

    if (m_phase == Phase::Idle) return;

    /// @note ボタンを押し続けているか。拘束中は «離した» 扱いにして、演出が明けたときに
    ///       構えたままにならないようにする。
    const bool stillHeld = !held && input.GetAction(actions::kParry);

    /// @note 構えている間は足を鈍らせる。全速で走りながら弾けると «受けた» に見えない。
    if (m_controller) m_controller->RequestMoveSpeedScale(std::max(moveScale, 0.05f));

    /// @note 構えは攻撃してきた側へ向く。向いていない弾きは «偶然当たった» に見える。
    if (m_phase == Phase::Window && m_controller)
        if (GameObject* boss = Boss()) {
            /// @note 蛇はルートが原点に据え置きで、動くのは胴だけ。ルートの位置へ向くと
            ///       «攻撃してきた側» ではなく盤面の決まった一点を向く (IBoss::FocusPoint)。
            Vector3 at = boss->transform.worldPosition;
            if (const IBoss* iboss = IBoss::Of(boss)) (void)iboss->FocusPoint(at);
            Vector3 to = at - transform.worldPosition;
            to.y = 0.0f;
            if (to.LengthSq() > EPSILON) m_controller->RequestFacing(to);
        }

    m_timer -= dt;

    /// @note 構えが出来上がったらループへ渡す。相ではなく時刻で切り替えるのが要点。ワンショットの Slot はクリップ末尾の fadeOut 秒に達した瞬間に自動で抜け始めるため (AnimatorSystem)、窓 (0.22 秒) より短いまま待つと構えが一度消えて次のクリップでまた立ち上がる («同じモーションが 2 回» の正体)。抜け始める前にループを被せれば以後は姿勢が動かない。
    if (!m_holdPlaying && m_guardHit <= 0.0f
        && (m_phase == Phase::Window || m_phase == Phase::Guard)) {
        m_toHold -= dt;
        if (m_toHold <= 0.0f) PlayHoldLoop();
    }

    switch (m_phase) {
    case Phase::Window:
        if (m_timer <= 0.0f) {
            /// @note 窓が閉じた。まだ押しているならガードへ落ちる ─ 止められるが崩しは
            ///       溜まらない。離していれば «外した» なので構えを解く。
            if (holdToGuard && stillHeld) {
                BeginGuard();
            } else {
                /// @note 外した構えにだけ息を払わせる。弾けた構えは見返りの側なので取らない
                ///       (PlayerTuning の Breath > 空振りの消費)。
                if (m_breath) m_breath->SpendParryWhiff();
                BeginRelease();
            }
        }
        break;

    case Phase::Guard:
        /// @note 構え続けるには息が要る。尽きたら押し負けて割れる。
        if (m_breath && m_breath->DrainGuard(dt)) {
            BreakGuard();
            break;
        }
        /// @note 受け止めた絵が明けたら構えへ戻す。戻さないと GuardHit の最終コマで固まる。
        if (m_guardHit > 0.0f) {
            m_guardHit -= dt;
            if (m_guardHit <= 0.0f && stillHeld) PlayHoldLoop();
        }
        if (!stillHeld) BeginRelease();
        break;

    case Phase::Recovery:
        if (m_timer <= 0.0f) SetPhase(Phase::Idle, 0.0f);
        break;

    case Phase::Execute: {
        const float lock = std::max(executeLock, 0.2f);
        const float hitAt = std::max(executeHitTime, 0.01f) / std::max(executeSpeed, 0.1f);
        const float elapsed = lock - m_timer;
        if (!m_executeSwingPlayed && elapsed >= hitAt - se::SWORD_SWING_PEAK_SECONDS) {
            m_executeSwingPlayed = true;
            se::Play(audio, se::kSwordSwingSlide);
        }
        if (!m_executeResolved && elapsed >= hitAt) ResolveExecute();
        if (m_timer <= 0.0f) {
            /// @note とどめのクリップは 2 秒超ある。斬り抜けが済んだら残心を待たずに畳む。
            StopBoth(fadeOut);
            SetPhase(Phase::Idle, 0.0f);
        }
        break;
    }

    case Phase::Idle:
        break;
    }

    DriveTempo(dt);
}

} // namespace sandbox
