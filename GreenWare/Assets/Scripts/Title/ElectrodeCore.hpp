/// @file    ElectrodeCore.hpp
/// @brief   電極の芯に立てる ＋ / − の端子。輪 + 符号の 2 部品でできた小さな紋章。
/// @author  Hasegawa Jin
/// @date    2026-08-24
///
/// WHY 色があるのに記号も出すか:
///   PolarityTypes.hpp の 12.3 が «色覚に頼らず読めるようにするための二重表現» として
///   ＋ / − の記号を要求している。タイトルは盤面のルールを最初に見せる場所なので、
///   赤青の対比だけでなく符号そのものを置く。
///
/// WHY 符号だけでなく輪を巻くか:
///   棒 2 本だけだと «線を 2 本引いた» にしか見えない。輪で囲うと «端子» という
///   ひとつの部品になり、そこから力線が生えている構図が成立する。
///   輪はさらに 2 つ仕事をする — 力線の湧き出し口を目に見える形で示すことと、
///   符号を «輪の中の小さな刻印» に落として大きさを稼がずに読ませること。
///
/// WHY 輪を «1 周ぶんより長く» 引くか:
///   ElectricArc.hlsl は帯の両端を必ず絞る (ends は along の両端 8% を落とす)。
///   ちょうど 1 周ぶんだけ引くと、絞られた頭と尻尾が同じ場所に並んで
///   輪の 1/6 近くが欠ける。少し回り込ませて頭の «薄い区間» を尻尾の
///   «濃い区間» の上へ重ねると、継ぎ目の見えない閉じた輪になる。
///
/// WHY 記号を «帯» (LineRenderer) で組むか:
///   テクスチャや手続きシェーダーでも描けるが、LineRenderer は billboard を持つので
///   カメラを向く板が無料で手に入り、放電と同じ ElectricArc.mat をそのまま差せる。
///   端子だけ別の質感になると «同じ電荷から出ている» ように見えなくなる。
///
/// WHY 腕をワールドの X / Y に固定するか:
///   極は XZ 平面を周回する (ElectrodeRig::Integrate)。腕を極の並びに合わせると、
///   組がカメラ方向を向いた瞬間に ＋ が縦棒 1 本へ潰れて − と区別できなくなる。
///   タイトルのカメラは正面固定なので、ワールド XY = 画面平面が常に成り立つ。
#pragma once

#include <Engine/Scene/Components/PresentationComponents.hpp>
#include <Engine/Scene/EntityRef.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include <Math/Vector4.hpp>
#include <Scripts/Utils/PolarityTypes.hpp>
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using namespace fbzz::scene;
using namespace fbzz::math;

namespace sandbox {

/// 端子 1 個ぶんの見た目の設定。
struct ElectrodeCoreStyle {
    /// 符号の差し渡し [m]。＋ の横棒 / 縦棒はどちらもこの長さになる。0 で端子を出さない。
    float size        = 0.62f;
    /// 符号の腕の太さ [m]。
    float thickness   = 0.13f;
    /// 輪の直径 ÷ size。1 に近づけると符号が輪へ触れて紋章が潰れる。
    float ringScale   = 1.85f;
    /// 輪の太さ ÷ thickness。符号より細い輪が «囲い» に見える下限が 0.5 あたり。
    float ringWidth   = 0.55f;
    /// 輪を回る粒の数。0 で粒を出さない。
    float ringBeads   = 5.0f;
    /// 粒が輪を回る速さ [粒間隔 / 秒]。1 周の時間は ringBeads / ringTravel 秒。
    float ringTravel  = 1.2f;
    /// 明るさ (HDR)。粒子の靄に負けると符号が読めないので放電より高く始める。
    /// WHY 上げすぎないか: ブルームのしきい値 (既定 0.7) を大きく越えると、
    ///     紋章がそのまま白い塊として滲み、極性色ごと画面から消える。
    float intensity   = 2.8f;
    /// 帯の断面のうち芯が占める幅 [0,1]。1 に近いほど «塗り潰した棒» になる。
    float coreWidth   = 0.62f;
    float glowFalloff = 1.5f;
    /// 明滅の速さ [Hz] と深さ [0,1]。深さ 0 で止まる。
    float pulseRate   = 1.6f;
    float pulseDepth  = 0.18f;
    /// 極性色。ElectrodeRig が PolarityTypes から入れる。
    Vector4 color     = { 1.0f, 0.16f, 0.18f, 1.0f };
    /// 芯の色。白熱させるため 1 を超える値を入れる。
    Vector4 coreColor = { 4.2f, 3.9f, 3.7f, 1.0f };
    /// 芯を極性色へ寄せる量 [0,1]。0 だと棒が白くなり、大きくするほど赤 / 青が残る。
    /// 端子は «符号» と «極» の両方を伝える札なので、色が抜けたら半分しか仕事をしない。
    float coreTint    = 0.7f;

    std::string materialPath = "Assets/Materials/Effects/ElectricArc.mat";
};

/// 電極 1 本の芯に立つ端子。輪 1 本 + 符号 (＋ は 2 本 / − は 1 本)。
class ElectrodeCoreGlyph {
public:
    /// 毎フレーム呼ぶ。center はワールド座標。
    void Update(Script& owner, Polarity polarity, const Vector3& center,
                const ElectrodeCoreStyle& style, float dt);
    /// 部品ごと片付ける。
    void Detach(const Script& owner);

private:
    /// 帯 1 本を作る。空きが無ければ足し、余りは畳む。
    void EnsureParts(Script& owner, std::size_t count, const ElectrodeCoreStyle& style);
    /// 共通の材質設定。輪も符号も同じ質感で、太さと粒の流れだけが違う。
    void PushMaterial(const Script& owner, EntityID id, const ElectrodeCoreStyle& style,
                      float brightness, float beads, float travel) const;

    /// [0] = 輪、[1..] = 符号の腕。
    std::vector<EntityID> m_parts;
    std::vector<Vector3>  m_points;   // 毎フレームの再確保を避けるための作業領域
    float                 m_phase = 0.0f;
};

inline constexpr int kElectrodeRingSegments = 56;
/// 輪を 1 周からどれだけ回り込ませるか (両端ぶん)。ends が落とす 8% を覆える幅。
inline constexpr float kElectrodeRingOverlap = 0.09f;

inline void ElectrodeCoreGlyph::EnsureParts(Script& owner, std::size_t count,
                                            const ElectrodeCoreStyle& style)
{
    while (m_parts.size() > count) {
        if (GameObject* object = owner.scene.GetGameObject(m_parts.back()))
            owner.scene.Destroy(*object);
        m_parts.pop_back();
    }

    while (m_parts.size() < count) {
        // WHY 原点・無回転のルートへ置くか: ElectricArc の EnsureStrands と同じ理由で、
        //     LineRenderer の World 空間は渡したワールド点を所有 GameObject のローカルへ
        //     引き戻す。極の子にすると、極が動いたぶんだけ紋章が二重にずれる。
        GameObject& object = owner.scene.Create("ElectrodeCore_Part");
        const EntityID id  = object.GetID();
        object.runtimeGenerated   = true;
        object.transform.position = Vector3::ZERO;
        m_parts.push_back(id);

        // Create / AddComponent が配列を伸ばしうるので、設定は ID から引き直す。
        if (GameObject* created = owner.scene.GetGameObject(id)) {
            auto& line = created->AddComponent<LineRendererComponent>();
            line.materialPath = style.materialPath;
            line.space        = LineSpace::World;
            line.billboard    = true;
            // 符号が放電や力線に隠れると端子を出した意味が消えるので、対の中で最前に置く。
            line.orderInLayer = 40;
        }
    }
}

inline void ElectrodeCoreGlyph::PushMaterial(const Script& owner, EntityID id,
                                             const ElectrodeCoreStyle& style,
                                             float brightness, float beads, float travel) const
{
    const MaterialInstance instance = owner.material.Instance(EntityRef{ id });
    // MaterialComponent を張るのは LineRenderer 側 (Phase::LateUpdate) なので、
    // 最初の 1 フレームはまだ無い。HasProperty は無言で false を返すので静かに待てる。
    if (!instance.HasProperty(MaterialPropertyId("coreColor"))) return;

    instance.SetVector4(MaterialPropertyId("coreColor"), style.coreColor);
    instance.SetVector4(MaterialPropertyId("tipColor"),  style.color);
    instance.SetFloat(MaterialPropertyId("coreWidth"),   Clamp01(style.coreWidth));
    instance.SetFloat(MaterialPropertyId("glowFalloff"), Max(style.glowFalloff, 0.01f));
    instance.SetFloat(MaterialPropertyId("intensity"),   Max(brightness, 0.0f));
    instance.SetFloat(MaterialPropertyId("coreTint"),    Clamp01(style.coreTint));
    instance.SetFloat(MaterialPropertyId("phase"),       m_phase);
    instance.SetFloat(MaterialPropertyId("beadDensity"), Max(beads, 0.0f));
    instance.SetFloat(MaterialPropertyId("beadFalloff"), 12.0f);
    // 端子は «形が読めること» が仕事なので、放電の揺らぎは止める。
    instance.SetFloat(MaterialPropertyId("breakup"), 0.0f);
    instance.SetFloat(MaterialPropertyId("travel"),  travel);
    // WHY きっかり 0 にしないか: シェーダーの侵食は smoothstep(1 - erode, 1, x) で、
    //     0 を渡すと両端が 0 幅の smoothstep = 0 除算になり、端点だけ NaN が出うる。
    //     0.02 なら削れるのは端の 1% で、その手前で ends が既に絞りきっている。
    instance.SetFloat(MaterialPropertyId("erode"), 0.02f);
}

inline void ElectrodeCoreGlyph::Update(Script& owner, Polarity polarity, const Vector3& center,
                                       const ElectrodeCoreStyle& style, float dt)
{
    // 輪 1 本 + 符号。＋ は横棒と縦棒、− は横棒だけで、本数がそのまま符号になる。
    const std::size_t strokes = polarity == Polarity::Plus ? 2u : 1u;
    const std::size_t wanted  = style.size <= 0.0f ? 0u : strokes + 1u;
    EnsureParts(owner, wanted, style);
    if (m_parts.empty()) return;

    // 位相は巻き取る。放置したタイトル画面で sin の引数が育つと明滅が乱れる。
    m_phase = std::fmod(m_phase + dt, 1024.0f);

    const float pulse = 1.0f
        + std::sin(m_phase * TWO_PI * Max(style.pulseRate, 0.0f)) * Clamp01(style.pulseDepth);
    const float bright = Max(style.intensity, 0.0f) * pulse;

    const float half   = Max(style.size, 0.01f) * 0.5f;
    const float width  = Max(style.thickness, 0.001f);
    const float radius = Max(style.size * style.ringScale, 0.01f) * 0.5f;

    // ── 輪 ──
    if (GameObject* object = owner.scene.GetGameObject(m_parts[0])) {
        if (auto* line = object->GetComponent<LineRendererComponent>()) {
            // WHY loop=true を使わないか: 最後の点と最初の点は繋がるが uv.x は 0 と 1 の
            //     ままなので、継ぎ目で色も粒も飛ぶ。開いた帯を 1 周より長く引いて
            //     頭と尻尾を重ねるほうが、繋ぎ目のない輪になる。
            const float span  = TWO_PI * (1.0f + kElectrodeRingOverlap * 2.0f);
            const float start = -TWO_PI * kElectrodeRingOverlap;
            m_points.clear();
            m_points.reserve(static_cast<std::size_t>(kElectrodeRingSegments) + 1u);
            for (int i = 0; i <= kElectrodeRingSegments; ++i) {
                const float angle = start
                    + span * static_cast<float>(i) / static_cast<float>(kElectrodeRingSegments);
                m_points.push_back(center + Vector3::RIGHT * (std::cos(angle) * radius)
                                          + Vector3::UP    * (std::sin(angle) * radius));
            }
            line->points     = m_points;
            line->loop       = false;
            line->enabled    = true;
            line->space      = LineSpace::World;
            line->billboard  = true;
            line->startWidth = width * Max(style.ringWidth, 0.05f);
            line->endWidth   = line->startWidth;
            line->startColor = style.color;
            line->endColor   = style.color;
            PushMaterial(owner, m_parts[0], style, bright * 0.85f,
                         style.ringBeads, style.ringTravel);
        }
    }

    // ── 符号 ──
    for (std::size_t index = 1; index < m_parts.size(); ++index) {
        GameObject* object = owner.scene.GetGameObject(m_parts[index]);
        if (!object) continue;
        auto* line = object->GetComponent<LineRendererComponent>();
        if (!line) continue;

        const Vector3 axis = index == 1 ? Vector3::RIGHT : Vector3::UP;
        line->points     = { center - axis * half, center + axis * half };
        line->loop       = false;
        line->enabled    = true;
        line->space      = LineSpace::World;
        line->billboard  = true;
        line->startWidth = width;
        line->endWidth   = width;
        line->startColor = style.color;
        line->endColor   = style.color;
        // 符号に粒は流さない。刻印は動かないほうが «部品» に見える。
        PushMaterial(owner, m_parts[index], style, bright, 0.0f, 0.0f);
    }
}

inline void ElectrodeCoreGlyph::Detach(const Script& owner)
{
    for (const EntityID id : m_parts) {
        if (GameObject* object = owner.scene.GetGameObject(id))
            owner.scene.Destroy(*object);
    }
    m_parts.clear();
}

} // namespace sandbox
