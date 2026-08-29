/// @file    SlashArcComponent.hpp
/// @brief   斬撃の軌跡。刃が通る弧を手続きメッシュで組み、PolaritySlash.hlsl へ渡す
/// @author  Hasegawa Jin
/// @date    2026-08-29
///
/// シーンへは付けない。PlayerComponent が内部モジュールとして持ち、
/// PolarityBladeComponent へ注入する (他の Player モジュールと同じ形)。
///
/// WHY 剣の «判定» から分けるか:
///   弧の見た目を触っても射程・発生・硬直には波及しない。逆にこのモジュールを丸ごと
///   外しても «斬る → 極が乗る → 自分が引かれる / 弾かれる» の芯は全部成立する。
///
/// WHY .vfx ではなく手続きメッシュか:
///   斬撃の軌跡は «向きと形が毎回違う 1 枚の帯» で、粒の集まりではない。
///   ParticlePass は粒の初期回転を必ず乱数で決める (ParticlePass.cpp の SpawnParticle)
///   ため、ビルボードへ弧を描いても振るたびに傾きが変わる。振った向きに沿った弧は、
///   幾何の側で作るしかない。MeshBuilder::AddRibbonFacing はそのための入口として
///   «斬撃・ビームはこちらを使う» と書かれている。
///
/// WHY 形 (三日月) を widths で作るか:
///   両端が尖って中央が太い、という斬撃の形は帯の幅そのもの。シェーダー側でも
///   切り直すと、幅を変えたときに «幾何が広がったぶんシェーダーが削る» で絵が動かず、
///   どちらを触っても同じに見えない状態になる。形は幾何、光はシェーダーで分ける。
///
/// WHY 弧を 1 度だけ組んで、あとは progress だけ進めるか:
///   軌跡は 0.12 秒しか出ない (Docs/presentation.md「刃が通った軌跡を極の色で残す」)。
///   その間カメラが動く量では帯の正対はずれない一方、毎フレーム組み直すと
///   MeshDirty::All としてインデックスバッファまで上げ直すことになる。
///   時間で変わるのは «光がどこまで走ったか» だけなので、それは cbuffer で渡す。
///
/// WHY ＋と− で .mat を分けないか:
///   撃破コア (FX_POL_Core_Plus/Minus) が分かれているのは ParticlePass が材質を
///   materialPath でグローバルに 1 つだけ持つため。こちらは MeshRenderer なので
///   MaterialInstance が GameObject 単位で効く。極性色は albedo へ per-instance で書く。
///
/// WHY フィニッシュだけ色を頂点カラーへ移すか:
///   3 段目は両刀を交差させて斬る (Docs/blades.md) ので、1 つのメッシュに赤と青の
///   弧が同居する。cbuffer の albedo は 1 枚につき 1 色しか持てないので、
///   albedo を白にして層ごとの色を頂点カラーへ置く。シェーダーは両者の積を採るため、
///   1・2 段目 (albedo = 極性色 / 頂点カラー = 白) と同じ経路で描ける。
#pragma once

#include <Engine/Scene/Components/LightComponent.hpp>
#include <Engine/Scene/Components/ProceduralMeshComponent.hpp>
#include <Engine/Scene/MeshBuilder.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include <Math/Quaternion.hpp>
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>
#include <Scripts/Utils/PolarityTypes.hpp>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

using namespace fbzz::scene;
using namespace fbzz::math;
using fbzz::Time;

namespace sandbox {

// PolaritySlash.hlsl の [params] のうち、スクリプトが毎フレーム書くもの。
inline constexpr MaterialPropertyId kSlashAlbedoId  { "albedo" };
inline constexpr MaterialPropertyId kSlashProgressId{ "progress" };
inline constexpr MaterialPropertyId kSlashDecayId   { "decay" };
inline constexpr MaterialPropertyId kSlashFlashId   { "flash" };
inline constexpr MaterialPropertyId kSlashPhaseId   { "phase" };

// 未割り当てでも軌跡が出る状態にする。差し替えは Inspector が優先する。
inline constexpr const char* kSlashMaterialPath = "Assets/Materials/Effects/FX_BLD_Slash.mat";

// 弧ごとに位相をずらす量。同じ値だと、同時に出ている弧の放電が揃って明滅する。
inline constexpr float kSlashPhaseSpread = 3.7f;

class SlashArcComponent : public Script {
    FBZZ_SCRIPT(SlashArcComponent)

public:
    // WHY 名前に arc を付けるか: PlayerComponent は全モジュールの Reflect を 1 つの
    //     名前空間へ平らに並べる。radius / width / duration のような汎用名を出すと、
    //     後から別モジュールが同じ名前を足した瞬間に 2 つの値が黙って 1 つになる。
    FBZZ_GROUP("Slash Arc")
    FBZZ_ASSET_FIELD(MaterialRef, slashMaterial, "Material")
    FBZZ_TOOLTIP("未割り当てなら FX_BLD_Slash.mat を使う")
    // WHY 射程と角度を «持たない» か:
    //   届く先と扇の角度は判定が決めていて、その正本は PolarityTuning 1 枚しか無い。
    //   ここに同じ意味の数値をもう 1 つ置くと、片方を触るたびに «光っているのに
    //   当たらない» / «当たったのに何も光っていない» が生まれ、どちらの数字が
    //   悪いのか画面から切り分けられなくなる ─ PolarityBladeComponent が
    //   «判定の時刻» に対して取っているのと同じ判断を、«判定の広がり» にも通す。
    //   PolarityBladeComponent::ResolveHit が 1 振りごとに実測値を渡す。
    FBZZ_FIELD_RANGE(float, arcHeight, 1.25f, "Height", 0.0f, 3.0f)
    FBZZ_TOOLTIP("弧の中心の高さ。全高 2.51m の胸の位置。判定は Y を見ないので、"
                 "ここは «どの高さに絵を置くか» だけを決める")
    FBZZ_FIELD_RANGE(float, arcRise, 1.4f, "Rise", 0.0f, 3.0f)
    FBZZ_TOOLTIP("袈裟斬りの落差 [m]。振り始めが Height + 半分、振り終わりが Height − 半分。"
                 "0 で水平の薙ぎ払い")
    // 帯は雷が泳ぐ場所でもある。細くすると筋が刃に重なって «太い 1 本» に潰れる。
    FBZZ_FIELD_RANGE(float, arcWidth, 0.95f, "Width", 0.05f, 3.0f)
    FBZZ_FIELD_RANGE(float, arcFinisherWidthScale, 1.7f, "Finisher Width", 1.0f, 4.0f)
    FBZZ_TOOLTIP("3 段目だけ軌跡を太くして、ノックバックの大きさを絵で予告する")
    FBZZ_FIELD_RANGE(float, arcTipPower, 0.55f, "Tip Power", 0.1f, 3.0f)
    FBZZ_TOOLTIP("両端の尖り。小さいほど «中央が太い三日月»、大きいほど細い弓になる")
    FBZZ_FIELD_RANGE_INT(int, arcSegments, 32, "Segments", 6, 64)

    // 芯の外側へもう 1 枚。帯の中では届かない «広がり» はここでしか作れない。
    // 事前乗算は描画順で結果が変わるので、暈を先に積んで芯を上へ載せる。
    FBZZ_FIELD_RANGE(float, arcHaloScale, 1.8f, "Halo Width", 1.0f, 4.0f)
    FBZZ_FIELD_RANGE(float, arcHaloGain, 0.32f, "Halo Gain", 0.0f, 1.0f)
    FBZZ_TOOLTIP("外側の暈の重み。0 で 1 枚だけになる")

    // WHY 時計を 2 本持つか: 刃が通り抜けるのと、通った跡が帯電したまま崩れるのは
    //     時間の桁が違う。1 本にすると、速い方に合わせれば跡が残らず、遅い方に
    //     合わせれば刃が鈍る (PolaritySlash.hlsl のヘッダー)。
    FBZZ_FIELD_RANGE(float, arcSweepSeconds, 0.11f, "Sweep", 0.02f, 0.5f)
    FBZZ_TOOLTIP("刃が弧を走り抜けるまで。Docs/presentation.md の «0.12 秒» はここ")
    FBZZ_FIELD_RANGE(float, arcDuration, 0.40f, "Lifetime", 0.02f, 1.5f)
    FBZZ_TOOLTIP("跡が崩れ切って消えるまで。Sweep より短くはできない")
    FBZZ_FIELD_RANGE(float, arcFlashSeconds, 0.05f, "Flash", 0.0f, 0.3f)
    FBZZ_TOOLTIP("当たったときだけ乗る白熱の長さ。空振りには乗らない")
    FBZZ_FIELD_RANGE(float, arcCrackleRate, 9.0f, "Crackle Rate", 0.0f, 40.0f)
    FBZZ_TOOLTIP("雷と火花の位相が進む速さ [周/秒]。上げるほど «びりびり» が細かくなる")
    FBZZ_FIELD_RANGE_INT(int, arcSlots, 6, "Slots", 1, 12)
    FBZZ_TOOLTIP("同時に出せる軌跡の数。跡が長く残るぶん連撃で重なる")

    // WHY 光源を足すか:
    //   弧そのものがどれだけ明るくても、周りが暗いままだと «画面に貼った絵» から
    //   抜けられない。斬った瞬間に床と敵と自分の体が同じ色で染まって初めて、
    //   その光が «その場に在る» ものとして読める。
    //
    // WHY 弧に沿って動かすか:
    //   中心に 1 個置くと «斬った瞬間に部屋が光った» になる。刃が通っている点を
    //   光源にすれば、影の伸びる向きが刃と一緒に動き、どこを切ったかが光でも分かる。
    //
    // WHY 短く・弱くするか (企画書 12.2):
    //   強い色光を長く置くと、盤面の赤青がその光で塗り潰されて «どの敵が何極か»
    //   が読めなくなる。斬った瞬間だけ差して、すぐ引く。
    FBZZ_FIELD_RANGE(float, arcLightIntensity, 9.0f, "Light", 0.0f, 40.0f)
    FBZZ_TOOLTIP("斬った点が放つ光の強さ。0 で光源を出さない")
    FBZZ_FIELD_RANGE(float, arcLightRangeScale, 2.5f, "Light Range", 0.5f, 6.0f)
    FBZZ_TOOLTIP("光が届く距離。判定の射程に対する倍率")
    FBZZ_FIELD_RANGE(float, arcLightSeconds, 0.14f, "Light Time", 0.0f, 0.5f)
    FBZZ_TOOLTIP("光が消えるまで。Sweep より少し長くすると、振り抜いた余韻が残る")

    FBZZ_FIELD_READ_ONLY(int, debugLiveArcs, 0, "Live Arcs")

    /// 1 振りぶんの軌跡を出す。origin は振った本人の足元、direction は斬る向き (水平)。
    /// finisher で 3 段目 (両刀の交差)、hit は当たったかどうか (白熱の有無)。
    ///
    /// range / angleDegrees は **その一振りで実際に判定した扇** をそのまま渡すこと
    /// (溜め斬りは射程が伸びて 360 度になる)。呼び出し側が判定に使った値と同じ変数を
    /// 渡す限り、絵と判定は «合わせる» ものではなく構造的に一致する。
    void Play(const Vector3& origin, const Vector3& direction, Polarity polarity,
              bool finisher, bool hit, float range, float angleDegrees);

    void OnStart()   override;
    void OnUpdate()  override;
    void OnDestroy() override;

private:
    /// 出ている軌跡 1 本ぶん。実体は原点へ置いたルートの GameObject。
    ///
    /// WHY 色を枠が覚えるか: MaterialComponent へ .mat が差さるのは RuntimeMeshSystem の
    ///     LateUpdate で、Play した瞬間はまだ変数表が空 (SetColor が捨てられる)。
    ///     毎フレーム書き直せば、どのフレームで材質が解決しても色が乗る。
    struct Slot {
        EntityRef ref;
        /// 光源。弧とは別の実体にする ─ 弧は原点に置いてローカルで組んであるので、
        /// 同じ GameObject を動かすと帯ごと持っていかれる。
        EntityRef light;
        Vector4   albedo  = { 1.0f, 1.0f, 1.0f, 1.0f };
        /// 光源が辿る道。弧の始点 / 中点 / 終点 (ワールド)。
        Vector3   pathStart = Vector3::ZERO;
        Vector3   pathMid   = Vector3::ZERO;
        Vector3   pathEnd   = Vector3::ZERO;
        float     lightRange = 6.0f;
        /// 放電の位相のずらし。弧ごとに違えないと、同時に出ている弧が揃って明滅する。
        float     phaseSeed = 0.0f;
        float     elapsed = 0.0f;
        bool      live    = false;
        bool      hit     = false;
    };

    /// 3 点を通る二次ベジエ。円弧の近似としてはこれで十分で、弧の点列を丸ごと
    /// 抱えずに «刃が今どこか» を引ける。
    [[nodiscard]] static Vector3 PathAt(const Slot& slot, float t);
    /// 光源を刃の位置へ運び、強さを落とす。
    void DriveLight(const Slot& slot, float progress);

    /// 空いている枠を返す。全部埋まっていれば最も古い 1 本を奪う。
    [[nodiscard]] Slot* AcquireSlot();
    /// 枠の実体を確保する。DLL リロードを跨いでも名前で拾い直す。
    [[nodiscard]] GameObject* EnsureObject(Slot& slot, int index);
    /// 弧 1 本を m_builder へ足す。hand は ＋1 = 右剣 (右から左へ) / −1 = 左剣。
    /// range / angleDegrees は判定の扇そのもの。
    void AppendArc(float hand, float bladeWidth, const Vector4& tint,
                   const Vector3& direction, const Vector3& viewLocal,
                   float range, float angleDegrees);
    void ReleaseSlots();

    [[nodiscard]] std::string MaterialPath() const;

    std::vector<Slot>     m_slots;
    MeshBuilder           m_builder;
    std::vector<Vector3>  m_points;
    std::vector<float>    m_widths;
};

FBZZ_REFLECT(SlashArcComponent)


inline void SlashArcComponent::OnStart()
{
    // 前回 Play / DLL リロードの枠は OnDestroy で畳まれている。畳めていない経路が
    // 残っても EnsureObject が名前で拾い直す。
    m_slots.clear();
    debugLiveArcs = 0;
}

inline void SlashArcComponent::OnDestroy()
{
    ReleaseSlots();
}

inline void SlashArcComponent::ReleaseSlots()
{
    // 枠はルートに置いてあるので、このスクリプトが消えても一緒には消えない。持ち主が畳む。
    for (Slot& slot : m_slots) {
        if (GameObject* object = slot.ref.Resolve(scene)) scene.Destroy(*object);
        if (GameObject* light = slot.light.Resolve(scene)) scene.Destroy(*light);
    }
    m_slots.clear();
    debugLiveArcs = 0;
}

inline Vector3 SlashArcComponent::PathAt(const Slot& slot, float t)
{
    // 中点をちょうど通らせるための制御点。素の中点を制御点にすると、曲線は
    // 弧の内側へ膨らんで «刃より手前» を照らす。
    const Vector3 control = slot.pathMid * 2.0f
                          - (slot.pathStart + slot.pathEnd) * 0.5f;
    const float u = 1.0f - t;
    return slot.pathStart * (u * u)
         + control * (2.0f * u * t)
         + slot.pathEnd * (t * t);
}

inline std::string SlashArcComponent::MaterialPath() const
{
    std::string path = slashMaterial.ResolvePath();
    return path.empty() ? std::string(kSlashMaterialPath) : path;
}

inline SlashArcComponent::Slot* SlashArcComponent::AcquireSlot()
{
    const int capacity = std::clamp(arcSlots, 1, 12);

    for (Slot& slot : m_slots)
        if (!slot.live) return &slot;

    if (static_cast<int>(m_slots.size()) < capacity) {
        m_slots.push_back(Slot{});
        return &m_slots.back();
    }

    // 埋まっている。最も長く出ている 1 本を奪う ─ いちばん薄くなっているので、
    // 消え方が飛んでも目に留まりにくい。
    Slot* oldest = &m_slots.front();
    for (Slot& slot : m_slots)
        if (slot.elapsed > oldest->elapsed) oldest = &slot;
    return oldest;
}

inline GameObject* SlashArcComponent::EnsureObject(Slot& slot, int index)
{
    if (GameObject* existing = slot.ref.Resolve(scene)) return existing;

    const std::string name = "SlashArc_" + std::to_string(index);
    // WHY 先に拾い直すか: スクリプト DLL をリロードするとこの Script は作り直され、
    //     EntityRef は空に戻る。一方で弧の GameObject は Scene 側に残っているため、
    //     拾わずに作るとリロードのたびに枠が 1 本ずつ増えていく。
    GameObject* object = scene.Find(name);
    if (!object) {
        // WHY プレイヤーの子にしないか: 子にすると弧が本人の移動と回転を引き継ぎ、
        //     斬った «場所» に残らず一緒に流れていく。振り終わった軌跡は空間に残す。
        GameObject& created = scene.Create(name);
        created.runtimeGenerated = true;
        object = &created;
    }
    slot.ref = EntityRef{ object->GetID() };

    // Create / AddComponent はシーンの配列を伸ばしうる。設定は必ず ID から引き直した
    // 個体へ入れる (BossShockwaveComponent と同じ理由)。
    GameObject* arc = slot.ref.Resolve(scene);
    if (!arc) return nullptr;

    // WHY ScriptMeshProxy::SetProceduralMaterial を使わないか: あれは自分自身の
    //     GameObject にしか効かない。枠は別の実体なので、ここへ直接書く。
    //     RuntimeMeshSystem は appliedMaterialPath との差分でしか反映しないので、
    //     毎フレーム上書きされる心配は無い。
    auto* procedural = arc->GetComponent<ProceduralMeshComponent>();
    if (!procedural) procedural = &arc->AddComponent<ProceduralMeshComponent>();
    procedural->materialPath = MaterialPath();

    // 光源は別の実体。弧の子にもしない ─ 子にすると弧のローカル空間で動かすことに
    // なり、«帯は原点に置く» という前提と、光を弧に沿って走らせる話が絡まる。
    const std::string lightName = name + "_Light";
    GameObject* lightObject = scene.Find(lightName);
    if (!lightObject) {
        GameObject& createdLight = scene.Create(lightName);
        createdLight.runtimeGenerated = true;
        lightObject = &createdLight;
    }
    slot.light = EntityRef{ lightObject->GetID() };

    if (GameObject* light = slot.light.Resolve(scene)) {
        auto* component = light->GetComponent<LightComponent>();
        if (!component) component = &light->AddComponent<LightComponent>();
        component->type        = LightComponent::Type::Point;
        // 斬撃は 1 秒に何度も出る。影まで持たせると、そのたびに影の描画が要る。
        component->castShadows = false;
        component->enabled     = false;
        light->SetActive(false);
    }

    return slot.ref.Resolve(scene);
}

inline void SlashArcComponent::AppendArc(float hand, float bladeWidth, const Vector4& tint,
                                         const Vector3& direction, const Vector3& viewLocal,
                                         float range, float angleDegrees)
{
    const int   count   = std::clamp(arcSegments, 6, 64);
    const float degrees = Clamp(angleDegrees, 10.0f, 360.0f);
    const float reach   = Max(range, 0.1f);
    // 溜め斬りの全周は «振り下ろす» 動作ではない。傾けると輪が波打って、
    // 届いた範囲を表す環として読めなくなる。
    const bool  closed  = degrees >= 300.0f;
    const float rise    = closed ? 0.0f : Max(arcRise, 0.0f);

    // WHY 全周だけ少し行き過ぎさせるか: シェーダーは帯の両端を rootFade / tipFade で
    //     畳んでいる (畳まないと切り口が四角く残る)。閉じた弧ではその 2 つが同じ
    //     場所に来るので、輪に切れ目が 1 か所開く。12 度ぶん重ねて巻けば、薄くなった
    //     端どうしが重なって繋がる。水平の投影は同じ円のままなので判定とはずれない。
    const float half = ToRad(degrees * 0.5f) + (closed ? ToRad(12.0f) : 0.0f);

    const Vector3 forward = direction;
    const Vector3 right   = Vector3::Cross(Vector3::UP, forward).NormalizedOr(Vector3::RIGHT);

    // 端で ±rise/2 になるよう正規化する。扇が 180 度を超えると sin は途中で 1 を
    // 取るので、そこを基準にしないと真横で落差を超えてしまう。
    const float liftScale = half >= HALF_PI ? 1.0f : Max(std::sin(half), 1.0e-3f);

    m_points.clear();
    m_widths.clear();
    m_points.reserve(static_cast<std::size_t>(count));
    m_widths.reserve(static_cast<std::size_t>(count));

    for (int i = 0; i < count; ++i) {
        const float t     = static_cast<float>(i) / static_cast<float>(count - 1);
        const float angle = Lerp(half, -half, t);

        // WHY 面ごと回さず «高さだけ» を足すか (判定と絵を揃える要):
        //   弧の面を傾けると、傾けた角度のぶん水平方向の到達先が cos 倍に縮む。
        //   判定は Y を捨てた水平の扇なので、面を回した瞬間に «光っている先» と
        //   «当たる先» がずれる (32 度なら端で 2.6m の絵が 2.2m まで引っ込む)。
        //   水平成分を判定の扇そのものにして、傾きは縦のせん断として足せば、
        //   絵を地面へ落とした影がそのまま判定の扇になる。
        const Vector3 flat = forward * std::cos(angle) + right * (hand * std::sin(angle));
        const float   lift = (std::sin(angle) / liftScale) * rise * 0.5f;

        m_points.push_back(flat * reach
                         + Vector3{ 0.0f, std::max(arcHeight, 0.0f) + lift, 0.0f });

        // 両端が 0 へ落ちる三日月。ここが弧の «形» そのものなので、シェーダー側では削らない。
        //
        // WHY 全周だけ尖らせないか: 閉じた弧は «両端» が同じ場所に来る。尖らせると
        //     背後で帯がゼロ幅に潰れ、輪が 1 か所だけ切れて見える。
        //
        // WHY sin を 0 で止めるか: float の sin(PI) は -8.7e-8 で、わずかに負に出る。
        //     負の底を小数乗すると pow は NaN を返し、末端の 2 頂点だけが消し飛ぶ。
        const float taper = closed
            ? 1.0f
            : std::pow(Max(std::sin(PI * t), 0.0f), std::max(arcTipPower, 0.1f));
        m_widths.push_back(bladeWidth * taper);
    }

    // ── 帯を張る ────────────────────────────────────────────────────────────
    //
    // WHY MeshBuilder::AddRibbonFacing を使わないか (弧が途切れて見える正体):
    //   あれは点ごとに side = Cross(視線, 進行方向) を取り直す。弧は 120 度あるので、
    //   カメラの位置によっては «進行方向が視線とほぼ並ぶ点» が必ずどこかに来る。
    //   そこで外積の長さが 0 へ落ちて side が急回転し、帯がねじれて真横を向く ─
    //   カメラへ正対させるための式が、そこだけ «厚みゼロの板» を作ってしまう。
    //   前の点の side を接線へ直交化して引き継げば、並んだ点でも向きが連続する。
    //
    // WHY 反転も見るか: 外積は degeneracy を跨ぐと符号ごと裏返る。1 点で裏返ると
    //   帯がそこで 180 度ねじれ、砂時計形の交差が 1 か所できる。
    const uint32_t base = m_builder.VertexCount();
    Vector3 previousSide = Vector3::ZERO;

    for (int i = 0; i < count; ++i) {
        const Vector3 forwardStep = (i + 1 < count)
            ? m_points[i + 1] - m_points[i]
            : m_points[i] - m_points[i - 1];
        const Vector3 tangent = forwardStep.NormalizedOr(Vector3::RIGHT);
        const Vector3 toView  = (viewLocal - m_points[i]).NormalizedOr(Vector3::UP);

        Vector3 side = Vector3::Cross(toView, tangent);
        // 外積が短い = 視線と進行方向が並んでいる (7 度以内)。ここで正規化すると
        // 数値誤差がそのまま向きになる。
        if (side.LengthSq() < 0.12f * 0.12f && previousSide.LengthSq() > 0.0f)
            side = previousSide - tangent * Vector3::Dot(previousSide, tangent);
        side = side.NormalizedOr(
            (Vector3::UP - tangent * tangent.y).NormalizedOr(Vector3::RIGHT));
        if (previousSide.LengthSq() > 0.0f && Vector3::Dot(side, previousSide) < 0.0f)
            side = -side;
        previousSide = side;

        const Vector3 normal = Vector3::Cross(tangent, side).NormalizedOr(toView);
        const float   half   = m_widths[static_cast<std::size_t>(i)] * 0.5f;
        const float   v      = static_cast<float>(i) / static_cast<float>(count - 1);

        // uv.x = 弧に沿った進み / uv.y = 帯の横断。PolaritySlash.hlsl がこの並びを読む。
        const uint32_t inner =
            m_builder.AddVertex(m_points[i] - side * half, normal, Vector2{ v, 0.0f }, tint);
        const uint32_t outer =
            m_builder.AddVertex(m_points[i] + side * half, normal, Vector2{ v, 1.0f }, tint);
        // VS は cross(normal, tangent) で帯の横方向を復元する。接線は自分で入れる。
        m_builder.Vertices()[inner].tangent = tangent;
        m_builder.Vertices()[outer].tangent = tangent;
    }

    for (int i = 0; i + 1 < count; ++i) {
        const uint32_t quad = base + static_cast<uint32_t>(i) * 2u;
        m_builder.AddQuad(quad + 1u, quad + 3u, quad + 2u, quad);
    }
}

inline void SlashArcComponent::Play(const Vector3& origin, const Vector3& direction,
                                    Polarity polarity, bool finisher, bool hit,
                                    float range, float angleDegrees)
{
    if (!enabled) return;

    Vector3 forward = direction;
    forward.y = 0.0f;
    forward = forward.NormalizedOr(Vector3::FORWARD);

    Slot* slot = AcquireSlot();
    if (!slot) return;
    const int index = static_cast<int>(slot - m_slots.data());

    GameObject* arc = EnsureObject(*slot, index);
    if (!arc) return;

    // ルートへ原点で置く。帯はここからのローカルで組むので、置き直しても弧が
    // 二重に動かない。
    arc->transform.position      = origin;
    arc->transform.worldPosition = origin;
    arc->transform.rotation      = Quaternion::Identity();
    arc->transform.worldRotation = Quaternion::Identity();
    arc->SetActive(true);

    // 帯をカメラへ正対させる基準点。弧はローカルで組むので視点もローカルへ引き戻す。
    Vector3 viewLocal = Vector3::UP * 10.0f;
    if (GameObject* camera = scene.GetMainCameraObject())
        viewLocal = camera->transform.worldPosition - origin;

    const float bladeWidth = std::max(arcWidth, 0.01f)
                           * (finisher ? std::max(arcFinisherWidthScale, 1.0f) : 1.0f);

    // 何本の弧をどの色で引くか。3 段目だけ両刀の交差で 2 本になる。
    //
    // WHY 全周を «交差» から外すか: PolarityBladeComponent は溜め斬りにも finisher を
    //     立てる (ノックバックを最終段と同じ量にするため)。だが溜め斬りは片方の剣で
    //     周りを薙ぐ一撃で、両刀の交差ではない。閉じた弧を 2 本引いても «同じ輪が
    //     2 枚重なる» だけで交差の絵にならないうえ、赤と青が同時に出て «どちらの極を
    //     乗せたか» という盤面の情報 (企画書 12.2) が絵から消える。
    const bool closed = Clamp(angleDegrees, 10.0f, 360.0f) >= 300.0f;
    const bool cross  = finisher && !closed;

    struct Blade { float hand; Vector4 tint; };
    Blade   blades[2]{};
    int     bladeCount = 0;
    Vector4 albedo     = PolarityColor(polarity);
    if (cross) {
        // 3 段目は両刀の交差。1 枚のメッシュに赤と青が同居するので、色は層 (頂点カラー)
        // が持ち、albedo は明るさと «隠す量» だけを受け持つ。
        albedo     = Vector4{ 1.0f, 1.0f, 1.0f, 1.0f };
        blades[0]  = { 1.0f, kColorPlus };
        blades[1]  = { -1.0f, kColorMinus };
        bladeCount = 2;
    } else {
        // 右剣 (＋) は右から左へ、左剣 (−) は左から右へ。入力の左右と刃の走る向きを
        // 一致させないと、どちらで斬ったかが絵から読めない。
        blades[0]  = { polarity == Polarity::Minus ? -1.0f : 1.0f,
                       Vector4{ 1.0f, 1.0f, 1.0f, 1.0f } };
        bladeCount = 1;
    }

    m_builder.Clear();
    // 暈を先に積む。事前乗算は描画順で結果が変わるので、後から積んだ芯が上に載る。
    const float halo = Clamp01(arcHaloGain);
    if (halo > 0.0f) {
        for (int i = 0; i < bladeCount; ++i) {
            const Vector4& t = blades[i].tint;
            AppendArc(blades[i].hand, bladeWidth * std::max(arcHaloScale, 1.0f),
                      Vector4{ t.x, t.y, t.z, halo }, forward, viewLocal,
                      range, angleDegrees);
        }
    }
    for (int i = 0; i < bladeCount; ++i) {
        const Vector4& t = blades[i].tint;
        AppendArc(blades[i].hand, bladeWidth,
                  Vector4{ t.x, t.y, t.z, 1.0f }, forward, viewLocal,
                  range, angleDegrees);
    }
    mesh.Apply(*arc, m_builder);

    // 光源が辿る道。m_points には最後に積んだ芯の弧が残っているので、形の式を
    // ここへ写さずに «刃が通る線» がそのまま取れる (写すと必ずいつか食い違う)。
    if (m_points.size() >= 3) {
        const std::size_t last = m_points.size() - 1;
        slot->pathStart = origin + m_points[0];
        slot->pathMid   = origin + m_points[last / 2];
        slot->pathEnd   = origin + m_points[last];
    } else {
        slot->pathStart = slot->pathMid = slot->pathEnd = origin;
    }
    slot->lightRange = Max(range, 0.1f) * Max(arcLightRangeScale, 0.5f);

    slot->albedo  = albedo;
    slot->elapsed = 0.0f;
    slot->live    = true;
    slot->hit     = hit;
    // 枠の番号で散らす。乱数を引かないのは、同じ振りが毎回同じ絵になる方が
    // 調整中に «今の変更で何が変わったか» を見分けやすいため。
    slot->phaseSeed = static_cast<float>(index) * kSlashPhaseSpread;

    // 光の色は «振った剣の極»。交差 (albedo が白) でも極性色を差す ─ 盤面を白で
    // 塗ると、その瞬間だけ敵の赤青が読めなくなる (企画書 12.2)。
    if (GameObject* light = slot->light.Resolve(scene)) {
        if (auto* component = light->GetComponent<LightComponent>()) {
            const Vector4 tint = PolarityColor(polarity);
            component->color = Vector3{ tint.x, tint.y, tint.z };
            component->range = slot->lightRange;
        }
    }
}

inline void SlashArcComponent::DriveLight(const Slot& slot, float progress)
{
    GameObject* light = slot.light.Resolve(scene);
    if (!light) return;

    auto* component = light->GetComponent<LightComponent>();
    if (!component) return;

    const float life = Max(arcLightSeconds, 0.0f);
    const float peak = Max(arcLightIntensity, 0.0f);
    // 二乗で落とす。線形だと «消える瞬間に段差» として残り、光が切れたことが分かる。
    const float fade = life > 0.0f ? Clamp01(1.0f - slot.elapsed / life) : 0.0f;
    const float gain = peak * fade * fade;

    if (gain <= 0.001f) {
        component->enabled = false;
        if (light->activeSelf()) light->SetActive(false);
        return;
    }

    const Vector3 at = PathAt(slot, Clamp01(progress));
    // 親を持たないので local = world。両方入れるのは、描画が worldPosition を読むため。
    light->transform.position      = at;
    light->transform.worldPosition = at;
    light->SetActive(true);

    component->enabled   = true;
    component->intensity = gain;
    component->range     = slot.lightRange;
}

inline void SlashArcComponent::OnUpdate()
{
    if (!enabled) return;

    // WHY Time::deltaTime か: ヒットストップで画面が止まっている間は軌跡も止まる。
    //     止まった絵の中で光だけが走り抜けると、止めが «斬った瞬間» から外れる。
    const float dt        = Max(Time::deltaTime, 0.0f);
    const float sweepSpan = Max(arcSweepSeconds, 0.01f);
    // 跡は刃より短く消えられない。Inspector で逆転させても «刃が消えてから跡が残る»
    // にはならず、単に跡が出ないまま終わるので、ここで下限を掛ける。
    const float span      = Max(arcDuration, sweepSpan);

    int live = 0;
    for (Slot& slot : m_slots) {
        if (!slot.live) continue;

        GameObject* arc = slot.ref.Resolve(scene);
        if (!arc) {
            slot.live = false;
            continue;
        }

        slot.elapsed += dt;
        if (slot.elapsed >= span) {
            slot.live = false;
            arc->SetActive(false);
            if (GameObject* light = slot.light.Resolve(scene)) light->SetActive(false);
            continue;
        }
        ++live;

        // 刃 (速い) と跡 (遅い) の 2 本。progress が 1 に着いた後も decay は進み続ける。
        const float progress = Clamp01(slot.elapsed / sweepSpan);
        const float decay    = Clamp01(slot.elapsed / span);
        // 当たった一撃だけ、斬り抜けの直後を白熱させる。二乗で落とすのは、線形だと
        // 終わりが «急に暗くなった» 段差として見えるため。
        const float flash01 = slot.hit && arcFlashSeconds > 0.0f
                            ? Clamp01(1.0f - slot.elapsed / arcFlashSeconds)
                            : 0.0f;

        const MaterialInstance instance = material.Instance(slot.ref, 0u);
        if (instance.HasProperty(kSlashAlbedoId))
            instance.SetColor(kSlashAlbedoId, slot.albedo);
        if (instance.HasProperty(kSlashProgressId))
            instance.SetFloat(kSlashProgressId, progress);
        if (instance.HasProperty(kSlashDecayId))
            instance.SetFloat(kSlashDecayId, decay);
        if (instance.HasProperty(kSlashFlashId))
            instance.SetFloat(kSlashFlashId, flash01 * flash01);
        // WHY 全体時計 (Time::time) を渡さないか:
        //   位相はハッシュの種として «係数を掛けてから» 使われる (strobe は 2.5 倍、
        //   格子番号にはさらに大きな係数が乗る)。Time::time を巻き取って渡していた
        //   ときは、巻き取り幅 128 に対して最終的な引数が数千まで伸び、そこで
        //   ハッシュの分布が壊れて «規則的な縞» になっていた。
        //   弧は 0.4 秒しか生きないので、自分の経過時間を渡せば位相は数の範囲に収まる。
        //
        //   おまけに、全体時計だと同時に出ている弧が全部同じ位相を引くので、
        //   放電が揃って明滅していた。弧ごとの種でそれもばらける。
        const float phase = slot.elapsed * Max(arcCrackleRate, 0.0f) + slot.phaseSeed;
        if (instance.HasProperty(kSlashPhaseId))
            instance.SetFloat(kSlashPhaseId, phase);

        DriveLight(slot, progress);
    }

    debugLiveArcs = live;
}

} // namespace sandbox
