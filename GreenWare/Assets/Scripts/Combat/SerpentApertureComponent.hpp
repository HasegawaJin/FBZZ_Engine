/// @file    SerpentApertureComponent.hpp
/// @brief   床の 16 口の開閉と予兆。付ける先は Boss02_Arena_Map (口は場の設備)
/// @author  Hasegawa Jin
/// @date    2026-08-31
///
/// @note 16 口は共有クリップでなく口ごとの進み 1 float で開閉を表す (進みを逆に戻す
///       だけで反転でき、DLL リロードでも消えない。曲線は `ScriptTweenProxy::Evaluate`)。
///       位置は輪の半径・口数・位相の式から解く ─ `ARENA_Rim_<口>` は取り込み時に変換を
///       頂点へ焼かれ Transform が全口原点。既定値は実測 (I1=(8,·,0)/O1=(15.2169,·,
///       4.9443)) と一致。`serpent_map_build.py` の RINGS を直したら要追随。
#pragma once

#include <Engine/Scene/Components/ColliderComponent.hpp>
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include <Engine/Scene/EntityRef.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Engine/Scene/ScriptProxy/ScriptTweenProxy.hpp>
#include <Math/MathUtils.hpp>
#include <Scripts/Combat/PlayerHit.hpp>
#include <Scripts/Game/CombatManagerComponent.hpp>
#include <Scripts/Game/VfxManagerComponent.hpp>
#include <Scripts/Player/PlayerComponent.hpp>
#include <Scripts/Utils/GlowMaterial.hpp>
#include <Scripts/Utils/BladeColors.hpp>
#include <Scripts/Utils/SeLibrary.hpp>
#include <Scripts/Utils/WeaponSockets.hpp>
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using namespace fbzz::scene;
using namespace fbzz::math;
using fbzz::Time;

namespace sandbox {

class SerpentApertureComponent : public Script {
    FBZZ_SCRIPT(SerpentApertureComponent)

public:
    /// 30fps 前提のフレーム数を秒へ直した値 (boss-serpent.md「開口の動き」)。
    FBZZ_GROUP("タイミング")
    FBZZ_FIELD_RANGE(float, telegraphSeconds, 0.60f, "予告", 0.0f, 3.0f)
    FBZZ_TOOLTIP("縁が灯りきるまで。18F。«ここが開く» を読む時間そのもの")
    FBZZ_FIELD_RANGE(float, unlockSeconds, 0.20f, "Unlock", 0.0f, 2.0f)
    FBZZ_TOOLTIP("カラーが半ピッチ回って歯が外れるまで。6F")
    FBZZ_FIELD_RANGE(float, sinkSeconds, 0.13f, "沈み", 0.0f, 2.0f)
    FBZZ_TOOLTIP("羽が床下へ抜けるまで。4F")
    FBZZ_FIELD_RANGE(float, slideSeconds, 0.27f, "滑り", 0.0f, 2.0f)
    FBZZ_TOOLTIP("羽が外へ滑りきるまで。8F")

    FBZZ_GROUP("動き")
    FBZZ_FIELD_RANGE(float, lockDegrees, 22.5f, "Collar Turn", 0.0f, 45.0f)
    FBZZ_TOOLTIP("カラーの回転。歯 8 枚 45 度おきの半ピッチで爪の隙間へ来る")
    FBZZ_FIELD_RANGE(float, sinkMeters, 0.30f, "沈み", 0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, slideMeters, 2.53f, "滑り", 0.0f, 5.0f)
    FBZZ_TOOLTIP("羽が外へ逃げる距離。口の法線 (中心から見た放射) に沿う")

    FBZZ_GROUP("予告")
    FBZZ_FIELD_RANGE(float, glowStrength, 6.0f, "発光", 0.0f, 20.0f)
    FBZZ_TOOLTIP("開く口の縁の自発光。0 だと予兆が出ないので、口が突然開く。"
                 "ブルームのしきい値 (Default.fzdata で 4.0) を越える値にしておくと、"
                 "予兆だけが滲んで «そこが開く» が視界の端でも読める")
    FBZZ_FIELD_RANGE(float, glowIdle, 1.2f, "Glow (idle)", 0.0f, 10.0f)
    FBZZ_TOOLTIP("閉じている口の縁の明るさ。0 にすると床の 16 口が完全に消えて、"
                 "«どこが開きうるか» が予兆の瞬間まで分からない。"
                 "既定は M_AR_RimGlow の emissiveScale と同じ値")
    FBZZ_FIELD_COLOR(glowColor, (Vector4{ 1.00f, 0.62f, 0.16f, 1.0f }), "Glow Color")
    FBZZ_TOOLTIP("中立の琥珀。極の赤青を使うと «その口が帯電している» と読まれる")
    FBZZ_FIELD_RANGE(float, glowPulseHz, 4.0f, "脈動の周波数 [Hz]", 0.0f, 16.0f)
    FBZZ_TOOLTIP("灯りきるまでの明滅。0 で滑らかに上がるだけ")

    /// 輪の作り。既定はビルダー (serpent_map_build.py の RINGS) と同じ。
    FBZZ_GROUP("Rings")
    FBZZ_FIELD_RANGE_INT(int, innerCount, 6, "Inner Count", 1, 24)
    FBZZ_FIELD_RANGE(float, innerRadius, 8.0f, "Inner Radius", 1.0f, 40.0f)
    FBZZ_FIELD_RANGE(float, innerPhaseDegrees, 0.0f, "Inner Phase", -180.0f, 180.0f)
    FBZZ_FIELD_RANGE_INT(int, outerCount, 10, "Outer Count", 1, 32)
    FBZZ_FIELD_RANGE(float, outerRadius, 16.0f, "Outer Radius", 1.0f, 40.0f)
    FBZZ_FIELD_RANGE(float, outerPhaseDegrees, 18.0f, "Outer Phase", -180.0f, 180.0f)
    FBZZ_TOOLTIP("外輪は内輪の口の間へ来るよう半ピッチずらしてある (π/10)")

    /// 口は «場の設備» なので、動くたびに機械の音と埃を返す。
    /// @note 段ごとに別の音を鳴らす: 開くまでの 1.2 秒はほぼ予兆で、画面外の口でも
    ///       段の進みが歯・羽・閉じ切りの音で耳から読める。
    FBZZ_GROUP("手触り")
    FBZZ_FIELD(bool, playSound, true, "Sound")
    FBZZ_FIELD_RANGE(float, soundVolume, 0.85f, "Volume", 0.0f, 2.0f)
    FBZZ_FIELD(bool, playDust, true, "Dust")
    FBZZ_TOOLTIP("羽が滑り始めた瞬間に縁から埃を吹く。開口が «床を割って開いた» に見える")
    FBZZ_FIELD_RANGE(float, dustStrength, 0.7f, "Dust Strength", 0.0f, 1.0f)
    FBZZ_FIELD(bool, alternateCollar, true, "Alternate Collar")
    FBZZ_TOOLTIP("隣り合う口でカラーの回る向きを逆にする。16 個が同じ向きに回ると"
                 "«1 つの仕掛けのコピー» に見える")

    /// 胴が口を «通っている» 間の絵。
    /// @note 羽の埃は滑り出した 1 フレームだけで、11m の胴の出入り (約 7 秒ごと、戦闘中
    ///       最頻の動作) には出ない。«縁の位置» を知るのは口側だけなのでここで持つ
    ///       (AI へ書くと口を増やすたびに演出側も直すことになる)。
    FBZZ_GROUP("Passage")
    FBZZ_FIELD(bool, passageDust, true, "Body Dust")
    FBZZ_TOOLTIP("胴が縁を出入りしている間、口から土煙を吹く")
    FBZZ_FIELD_RANGE(float, passageInterval, 0.20f, "間隔", 0.02f, 1.0f)
    FBZZ_TOOLTIP("通っている間に吹く刻み [秒]。詰めると口が煙で埋まって胴が見えない。"
                 "枠の勘定も要る ─ 渡っている最中は 2 口が同時に鳴るので、"
                 "«2 ÷ ここ» が VfxManager の Geyser Slots ÷ 1.05 秒 を超えないこと")
    FBZZ_FIELD_RANGE(float, passageStrength, 0.75f, "強度", 0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, passageBurst, 1.0f, "バースト", 0.0f, 2.0f)
    FBZZ_TOOLTIP("«通り始めた 1 発» の強さの倍率。頭が縁を割る瞬間だけ一段強くする")

    FBZZ_GROUP("Collision")
    FBZZ_FIELD(bool, driveShutterCollision, true, "Drive Shutter Floor")
    FBZZ_TOOLTIP("閉じている間だけ COL_Shutter_<口> を床として有効にする。"
                 "切ると 16 口が最初から穴になり、乗ると落ちる")

    /// 開いた口へプレイヤーが落ちたときの後始末。
    /// @note 即死にしない (2026-09-11): HP は 5 で踏みつけ2/突進3/突き上げ1の前提で
    ///       組んであり、無条件即死を混ぜると他攻撃の目盛りが壊れる。落下はダメージ
    ///       のみ扱う。
    /// @note 坑は半径2.2m・深さ7mの四方壁で自力脱出不可 (2026-09-11 まで受け皿無し)
    ///       のため引き上げが必須。
    FBZZ_GROUP("落ちたとき")
    FBZZ_FIELD(bool, rescueFallen, true, "落ちたら引き上げる")
    FBZZ_TOOLTIP("開いた口へ落ちたプレイヤーへダメージを入れ、床へ引き上げる。"
                 "切ると縦坑の底に取り残される (出る手段は無い)")
    FBZZ_FIELD_TAG(playerTag, "Player", "プレイヤーのタグ")
    FBZZ_FIELD_RANGE(float, holeRadius, 2.2f, "開口の半径 [m]", 0.5f, 6.0f)
    FBZZ_TOOLTIP("戻す先を選ぶのに使う。ARENA_Rim の実寸に合わせること")
    FBZZ_FIELD_RANGE(float, fallY, -1.6f, "落下と見なす高さ [m]", -20.0f, 2.0f)
    FBZZ_TOOLTIP("床 (0) からこれより下へ行ったら «落ちた»。羽の沈み (-0.30m) より"
                 "深く取らないと、開きかけの床で誤爆する")
    FBZZ_FIELD_RANGE(float, floorY, 0.0f, "床の高さ [m]", -10.0f, 10.0f)
    FBZZ_FIELD_RANGE_INT(int, fallDamage, 2, "ダメージ", 0, 10)
    FBZZ_TOOLTIP("叩きつけ・踏みつけと同格に置く。体力 5 なので **3 回落ちると死ぬ** ─ "
                 "緊張は残しつつ、蛇が作った状況で一瞬で終わることはない")
    FBZZ_FIELD_RANGE(float, rescueSeconds, 0.60f, "引き上げ [s]", 0.05f, 3.0f)
    FBZZ_FIELD_RANGE(float, rescueArc, 1.40f, "引き上げの山 [m]", 0.0f, 6.0f)
    FBZZ_TOOLTIP("引き上げる軌道の山の高さ。0 だと坑の壁を斜めに突き抜けて上がる")
    FBZZ_FIELD_RANGE(float, rescueMargin, 1.30f, "縁からの余白 [m]", 0.0f, 5.0f)
    FBZZ_TOOLTIP("戻す先を縁からどれだけ離すか。0 だと縁ちょうどに置かれ、"
                 "着地の 1 歩でまた落ちる")
    FBZZ_FIELD_RANGE(float, arenaRadius, 20.0f, "闘技場の実効半径 [m]", 1.0f, 60.0f)
    FBZZ_TOOLTIP("戻す先がここより外なら別の向きを探す。壁の中へ置かないための枠")

    FBZZ_GROUP("デバッグ")
    FBZZ_FIELD_READ_ONLY(std::string, debugRescue, "-", "引き上げ")
    FBZZ_FIELD_READ_ONLY(std::string, debugOpen, "-", "Open")
    FBZZ_FIELD_READ_ONLY(int, debugHoles, 0, "穴")
    FBZZ_FIELD_READ_ONLY(int, debugMissing, 0, "Missing Nodes")
    FBZZ_FIELD(bool, drawHoles, false, "Draw Holes")
    FBZZ_TOOLTIP("口の中心と半径を線で出す。位置が合っているかの確認用")

    /// 予兆から開くところまで通す。既に開いていれば何もしない。
    void Open(const std::string& hole);
    /// 逆順に閉じる。
    void Close(const std::string& hole);
    void CloseAll();
    [[nodiscard]] bool IsOpen(const std::string& hole) const;
    /// 予兆も含めて «今この口で何か起きているか»。
    [[nodiscard]] bool IsBusy(const std::string& hole) const;
    /// 開き具合 [0,1]。1 = 完全に開いた。
    [[nodiscard]] float OpenRatio(const std::string& hole) const;

    /// 口の中心 (ワールド)。床面の高さで返す。
    [[nodiscard]] Vector3 HoleCenter(const std::string& hole) const;
    /// 口の id 一覧 ("I1".."O10")。
    [[nodiscard]] const std::vector<std::string>& Holes() const { return m_ids; }
    /// 2 口の間隔 [m]。経路が組めるかの判定に使う。
    [[nodiscard]] float HoleDistance(const std::string& a, const std::string& b) const;
    /// 点に最も近い口。盤面に口が無ければ空。
    [[nodiscard]] std::string NearestHole(const Vector3& point) const;

    /// この口を «今、胴が通っている»。AI が経路の口ごとに毎フレーム申告する。
    /// @note 押し込む形にする: 弧長の突き合わせは経路を持つ AI だけができる。押されな
    ///       かった口は自然に止まる (SetAim / SetUndulationScale と同じ約束)。
    void ReportPassage(const std::string& hole);

    /// 今プレイヤーを引き上げている最中か。演出・AI が «触るな» を読むための窓。
    [[nodiscard]] bool IsRescuing() const { return m_rescueTime >= 0.0f; }

    void OnStart() override;
    void OnUpdate() override;

private:
    /// 落ちたプレイヤーを床へ戻す。毎フレーム。
    void DriveFallRescue(float dt);
    /// 引き上げ先。落ちた口の «縁の外» で、他のどの開いた口にも掛からない点。
    [[nodiscard]] Vector3 SafeSpot(const Vector3& from) const;
    /// その点が «開いている / 開きかけの» 口に掛かっているか。
    [[nodiscard]] bool OverOpenHole(const Vector3& at, float margin) const;

    /// 口 1 つぶんの状態。
    struct Hole {
        std::string id;
        /// ローカル (メッシュ) 空間での中心。輪の式から解く。
        Vector3     center{};
        /// 口の «法線»。輪の接線に直交する水平で、中心を向く。羽はこの向きへ逃げる。
        Vector3     normal{ 1.0f, 0.0f, 0.0f };
        /// 開閉の進み [0, Total()]。目標へ向かって実時間で進む。
        float       phase   = 0.0f;
        /// 前フレームの進み。段をまたいだ瞬間を 1 度だけ拾うため保持 (閾値の大小比較
        /// だけだと開いている間ずっと鳴り続ける)。
        float       lastPhase = 0.0f;
        bool        wantOpen = false;
        /// カラーの回る向き。隣どうしで逆にする。
        float       spin    = 1.0f;
        /// 参照は DLL リロードで空へ戻る。空なら名前で引き直す。
        EntityRef   rim;
        EntityRef   collar;
        EntityRef   leafA;
        EntityRef   leafB;
        EntityRef   floorCollision;
        /// COL_Shutter へ床を生やしたか。生やす対象はシーンに保存されない。
        bool        collisionReady = false;
        /// 最後に書いた床の有効/無効。SetActive を毎フレーム叩かないための札。
        bool        floorActive = true;
        /// 今フレーム «胴が通っている» と申告されたか。押されなければ止まる。
        bool        passing     = false;
        bool        wasPassing  = false;
        /// 次に土煙を吹くまでの残り [秒]。
        float       passageTimer = 0.0f;
    };

    [[nodiscard]] float TelegraphEnd() const { return Max(telegraphSeconds, 0.0f); }
    [[nodiscard]] float UnlockEnd()    const { return TelegraphEnd() + Max(unlockSeconds, 0.0f); }
    [[nodiscard]] float SinkEnd()      const { return UnlockEnd() + Max(sinkSeconds, 0.0f); }
    [[nodiscard]] float Total()        const { return SinkEnd() + Max(slideSeconds, 0.0f); }

    void  BuildHoles();
    void  ResolveNodes(Hole& hole);
    void  ApplyHole(Hole& hole);
    /// 段をまたいだ «瞬間» に音と埃を返す。
    void  ReportStages(const Hole& hole);
    /// 胴が通っている間の土煙。通り始めの 1 発だけ強くする。
    void  TickPassage(Hole& hole, float dt);
    void  EnsureShutterFloor(Hole& hole);
    [[nodiscard]] Hole*       Find(const std::string& id);
    [[nodiscard]] const Hole* Find(const std::string& id) const;
    /// 親のスケールを割り戻す。取り込みの unit scale がルートに残っていても
    /// 移動量がメートルのままになる。
    [[nodiscard]] Vector3 ToLocalOffset(const Vector3& worldOffset) const;

    std::vector<Hole>        m_holes;
    std::vector<std::string> m_ids;
    /// 輪の作りが変わったら組み直すための札。
    float m_builtSignature = -1.0f;

    /// 引き上げの経過 [秒]。負なら引き上げていない。
    float   m_rescueTime = -1.0f;
    Vector3 m_rescueFrom;
    Vector3 m_rescueTo;
};

FBZZ_REFLECT(SerpentApertureComponent)

inline void SerpentApertureComponent::OnStart()
{
    m_builtSignature = -1.0f;
    BuildHoles();
    /// @note 口は盤面のあちこちにある。どの方向で何が起きたかが分かる必要があるので 3D。
    se::EnsureSource(scene, "SE", 1.0f);
}

inline bool SerpentApertureComponent::OverOpenHole(const Vector3& at, float margin) const
{
    const float reach = Max(holeRadius, 0.1f) + Max(margin, 0.0f);
    for (const Hole& hole : m_holes) {
        /// @note 閉じきっている口は «床»。開いている口と、開閉の途中 (羽が沈んでいる) は避ける。
        if (!IsOpen(hole.id) && !IsBusy(hole.id)) continue;
        const Vector3 c = HoleCenter(hole.id);
        if (Vector3{ at.x - c.x, 0.0f, at.z - c.z }.Length() <= reach) return true;
    }
    return false;
}

inline Vector3 SerpentApertureComponent::SafeSpot(const Vector3& from) const
{
    const Vector3 center = transform.worldPosition;
    const float   lift   = floorY + 0.05f;
    const Vector3 flat{ from.x, lift, from.z };

    const std::string hole = NearestHole(flat);
    if (hole.empty()) return flat;

    const Vector3 mouth = HoleCenter(hole);
    const float   out   = Max(holeRadius, 0.1f) + Max(rescueMargin, 0.0f);

    /// @note 8 方向を試し、壁の内側かつ他の開いた口に掛からない最初の点を採る。内輪の
    ///       口は隣と 8.0m しかなく、決め打ちの1方向だと突き上げの隣口の上に置いて
    ///       次の1歩でまた落ちる。
    for (int i = 0; i < 8; ++i) {
        const float   angle = TWO_PI * static_cast<float>(i) / 8.0f;
        const Vector3 at{ mouth.x + std::cos(angle) * out, lift,
                          mouth.z + std::sin(angle) * out };
        if (Vector3{ at.x - center.x, 0.0f, at.z - center.z }.Length() > Max(arenaRadius, 1.0f))
            continue;
        if (OverOpenHole(at, Max(rescueMargin, 0.0f) * 0.5f)) continue;
        return at;
    }

    /// @note 8 方向とも塞がっている。場の中心は内輪 (r=8) の内側なので口が無い ─
    ///       どこにも置けないときの最後の床になる。
    return Vector3{ center.x, lift, center.z };
}

inline void SerpentApertureComponent::DriveFallRescue(float dt)
{
    if (!rescueFallen) { debugRescue = "Off"; return; }

    GameObject* player = scene.FindWithTag(playerTag, true);
    if (!player) { debugRescue = "No player"; return; }

    if (m_rescueTime >= 0.0f) {
        m_rescueTime += dt;
        const float t = Clamp01(m_rescueTime / Max(rescueSeconds, 0.05f));

        /// @note 拘束は毎フレーム言い直す (RequestSuspend は1フレームぶんの要求)。
        ///       PlayerControllerComponent は PlayerComponent の内部メンバーで別スクリプト
        ///       として載らないため `scene.GetScript<PlayerControllerComponent>()` は空。
        if (auto* control = scene.GetScript<PlayerComponent>(player))
            control->RequestSuspend(true);
        physics.SetVelocity(player, Vector3::ZERO);

        Vector3 at = Vector3::Lerp(m_rescueFrom, m_rescueTo, t);
        /// @note 縦は山を描く。直線だと坑の壁を斜めに突き抜けて上がる。
        at.y += std::sin(t * PI) * Max(rescueArc, 0.0f);
        /// @note position も書く: worldPosition だけだと次の PrePhysics が local から
        ///       組み直した時点で捨てられる (プレイヤーを動かす際の共通の落とし穴)。
        player->transform.position      = at;
        player->transform.worldPosition = at;

        if (t >= 1.0f) { m_rescueTime = -1.0f; debugRescue = "-"; }
        else           { debugRescue = "Lifting"; }
        return;
    }

    if (player->transform.worldPosition.y > fallY) { debugRescue = "-"; return; }

    m_rescueFrom = player->transform.worldPosition;
    m_rescueTo   = SafeSpot(m_rescueFrom);
    m_rescueTime = 0.0f;
    debugRescue  = "Lifting";

    /// @note 押し (source) は渡さない: 殴られた向きへ流す仕組みで、引き上げの軌道と
    ///       喧嘩する。落下は向きを持たない出来事としてダメージのみ入れる。
    if (auto* combat = CombatManagerComponent::Instance())
        (void)combat->HitPlayer(player, std::max(fallDamage, 0), nullptr,
                                PlayerHitKind::Unblockable);

    se::Play(audio, se::kImpactHeavy, 0.8f);
    if (auto* vfx = VfxManagerComponent::Instance())
        vfx->PlayGroundDust(m_rescueTo, Vector3::UP, 0.8f, 1.4f);
}

inline void SerpentApertureComponent::ReportStages(const Hole& hole)
{
    /// @note 上りと下りで別の段を報告する。開くのは 3 段の機械音、閉じるのは «噛んだ» 1 発。
    const auto rose    = [&](float at) { return hole.lastPhase < at && hole.phase >= at; };
    const auto fellTo0 = hole.lastPhase > 0.0f && hole.phase <= 0.0f;

    const Vector3 center = HoleCenter(hole.id);

    if (playSound) {
        /// @note 歯が外れる。予兆の «終わり» を告げる音なので、ここが一番耳を引く必要がある。
        if (rose(TelegraphEnd())) se::PlayAt(audio, se::kImpactLight, center, soundVolume);
        /// @note 羽が滑り出す。
        if (rose(SinkEnd()))      se::PlayAt(audio, se::kImpactDebris, center, soundVolume);
        /// @note 閉じ切って噛む。開くときより重く鳴らして «もう通れない» を返す。
        if (fellTo0)              se::PlayAt(audio, se::kImpactMid, center, soundVolume);
    }

    /// @note 埃は滑り出しの 1 度だけ。開いている間ずっと吹くと «煙が出ている穴» になる。
    if (playDust && rose(SinkEnd())) {
        if (auto* vfx = VfxManagerComponent::Instance()) {
            /// @note 床が押し退けられた側 ─ つまり羽が逃げていく向きへ吹かせる。
            vfx->PlayGroundDust(center, hole.normal, Clamp01(dustStrength), 1.4f);
            vfx->PlayGroundDust(center, -hole.normal, Clamp01(dustStrength), 1.4f);
        }
    }
}

inline void SerpentApertureComponent::ReportPassage(const std::string& hole)
{
    if (Hole* found = Find(hole)) found->passing = true;
}

inline void SerpentApertureComponent::TickPassage(Hole& hole, float dt)
{
    /// @note 押されなかったフレームは «通っていない»。次に通り始めたときへ向けて畳む。
    const bool passing = hole.passing;
    hole.passing = false;

    if (!passageDust || !passing) {
        hole.wasPassing  = passing;
        hole.passageTimer = 0.0f;
        return;
    }

    /// @note 通り始めの 1 発は «頭が縁を割った» 瞬間なので、続きの刻みより強く出す。
    const bool first = !hole.wasPassing;
    hole.wasPassing = true;

    hole.passageTimer -= dt;
    if (!first && hole.passageTimer > 0.0f) return;
    hole.passageTimer = Max(passageInterval, 0.02f);

    if (auto* vfx = VfxManagerComponent::Instance()) {
        const float strength = Clamp01(passageStrength) *
                               (first ? Max(passageBurst, 0.0f) : 1.0f);
        vfx->PlaySerpentMouth(HoleCenter(hole.id), Clamp01(strength));
    }
    if (first && playSound)
        se::PlayAt(audio, se::kImpactDebris, HoleCenter(hole.id), soundVolume * 0.7f);
}

inline void SerpentApertureComponent::BuildHoles()
{
    /// @note 輪の作りを 1 つの値に畳んで «変わったか» を見る。Inspector で半径を触った
    ///       ときにその場で並び直ってほしいので、開始時だけの組み立てにはしない。
    const float signature = static_cast<float>(innerCount) * 1000.0f + innerRadius * 7.0f +
                            innerPhaseDegrees * 0.5f + static_cast<float>(outerCount) * 13.0f +
                            outerRadius * 11.0f + outerPhaseDegrees * 0.25f;
    if (std::fabs(signature - m_builtSignature) < 1.0e-4f && !m_holes.empty()) return;
    m_builtSignature = signature;

    /// @note 開き途中の口は覚えておく。半径を触っただけで開いていた口が閉じると、
    ///       調整中に蛇が床へ埋まる。
    std::vector<std::pair<std::string, float>> previous;
    for (const Hole& hole : m_holes) previous.emplace_back(hole.id, hole.phase);

    m_holes.clear();
    m_ids.clear();

    struct Ring { const char* tag; int count; float radius; float phase; };
    const Ring rings[2] = {
        { "I", std::max(innerCount, 1), innerRadius, ToRad(innerPhaseDegrees) },
        { "O", std::max(outerCount, 1), outerRadius, ToRad(outerPhaseDegrees) },
    };

    for (const Ring& ring : rings) {
        for (int i = 0; i < ring.count; ++i) {
            const float angle = ring.phase +
                                TWO_PI * static_cast<float>(i) / static_cast<float>(ring.count);
            Hole hole;
            hole.id = std::string(ring.tag) + std::to_string(i + 1);
            /// @note Blender の (x, y) が取り込みで (x, z) になる。cos が x / sin が z。
            hole.center = Vector3{ ring.radius * std::cos(angle), 0.0f,
                                   ring.radius * std::sin(angle) };
            /// @note 羽が逃げる向きは «輪の接線に直交する水平» = 中心を向く放射。
            hole.normal = Vector3{ -hole.center.x, 0.0f, -hole.center.z }
                              .NormalizedOr(Vector3{ 1.0f, 0.0f, 0.0f });
            hole.spin = (!alternateCollar || (i % 2) == 0) ? 1.0f : -1.0f;
            for (const auto& [id, phase] : previous)
                if (id == hole.id) {
                    hole.phase     = phase;
                    hole.lastPhase = phase;
                    hole.wantOpen  = phase > 0.0f;
                }
            m_ids.push_back(hole.id);
            m_holes.push_back(std::move(hole));
        }
    }
    debugHoles = static_cast<int>(m_holes.size());
}

inline SerpentApertureComponent::Hole* SerpentApertureComponent::Find(const std::string& id)
{
    for (Hole& hole : m_holes)
        if (hole.id == id) return &hole;
    return nullptr;
}

inline const SerpentApertureComponent::Hole*
SerpentApertureComponent::Find(const std::string& id) const
{
    for (const Hole& hole : m_holes)
        if (hole.id == id) return &hole;
    return nullptr;
}

inline Vector3 SerpentApertureComponent::ToLocalOffset(const Vector3& worldOffset) const
{
    GameObject* self = scene.Self();
    if (!self) return worldOffset;
    const Vector3& s = self->transform.worldScale;
    return Vector3{ worldOffset.x / (std::fabs(s.x) > EPSILON ? s.x : 1.0f),
                    worldOffset.y / (std::fabs(s.y) > EPSILON ? s.y : 1.0f),
                    worldOffset.z / (std::fabs(s.z) > EPSILON ? s.z : 1.0f) };
}

/// @note 親のスケールを掛けない (2026-09-05・蛇が出てこなかった原因): Inner/Outer
///       Radius (8.0m/16.0m) はアリーナ実効半径22mの内側に置いた実寸そのものだが、
///       Boss02_Arena_Map のルートには取り込みの unit scale 100 が残る (Stage_01の
///       Boss_Arena_Map も同じ)。掛けると口が800m/1600mへ飛び、SerpentPathComponent の
///       渡れる窓 (Min/Max Chord 8.0〜10.5m) に入る口が無くなり BeginFirstRoute() が
///       毎秒失敗し続ける ─ 蛇は parkDepth 40m の床下に居座り、画面には何も出ず
///       間接的な警告しか出ない。ToLocalOffset() と同様、この系の値はすべてメートルで
///       位置と向きだけ親から借りる。
inline Vector3 SerpentApertureComponent::HoleCenter(const std::string& hole) const
{
    const Hole* found = Find(hole);
    GameObject* self  = scene.Self();
    if (!found || !self) return Vector3::ZERO;

    return self->transform.worldPosition + self->transform.worldRotation * found->center;
}

inline float SerpentApertureComponent::HoleDistance(const std::string& a,
                                                    const std::string& b) const
{
    const Vector3 pa = HoleCenter(a);
    const Vector3 pb = HoleCenter(b);
    return Vector3{ pb.x - pa.x, 0.0f, pb.z - pa.z }.Length();
}

inline std::string SerpentApertureComponent::NearestHole(const Vector3& point) const
{
    std::string best;
    float bestSq = 0.0f;
    for (const std::string& id : m_ids) {
        const Vector3 c = HoleCenter(id);
        const float   d = Vector3{ c.x - point.x, 0.0f, c.z - point.z }.LengthSq();
        if (best.empty() || d < bestSq) { best = id; bestSq = d; }
    }
    return best;
}

inline void SerpentApertureComponent::Open(const std::string& hole)
{
    if (Hole* found = Find(hole)) found->wantOpen = true;
}

inline void SerpentApertureComponent::Close(const std::string& hole)
{
    if (Hole* found = Find(hole)) found->wantOpen = false;
}

inline void SerpentApertureComponent::CloseAll()
{
    for (Hole& hole : m_holes) hole.wantOpen = false;
}

inline bool SerpentApertureComponent::IsOpen(const std::string& hole) const
{
    const Hole* found = Find(hole);
    return found && found->phase >= Total() - 1.0e-4f;
}

inline bool SerpentApertureComponent::IsBusy(const std::string& hole) const
{
    const Hole* found = Find(hole);
    return found && found->phase > 0.0f;
}

inline float SerpentApertureComponent::OpenRatio(const std::string& hole) const
{
    const Hole* found = Find(hole);
    return found ? Clamp01(found->phase / Max(Total(), 0.01f)) : 0.0f;
}

inline void SerpentApertureComponent::ResolveNodes(Hole& hole)
{
    /// @note 毎フレーム確かめ直す: スクリプト DLL リロードで Script は作り直され
    ///       EntityRef は空へ戻る。覚えたままだと以後どの口も動かない。
    if (hole.rim.Resolve(scene) && hole.collar.Resolve(scene) &&
        hole.leafA.Resolve(scene) && hole.leafB.Resolve(scene))
        return;

    GameObject* self = scene.Self();
    if (!self) return;

    const auto grab = [&](const char* prefix, EntityRef& ref) {
        if (ref.Resolve(scene)) return true;
        if (GameObject* node = FindInSubtree(*self, prefix + hole.id)) {
            ref = EntityRef{ node->GetID() };
            return true;
        }
        ++debugMissing;
        return false;
    };

    grab("ARENA_Rim_", hole.rim);
    grab("ARENA_Collar_", hole.collar);
    grab("ARENA_ShutterA_", hole.leafA);
    grab("ARENA_ShutterB_", hole.leafB);
    if (driveShutterCollision) grab("COL_Shutter_", hole.floorCollision);
}

inline void SerpentApertureComponent::EnsureShutterFloor(Hole& hole)
{
    if (!driveShutterCollision || hole.collisionReady) return;

    GameObject* floor = hole.floorCollision.Resolve(scene);
    if (!floor) return;

    /// @note 既に置いてあればそれを使う。無ければ描画メッシュから床を起こす ─
    ///       `COL_Shutter_<口>` は16枚とも同じ形の板で、シーンに手で置くと作り直す
    ///       たびに置き直しになるため。
    if (!floor->GetComponent<MeshColliderComponent>()) {
        if (const auto* renderer = floor->GetComponent<MeshRenderer>()) {
            auto& collider = floor->AddComponent<MeshColliderComponent>();
            collider.SetMesh(renderer->meshPath, 0);
            collider.isTrigger = false;
        }
    }
    hole.collisionReady = true;
}

inline void SerpentApertureComponent::ApplyHole(Hole& hole)
{
    const float t1 = TelegraphEnd();
    const float t2 = UnlockEnd();
    const float t3 = SinkEnd();

    const float glow01   = Clamp01(hole.phase / Max(t1, 0.01f));
    const float unlock01 = Clamp01((hole.phase - t1) / Max(unlockSeconds, 0.01f));
    const float sink01   = Clamp01((hole.phase - t2) / Max(sinkSeconds, 0.01f));
    const float slide01  = Clamp01((hole.phase - t3) / Max(slideSeconds, 0.01f));

    /// @note 縁。«まだ間に合う» と «もう開く» を明滅の速さで分ける。
    if (GameObject* rim = hole.rim.Resolve(scene)) {
        float gain = Lerp(Max(glowIdle, 0.0f), glowStrength, glow01);
        if (glowPulseHz > 0.0f && glow01 > 0.0f && glow01 < 1.0f) {
            const float wave = 0.5f + 0.5f * std::sin(Time::time * TWO_PI * glowPulseHz);
            gain *= Lerp(0.55f, 1.0f, wave);
        }
        const auto instance = material.Instance(EntityRef{ rim->GetID() });
        instance.SetVector3(kEmissiveColorId, Vector3{ glowColor.x, glowColor.y, glowColor.z });
        instance.SetFloat(kEmissiveScaleId, gain);
    }

    /// @note カラーは口の中心まわりに回す。取り込んだノードの原点はワールド原点なので、
    ///       T(P)·R·T(-P) を «回転 + 位置» の 2 つへ畳んで入れる。
    if (GameObject* collar = hole.collar.Resolve(scene)) {
        const float      rad   = ToRad(lockDegrees) * hole.spin *
                                 ScriptTweenProxy::Evaluate(TweenEase::OutCubic, unlock01);
        const Quaternion rot   = Quaternion::FromAxisAngle(Vector3::UP, rad);
        const Vector3    moved = rot * hole.center;
        collar->transform.rotation = rot;
        collar->transform.position = hole.center - moved;
    }

    /// @note 羽は «下げてから外へ»。真横へ滑らせると床スラブと交差する。
    const float sinkEase  = ScriptTweenProxy::Evaluate(TweenEase::OutQuad, sink01);
    const float slideEase = ScriptTweenProxy::Evaluate(TweenEase::InOutCubic, slide01);
    const Vector3 drop{ 0.0f, -sinkMeters * sinkEase, 0.0f };
    const Vector3 slide = hole.normal * (slideMeters * slideEase);

    if (GameObject* leaf = hole.leafA.Resolve(scene))
        leaf->transform.position = ToLocalOffset(drop + slide);
    if (GameObject* leaf = hole.leafB.Resolve(scene))
        leaf->transform.position = ToLocalOffset(drop - slide);

    /// @note 床は «完全に閉じている間» だけ。開き始めた瞬間に切らないと、沈んだ羽の上に
    ///       見えない床が残って、開いた口の上を歩けてしまう。
    if (driveShutterCollision) {
        if (GameObject* floor = hole.floorCollision.Resolve(scene)) {
            const bool active = hole.phase <= t1 + 1.0e-4f;
            if (active != hole.floorActive) {
                floor->SetActive(active);
                hole.floorActive = active;
            }
        }
    }
}

inline void SerpentApertureComponent::OnUpdate()
{
    BuildHoles();

    const float dt    = Max(Time::deltaTime, 0.0f);
    const float total = Total();

    debugMissing = 0;
    std::string open;

    for (Hole& hole : m_holes) {
        ResolveNodes(hole);
        EnsureShutterFloor(hole);

        const float target = hole.wantOpen ? total : 0.0f;
        hole.lastPhase = hole.phase;
        hole.phase = hole.phase < target ? Min(hole.phase + dt, target)
                                         : Max(hole.phase - dt, target);
        ApplyHole(hole);
        ReportStages(hole);
        TickPassage(hole, dt);

        if (hole.phase > 0.0f) {
            if (!open.empty()) open += " ";
            open += hole.id;
            if (!IsOpen(hole.id)) open += "*";
        }

        if (drawHoles) {
            const Vector3 c = HoleCenter(hole.id);
            const Vector4 color = hole.phase > 0.0f ? glowColor
                                                    : Vector4{ 0.35f, 0.38f, 0.42f, 1.0f };
            constexpr int kSteps = 24;
            for (int i = 0; i < kSteps; ++i) {
                const float a = TWO_PI * static_cast<float>(i) / kSteps;
                const float b = TWO_PI * static_cast<float>(i + 1) / kSteps;
                debug.DrawLine({ c.x + std::cos(a) * 2.2f, c.y + 0.05f, c.z + std::sin(a) * 2.2f },
                               { c.x + std::cos(b) * 2.2f, c.y + 0.05f, c.z + std::sin(b) * 2.2f },
                               color);
            }
        }
    }

    debugOpen = open.empty() ? "-" : open;

    /// @note 落ちた後始末は口を全部進めてから。開閉の途中の口を «避けるべき穴» として
    ///       数えるので、この 1 フレームの開き具合が確定した後でないと 1 コマぶんずれる。
    DriveFallRescue(dt);

    /// @note 名前が 1 つでも外れると «その口だけ開かない» という形でしか出ない。名指しで言う。
    if (debugMissing > 0)
        debug.LogError("SerpentApertureComponent could not find " +
                       std::to_string(debugMissing) +
                       " aperture node(s) under this object. Expected ARENA_Rim_<hole> / "
                       "ARENA_Collar_<hole> / ARENA_ShutterA_<hole> / ARENA_ShutterB_<hole> / "
                       "COL_Shutter_<hole> for I1..I6 and O1..O10.");
}

} // namespace sandbox
