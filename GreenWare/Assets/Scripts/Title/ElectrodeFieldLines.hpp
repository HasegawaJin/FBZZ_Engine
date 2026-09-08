/// @file    ElectrodeFieldLines.hpp
/// @brief   電荷の芯から外へ伸びる電気力線。極を «線の湧き出し口» として見せる。
/// @author  Hasegawa Jin
/// @date    2026-08-24
///
/// WHY 放電と別にもう 1 種類の線を引くか:
///   放電 (ElectricArc) は «対» の持ち物で、＋と−の間だけを繋ぐ。極を 1 本だけ見たとき、
///   そこから何かが出ているようには見えない。力線は 1 個の電荷の持ち物なので、
///   極ごとに放射状へ生やせる。«芯に電荷があって、そこから線が出ている» を作るのはこちら。
///
/// WHY まっすぐ伸ばさず場を積分するか:
///   放射状の直線を生やすだけだと «ウニ» で、盤面に電荷が 2 つある事実が線に出ない。
///   相手を含めた場 E = Σ q(x - p)/|x - p|^3 を積分すると、相手側へ向いた線は引き寄せられ、
///   反対側の線は逃げていく。引力と斥力が線の «形» から読めるようになる。
///
/// WHY 種を画面平面 (ワールド XY) に置くか:
///   ElectrodeCore と同じ理由。極は XZ 平面を周回するので、極の並びを基準に種を撒くと
///   組がカメラ方向を向いた瞬間に線束が 1 本へ潰れる。出口だけ画面平面に固定しておけば、
///   極がどこへ動いても «芯から四方へ出ている» ように見える (曲がり方は 3 次元のまま)。
#pragma once

#include <Engine/Scene/Components/PresentationComponents.hpp>
#include <Engine/Scene/EntityRef.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include <Math/Vector4.hpp>
#include <Scripts/Title/ElectrodePole.hpp>
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using namespace fbzz::scene;
using namespace fbzz::math;

namespace sandbox {

/// 力線を曲げる電荷 1 個。ElectrodeRig が盤面の全極ぶんを組んで渡す。
struct ElectrodeCharge {
    Vector3 position = Vector3::ZERO;
    /// ＋で +1、−で −1。電荷量は全極で等しい前提 (タイトルは対の演出しか置かない)。
    float   sign     = 1.0f;
};

/// 1 本の極から出る力線束の見た目の設定。
struct ElectrodeFieldStyle {
    /// 芯から出る本数。0 で力線を出さない。
    int   lineCount   = 10;
    /// 1 本あたりの折れ点数。増やしても «曲がりの滑らかさ» にしか効かない。
    /// WHY 長さを伸ばしたら点も増やすか: 刻み幅は length / segments なので、
    ///     長いまま点が少ないと極のそばの «曲がりのきつい所» で追従できず、
    ///     線が相手を素通りして裏へ回る。
    int   segments    = 44;
    /// 線の道のり [m]。直線距離ではないので、曲がるぶん到達点は近くなる。
    ///
    /// WHY 既定を «相手に届く» 長さにするか:
    ///   途中で切ると 2 つの電荷が «たまたま隣にある別のもの» に見える。
    ///   ＋から出た線が−へ吸い込まれて終わると、はじめて対が 1 つの系に見える。
    ///   届かなかった線 (相手と逆を向いて出たもの) は先細りして消え、
    ///   «外へ逃げる力線» として残る。
    float length      = 9.0f;
    /// 湧き出し口の半径 [m]。端子の輪より外に置かないと符号が線に埋もれる。
    float startRadius = 0.75f;
    float width       = 0.055f;
    /// 先端の太さの比 [0,1]。シェーダーに端のフェードが無いので、細らせて消し際を作る。
    float tipTaper    = 0.1f;
    float intensity   = 1.1f;
    /// 線を流れる粒の数 (1 本あたり)。0 で粒を出さない。
    float beads       = 4.0f;
    /// 粒の速さ。向きは極性が決める (＋は外向き / −は内向き)。
    float travel      = 1.1f;
    /// 束全体を回す角速度 [deg/s]。0 で止まる。
    float spin        = 7.0f;
    /// 種の角度を隣の線との間隔の何割ずらすか [0,1)。
    ///
    /// WHY 極ごとにずらすか:
    ///   ＋から引いた線と−から引いた線は同じ場の同じ族なので、種の角度が揃うと
    ///   同じ曲線を 2 度描いて «明るさだけ倍の 1 本» になる。半間隔ずらすと、
    ///   力線どうしは交わらない性質から入れ子の束になり、間が編んだように詰まる。
    float seedStagger = 0.0f;

    /// 根元の色 = この極の極性色。ElectrodeRig が ElectrodePole から入れる。
    Vector4 color     = { 1.0f, 0.16f, 0.18f, 1.0f };
    /// 相手の極まで届いた線の先端の色 = 逆極の極性色。対になっていることを線で示す。
    Vector4 tipColor  = { 0.10f, 0.35f, 1.00f, 1.0f };
    Vector4 coreColor = { 2.6f, 2.5f, 2.4f, 1.0f };
    float   coreWidth   = 0.30f;
    float   glowFalloff = 2.2f;
    float   coreTint    = 0.7f;

    std::string materialPath = "Assets/Materials/Effects/ElectricArc.mat";
};

/// 場の強さ。芯の中で 1/r^2 が発散すると 1 フレームで線が画面外まで飛ぶので、
/// 極に潰れるほど近い点は寄与から外す。
[[nodiscard]] inline Vector3 ElectrodeFieldAt(const Vector3& point,
                                              const std::vector<ElectrodeCharge>& charges)
{
    Vector3 field = Vector3::ZERO;
    for (const ElectrodeCharge& charge : charges) {
        const Vector3 delta  = point - charge.position;
        const float   distSq = delta.LengthSq();
        if (distSq <= 1.0e-4f) continue;
        field = field + delta * (charge.sign / (distSq * std::sqrt(distSq)));
    }
    return field;
}

/// 進む向き。flow は ＋極で +1 (場に沿って外へ)、−極で −1 (場を遡って外へ)。
[[nodiscard]] inline Vector3 ElectrodeFlowDirection(const Vector3& point, float flow,
                                                    const std::vector<ElectrodeCharge>& charges,
                                                    const Vector3& fallback)
{
    const Vector3 field  = ElectrodeFieldAt(point, charges) * flow;
    const float   length = field.Length();
    // 場が打ち消し合う中立点では向きが決まらない。直前の向きのまま通り抜ける。
    return length > EPSILON ? field * (1.0f / length) : fallback;
}

/// 電極 1 本ぶんの力線束。線ごとに 1 つの LineRenderer を実行時に作って持つ。
class ElectrodeFieldLines {
public:
    /// 毎フレーム呼ぶ。origin と charges はワールド座標。
    void Update(Script& owner, Pole pole, const Vector3& origin,
                const std::vector<ElectrodeCharge>& charges,
                const ElectrodeFieldStyle& style, float dt);
    /// 線ごと片付ける。
    void Detach(const Script& owner);

private:
    void EnsureLines(Script& owner, std::size_t count, const ElectrodeFieldStyle& style);
    /// 1 本ぶんの経路を組む。逆極の芯に着いて打ち切ったときだけ true。
    [[nodiscard]] bool BuildPath(std::vector<Vector3>& out, const Vector3& origin,
                                 const Vector3& seedDirection, float flow,
                                 const std::vector<ElectrodeCharge>& charges,
                                 const ElectrodeFieldStyle& style) const;

    std::vector<EntityID> m_lines;
    std::vector<Vector3>  m_points;   // 毎フレームの再確保を避けるための作業領域
    float                 m_spin  = 0.0f;
    float                 m_phase = 0.0f;
};

inline void ElectrodeFieldLines::EnsureLines(Script& owner, std::size_t count,
                                             const ElectrodeFieldStyle& style)
{
    while (m_lines.size() > count) {
        if (GameObject* object = owner.scene.GetGameObject(m_lines.back()))
            owner.scene.Destroy(*object);
        m_lines.pop_back();
    }

    while (m_lines.size() < count) {
        // ElectricArc / ElectrodeCore と同じく、World 空間の点を素直に渡すため
        // 原点・無回転のルートへ置く。
        GameObject& object = owner.scene.Create("ElectrodeField_Line");
        const EntityID id  = object.GetID();
        object.runtimeGenerated   = true;
        object.transform.position = Vector3::ZERO;
        m_lines.push_back(id);

        if (GameObject* created = owner.scene.GetGameObject(id)) {
            auto& line = created->AddComponent<LineRendererComponent>();
            line.materialPath = style.materialPath;
            line.space        = LineSpace::World;
            line.billboard    = true;
            line.loop         = false;
            // 記号 (40) より後ろ、放電より前。線が符号を隠さない順にする。
            line.orderInLayer = 10;
        }
    }
}

inline bool ElectrodeFieldLines::BuildPath(std::vector<Vector3>& out, const Vector3& origin,
                                           const Vector3& seedDirection, float flow,
                                           const std::vector<ElectrodeCharge>& charges,
                                           const ElectrodeFieldStyle& style) const
{
    const int   count  = (std::max)(2, (std::min)(style.segments, 64));
    const float radius = Max(style.startRadius, 0.01f);
    const float step   = Max(style.length, 0.01f) / static_cast<float>(count);

    out.clear();
    out.reserve(static_cast<std::size_t>(count) + 1);

    Vector3 point   = origin + seedDirection * radius;
    Vector3 heading = seedDirection;
    out.push_back(point);

    for (int i = 0; i < count; ++i) {
        // 中点法。前進オイラーだと極の近くで曲率に追いつけず、線が芯を突き抜けて裏へ回る。
        const Vector3 first  = ElectrodeFlowDirection(point, flow, charges, heading);
        const Vector3 middle = ElectrodeFlowDirection(point + first * (step * 0.5f), flow,
                                                      charges, first);
        point   = point + middle * step;
        heading = middle;
        out.push_back(point);

        // 逆極の芯へ着いたら止める。中まで潜らせると芯の中の 1/r^2 に振り回される。
        for (const ElectrodeCharge& charge : charges) {
            if ((point - charge.position).LengthSq() <= radius * radius)
                return true;
        }
    }
    return false;
}

inline void ElectrodeFieldLines::Update(Script& owner, Pole pole, const Vector3& origin,
                                        const std::vector<ElectrodeCharge>& charges,
                                        const ElectrodeFieldStyle& style, float dt)
{
    const std::size_t wanted = style.length <= 0.0f || style.lineCount <= 0
        ? 0u
        : static_cast<std::size_t>((std::min)(style.lineCount, 32));
    EnsureLines(owner, wanted, style);
    if (m_lines.empty()) return;

    m_spin  = std::fmod(m_spin + ToRad(style.spin) * dt, TWO_PI);
    // ElectricArc.hlsl の frac(sin(x * 12.9898)) は x が育つほど精度を失う。巻き取る。
    m_phase = std::fmod(m_phase + dt, 1024.0f);

    // ＋は場に沿って外へ、−は場を遡って外へ。符号 1 つで «出る / 入る» が入れ替わる。
    const float flow = pole == Pole::Minus ? -1.0f : 1.0f;
    // 輝点の流れる向きは «電荷から見た向き» に合わせる。−極は根元へ吸い込まれて見える。
    const float travel = style.travel * flow;

    const float width   = Max(style.width, 0.001f);
    const float spacing = TWO_PI / static_cast<float>(m_lines.size());

    for (std::size_t index = 0; index < m_lines.size(); ++index) {
        GameObject* object = owner.scene.GetGameObject(m_lines[index]);
        if (!object) continue;
        auto* line = object->GetComponent<LineRendererComponent>();
        if (!line) continue;

        const float   angle = m_spin
            + spacing * (static_cast<float>(index) + style.seedStagger);
        const Vector3 seed  = Vector3::RIGHT * std::cos(angle) + Vector3::UP * std::sin(angle);
        const bool    landed = BuildPath(m_points, origin, seed, flow, charges, style);

        // 逃げた線の先端は «同じ色相のまま暗く» する。黒にすると、アルファ合成のまま
        // 背景を削って黒い髪の毛が残る。相手へ着いた線は逆極の色で終わらせて対を示す。
        const Vector4 tip = landed
            ? style.tipColor
            : Vector4{ style.color.x * 0.12f, style.color.y * 0.12f,
                       style.color.z * 0.12f, style.color.w };

        line->points     = m_points;
        line->enabled    = true;
        line->space      = LineSpace::World;
        line->billboard  = true;
        line->loop       = false;
        line->startWidth = width;
        // 相手に着いた線は太さを保って «繋がっている» を見せ、逃げた線は細って消える。
        line->endWidth   = landed ? width : width * Clamp01(style.tipTaper);
        line->startColor = style.color;
        line->endColor   = tip;

        const MaterialInstance instance = owner.material.Instance(EntityRef{ m_lines[index] });
        if (!instance.HasProperty(MaterialPropertyId("coreColor"))) continue;

        instance.SetVector4(MaterialPropertyId("coreColor"), style.coreColor);
        instance.SetVector4(MaterialPropertyId("tipColor"),  tip);
        instance.SetFloat(MaterialPropertyId("coreWidth"),   Clamp01(style.coreWidth));
        instance.SetFloat(MaterialPropertyId("glowFalloff"), Max(style.glowFalloff, 0.01f));
        instance.SetFloat(MaterialPropertyId("coreTint"),    Clamp01(style.coreTint));
        // 相手に着いた線は場が «通っている» ぶん明るく。逃げた線は控えめに引く。
        instance.SetFloat(MaterialPropertyId("intensity"),
                          Max(style.intensity, 0.0f) * (landed ? 1.0f : 0.55f));
        // 線ごとに位相をずらす。揃えると粒が横並びで進み、線ではなく «櫛» に見える。
        instance.SetFloat(MaterialPropertyId("phase"),
                          m_phase + static_cast<float>(index) * 0.37f);
        instance.SetFloat(MaterialPropertyId("travel"), travel);
        // 粒は «相手へ流れ込んでいる» 線にだけ乗せる。逃げた線にも流すと、
        // どこへも着かない粒が画面外へ出続けて «漏れている» ように見える。
        instance.SetFloat(MaterialPropertyId("beadDensity"),
                          landed ? Max(style.beads, 0.0f) : 0.0f);
        instance.SetFloat(MaterialPropertyId("beadFalloff"), 14.0f);
        // 力線は途切れない。放電と同じ揺らぎを乗せると «弱い放電» にしか見えなくなる。
        instance.SetFloat(MaterialPropertyId("breakup"), 0.0f);
        // 侵食はきっかり 0 にしない (ElectrodeCore の同じ行の WHY を参照)。
        instance.SetFloat(MaterialPropertyId("erode"),   0.02f);
    }
}

inline void ElectrodeFieldLines::Detach(const Script& owner)
{
    for (const EntityID id : m_lines) {
        if (GameObject* object = owner.scene.GetGameObject(id))
            owner.scene.Destroy(*object);
    }
    m_lines.clear();
}

} // namespace sandbox
