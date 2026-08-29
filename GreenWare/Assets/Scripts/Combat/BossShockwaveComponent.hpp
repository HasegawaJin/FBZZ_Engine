/// @file    BossShockwaveComponent.hpp
/// @brief   ボスの着地から地面を走る円形衝撃波。跳ばないと越えられない
/// @author  Hasegawa Jin
/// @date    2026-08-27
///
/// WHY 大ジャンプに «外向きの波» を足すか:
///   着地の判定は落下点を中心にした 1 つの円で、遠くへ居れば何もせずに済んでしまう。
///   8 章が大ジャンプへ与えた «踏みつけの間合いから逃げた相手を追う» は、逃げた先にも
///   何かが届いて初めて成立する。落下点の外へ広がる波なら、距離を取ったこと自体は
///   正しいまま「その 1 回だけ跳ぶ」という別の答えを要求できる。
///
/// WHY «跳んで避ける» にするか (横へ逃げるではなく):
///   横へ逃げる攻撃は盤面に既に 3 つある (突進・コアビーム・踏みつけ)。同じ答えを
///   4 つ目に足しても、覚えることが増えるだけで択が増えない。波は円なので横移動では
///   絶対に外れず、走って逃げ切れる速さでもない。残る手が跳躍だけになる。
///
/// WHY 落下点の直撃と «別の当たり» にするか:
///   直撃 (BossAi の Shock Radius) は «腹の下に居た» ことへの罰で、跳んでも避けられない。
///   波はその外側だけを担当する。1 つの判定で兼ねると、跳べば真下でも助かることになり、
///   腹下へ潜る危険が消える。
///
/// WHY 高さで «越えた» を決めるか (接地フラグではなく):
///   CharacterController の isGrounded は接地の «猶予» を持っていて、跳んだ直後や
///   降り際に true と false が入れ替わる。跳んだのに当たった / 立っているのに抜けた、
///   のどちらもそこから出る。波が走っているのは地面なので、«波の面から何 m 上に居るか»
///   だけで決める方が、絵と規則が一致する。
///
/// WHY 帯より «見た目» を太くするか:
///   6.4 の «見た目どおりに当たる» は、掠って見えたのに食らう方を強く嫌う。リングの
///   外周をわずかに当たりの外へ出しておけば、縁を跨いだ絵で助かることはあっても、
///   何も無い所で食らうことはない。
#pragma once

#include <Engine/Scene/Components/AudioSourceComponent.hpp>
#include <Engine/Scene/Components/PresentationComponents.hpp>
#include <Engine/Scene/EntityRef.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Scripts/Combat/BossPolarityCoreComponent.hpp>
#include <Scripts/Game/CameraShakeManagerComponent.hpp>
#include <Scripts/Game/CombatManagerComponent.hpp>
#include <Scripts/Game/RumbleManagerComponent.hpp>
#include <Scripts/Utils/PolarityTypes.hpp>
#include <Scripts/Utils/SeLibrary.hpp>
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using namespace fbzz::scene;
using namespace fbzz::math;
using fbzz::Time;

namespace sandbox {

/// リングの円周を何分割するかの下限と上限。
/// WHY 半径で分割数を変えるか: 固定数だと、生まれたてのリングは点が密集して潰れ、
///     広がりきったリングは 1 区間が数 m の «多角形» になる。1m あたりの点数を
///     決めておけば、どの大きさでも同じ滑らかさで見える。
inline constexpr int kShockwaveMinSegments = 24;
inline constexpr int kShockwaveMaxSegments = 128;

class BossShockwaveComponent : public Script {
    FBZZ_SCRIPT(BossShockwaveComponent)

    // 波の発生音はここから鳴らす。無ければ OnStart が自分で足す。
    FBZZ_OPTIONAL_COMPONENT(AudioSourceComponent)

public:
    FBZZ_GROUP("Wave")
    FBZZ_FIELD_RANGE(float, startRadius, 1.5f, "Start Radius", 0.0f, 20.0f)
    FBZZ_TOOLTIP("波が生まれる半径 [m]。ボスの胴体ぶん。0 にすると落下点で 1 フレーム "
                 "«点» が出てから広がるので、生まれた瞬間が読めない")
    FBZZ_FIELD_RANGE(float, maxRadius, 20.0f, "Max Radius", 1.0f, 60.0f)
    FBZZ_TOOLTIP("ここまで広がったら消える [m]。アリーナ半径 (実測 20m) が既定")
    FBZZ_FIELD_RANGE(float, speed, 11.0f, "Speed", 1.0f, 40.0f)
    FBZZ_TOOLTIP("広がる速さ [m/s]。プレイヤーの移動 6.0 m/s より明確に速くないと、"
                 "«走って逃げ切る» が最適解になって跳ぶ理由が消える")
    FBZZ_FIELD_RANGE(float, bandWidth, 1.0f, "Band Width", 0.1f, 5.0f)
    FBZZ_TOOLTIP("当たる帯の半幅 [m]。狭いほど «跳ぶ時刻» がシビアになる")
    FBZZ_FIELD_RANGE_INT(int, damage, 2, "Damage", 0, 100)
    FBZZ_TOOLTIP("直撃 (BossAi の Jump Stomp Damage) より軽く置く。"
                 "避け方が用意されている攻撃なので、当たった罰は腹下に居た罰より小さい")

    FBZZ_GROUP("Clearance")
    FBZZ_FIELD_TAG(playerTag, "Player", "Player Tag")
    FBZZ_FIELD_RANGE(float, clearHeight, 0.90f, "Clear Height", 0.0f, 4.0f)
    FBZZ_TOOLTIP("波の面からこの高さ以上に足が有れば越えられる [m]。"
                 "PlayerTuning の Apex Height (既定 1.7m) より十分低く保つこと。"
                 "近づけるほど «跳躍の頂点でしか抜けられない» になり、超えると理不尽になる")
    FBZZ_FIELD_RANGE(float, playerRadius, 0.45f, "Player Radius", 0.0f, 3.0f)
    FBZZ_TOOLTIP("プレイヤーの当たり半径 [m]。帯の半幅へ足して判定する")

    FBZZ_GROUP("Look")
    FBZZ_FIELD_FILE(ringMaterial, "Assets/Materials/Fallback/VFXMeshFallback.mat",
                    "Material", ".mat")
    FBZZ_TOOLTIP("加算・両面の Unlit が既定。色は極性色が startColor から毎フレーム乗る")
    FBZZ_FIELD_RANGE(float, ringWidthScale, 1.25f, "Ring Width Scale", 0.2f, 3.0f)
    FBZZ_TOOLTIP("リングの太さを当たりの帯幅の何倍で描くか。1 より大きくしておくと、"
                 "縁を跨いだ絵で助かることはあっても «何も無い所で食らう» は起きない")
    FBZZ_FIELD_RANGE(float, intensity, 2.2f, "Intensity", 0.1f, 8.0f)
    FBZZ_TOOLTIP("1 を超えた色がブルームに乗る。上げすぎると赤と青の区別が消える")
    FBZZ_FIELD_RANGE(float, lift, 0.06f, "Lift", 0.0f, 1.0f)
    FBZZ_TOOLTIP("床から浮かせる高さ [m]。0 だと Z ファイトでリングが縞に割れる")
    FBZZ_FIELD_RANGE(float, segmentsPerMeter, 3.0f, "Segments / m", 0.5f, 12.0f)
    FBZZ_FIELD_RANGE_INT(int, radialSegments, 6, "Tube Segments", 3, 16)
    FBZZ_TOOLTIP("筒の円周分割数。リング自体が細いので、増やしても遠目には変わらない")
    FBZZ_FIELD_RANGE(float, fadeFrom, 0.55f, "Fade From", 0.0f, 1.0f)
    FBZZ_TOOLTIP("広がりのこの割合から薄れ始める。1 で最後まで同じ濃さ")

    // WHY 等速をやめるか:
    //   等速の輪は «図形が大きくなっている» ようにしか見えず、叩きつけられた空気が
    //   走っている感じにならない。実際に跳ぶかどうかを決めるのは生まれた直後の
    //   1 秒未満なので、そこだけ速くすると «来る» の圧が出て、判断の時刻も前へ寄る。
    //
    // WHY 加速側にしか振らないか:
    //   Speed は «走って逃げ切れない» を保証している下限 (上の Speed の WHY)。
    //   減速側へ振ると外周で速度が落ち、逃げ切りが最適解に戻る。倍率は必ず 1 以上。
    FBZZ_GROUP("Burst")
    FBZZ_FIELD_RANGE(float, burstMultiplier, 2.4f, "Initial Speed x", 1.0f, 6.0f)
    FBZZ_TOOLTIP("生まれた瞬間の速さ倍率。1 で等速 (従来どおり)")
    FBZZ_FIELD_RANGE(float, burstFalloff, 5.0f, "Falloff", 0.5f, 20.0f)
    FBZZ_TOOLTIP("倍率が Speed へ落ち着く速さ。大きいほど早く終端速度になる")
    FBZZ_FIELD_RANGE(float, flashSeconds, 0.18f, "Flash", 0.0f, 1.5f)
    FBZZ_TOOLTIP("発生直後に輪を強く太くする時間 [秒]。着弾の «開いた» 瞬間を作る。0 で切る")
    FBZZ_FIELD_RANGE(float, flashGain, 1.8f, "Flash Gain", 0.0f, 6.0f)
    FBZZ_TOOLTIP("閃光中の明るさの上乗せ。加算なので上げすぎると極性色が白へ飛ぶ")

    // WHY 真円をやめるか:
    //   真円は幾何図形として読めてしまい、床を裂いて走る «力» に見えない。
    //   ただし歪みは必ず外向きだけに出す。内側へ凹ませると、絵が当たり判定より
    //   内側に入る瞬間ができ、6.4 の «見た目どおりに当たる» が破れる。
    FBZZ_GROUP("Distortion")
    FBZZ_FIELD_RANGE(float, wobbleAmplitude, 0.35f, "Wobble", 0.0f, 3.0f)
    FBZZ_TOOLTIP("外向きへの膨らみ [m]。0 で真円")
    FBZZ_FIELD_RANGE(float, wobbleLift, 0.25f, "Wobble Lift", 0.0f, 2.0f)
    FBZZ_TOOLTIP("膨らんだ所を持ち上げる高さ [m]。平らな輪に «うねり» が出る")
    FBZZ_FIELD_RANGE(float, wobbleSpin, 1.2f, "Spin", 0.0f, 8.0f)
    FBZZ_TOOLTIP("うねりが円周を回る速さ [rad/s]。0 で止まったまま広がる")

    // WHY 後続の輪を足すか:
    //   1 本だけだと «線が広がっている» で終わる。少し遅れた薄い輪を重ねると、
    //   前縁と後ろの間に厚みができ、どちらへ進んでいるのかが 1 フレームの絵から読める。
    //   当たるのは前縁だけなので、後続は必ず内側 (通り過ぎた側) へ置く。
    FBZZ_GROUP("Wake")
    FBZZ_FIELD_RANGE(float, wakeLag, 1.6f, "Lag", 0.0f, 8.0f)
    FBZZ_TOOLTIP("前縁から内側へ何 m 遅れるか。0 で後続を出さない")
    FBZZ_FIELD_RANGE(float, wakeWidthScale, 0.45f, "Width x", 0.05f, 2.0f)
    FBZZ_FIELD_RANGE(float, wakeIntensityScale, 0.35f, "Intensity x", 0.0f, 2.0f)
    FBZZ_TOOLTIP("後続の明るさ。前縁より暗くしないと «当たる線» がどれか読めなくなる")

    // WHY 越えた側にも返すか: 跳んで抜けた «成功» に何も返らないと、当たらなかったのが
    //     読み勝ちなのか、そもそも判定が無かったのかプレイヤーには区別できない。
    //     足の下を通ったことを手で返して初めて、跳ぶ操作が答えとして確定する。
    FBZZ_GROUP("Feedback")
    FBZZ_FIELD_RANGE(float, passRumble, 0.45f, "Pass Rumble", 0.0f, 1.0f)
    FBZZ_TOOLTIP("波がプレイヤーの位置を通り過ぎた瞬間。当たった側は被弾側が返すので、"
                 "ここは «越えた» の合図として軽く置く")
    FBZZ_FIELD_RANGE(float, passShakeRatio, 0.5f, "Shake Ratio", 0.0f, 1.0f)

    FBZZ_GROUP("Debug")
    FBZZ_FIELD_READ_ONLY(float, debugRadius, 0.0f, "Radius")
    FBZZ_FIELD_READ_ONLY(bool, debugDealt, false, "Dealt")
    FBZZ_FIELD(bool, drawDebugBand, false, "Draw Band")

    /// 着地の瞬間に 1 度だけ呼ぶ。groundPoint は波が走る «床» の高さを兼ねる。
    void Emit(const Vector3& groundPoint);
    /// 走っている波を畳む。倒された・シーンを抜けた、など «無かったことにする» 側。
    void Cancel();

    [[nodiscard]] bool  IsActive() const { return m_active; }
    [[nodiscard]] float Radius()   const { return m_radius; }

    void OnStart()      override;
    void OnLateUpdate() override;
    void OnDisable()    override { Cancel(); }
    void OnDestroy()    override;

private:
    /// 円周の点列を組む。
    /// @param wobble true で外向きのうねりを乗せる。false は素の真円。
    ///
    /// WHY 歪みを引数で切れるようにするか: 同じ関数をデバッグの帯描画も使う。
    ///     あちらが見たいのは «実際に当たる円» なので、絵のための歪みが乗ると
    ///     «見た目と当たりのズレ» を確かめるための線自体がズレる。
    void BuildCircle(std::vector<Vector3>& out, float radius, bool wobble) const;
    /// 前縁と後続の 2 本を今の半径で描き直す。
    void ShowRing();
    void HideRing();
    /// 1 本ぶんの輪を書き込む。前縁と後続で違うのは半径・太さ・明るさだけ。
    void WriteRing(EntityRef& ref, const char* suffix, float radius,
                   float width, const Vector4& color);
    /// 帯に触れているプレイヤーへ 1 度だけ当てる。跳んで越えていれば当てない。
    void ResolveHit(GameObject* player);
    /// 波がプレイヤーの立っている半径を追い越した瞬間を 1 度だけ返す。
    void NotifyPass(const Vector3& playerPoint);
    /// リングの GameObject。無ければ作る。suffix で前縁と後続を分ける。
    [[nodiscard]] GameObject* RingObject(EntityRef& ref, const char* suffix);
    [[nodiscard]] std::string  RingName(const char* suffix) const;
    /// ボスの極性色 × 明るさ。消灯中 (激突スタン) でも黒いリングにはしない。
    [[nodiscard]] Vector4 WaveColor() const;
    /// 水平距離。高さは «越えたか» の判定にしか使わないので、帯の測りには入れない。
    [[nodiscard]] static float PlanarDistance(const Vector3& a, const Vector3& b);

    EntityRef m_ring;
    /// 後続の輪。前縁と別の GameObject に分けるのは、LineRenderer が 1 つの
    /// GameObject につき 1 本しか点列を持てないため。
    EntityRef m_wake;
    Vector3   m_center = Vector3::ZERO;
    float     m_radius = 0.0f;
    /// 発生からの経過 [秒]。閃光の減衰とうねりの回転に使う。
    float     m_elapsed = 0.0f;
    bool      m_active = false;
    /// この波で 1 度でも当てたか。1 回の着地でダメージは 1 回だけ。
    bool      m_dealt  = false;
    /// プレイヤーの位置を追い越したか。越えた合図は 1 度だけ返す。
    bool      m_passed = false;
    bool      m_ringVisible = false;
    bool      m_warnedNoCombat = false;
    /// 点列は毎フレーム組み直すが、確保し直さない。
    std::vector<Vector3> m_points;
};

FBZZ_REFLECT(BossShockwaveComponent)


inline float BossShockwaveComponent::PlanarDistance(const Vector3& a, const Vector3& b)
{
    const float dx = a.x - b.x;
    const float dz = a.z - b.z;
    return std::sqrt(dx * dx + dz * dz);
}

inline std::string BossShockwaveComponent::RingName(const char* suffix) const
{
    // WHY 持ち主ごとに名前を変えるか: リングはルートに置くため、同名だと 2 体目のボス
    //     (デバッグ用の複製を含む) が 1 体目のリングを奪う。
    GameObject* owner = scene.Self();
    return "FX_BossShockwave_" + std::string(suffix) + "_" +
           (owner ? owner->instanceId : std::string{});
}

inline void BossShockwaveComponent::OnStart()
{
    m_active  = false;
    m_dealt   = false;
    m_passed  = false;
    m_radius  = 0.0f;
    m_warnedNoCombat = false;
    debugRadius = 0.0f;
    debugDealt  = false;

    // WHY 撃つ前に作っておくか: DLL をリロードすると Script は作り直されるが、
    //     リングの GameObject は Scene 側に «前回の点列を持ったまま» 残る。ここで
    //     拾い直して畳んでおかないと、リロードした瞬間に前回の輪が盤面へ焼き付く。
    (void)RingObject(m_ring, "Front");
    (void)RingObject(m_wake, "Wake");
    m_ringVisible = true;   // 直後の HideRing に確実に畳ませる
    HideRing();

    // 波は盤面の «場所» で起きる出来事なので 3D。2D にすると、アリーナの反対側で
    // 起きた着地も自分の足元と同じ音量で鳴り、どこへ跳べばよいのか判らなくなる。
    se::EnsureSource(scene, "SE", 1.0f);
}

inline void BossShockwaveComponent::OnDestroy()
{
    // ルートに置いた以上、ボスと一緒には消えない。持ち主が畳む。
    if (GameObject* ring = m_ring.Resolve(scene)) scene.Destroy(*ring);
    if (GameObject* wake = m_wake.Resolve(scene)) scene.Destroy(*wake);
    m_ring = {};
    m_wake = {};
}

inline void BossShockwaveComponent::Emit(const Vector3& groundPoint)
{
    m_center  = groundPoint;
    m_radius  = std::max(startRadius, 0.0f);
    m_elapsed = 0.0f;
    m_active  = true;
    m_dealt   = false;
    m_passed  = false;
    debugDealt = false;

    // WHY ボス本体ではなく着地点で鳴らすか: 大ジャンプの着地点は «プレイヤーの近く»
    //     なので、胴体の原点で鳴らしても大差ないように見える。だが波が走り出す場所は
    //     床であって腹ではない。床で鳴らしておけば、跳ぶかどうかを決める耳の手がかりと
    //     リングの出どころが一致する。
    se::PlayAt(audio, se::kImpactHeavy, groundPoint);
}

inline void BossShockwaveComponent::Cancel()
{
    m_active = false;
    m_radius = 0.0f;
    debugRadius = 0.0f;
    HideRing();
}

inline Vector4 BossShockwaveComponent::WaveColor() const
{
    Polarity polarity = Polarity::Plus;
    if (const auto* core = scene.GetScript<BossPolarityCoreComponent>()) {
        // 消灯中は極が無い。そのとき跳ぶことは無いが、色だけ黒くなって
        // «見えない衝撃波» になるのは避ける。
        if (core->CurrentPolarity() != Polarity::None) polarity = core->CurrentPolarity();
    }
    return PolarityColor(polarity);
}

inline GameObject* BossShockwaveComponent::RingObject(EntityRef& ref, const char* suffix)
{
    if (GameObject* existing = ref.Resolve(scene)) return existing;

    const std::string name = RingName(suffix);
    // WHY 先に拾い直すか: スクリプト DLL をリロードするとこの Script は作り直され、
    //     EntityRef は空に戻る。一方 リングの GameObject は Scene 側に残っているため、
    //     拾わずに作り直すとリロードのたびに 1 本ずつ増えていく。
    GameObject* object = scene.Find(name);
    if (!object) {
        // WHY ボスの子にしないか: 子にするとリングがボスの移動・回転を引き継ぎ、
        //     ワールド座標で置いた円が着地点から流れていく。ルートへ原点で置く。
        GameObject& created = scene.Create(name);
        created.runtimeGenerated = true;
        object = &created;
    }
    ref = EntityRef{ object->GetID() };

    // Create / AddComponent はシーンの配列を伸ばしうる。設定は必ず ID から引き直した
    // 個体へ入れる (ElectricArcBundle と同じ理由)。
    GameObject* ring = ref.Resolve(scene);
    if (!ring) return nullptr;

    // WHY 原点・無回転のルートへ置くか: LineRenderer の World 空間は、渡したワールド点を
    //     所有 GameObject のローカルへ引き戻してからメッシュにする。親に付けたり
    //     回したりすると、その変換ぶんだけ円がずれる。
    ring->transform.position = Vector3::ZERO;

    auto* line = ring->GetComponent<LineRendererComponent>();
    if (!line) line = &ring->AddComponent<LineRendererComponent>();
    line->space  = LineSpace::World;
    line->loop   = true;   // 閉じた円。始点を末尾へ足さなくてよい
    line->shape  = LineShape::Tube;
    // WHY 筒か: リボンはカメラへ向くので、円にすると手前と奥で帯がねじれる。
    //     筒なら視点に依存せず、床へ半分刺さった «押し寄せる輪» に見える。
    line->billboard = false;

    return ref.Resolve(scene);
}

inline void BossShockwaveComponent::BuildCircle(std::vector<Vector3>& out, float radius,
                                                bool wobble) const
{
    const float safe = std::max(radius, 0.01f);
    out.clear();

    // 円周 (2πr) に «1m あたりの点数» を掛けたものが必要な分割数。
    const int segments = std::clamp(
        static_cast<int>(safe * TWO_PI * std::max(segmentsPerMeter, 0.1f)),
        kShockwaveMinSegments, kShockwaveMaxSegments);
    out.reserve(static_cast<std::size_t>(segments));

    const bool  distort = wobble && (wobbleAmplitude > 0.0f || wobbleLift > 0.0f);
    const float phase   = m_elapsed * wobbleSpin;
    const float base    = m_center.y + std::max(lift, 0.0f);
    const float step    = TWO_PI / static_cast<float>(segments);

    for (int i = 0; i < segments; ++i) {
        const float angle = step * static_cast<float>(i);

        float bulge = 0.0f;
        if (distort) {
            // WHY (1 - cos) / 2 か: 値域が [0,1] に収まり、負へ落ちない。
            //     素直に sin を足すと内側へ凹む角度ができ、絵が当たり判定の内側へ
            //     入る瞬間が生まれる (6.4 の «見た目どおりに当たる» が破れる)。
            //     周期の違う 2 つを重ねるのは、1 つだと «花びら» に見えるため。
            const float a = 0.5f * (1.0f - std::cos(angle * 3.0f + phase));
            const float b = 0.5f * (1.0f - std::cos(angle * 7.0f - phase * 1.7f));
            bulge = a * 0.65f + b * 0.35f;
        }

        const float r = safe + std::max(wobbleAmplitude, 0.0f) * bulge;
        out.push_back({ m_center.x + std::cos(angle) * r,
                        base + std::max(wobbleLift, 0.0f) * bulge,
                        m_center.z + std::sin(angle) * r });
    }
}

inline void BossShockwaveComponent::WriteRing(EntityRef& ref, const char* suffix, float radius,
                                              float width, const Vector4& color)
{
    GameObject* object = RingObject(ref, suffix);
    if (!object) return;

    auto* line = object->GetComponent<LineRendererComponent>();
    if (!line) return;

    BuildCircle(m_points, radius, /*wobble=*/true);
    line->points         = m_points;
    line->enabled        = true;
    line->materialPath   = ringMaterial;
    line->radialSegments = std::clamp(radialSegments, 3, 16);
    line->startWidth     = width;
    line->endWidth       = width;
    line->startColor     = color;
    line->endColor       = color;
}

inline void BossShockwaveComponent::ShowRing()
{
    // 広がるほど薄く細くなる。円周が伸びても同じ濃さのままだと、届く範囲が
    // «増えている» のか «同じ量が広がっている» のかが絵から読めない。
    const float travelled = std::max(maxRadius - startRadius, EPSILON);
    const float progress  = Clamp01((m_radius - startRadius) / travelled);
    const float begin     = Clamp01(fadeFrom);
    const float fade      = progress <= begin
                          ? 1.0f
                          : 1.0f - Clamp01((progress - begin) / std::max(1.0f - begin, EPSILON));

    // 生まれた直後だけ強く太く。着弾が «開いた» 瞬間を作る一撃で、
    // ここが無いと輪は最初から同じ濃さで、ただ大きくなるだけに見える。
    const float flash = flashSeconds > 0.0f
                      ? Clamp01(1.0f - m_elapsed / flashSeconds)
                      : 0.0f;
    // 二乗で落とす。線形だと閃光の終わりが «急に暗くなった» 段差として見える。
    const float flash01 = flash * flash;

    const float width = std::max(bandWidth, 0.01f) * 2.0f * std::max(ringWidthScale, 0.01f);
    const float frontWidth = width * (0.35f + 0.65f * fade) * (1.0f + flash01);

    const Vector4 tint = WaveColor();
    const float   gain = std::max(intensity, 0.0f) * fade *
                         (1.0f + std::max(flashGain, 0.0f) * flash01);

    WriteRing(m_ring, "Front", m_radius, frontWidth,
              Vector4{ tint.x * gain, tint.y * gain, tint.z * gain, fade });

    // 後続は «通り過ぎた» 側なので必ず内側。前縁が生まれたばかりで内側に
    // 余地が無いうちは出さない (中心に潰れた点が 1 フレーム出るのを避ける)。
    const float wakeRadius = m_radius - std::max(wakeLag, 0.0f);
    if (wakeLag > 0.0f && wakeRadius > std::max(startRadius, 0.05f)) {
        const float wakeGain = gain * std::max(wakeIntensityScale, 0.0f);
        WriteRing(m_wake, "Wake", wakeRadius,
                  frontWidth * std::max(wakeWidthScale, 0.01f),
                  Vector4{ tint.x * wakeGain, tint.y * wakeGain, tint.z * wakeGain,
                           fade * Clamp01(wakeIntensityScale) });
    } else if (GameObject* wake = m_wake.Resolve(scene)) {
        // 条件を外れた輪は畳む。残すと «内側に張り付いた輪» が中心へ焼き付く。
        if (auto* line = wake->GetComponent<LineRendererComponent>()) line->enabled = false;
    }

    m_ringVisible = true;
}

inline void BossShockwaveComponent::HideRing()
{
    if (!m_ringVisible) return;
    m_ringVisible = false;

    for (EntityRef* ref : { &m_ring, &m_wake }) {
        GameObject* object = ref->Resolve(scene);
        if (!object) continue;
        if (auto* line = object->GetComponent<LineRendererComponent>()) {
            line->enabled = false;
            line->points.clear();
        }
    }
}

inline void BossShockwaveComponent::ResolveHit(GameObject* player)
{
    if (m_dealt || damage <= 0) return;

    const float distance = PlanarDistance(player->transform.worldPosition, m_center);
    const float reach    = std::max(bandWidth, 0.0f) + std::max(playerRadius, 0.0f);
    if (std::fabs(distance - m_radius) > reach) return;

    // 跳んで越えているか。足元の高さだけで決める (ヘッダー冒頭の WHY)。
    const float height = player->transform.worldPosition.y - m_center.y;
    if (height >= std::max(clearHeight, 0.0f)) return;

    m_dealt = true;
    debugDealt = true;

    auto* combat = CombatManagerComponent::Instance();
    if (!combat) {
        if (!m_warnedNoCombat) {
            m_warnedNoCombat = true;
            debug.LogError("BossShockwaveComponent found no CombatManagerComponent in the scene. "
                           "The landing shockwave deals no damage.");
        }
        return;
    }
    (void)combat->DamagePlayer(player, damage);
}

inline void BossShockwaveComponent::NotifyPass(const Vector3& playerPoint)
{
    if (m_passed) return;
    if (m_radius < PlanarDistance(playerPoint, m_center)) return;
    m_passed = true;

    // 通り過ぎた «場所» はプレイヤーの足元そのもの。減衰の中心を着地点にすると、
    // 遠くで跳んで越えたときだけ何も返らず、成功の合図が距離で消える。
    const float strength = Clamp01(passRumble);
    if (strength <= 0.0f) return;
    if (auto* pad = RumbleManagerComponent::Instance()) pad->Rumble(strength);
    if (auto* shake = CameraShakeManagerComponent::Instance())
        shake->Shake(strength * Clamp01(passShakeRatio));
}

inline void BossShockwaveComponent::OnLateUpdate()
{
    if (!m_active) return;

    // WHY 実時間ではなくゲーム時間で広げるか: 波は盤面の出来事で、避けるための
    //     «時刻» がヒットストップ中も進むと、止まっている画面の中で判定だけが動く。
    const float dt = std::max(Time::deltaTime, 0.0f);
    m_elapsed += dt;

    // 生まれた直後だけ速い。倍率は 1 以上にしかならないので、Speed が保証している
    // «走って逃げ切れない» 下限は崩れない。
    const float travelled = std::max(maxRadius - startRadius, EPSILON);
    const float progress  = Clamp01((m_radius - startRadius) / travelled);
    const float boost     = 1.0f + std::max(burstMultiplier - 1.0f, 0.0f) *
                                   std::exp(-progress * std::max(burstFalloff, 0.01f));

    m_radius += std::max(speed, 0.0f) * boost * dt;
    debugRadius = m_radius;

    if (m_radius >= std::max(maxRadius, startRadius)) {
        Cancel();
        return;
    }

    ShowRing();

    if (GameObject* player = scene.FindWithTag(playerTag)) {
        ResolveHit(player);
        NotifyPass(player->transform.worldPosition);
    }

    // WHY 帯を «別に» 引くか: リングは当たりより太く描いてあるので (Ring Width Scale)、
    //     見えている輪と当たる輪は一致しない。重ねて初めて «どれだけ外に描いてあるか»
    //     が見える。
    if (drawDebugBand) {
        const Vector4 edge{ 1.0f, 0.85f, 0.2f, 1.0f };
        std::vector<Vector3> band;
        for (const float r : { std::max(m_radius - bandWidth, 0.05f), m_radius + bandWidth }) {
            // 歪みは乗せない。ここで見たいのは «実際に当たる円» そのもの。
            BuildCircle(band, r, /*wobble=*/false);
            for (std::size_t i = 0; i < band.size(); ++i)
                debug.DrawLine(band[i], band[(i + 1) % band.size()], edge);
        }
    }
}

} // namespace sandbox
