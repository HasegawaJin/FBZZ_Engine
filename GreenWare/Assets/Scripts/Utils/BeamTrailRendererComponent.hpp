/// @file    BeamTrailRendererComponent.hpp
/// @brief   照射ビームの帯 1 層。頂点そのものを揺らした帯を 2 点間へ張る。
/// @author  Hasegawa Jin
/// @date    2026-08-26
///
/// WHY エンジンの Trail / Line をそのまま使わないか:
///   TrailComponent は «動いた軌跡» を記録して帯にするもので、照射ビームのように
///   «毎フレーム端点が飛ぶ線» を渡すと履歴が繋がって尾を引く。LineRendererComponent は
///   点列から帯を焼くところまでは正しいが、点列を «誰が・どう揺らすか» を持たない。
///   結果、これまでのビームは銃口と着弾点の 2 点だけを渡した完全な直線で、
///   帯が動いているように見せる仕事はすべてシェーダーの UV 任せになっていた。
///   UV の蛇行は «帯の中で芯が動く» までしか作れないので、帯の輪郭は硬い直線のまま残る。
///
/// WHY それでも LineRenderer の «上に» 乗るか:
///   頂点バッファを作れるのは ResourceManager を持つエンジン側だけで、スクリプトからは
///   触れない。ここが持つべきなのは «帯をどう曲げるか» であって GPU 転送ではないので、
///   点列の生成と .mat への流し込みをこのコンポーネントが持ち、焼くのは
///   PresentationSystem (Phase::LateUpdate) に任せる。役割はそこで切れている。
///
/// WHY 当たり判定は直線のままか:
///   塗る相手を決めるのは PlayerAimComponent が引いた線分で、この帯ではない。
///   帯を曲げた分だけ判定がずれると、企画書 6.4 の «見た目どおりに当たる» が崩れる。
///   そこで揺れは両端で必ず 0 に落とし、平均が直線と一致する «振れ» としてだけ乗せる。
///   銃口から生えていて着弾点へ刺さっていれば、途中がうねっていても線は線に見える。
///
/// WHY 波を «流す» か:
///   同じ形のまま振幅だけ動かすと «たわんだ棒» になる。銃口から着弾点へ波が抜けていくと、
///   同じ振幅でも «電流が通っている管» に見える。向きが読めることが要点なので、
///   travel の符号は «銃口 → 着弾点» を正にしてある。
///
/// WHY 芯と裾で別々の形にしないか:
///   裾は芯のまわりの光であって別の線ではない。独立に揺らすと 2 本の帯が交差して
///   «光が二重にある» ことがはっきり見えてしまう。同じ seed の同じ波を、
///   wobbleScale で浅くして渡す。裾は芯を鈍く追いかける。
#pragma once

#include <Engine/Scene/Components/MaterialComponent.hpp>
#include <Engine/Scene/Components/PresentationComponents.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>
#include <Scripts/Utils/ElectricArc.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

using namespace fbzz::scene;
using namespace fbzz::math;
using fbzz::Time;

namespace sandbox {

/// 帯 1 層ぶんの見た目。持ち主が毎フレーム組んで渡す。
///
/// WHY Inspector ではなく引数で受けるか:
///   同じ .mat と同じ形を左右 2 本 × 芯 / 裾の 4 枚が共有し、その中で層ごとに違うのは
///   数項目しかない。4 つの Inspector へ同じ値を並べると、調整のたびに 4 か所を
///   同じ数字で埋めることになり、1 か所ずらしたまま気付けない。値の正本は
///   撃っている側 (BossBeamComponent) に 1 つあれば足りる。
struct BeamTrailStyle {
    // ── 帯 ──
    /// 帯の太さ [m]。当たり判定の太さとは無関係。
    float   width = 0.09f;
    /// HDR の線色。LineRenderer が albedo へ流す。
    Vector4 color = { 1.0f, 0.12f, 0.14f, 1.0f };
    /// 芯層か裾層か。シェーダーの層別要素 (撚り・管の壁・リング) の入り切りを決める。
    bool    isCore = true;
    int     orderInLayer = 1;
    std::string materialPath = "Assets/Materials/Effects/FX_BOSS_Beam.mat";

    // ── 断面 ──
    /// 板 (常にカメラを向く) か、実体のある筒か。
    ///
    /// WHY 板を既定に残すか: プレイヤーのビームは «撚られた電流» で、細い芯が何本も
    ///     絡んで見えることが要点になっている。筒にすると芯が筒の «中» へ入り、
    ///     手前の壁越しに見ることになって撚りが読めなくなる。太い線ほど筒が効く。
    LineShape shape = LineShape::Ribbon;
    /// 筒の円周分割数。
    int radialSegments = 10;
    /// 筒の半径 [m]。シェーダーが視線と円柱を交差させるのに使う。
    /// WHY width から自動で出さないか: 裾層は «芯より太い筒» として張るが、断面を
    ///     解くときの基準は自分の半径でなければならない。層ごとに違う値になる。
    float tubeRadius = 0.0f;

    // ── 頂点の揺らぎ ──
    /// 左右で違う形にするための鍵。芯と裾には同じ値を渡すこと (同じ波を共有する)。
    uint32_t seed = 1u;
    /// 10m 先を撃ったときに帯が振れる幅 [m]。
    ///
    /// WHY 長さに比例させるか: 放電 (ElectricArcBundle) と同じ理由。固定幅にすると
    ///     40m 先を撃ったときに «少し太い直線» にしかならず、近距離とは別の武器に見える。
    float wobble = 0.10f;
    /// 芯の揺れをこの層がどれだけ追うか [0,1]。裾を 1 にすると芯と一緒に泳ぐ。
    float wobbleScale = 1.0f;
    /// 帯 1 本あたりの波の数。
    float wobbleFrequency = 3.0f;
    /// 波が銃口から着弾点へ抜ける速さ [周/秒]。負で銃口へ向かって戻る。
    float wobbleTravel = 1.6f;
    /// 振れが最大になる位置 [0,1]。両端は必ず 0。
    float wobbleBias = 0.68f;
    /// 1m あたりの折れ点数。少ないと «角» が見え、多くしても絵は変わらない。
    float segmentsPerMeter = 2.0f;

    // ── シェーダーへ渡す断面と流れ (Beam.hlsl の MaterialConstants) ──
    float coreWidth   = 0.45f;
    float edgeFalloff = 2.0f;
    float coreBoost   = 2.2f;
    float tiling      = 6.0f;
    float scroll      = 0.0f;
    float muzzleFade  = 0.04f;
    float tipFade     = 0.08f;
    float phase       = 0.0f;
    float arcAmp      = 0.35f;
    float arcFreq     = 9.0f;
    float crackle     = 0.45f;
    float flicker     = 0.22f;
    float beadDensity = 4.0f;
    float beadFalloff = 12.0f;
    float surge       = 0.0f;
};

/// 2 点間へ帯を 1 層張る。1 つの GameObject につき 1 層。
///
/// WHY 1 層 1 コンポーネントか:
///   帯を焼く LineRendererComponent は 1 GameObject に 1 つしか載らない。層ごとに
///   GameObject を分ける以上、点列と .mat を持つ側も同じ粒度で分かれているのが素直で、
///   «どの GameObject がどの帯か» をヒエラルキーだけで読めるようにもなる。
class BeamTrailRendererComponent : public Script {
    FBZZ_SCRIPT(BeamTrailRendererComponent)

public:
    FBZZ_GROUP("デバッグ")
    FBZZ_FIELD(bool, drawDebugPath, false, "Draw Debug Path")
    FBZZ_TOOLTIP("生成した折れ線をデバッグ線で重ねる。揺れが判定線からどれだけ"
                 "外れているかを確かめるためのもの")

    /// 毎フレーム呼ぶ。端点はワールド座標。
    void Show(const Vector3& from, const Vector3& to, const BeamTrailStyle& style);
    /// 描画を止める。点列は残すので、再開しても形は連続する。
    void Hide();

    /// 帯を張れる状態か。実行時に足したコンポーネントは、ScriptSystem が
    /// SetContext を通すまで scene / material プロキシが繋がっていない。
    [[nodiscard]] bool IsReady() const { return m_ready; }

    void OnStart() override;

private:
    /// 直線を折れ線へ開き、両端を残したまま途中を振らせる。
    void BuildPath(const Vector3& from, const Vector3& to, const BeamTrailStyle& style);
    /// 断面と流れを .mat のシェーダーへ送る。
    void PushMaterial(const BeamTrailStyle& style, float length) const;

    std::vector<Vector3> m_points; // 毎フレームの再確保を避けるための作業領域
    bool m_ready   = false;
    bool m_visible = false;
};

FBZZ_REFLECT(BeamTrailRendererComponent)

// Beam.hlsl の MaterialConstants に対応する名前。.mat の [params] のキーであると同時に、
// HLSL の cbuffer メンバー名でもある (MaterialInstance はシェーダーリフレクションで検証する)。
inline constexpr MaterialPropertyId kBeamTrailCoreWidthId  { "coreWidth" };
inline constexpr MaterialPropertyId kBeamTrailEdgeFalloffId{ "edgeFalloff" };
inline constexpr MaterialPropertyId kBeamTrailCoreBoostId  { "coreBoost" };
inline constexpr MaterialPropertyId kBeamTrailTilingId     { "tiling" };
inline constexpr MaterialPropertyId kBeamTrailScrollId     { "scroll" };
inline constexpr MaterialPropertyId kBeamTrailMuzzleFadeId { "muzzleFade" };
inline constexpr MaterialPropertyId kBeamTrailTipFadeId    { "tipFade" };
inline constexpr MaterialPropertyId kBeamTrailPhaseId      { "phase" };
inline constexpr MaterialPropertyId kBeamTrailArcAmpId     { "arcAmp" };
inline constexpr MaterialPropertyId kBeamTrailArcFreqId    { "arcFreq" };
inline constexpr MaterialPropertyId kBeamTrailCrackleId    { "crackle" };
inline constexpr MaterialPropertyId kBeamTrailFlickerId    { "flicker" };
inline constexpr MaterialPropertyId kBeamTrailBeadsId      { "beadDensity" };
inline constexpr MaterialPropertyId kBeamTrailBeadFallId   { "beadFalloff" };
inline constexpr MaterialPropertyId kBeamTrailLayerId      { "layer" };
inline constexpr MaterialPropertyId kBeamTrailSurgeId      { "surge" };
/// 筒のシェーダーだけが持つ。板用の .mat には無いので、別に門を構えて書く。
inline constexpr MaterialPropertyId kBeamTrailTubeRadiusId { "tubeRadius" };

/// 折れ点の下限と上限。下限を割ると «角のある直線»、上限を超えても画面では差が出ない。
inline constexpr int kBeamTrailMinSegments = 10;
inline constexpr int kBeamTrailMaxSegments = 64;

/// 波 1 周あたりに最低限置く折れ点の数。
///
/// WHY 長さだけで折れ点を決めないか:
///   LineRenderer は区間ごとに独立した四角形を焼くので、隣り合う区間の向きが変わると
///   その «蝶番» の外側に楔形の隙間が残る。角度差は «1 区間で波がどれだけ曲がるか» で
///   決まるため、波を細かくしたときに折れ点が足りないと、うねりではなく帯の縁の
///   ギザギザとして出る。長さ由来の点数とこの下限の大きい方を採る。
inline constexpr float kBeamTrailSamplesPerWave = 16.0f;

/// 振れ幅の基準距離 [m]。放電 (ElectricArcBundle) と同じ値を使う。
/// 揃えないと、同じ 1 本の線の «帯» と «そこから漏れる放電» が別々の距離感で振れる。
inline constexpr float kBeamTrailReferenceRange = 10.0f;

/// 波の位相を巻き取る周期。float の桁が落ちて波が縞へ潰れるのを防ぐ保険で、
/// 実際には «撃ちっぱなしで 10 時間» 相当なので踏まない。
inline constexpr float kBeamTrailTimeWrap = 65536.0f;

// ── 実装 (inline) ─────────────────────────────────────────────────────────────

inline void BeamTrailRendererComponent::OnStart()
{
    // WHY 原点・無回転へ固定するか: LineRenderer の World 空間は、渡したワールド点を
    //     所有 GameObject のローカルへ引き戻してからメッシュにする。親に付けたり
    //     回したりすると、その変換ぶんだけ端点がずれる (ElectricArc と同じ)。
    if (GameObject* self = scene.Self()) {
        self->transform.position = Vector3::ZERO;
        self->transform.rotation = Quaternion::Identity();
        self->transform.scale    = Vector3::ONE;
    }
    m_ready   = true;
    m_visible = false;
}

inline void BeamTrailRendererComponent::BuildPath(const Vector3& from, const Vector3& to,
                                                  const BeamTrailStyle& style)
{
    const Vector3 delta  = to - from;
    const float   length = delta.Length();

    m_points.clear();
    if (length <= EPSILON) {
        m_points.push_back(from);
        m_points.push_back(to);
        return;
    }

    const float frequency = Max(style.wobbleFrequency, 0.0f);
    const float wanted = Max(length * Max(style.segmentsPerMeter, 0.1f),
                             frequency * kBeamTrailSamplesPerWave);
    const int count = static_cast<int>(Clamp(wanted,
                                             static_cast<float>(kBeamTrailMinSegments),
                                             static_cast<float>(kBeamTrailMaxSegments)));
    m_points.reserve(static_cast<std::size_t>(count) + 1);

    const Vector3 axis = delta * (1.0f / length);
    // 軸に垂直な 2 軸。軸が真上に近いときだけ基準を前方へ倒す (外積が縮退するため)。
    const Vector3 reference = Abs(axis.y) > 0.9f ? Vector3::FORWARD : Vector3::UP;
    const Vector3 side = Vector3::Cross(axis, reference).Normalized();
    const Vector3 up   = Vector3::Cross(side, axis);

    const float amplitude = Max(style.wobble, 0.0f) * Clamp01(style.wobbleScale)
                          * Clamp(length / kBeamTrailReferenceRange, 0.35f, 3.0f);
    // WHY Time::time を直に使うか: ヒットストップで止まる時計をそのまま使うことで、
    //     画面が止まっている間は帯も止まる。位相を自前で積むと、止まった画面で
    //     ビームだけが泳ぎ続けて «時間が止まったこと» の方が嘘に見える。
    const float travel = std::fmod(Time::time, kBeamTrailTimeWrap) * style.wobbleTravel;

    for (int i = 0; i <= count; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(count);
        // 両端は 0。銃口から生えて着弾点へ刺さっている限り、途中は自由に振れてよい。
        const float taper = ArcTaper(t, style.wobbleBias);

        // 2 オクターブ。1 本調子の正弦にすると «たわんだ紐» になって電流に見えない。
        // 位相から travel を引くと、波が t の増える向き = 着弾点へ向かって流れる。
        //
        // WHY 細かい方を 2 倍程度に留めるか: 折れ点あたりの曲がり角は周波数の 2 乗で
        //     効く。3 倍 4 倍にすると、同じ折れ点数では帯の継ぎ目が段差として見え始める。
        const float wave = t * frequency - travel;
        const float fine = t * frequency * 2.1f - travel * 1.7f;
        const float nx = ArcNoise(style.seed,           wave) * 0.74f
                       + ArcNoise(style.seed + 7919u,   fine) * 0.26f;
        const float ny = ArcNoise(style.seed + 104729u, wave) * 0.74f
                       + ArcNoise(style.seed + 15485u,  fine) * 0.26f;

        m_points.push_back(from + delta * t + (side * nx + up * ny) * (amplitude * taper));
    }

    // 端は必ず «渡された点そのもの» にする。taper が 0 なので計算上も一致するが、
    // 丸め残りで銃口から数 mm 浮くと、発射口とビームの間に隙間が見える。
    m_points.front() = from;
    m_points.back()  = to;
}

inline void BeamTrailRendererComponent::PushMaterial(const BeamTrailStyle& style,
                                                     float length) const
{
    const MaterialInstance instance = material.Instance();

    // WHY HasProperty で先に門を閉めるか: MaterialComponent を張るのは LineRenderer 側
    //     (Phase::LateUpdate) なので、張り始めた最初の 1 フレームはまだ存在しない。
    //     Set 系は空振りのたびに警告を出すため、そのまま呼ぶと Console が埋まる。
    //     Beam.hlsl 以外の .mat を差した場合もここで静かに止まる。
    if (!instance.HasProperty(kBeamTrailCoreWidthId)) return;

    // 芯層は «芯のある断面»、裾層は «芯を持たない裾» にする。同じ形を 2 枚重ねると
    // 太さが変わるだけで、企画書 12.3 が言う 2 層の役割分担にならない。
    instance.SetFloat(kBeamTrailCoreWidthId,   style.isCore ? style.coreWidth : 0.0f);
    instance.SetFloat(kBeamTrailEdgeFalloffId, style.edgeFalloff);
    instance.SetFloat(kBeamTrailCoreBoostId,   style.isCore ? style.coreBoost : 0.0f);

    // 模様の密度は長さから決める。uv.x は常に [0,1] なので、タイルしないと
    // 近くを撃つほど模様が間延びし、同じビームが距離で別物に見える。
    instance.SetFloat(kBeamTrailTilingId, Max(length, 0.0f) * Max(style.tiling, 0.0f));
    instance.SetFloat(kBeamTrailScrollId,     style.scroll);
    instance.SetFloat(kBeamTrailMuzzleFadeId, style.muzzleFade);
    instance.SetFloat(kBeamTrailTipFadeId,    style.tipFade);

    // 帯電の乱れは別に門を構える。Beam.hlsl 由来ではない .mat (断面だけ同じ自作
    // シェーダー) を差した場合、ここを通すと «毎フレーム × 層 × 項目数» の
    // «そんなプロパティは無い» で Console が埋まる。
    if (!instance.HasProperty(kBeamTrailPhaseId)) return;

    instance.SetFloat(kBeamTrailPhaseId,   style.phase);
    instance.SetFloat(kBeamTrailArcAmpId,  style.arcAmp);
    instance.SetFloat(kBeamTrailArcFreqId, style.arcFreq);
    instance.SetFloat(kBeamTrailCrackleId, style.crackle);
    instance.SetFloat(kBeamTrailFlickerId, style.flicker);
    // 粒は芯だけに流す。裾にも流すと «光の玉が 2 重に走る» ので数が読めなくなる。
    instance.SetFloat(kBeamTrailBeadsId,    style.isCore ? style.beadDensity : 0.0f);
    instance.SetFloat(kBeamTrailBeadFallId, style.beadFalloff);

    // 立体まわりは古い Beam.hlsl を指した .mat には無い。ここも別に門を構える。
    if (!instance.HasProperty(kBeamTrailLayerId)) return;
    instance.SetFloat(kBeamTrailLayerId, style.isCore ? 1.0f : 0.0f);
    instance.SetFloat(kBeamTrailSurgeId, Clamp01(style.surge));

    // 筒の半径。板のシェーダーは持っていないので、ここでも門を構える。
    // 0 を書けば «板として断面を作る» 側へ倒れるので、形と絵が食い違わない。
    if (!instance.HasProperty(kBeamTrailTubeRadiusId)) return;
    instance.SetFloat(kBeamTrailTubeRadiusId,
                      style.shape == LineShape::Tube ? Max(style.tubeRadius, 0.0f) : 0.0f);
}

inline void BeamTrailRendererComponent::Show(const Vector3& from, const Vector3& to,
                                             const BeamTrailStyle& style)
{
    if (!m_ready) return;

    GameObject* self = scene.Self();
    if (!self) return;

    auto* line = self->GetComponent<LineRendererComponent>();
    if (!line) line = &self->AddComponent<LineRendererComponent>();

    BuildPath(from, to, style);

    // 拾い直した個体にも毎回入れ直す。Inspector で触った直後に Play し直しても
    // 反映されないと、調整のたびにビームを消して回ることになる。
    line->materialPath = style.materialPath;
    line->space        = LineSpace::World;
    // 筒は形が視点に依存しない。billboard を残しておくと、板へ戻したときに
    // «向きを作り直さない板» という誰も望まない組み合わせができる。
    line->billboard    = style.shape == LineShape::Ribbon;
    line->shape        = style.shape;
    line->radialSegments = style.radialSegments;
    line->loop         = false;
    line->orderInLayer = style.orderInLayer;
    line->points       = m_points;
    line->startWidth   = style.width;
    line->endWidth     = style.width;
    // 描画に効くのは startColor だけ (PresentationSystem がこれを albedo へ流す)。
    // endColor も揃えておかないと、Inspector で見たときに嘘の情報になる。
    line->startColor   = style.color;
    line->endColor     = style.color;
    line->enabled      = true;
    m_visible = true;

    PushMaterial(style, (to - from).Length());

    if (drawDebugPath)
        for (std::size_t i = 1; i < m_points.size(); ++i)
            debug.DrawLine(m_points[i - 1], m_points[i], style.color);
}

inline void BeamTrailRendererComponent::Hide()
{
    if (!m_visible) return;
    m_visible = false;

    // WHY SetActive ではなく enabled か: PresentationSystem は GameObject の有効・無効を
    //     見ずに全 LineRendererComponent を回す。enabled を落とすと同じ関数の中で
    //     MeshRenderer まで無効にしてくれるので、消し方が 1 箇所に閉じる。
    if (GameObject* self = scene.Self())
        if (auto* line = self->GetComponent<LineRendererComponent>())
            line->enabled = false;
}

} // namespace sandbox
