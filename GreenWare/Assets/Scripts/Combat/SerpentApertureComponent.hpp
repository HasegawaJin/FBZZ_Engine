/// @file    SerpentApertureComponent.hpp
/// @brief   床の 16 口の開閉と予兆。付ける先は Boss02_Arena_Map (口は場の設備)
/// @author  Hasegawa Jin
/// @date    2026-08-31
///
/// WHY リグもクリップも使わないか (boss-serpent.md「開口の動き」):
///   16 口を 1 つのアーマチュアに載せるとクリップが全口を同時に動かす。1 口ずつ開ける
///   ならクリップ 16 本かマスク 16 レイヤで、剛体の板 16 枚のために 16 個の何かが増える。
///   ここは «沈む → 滑る» の平行移動と «口の中心まわりの» 回転しかしない。
///
/// WHY Tween コルーチンではなく 1 本のタイムラインを進めるか:
///   ScriptTweenProxy は «自分の GameObject» を動かす口で、16 口 × 3 部品を別々に
///   走らせるには Value() へラムダを 48 本渡すことになる。しかも開いている途中で
///   閉じ直す (潜る先を変える) と、走っているコルーチンを止める手段が要る。
///   口ごとに «進み» を 1 つの float で持てば、途中で向きが変わっても同じ経路を
///   逆に戻るだけで済み、DLL リロードで走行中の状態が消えることもない。
///   曲線は同じものを使う (ScriptTweenProxy::Evaluate)。
///
/// WHY 口の位置を輪の式から出すか:
///   `ARENA_Rim_<口>` のノードは取り込み時に変換を頂点へ焼かれていて、Transform は
///   16 口とも原点 (0,0,0)。worldPosition から引くと 16 口が全部アリーナの中心に
///   重なる。位置を持っているのはメッシュだけで、そこはスクリプトから読めない。
///   ここでは «輪の半径・口数・位相» を公開フィールドにして、そこから解く ─
///   既定値は書き出し済みメッシュの実測と一致している (I1 = (8, ·, 0) /
///   O1 = (15.2169, ·, 4.9443))。ビルダー側 (`serpent_map_build.py` の RINGS) を
///   直したらここも直すこと。
#pragma once

#include <Engine/Scene/Components/ColliderComponent.hpp>
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include <Engine/Scene/EntityRef.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Engine/Scene/ScriptProxy/ScriptTweenProxy.hpp>
#include <Math/MathUtils.hpp>
#include <Scripts/Game/VfxManagerComponent.hpp>
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
    // 30fps 前提のフレーム数を秒へ直した値 (boss-serpent.md「開口の動き」)。
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

    // 輪の作り。既定はビルダー (serpent_map_build.py の RINGS) と同じ。
    FBZZ_GROUP("Rings")
    FBZZ_FIELD_RANGE_INT(int, innerCount, 6, "Inner Count", 1, 24)
    FBZZ_FIELD_RANGE(float, innerRadius, 8.0f, "Inner Radius", 1.0f, 40.0f)
    FBZZ_FIELD_RANGE(float, innerPhaseDegrees, 0.0f, "Inner Phase", -180.0f, 180.0f)
    FBZZ_FIELD_RANGE_INT(int, outerCount, 10, "Outer Count", 1, 32)
    FBZZ_FIELD_RANGE(float, outerRadius, 16.0f, "Outer Radius", 1.0f, 40.0f)
    FBZZ_FIELD_RANGE(float, outerPhaseDegrees, 18.0f, "Outer Phase", -180.0f, 180.0f)
    FBZZ_TOOLTIP("外輪は内輪の口の間へ来るよう半ピッチずらしてある (π/10)")

    // 口は «場の設備» なので、動くたびに機械の音と埃を返す。
    //
    // WHY 段ごとに別の音を鳴らすか: 開くまでの 1.2 秒はほぼ全部が予兆で、その間に
    //     プレイヤーが決めるのは «そこから離れるか» の 1 点。歯が外れる音・羽が滑る音・
    //     閉じ切る音が別々に鳴れば、画面の外の口でも «いまどこまで進んだか» が耳で読める。
    FBZZ_GROUP("手触り")
    FBZZ_FIELD(bool, playSound, true, "Sound")
    FBZZ_FIELD_RANGE(float, soundVolume, 0.85f, "Volume", 0.0f, 2.0f)
    FBZZ_FIELD(bool, playDust, true, "Dust")
    FBZZ_TOOLTIP("羽が滑り始めた瞬間に縁から埃を吹く。開口が «床を割って開いた» に見える")
    FBZZ_FIELD_RANGE(float, dustStrength, 0.7f, "Dust Strength", 0.0f, 1.0f)
    FBZZ_FIELD(bool, alternateCollar, true, "Alternate Collar")
    FBZZ_TOOLTIP("隣り合う口でカラーの回る向きを逆にする。16 個が同じ向きに回ると"
                 "«1 つの仕掛けのコピー» に見える")

    // 胴が口を «通っている» 間の絵。
    //
    // WHY 羽の埃だけでは足りないか: 埃は羽が滑り出した 1 フレームにしか出ない。
    //     そこから先、11 m の胴が床を突き破って出てきて、また潜っていく ─
    //     戦闘中いちばん多く見る動作 (約 7 秒ごと) に粒が 1 つも無かった。
    //     しかも羽は «蛇が来る前» に開くので、埃と胴の出入りは時間的に重なりもしない。
    //
    // WHY 口の側が持つか: どの口を今どの弧長で使っているかは AI が知っているが、
    //     «縁がどこにあるか» を持っているのはここだけ。AI へ書くと、口を増やす
    //     たびに演出の側も直すことになる。
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

    FBZZ_GROUP("デバッグ")
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
    ///
    /// WHY 押し込む形にするか: 通っているかは «その口の弧長と、頭 / 尾の弧長» を
    ///     突き合わせないと分からず、経路を持っているのは AI だけ。ここが自分で
    ///     調べようとすると、口の側が経路の形まで知ることになる。
    ///     押されなかった口は自然に止まる (SetAim / SetUndulationScale と同じ約束)。
    void ReportPassage(const std::string& hole);

    void OnStart() override;
    void OnUpdate() override;

private:
    /// 口 1 つぶんの状態。
    struct Hole {
        std::string id;
        /// ローカル (メッシュ) 空間での中心。輪の式から解く。
        Vector3     center{};
        /// 口の «法線»。輪の接線に直交する水平で、中心を向く。羽はこの向きへ逃げる。
        Vector3     normal{ 1.0f, 0.0f, 0.0f };
        /// 開閉の進み [0, Total()]。目標へ向かって実時間で進む。
        float       phase   = 0.0f;
        /// 前フレームの進み。段をまたいだ «瞬間» を拾うために持つ。
        ///
        /// WHY 進みだけで足りないか: 音と埃は «その段に入った 1 フレーム» で 1 度だけ
        ///     鳴らしたい。閾値との大小比較だけだと、開いている間ずっと鳴り続ける。
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
};

FBZZ_REFLECT(SerpentApertureComponent)

inline void SerpentApertureComponent::OnStart()
{
    m_builtSignature = -1.0f;
    BuildHoles();
    // 口は盤面のあちこちにある。どの方向で何が起きたかが分かる必要があるので 3D。
    se::EnsureSource(scene, "SE", 1.0f);
}

inline void SerpentApertureComponent::ReportStages(const Hole& hole)
{
    // 上りと下りで別の段を報告する。開くのは 3 段の機械音、閉じるのは «噛んだ» 1 発。
    const auto rose    = [&](float at) { return hole.lastPhase < at && hole.phase >= at; };
    const auto fellTo0 = hole.lastPhase > 0.0f && hole.phase <= 0.0f;

    const Vector3 center = HoleCenter(hole.id);

    if (playSound) {
        // 歯が外れる。予兆の «終わり» を告げる音なので、ここが一番耳を引く必要がある。
        if (rose(TelegraphEnd())) se::PlayAt(audio, se::kImpactLight, center, soundVolume);
        // 羽が滑り出す。
        if (rose(SinkEnd()))      se::PlayAt(audio, se::kImpactDebris, center, soundVolume);
        // 閉じ切って噛む。開くときより重く鳴らして «もう通れない» を返す。
        if (fellTo0)              se::PlayAt(audio, se::kImpactMid, center, soundVolume);
    }

    // 埃は滑り出しの 1 度だけ。開いている間ずっと吹くと «煙が出ている穴» になる。
    if (playDust && rose(SinkEnd())) {
        if (auto* vfx = VfxManagerComponent::Instance()) {
            // 床が押し退けられた側 ─ つまり羽が逃げていく向きへ吹かせる。
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
    // 押されなかったフレームは «通っていない»。次に通り始めたときへ向けて畳む。
    const bool passing = hole.passing;
    hole.passing = false;

    if (!passageDust || !passing) {
        hole.wasPassing  = passing;
        hole.passageTimer = 0.0f;
        return;
    }

    // 通り始めの 1 発は «頭が縁を割った» 瞬間なので、続きの刻みより強く出す。
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
    // 輪の作りを 1 つの値に畳んで «変わったか» を見る。Inspector で半径を触った
    // ときにその場で並び直ってほしいので、開始時だけの組み立てにはしない。
    const float signature = static_cast<float>(innerCount) * 1000.0f + innerRadius * 7.0f +
                            innerPhaseDegrees * 0.5f + static_cast<float>(outerCount) * 13.0f +
                            outerRadius * 11.0f + outerPhaseDegrees * 0.25f;
    if (std::fabs(signature - m_builtSignature) < 1.0e-4f && !m_holes.empty()) return;
    m_builtSignature = signature;

    // 開き途中の口は覚えておく。半径を触っただけで開いていた口が閉じると、
    // 調整中に蛇が床へ埋まる。
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
            // Blender の (x, y) が取り込みで (x, z) になる。cos が x / sin が z。
            hole.center = Vector3{ ring.radius * std::cos(angle), 0.0f,
                                   ring.radius * std::sin(angle) };
            // 羽が逃げる向きは «輪の接線に直交する水平» = 中心を向く放射。
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

// WHY 親のスケールを «掛けない» か (2026-09-05・蛇が出てこなかった原因):
//   Inner / Outer Radius は builder (serpent_map_build.py の RINGS) と同じ **メートル**
//   で、8.0m / 16.0m は闘技場の実効半径 22m の内側に置いた実寸そのもの。
//   一方 Boss02_Arena_Map のルートには取り込みの unit scale 100 が残っている
//   (Stage_01 の Boss_Arena_Map も同じ)。ここで掛けると口が 800m / 1600m へ飛び、
//   口どうしの間隔も 100 倍になる。
//
//   間隔が壊れると «蛇が渡れる窓» (SerpentPathComponent の Min/Max Chord 8.0〜10.5m)
//   に入る口が 1 つも無くなり、BeginFirstRoute() が毎秒失敗し続ける。
//   蛇は Dormant のまま床下 40m (parkDepth) に居座るので、**画面には何も出ず、
//   エラーも «口が窓の中に無い» という間接的な警告しか出ない。**
//
//   すぐ上の ToLocalOffset() が «スケールを割り戻して移動量をメートルに保つ» と
//   書いているとおり、この系の値はすべてメートル。位置と向きだけ親から借りる。
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
    // WHY 毎フレーム確かめ直すか: スクリプト DLL をリロードすると Script は作り直され、
    //     EntityRef は空へ戻る。「引いた」を覚えたままだと、以後どの口も動かない。
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

    // 既に置いてあればそれを使う。無ければ «描いているメッシュ» から床を起こす。
    //
    // WHY スクリプトから生やすか: COL_Shutter_<口> は 16 枚とも同じ形の板で、
    //     シーンに手で 16 個コライダーを置くと、口を作り直すたびに置き直しになる。
    //     どのサブメッシュを見るかは描画側が既に知っているので、そこから写す。
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

    // 縁。«まだ間に合う» と «もう開く» を明滅の速さで分ける。
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

    // カラーは口の中心まわりに回す。取り込んだノードの原点はワールド原点なので、
    // T(P)·R·T(-P) を «回転 + 位置» の 2 つへ畳んで入れる。
    if (GameObject* collar = hole.collar.Resolve(scene)) {
        const float      rad   = ToRad(lockDegrees) * hole.spin *
                                 ScriptTweenProxy::Evaluate(TweenEase::OutCubic, unlock01);
        const Quaternion rot   = Quaternion::FromAxisAngle(Vector3::UP, rad);
        const Vector3    moved = rot * hole.center;
        collar->transform.rotation = rot;
        collar->transform.position = hole.center - moved;
    }

    // 羽は «下げてから外へ»。真横へ滑らせると床スラブと交差する。
    const float sinkEase  = ScriptTweenProxy::Evaluate(TweenEase::OutQuad, sink01);
    const float slideEase = ScriptTweenProxy::Evaluate(TweenEase::InOutCubic, slide01);
    const Vector3 drop{ 0.0f, -sinkMeters * sinkEase, 0.0f };
    const Vector3 slide = hole.normal * (slideMeters * slideEase);

    if (GameObject* leaf = hole.leafA.Resolve(scene))
        leaf->transform.position = ToLocalOffset(drop + slide);
    if (GameObject* leaf = hole.leafB.Resolve(scene))
        leaf->transform.position = ToLocalOffset(drop - slide);

    // 床は «完全に閉じている間» だけ。開き始めた瞬間に切らないと、沈んだ羽の上に
    // 見えない床が残って、開いた口の上を歩けてしまう。
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

    // 名前が 1 つでも外れると «その口だけ開かない» という形でしか出ない。名指しで言う。
    if (debugMissing > 0)
        debug.LogError("SerpentApertureComponent could not find " +
                       std::to_string(debugMissing) +
                       " aperture node(s) under this object. Expected ARENA_Rim_<hole> / "
                       "ARENA_Collar_<hole> / ARENA_ShutterA_<hole> / ARENA_ShutterB_<hole> / "
                       "COL_Shutter_<hole> for I1..I6 and O1..O10.");
}

} // namespace sandbox
