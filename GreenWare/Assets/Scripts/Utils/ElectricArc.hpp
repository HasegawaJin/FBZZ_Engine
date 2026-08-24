/// @file    ElectricArc.hpp
/// @brief   2 点間に走る放電。折れ線の生成と ElectricArc マテリアルへの流し込み。
/// @author  Hasegawa Jin
/// @date    2026-08-23
///
/// WHY 電極ではなく「2 点」を受け取るか:
///   放電は極の持ち物ではなく対の持ち物で、どちらか一方に属させると必ず所有権が
///   ねじれる。端点だけを受ける形にしておけば持ち主の型を知らずに済み、
///   タイトルの電極・銃のビーム・盤面の引力が同じ 1 実装を共有できる。
///
/// WHY Title ではなく Utils に置くか:
///   放電はタイトル画面の飾りではなく、この作品の «電気» そのものの描き方。
///   タイトルに置いたままだと、ゲームプレイ側 (銃・引力) が演出のために
///   Title を include することになり、依存が逆流する。
///
/// WHY 形を毎フレーム作り直さないか:
///   フレームごとに折れ線を引き直すと、60Hz の白色雑音になって「放電」ではなく
///   «震えるノイズの帯» に見える。人が稲妻として読めるのは 15〜30Hz あたりなので、
///   strikeRate の間隔で形を固定し、その間は明るさだけを減衰させる。
///
/// WHY 芯の明るさをアルファでなく HDR の RGB で出すか:
///   ElectricArc.mat はアルファ合成を選んでいる。1 を超える RGB を出せば、
///   アルファ合成のままでもブルームは拾う。詳細は ElectricArc.hlsl のヘッダー。
#pragma once

#include <Engine/Scene/Components/PresentationComponents.hpp>
#include <Engine/Scene/EntityRef.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include <Math/Vector4.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

using namespace fbzz::scene;
using namespace fbzz::math;

namespace sandbox {

/// 放電 1 束ぶんの見た目の設定。
struct ElectricArcStyle {
    /// 同時に走らせる筋の本数。1 本だと «線» に見えるので既定は束ねる。
    int   strandCount = 3;
    /// 1 本あたりの折れ点数。少ないと直線、多いと計算だけ増えて見た目は変わらない。
    int   segments    = 24;
    /// 折れの振れ幅 [m]。両端は 0 で、taperBias の位置が最大。
    float amplitude   = 0.55f;
    /// 振れが最大になる位置 [0,1]。0.5 で中央、1 に寄せると終点側で暴れる。
    ///
    /// WHY 要るか: 電極どうしの放電は «間» で暴れるが、ビームの放電は着弾点で
    ///     暴れてほしい。同じ束で両方を出すには、膨らむ場所を選べる必要がある。
    float taperBias   = 0.5f;
    /// 帯の太さ [m]。
    float width       = 0.10f;
    /// 形を組み替える頻度 [Hz]。
    float strikeRate  = 22.0f;
    /// この距離を超えると放電が消える [m]。0 以下で距離による減衰なし。
    float strikeRange = 7.0f;
    /// 描画の並び。ビームの層と重ねるときだけ触る。
    int   orderInLayer = 0;

    // ── シェーダーへ渡す形と明るさ ──
    float intensity   = 1.6f;
    float coreWidth   = 0.18f;
    float glowFalloff = 2.6f;
    float breakup     = 0.55f;
    float travel      = 6.0f;
    /// 芯をレール色へ寄せる量 [0,1]。0 で純白の芯、1 で完全に極性色。
    float coreTint    = 0.45f;
    /// 帯を流れる粒の数。0 で粒を出さない。
    float beadDensity = 0.0f;
    float beadFalloff = 10.0f;

    Vector4 fromColor = { 1.0f, 0.16f, 0.18f, 1.0f };
    Vector4 toColor   = { 0.14f, 0.42f, 1.0f, 1.0f };
    /// 芯の色。白熱させるため 1 を超える値を入れる。
    Vector4 coreColor = { 6.0f, 5.4f, 5.0f, 1.0f };

    std::string materialPath = "Assets/Materials/Effects/ElectricArc.mat";
};

// WHY 名前空間で包まないか: fbzz::scene にも detail があり、using namespace 下で
//     sandbox::detail を足すと読む側が «どちらの detail か» を毎回確かめることになる。
//     Arc 接頭辞で衝突は避けられるので、入れ子を増やさない。
[[nodiscard]] inline float ArcHash01(uint32_t x)
{
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return static_cast<float>(x) * (1.0f / 4294967295.0f);
}

/// 格子間を滑らかに繋いだ [-1,1] の値ノイズ。x は非負を前提とする。
[[nodiscard]] inline float ArcNoise(uint32_t seed, float x)
{
    const float floored = std::floor(x);
    const auto  cell    = static_cast<uint32_t>(static_cast<int>(floored));
    const float f       = x - floored;
    const float s       = f * f * (3.0f - 2.0f * f);
    const float a       = ArcHash01(seed + cell) * 2.0f - 1.0f;
    const float b       = ArcHash01(seed + cell + 1u) * 2.0f - 1.0f;
    return Lerp(a, b, s);
}

/// 両端 0・taperBias の位置で 1 になる山。振れ幅の分布を決める。
[[nodiscard]] inline float ArcTaper(float t, float bias)
{
    const float peak = Clamp(bias, 0.02f, 0.98f);
    const float side = t < peak ? t / peak : (1.0f - t) / (1.0f - peak);
    return std::sin(HALF_PI * Clamp01(side));
}

/// 2 点間の放電 1 束。筋ごとに 1 つの LineRenderer を実行時に作って持つ。
class ElectricArcBundle {
public:
    /// 筋の GameObject 名に使う識別子。同じ束が毎回同じ名前を掴むための鍵。
    ///
    /// WHY 要るか: スクリプト DLL をリロードすると持ち主の Script は作り直され、
    ///     この束の m_strands は空に戻る。一方で筋の GameObject は Scene 側に
    ///     残っているため、名前で拾い直せないとリロードのたびに筋が増え続ける。
    ///     持ち主・極・番号を混ぜた鍵を渡すこと (束どうしで衝突すると奪い合う)。
    void SetKey(std::string key) { m_key = std::move(key); }

    /// 毎フレーム呼ぶ。端点はワールド座標。
    void Update(Script& owner, const Vector3& from, const Vector3& to,
                const ElectricArcStyle& style, float dt);
    /// 放電を止める (筋は残したまま消灯)。距離が離れたときに使う。
    void Extinguish(const Script& owner);
    /// 子ごと片付ける。
    void Detach(const Script& owner);

private:
    struct Strand {
        EntityID id    = EntityID::INVALID;
        uint32_t seed  = 1u;
        float    phase = 0.0f;
        /// 直前の strike からの明るさ。1 で走った瞬間、0 で消えかけ。
        float    life  = 1.0f;
        /// 束の中での立ち位置。0 が主筋で、離れるほど細く暗くなる。
        float    rank  = 0.0f;
    };

    [[nodiscard]] std::string StrandName(std::size_t index) const;
    void EnsureStrands(Script& owner, int count, const ElectricArcStyle& style);
    void BuildPath(std::vector<Vector3>& out, const Vector3& from, const Vector3& to,
                   const ElectricArcStyle& style, const Strand& strand, float spread) const;
    static void PushMaterial(const Script& owner, const Strand& strand,
                             const ElectricArcStyle& style, float brightness);

    std::string          m_key = "Arc";
    std::vector<Strand>  m_strands;
    std::vector<Vector3> m_points;   // 毎フレームの再確保を避けるための作業領域
    float                m_strikeTimer = 0.0f;
    uint32_t             m_seedCounter = 1u;
};

// ── 実装 (inline) ─────────────────────────────────────────────────────────────

inline std::string ElectricArcBundle::StrandName(std::size_t index) const
{
    return "FX_Arc_" + m_key + "_" + std::to_string(index);
}

inline void ElectricArcBundle::EnsureStrands(Script& owner, int count,
                                             const ElectricArcStyle& style)
{
    // math::Clamp は float 版しか無い。本数を float 経由で丸めると境界で 1 本ぶれる。
    const int wanted = (std::max)(0, (std::min)(count, 16));

    while (static_cast<int>(m_strands.size()) > wanted) {
        if (GameObject* object = owner.scene.GetGameObject(m_strands.back().id))
            owner.scene.Destroy(*object);
        m_strands.pop_back();
    }

    while (static_cast<int>(m_strands.size()) < wanted) {
        const std::string name = StrandName(m_strands.size());

        // WHY 先に拾い直すか: スクリプト DLL をリロードすると持ち主の Script は作り直され、
        //     m_strands は空に戻る。一方で筋の GameObject は Scene 側に残っているため、
        //     拾わずに作るとリロードのたびに筋が増えていく。
        GameObject* object = owner.scene.Find(name);
        if (!object) object = &owner.scene.Create(name);
        const EntityID id = object->GetID();

        Strand strand;
        strand.id   = id;
        strand.seed = m_seedCounter * 2654435761u + 1u;
        strand.rank = static_cast<float>(m_strands.size());
        ++m_seedCounter;
        m_strands.push_back(strand);

        // Create / AddComponent がコンポーネント配列を伸ばしうるので、設定は ID から
        // 引き直す。拾い直した個体にも毎回入れ直す — 同名の GameObject がシーンに
        // 残っていても «枠だけあって描かれない» にはさせない。
        GameObject* created = owner.scene.GetGameObject(id);
        if (!created) continue;

        // WHY 原点・無回転のルートへ置くか: LineRenderer の World 空間は、渡した
        //     ワールド点を所有 GameObject のローカルへ引き戻してからメッシュにする。
        //     親に付けたり回したりすると、その変換ぶんだけ端点がずれる。
        created->runtimeGenerated   = true;
        created->transform.position = Vector3::ZERO;

        auto* line = created->GetComponent<LineRendererComponent>();
        if (!line) line = &created->AddComponent<LineRendererComponent>();
        line->materialPath = style.materialPath;
        line->space        = LineSpace::World;
        line->billboard    = true;
        line->loop         = false;
        line->orderInLayer = style.orderInLayer;
    }
}

inline void ElectricArcBundle::BuildPath(std::vector<Vector3>& out,
                                         const Vector3& from, const Vector3& to,
                                         const ElectricArcStyle& style,
                                         const Strand& strand, float spread) const
{
    const int count = (std::max)(2, (std::min)(style.segments, 64));
    out.clear();
    out.reserve(static_cast<std::size_t>(count) + 1);

    const Vector3 delta  = to - from;
    const float   length = delta.Length();
    if (length <= EPSILON) {
        out.push_back(from);
        out.push_back(to);
        return;
    }
    const Vector3 axis = delta * (1.0f / length);

    // 軸に垂直な 2 軸。軸が真上に近いときだけ基準を前方へ倒す (外積が縮退するため)。
    const Vector3 reference = Abs(axis.y) > 0.9f ? Vector3::FORWARD : Vector3::UP;
    const Vector3 side = Vector3::Cross(axis, reference).Normalized();
    const Vector3 up   = Vector3::Cross(side, axis);

    for (int i = 0; i <= count; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(count);
        // 端は電極に刺さっていてほしいので振れを 0 に落とす。
        const float taper = ArcTaper(t, style.taperBias);
        // 2 オクターブ。1 本調子の波にせず、粗い折れの上に細かいギザギザを乗せる。
        const float nx = ArcNoise(strand.seed,          t * 4.0f) * 0.65f
                       + ArcNoise(strand.seed + 7919u,  t * 11.0f) * 0.35f;
        const float ny = ArcNoise(strand.seed + 104729u, t * 4.0f) * 0.65f
                       + ArcNoise(strand.seed + 15485u,  t * 11.0f) * 0.35f;

        const Vector3 straight = from + delta * t;
        out.push_back(straight + (side * nx + up * ny) * (spread * taper));
    }
}

inline void ElectricArcBundle::PushMaterial(const Script& owner, const Strand& strand,
                                            const ElectricArcStyle& style, float brightness)
{
    const MaterialInstance instance = owner.material.Instance(EntityRef{ strand.id });

    // WHY HasProperty で先に門を閉めるか:
    //   MaterialComponent を張るのは LineRenderer 側 (Phase::LateUpdate) なので、
    //   最初の 1 フレームはまだ存在しない。Set 系は空振りのたびに警告を出すため、
    //   そのまま呼ぶと «毎フレーム × 筋の本数 × プロパティ数» のログで埋まる。
    //   HasProperty は無言で false を返すので、揃うまで静かに待てる。
    if (!instance.HasProperty(MaterialPropertyId("coreColor"))) return;

    instance.SetVector4(MaterialPropertyId("coreColor"), style.coreColor);
    instance.SetVector4(MaterialPropertyId("tipColor"),  style.toColor);
    instance.SetFloat(MaterialPropertyId("coreWidth"),   style.coreWidth);
    instance.SetFloat(MaterialPropertyId("glowFalloff"), style.glowFalloff);
    instance.SetFloat(MaterialPropertyId("intensity"),   style.intensity * brightness);
    instance.SetFloat(MaterialPropertyId("phase"),       strand.phase);
    instance.SetFloat(MaterialPropertyId("breakup"),     style.breakup);
    instance.SetFloat(MaterialPropertyId("travel"),      style.travel);
    instance.SetFloat(MaterialPropertyId("coreTint"),    style.coreTint);
    instance.SetFloat(MaterialPropertyId("beadDensity"), style.beadDensity);
    instance.SetFloat(MaterialPropertyId("beadFalloff"), style.beadFalloff);
    instance.SetFloat(MaterialPropertyId("erode"),       1.0f - Clamp01(brightness));
}

inline void ElectricArcBundle::Update(Script& owner, const Vector3& from, const Vector3& to,
                                      const ElectricArcStyle& style, float dt)
{
    EnsureStrands(owner, style.strandCount, style);
    if (m_strands.empty()) return;

    // 距離が開くほど暗く、strikeRange で消える。端点が近づいたときだけ放電が
    // 立つので、対象どうしの運動がそのまま «溜まって放電する» 演出になる。
    const float distance = (to - from).Length();
    float reach = 1.0f;
    if (style.strikeRange > 0.0f)
        reach = Clamp01(1.0f - distance / style.strikeRange);
    if (reach <= 0.0f) {
        Extinguish(owner);
        return;
    }

    // 形の組み替え。間隔の間は形を保ち、明るさだけ減衰させる。
    m_strikeTimer -= dt;
    const bool struck = m_strikeTimer <= 0.0f;
    if (struck) {
        m_strikeTimer += 1.0f / Max(style.strikeRate, 0.01f);
        if (m_strikeTimer < 0.0f) m_strikeTimer = 0.0f; // 大きな dt で溜め込まない
        for (Strand& strand : m_strands) {
            strand.seed  = strand.seed * 1664525u + 1013904223u;
            strand.life  = 1.0f;
            // 位相は巻き取る。シェーダー側の frac(sin(x * 12.9898)) は x が大きくなるほど
            // 精度を失い、放置した画面で数十分後にノイズが縞へ潰れる。
            strand.phase = std::fmod(strand.phase + 0.37f + ArcHash01(strand.seed) * 0.5f,
                                     1024.0f);
        }
    }

    for (Strand& strand : m_strands) {
        strand.life = Max(strand.life - dt * Max(style.strikeRate, 0.01f) * 0.85f, 0.0f);

        GameObject* object = owner.scene.GetGameObject(strand.id);
        if (!object) continue;
        auto* line = object->GetComponent<LineRendererComponent>();
        if (!line) continue;

        // 副筋ほど大きく振れさせて細くする。主筋 1 本の «太い線» に見せないため。
        const float rankFade = 1.0f / (1.0f + strand.rank * 0.85f);
        const float spread   = style.amplitude * (1.0f + strand.rank * 0.6f);
        BuildPath(m_points, from, to, style, strand, spread);

        line->points       = m_points;
        line->enabled      = true;
        line->startWidth   = style.width * rankFade;
        line->endWidth     = style.width * rankFade;
        line->space        = LineSpace::World;
        line->billboard    = true;
        line->orderInLayer = style.orderInLayer;
        // startColor は LineRenderer が albedo へ流す = シェーダーの始点側の色。
        // 明るさは intensity 側で振るので、ここは色と不透明度だけを持たせる。
        line->startColor = { style.fromColor.x, style.fromColor.y, style.fromColor.z,
                             style.fromColor.w * reach };
        line->endColor   = style.toColor;

        // 走った直後が最も明るく、次の strike までに落ちる。0.35 は消えきらない下限で、
        // 完全に 0 にすると筋が明滅ではなく点滅して見える。
        const float brightness = (0.35f + 0.65f * strand.life) * rankFade * reach;
        PushMaterial(owner, strand, style, brightness);
    }
}

inline void ElectricArcBundle::Extinguish(const Script& owner)
{
    for (const Strand& strand : m_strands) {
        GameObject* object = owner.scene.GetGameObject(strand.id);
        if (!object) continue;
        if (auto* line = object->GetComponent<LineRendererComponent>())
            line->enabled = false;
    }
}

inline void ElectricArcBundle::Detach(const Script& owner)
{
    for (const Strand& strand : m_strands) {
        if (GameObject* object = owner.scene.GetGameObject(strand.id))
            owner.scene.Destroy(*object);
    }
    m_strands.clear();
}

} // namespace sandbox
