/// @file    SpinSlashFxComponent.hpp
/// @brief   回転斬りの «一周»。刃が通る高さを追って、体のまわりを開いていく光の輪
/// @author  Hasegawa Jin
/// @date    2026-09-13
///
/// シーンへは付けない。PlayerComponent が内部モジュールとして持ち、BladeComponent が
/// 回転斬りの段を振り出したときだけ Play を呼ぶ。丸ごと外しても斬撃の芯は全部成立する。
///
/// WHY 回転斬りだけ専用の層を持つか:
///   軌跡 (BladeTrail) は刀身が通った «面» をそのまま張る層で、振りの形をよく写す。
///   ただし体ごと 1 周する段では、刃はカメラの裏側を半周ぶん通る ─ その区間の帯は
///   自分の体に隠れるか、真横から見て面積が 0 になる。結果、連撃で一番大きい動きの
///   はずの回転斬りだけ «画面に出ている絵» が他の段より少ない。
///   «どこまで回ったか» を体の外側の輪で言えば、刃が裏へ回っている間も回転が続いて
///   いることが絵に残る。
///
/// WHY 輪を最初から閉じないか:
///   完成した輪を出すと «そこに輪が置かれた» になり、回った順番が消える。振り出しの
///   向きから刃と同じ側へ開いていくと、輪そのものが «今ここまで回った» を指す。
///
/// WHY 射程を持たないか:
///   輪の半径は BladeComponent が判定に使った射程 (bladeRange) から来る。ここに
///   «見た目用の半径» を置くと、射程を触るたびに «届いていないのに輪は届いている»
///   が生まれる (旧 SlashArc と ResolveHit の契約と同じ理由)。ここが持つのはその比だけ。
///
/// WHY 時計がスケール時間か:
///   輪は振りと同時に開く。実時間で回すと、ヒットストップで体が止まっている間に
///   輪だけが回り切って、止めが明けた画面には «回り終わった輪» しか残らない。
#pragma once

#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>
#include <Scripts/Utils/BeamTrailRendererComponent.hpp>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

using namespace fbzz::scene;
using namespace fbzz::math;
using fbzz::Time;

namespace sandbox {

class SpinSlashFxComponent : public Script {
    FBZZ_SCRIPT(SpinSlashFxComponent)

public:
    // WHY 名前に spinRing を付けるか: PlayerComponent は全モジュールの Reflect を 1 つの
    //     名前空間へ平らに並べる。radius / width のような汎用名は必ずどこかと衝突する。
    FBZZ_GROUP("Spin Ring")
    FBZZ_FIELD_FILE(spinRingMaterial, "Assets/Materials/Effects/FX_BLD_SpinRing.mat",
                    "Material", ".mat")
    FBZZ_FIELD_RANGE(float, spinRingReach, 0.92f, "Radius [reach]", 0.3f, 1.6f)
    FBZZ_TOOLTIP("輪の半径。斬撃の射程 (Blade Tuning の Range) に対する比。"
                 "1 を超えると «輪の中なのに届かない» 帯ができる")
    FBZZ_FIELD_RANGE(float, spinRingGrow, 0.5f, "Grow [m]", 0.0f, 2.0f)
    FBZZ_TOOLTIP("回り切った後、消えるまでに外へ広がる距離 [m]。0 で膨らまない")
    FBZZ_FIELD_RANGE(float, spinRingHeight, 1.05f, "Height [m]", 0.0f, 2.5f)
    FBZZ_TOOLTIP("足元から輪までの高さ [m]。刃が通る高さ ─ 低すぎると床の絵に、"
                 "高すぎると «頭の上の光輪» に見える")
    FBZZ_FIELD_RANGE(float, spinRingRise, 0.35f, "Rise [m]", -1.0f, 1.5f)
    FBZZ_TOOLTIP("輪の尻から刃先へかけての持ち上がり [m]。0 で真っ平ら。"
                 "少し付けると輪が螺旋になり «回りながら上がった» が出る")
    FBZZ_FIELD_RANGE(float, spinRingWidth, 0.55f, "Width [m]", 0.05f, 2.0f)
    FBZZ_FIELD_RANGE(float, spinRingTurns, 1.0f, "Turns", -3.0f, 3.0f)
    FBZZ_TOOLTIP("開き切るまでの周回数。負で逆回り ─ モーションの回る向きと"
                 "合っていないと «輪だけ逆走» する")
    FBZZ_FIELD_RANGE(float, spinRingSweepScale, 1.25f, "Sweep [startup]", 0.2f, 3.0f)
    FBZZ_TOOLTIP("開き切るまでの時間。その一振りの発生に対する比。1 で «斬り抜けと同時に"
                 "閉じる»。少し超えさせると振り抜きまで回り続ける")
    FBZZ_FIELD_RANGE(float, spinRingHold, 0.04f, "Hold [s]", 0.0f, 0.5f)
    FBZZ_TOOLTIP("閉じた輪をそのまま見せる時間 [秒, スケール時間]")
    FBZZ_FIELD_RANGE(float, spinRingFade, 0.26f, "Fade [s]", 0.02f, 1.0f)
    FBZZ_FIELD_RANGE(float, spinRingIntensity, 3.0f, "Intensity", 0.0f, 20.0f)
    FBZZ_FIELD_RANGE(float, spinRingWhite, 0.5f, "White", 0.0f, 1.0f)
    FBZZ_TOOLTIP("刃先の側をどれだけ白へ寄せるか。0 で刀の色そのまま")
    FBZZ_FIELD_RANGE(float, spinRingHeat, 1.3f, "Heat Scale", 1.0f, 2.5f)
    FBZZ_TOOLTIP("拍に乗った一振り (heat = 1) のときの太さと明るさの倍率")
    FBZZ_FIELD_RANGE_INT(int, spinRingSegments, 44, "Segments", 8, 96)
    FBZZ_TOOLTIP("1 周ぶんの折れ点数。少ないと輪が多角形に見える")
    FBZZ_FIELD_READ_ONLY(float, debugSpinRing, 0.0f, "Sweep (0-1)")

    /// 輪を出す。center は足元 (プレイヤーの worldPosition)、forward は斬る向き、
    /// reach は判定に使った射程 [m]、seconds はその一振りの発生、heat は «格» [0,1]。
    void Play(const Vector3& center, const Vector3& forward, float reach,
              float seconds, float heat, const Vector4& tint);
    /// 振りが打ち切られた (回避・弾き)。開きかけの輪をその場で消し込みへ回す。
    void Cut();

    void OnStart()   override;
    void OnUpdate()  override;
    void OnDestroy() override;

private:
    /// 輪の実体を確保する。DLL リロードを跨いでも名前で拾い直す。
    [[nodiscard]] GameObject* EnsureObject();
    [[nodiscard]] BeamTrailRendererComponent* Renderer() const;

    static constexpr const char* kObjectName = "SpinSlashRing";

    EntityRef m_ref;
    /// 輪を張る基準。振り出しの姿勢で固定する ─ 追従させると、踏み込みで
    /// 前へ出たぶん «回った跡» が一緒に動いて、どこで回ったのかが消える。
    Vector3 m_center  = Vector3::ZERO;
    Vector3 m_forward = Vector3::FORWARD;
    Vector3 m_right   = Vector3::RIGHT;
    Vector4 m_tint    = Vector4{ 1.0f, 1.0f, 1.0f, 1.0f };
    float   m_radius  = 2.4f;
    float   m_sweep   = 0.2f;
    float   m_heat    = 0.0f;
    float   m_age     = 0.0f;
    bool    m_live    = false;

    std::vector<Vector3> m_points;
};

FBZZ_REFLECT(SpinSlashFxComponent)

// ── 実装 (inline) ─────────────────────────────────────────────────────────────

inline GameObject* SpinSlashFxComponent::EnsureObject()
{
    // WHY 先に拾い直すか: DLL をリロードするとこの Script は作り直されるが、輪の
    //     GameObject はシーンに残る。拾わずに作るとリロードのたびに枠が増えていく。
    GameObject* object = scene.Find(kObjectName);
    if (!object) {
        // WHY 誰の子にもしないか: LineRenderer はワールド点を所有者のローカルへ引き戻して
        //     焼くので、親の移動・回転がそのまま輪のずれになる。ルートの原点に置く。
        GameObject& created = scene.Create(kObjectName);
        created.runtimeGenerated = true;
        object = &created;
    }
    if (!scene.GetScript<BeamTrailRendererComponent>(object))
        object->AddScript<BeamTrailRendererComponent>();
    // Create / AddScript はシーンの配列を伸ばしうる。返すのは名前から引き直した個体。
    return scene.Find(kObjectName);
}

inline BeamTrailRendererComponent* SpinSlashFxComponent::Renderer() const
{
    GameObject* object = m_ref.Resolve(scene);
    return object ? scene.GetScript<BeamTrailRendererComponent>(object) : nullptr;
}

inline void SpinSlashFxComponent::OnStart()
{
    // WHY 最初に作るか: 実行時に足した Script は、次のフレームに ScriptSystem が
    //     OnStart を通すまで帯を張れない (IsReady)。振り出しの 1 コマ目で作ると、
    //     輪が開き «始める» ところが必ず抜ける。
    if (GameObject* object = EnsureObject()) m_ref = EntityRef{ object->GetID() };
    m_age  = 0.0f;
    m_live = false;
    debugSpinRing = 0.0f;
}

inline void SpinSlashFxComponent::OnDestroy()
{
    // 枠はルートに置いてあるので、このスクリプトが消えても一緒には消えない。
    if (GameObject* object = m_ref.Resolve(scene)) scene.Destroy(*object);
    m_live = false;
    debugSpinRing = 0.0f;
}

inline void SpinSlashFxComponent::Play(const Vector3& center, const Vector3& forward,
                                       float reach, float seconds, float heat,
                                       const Vector4& tint)
{
    if (!enabled) return;

    m_center  = center;
    m_forward = forward.NormalizedOr(Vector3::FORWARD);
    m_right   = Vector3::Cross(Vector3::UP, m_forward).NormalizedOr(Vector3::RIGHT);
    m_tint    = tint;
    m_radius  = Max(reach, 0.1f) * Max(spinRingReach, 0.1f);
    // 発生をそのまま受け取る。ここに «輪用の長さ» をもう 1 つ持たせると、モーションを
    // 差し替えるたびに «体はもう止まっているのに輪だけ回り続ける» が生まれる。
    m_sweep   = Max(seconds, 0.02f) * Max(spinRingSweepScale, 0.05f);
    m_heat    = Clamp01(heat);
    m_age     = 0.0f;
    m_live    = true;
}

inline void SpinSlashFxComponent::Cut()
{
    if (!m_live) return;
    // 開いている途中でも «そこまで回った輪» として消し込みへ送る。頭出しへ戻すと
    // 打ち切った瞬間に輪が消え、回避で切ったことが «絵の消失» として出る。
    m_age = Max(m_age, m_sweep);
}

inline void SpinSlashFxComponent::OnUpdate()
{
    if (!enabled || !m_live) return;

    BeamTrailRendererComponent* renderer = Renderer();
    if (!renderer) {
        // 拾えないのは枠を作る前か、シーンから消えたとき。次のフレームに拾い直す。
        if (GameObject* object = EnsureObject()) m_ref = EntityRef{ object->GetID() };
        return;
    }
    // まだ張れない枠は時計を進めずに待つ。進めると開き始めの数コマを捨てることになる。
    if (!renderer->IsReady()) return;

    const float hold = Max(spinRingHold, 0.0f);
    const float fade = Max(spinRingFade, 0.02f);
    m_age += Max(Time::deltaTime, 0.0f);
    if (m_age >= m_sweep + hold + fade) {
        renderer->Hide();
        m_live = false;
        debugSpinRing = 0.0f;
        return;
    }

    // 開き: 頭は刃と同じ勢いで出て、閉じ際で詰まる。等速で回すと «一定速で描かれる輪»
    // になり、刃の加速と輪の進みが別々の動きに見える。
    const float sweep01 = Clamp01(m_age / m_sweep);
    // 下限を置くのは «開き始めの 1 コマ» の保険。開きが 0 だと全部の点が 1 点に重なり、
    // 帯の向きを作る外積が縮退して LineRenderer が形を決められない。
    const float opened  = Max(1.0f - (1.0f - sweep01) * (1.0f - sweep01), 0.02f);
    debugSpinRing = opened;

    // 消し込みの進み [0,1]。保持の間は 0。
    const float after = m_age - m_sweep - hold;
    const float decay = after <= 0.0f ? 0.0f : Clamp01(after / fade);

    const float heatScale = Lerp(1.0f, Max(spinRingHeat, 1.0f), m_heat);
    const float radius    = m_radius + Max(spinRingGrow, 0.0f) * decay;
    const float span      = TWO_PI * spinRingTurns * opened;

    // 開いた角の大きさに合わせて折れ点を減らす。1 周ぶんを常に積むと、開き始めの
    // 数度に 40 点が詰まって «その場で震える点» になる。
    const int   full  = std::clamp(spinRingSegments, 8, 96);
    const float ratio = Clamp01(Abs(span) / TWO_PI);
    const int   steps = std::max(static_cast<int>(static_cast<float>(full) * ratio), 3);

    m_points.clear();
    m_points.reserve(static_cast<std::size_t>(steps) + 1u);
    for (int i = 0; i <= steps; ++i) {
        // u = 0 が輪の尻 (振り出しの向き)、1 が刃先。uv.x もこの向きに並ぶので、
        // シェーダーの «先を尖らせる» 側 (tipFade) が刃先に当たる。
        const float u     = static_cast<float>(i) / static_cast<float>(steps);
        const float angle = span * u;
        const Vector3 dir = m_forward * std::cos(angle) + m_right * std::sin(angle);
        m_points.push_back(m_center + dir * radius
                         + Vector3::UP * (spinRingHeight + spinRingRise * u));
    }

    // 太さと明るさ: 回っている間は満額。消し込みで細く暗くする。
    const float ease      = 1.0f - (1.0f - decay) * (1.0f - decay);
    const float width     = Max(spinRingWidth, 0.01f) * heatScale * Lerp(1.0f, 0.35f, ease);
    const float intensity = Max(spinRingIntensity, 0.0f) * heatScale
                          * (1.0f - decay) * (1.0f - decay);
    const float white     = Clamp01(spinRingWhite) * (1.0f - decay);

    BeamTrailStyle style;
    style.materialPath = spinRingMaterial;
    style.width        = width;
    style.color        = Vector4{ Lerp(m_tint.x, 1.0f, white) * intensity,
                                  Lerp(m_tint.y, 1.0f, white) * intensity,
                                  Lerp(m_tint.z, 1.0f, white) * intensity, 1.0f };
    style.isCore       = true;
    // 一閃 (6) より後ろ、火花より手前。当たりの «斬れた» が輪に埋もれないようにする。
    style.orderInLayer = 5;
    style.shape        = LineShape::Ribbon;
    // 形の正本はここが持つ (ShowPath は振らせない)。揺らぎは素材の流れだけで出す。
    style.coreWidth   = 0.26f;
    style.edgeFalloff = 1.9f;
    style.coreBoost   = 2.2f;
    style.muzzleFade  = 0.22f;
    style.tipFade     = 0.02f;
    style.tiling      = 0.35f;
    style.scroll      = m_age * 1.4f;

    renderer->ShowPath(m_points, style, /*loop=*/false);
}

} // namespace sandbox
