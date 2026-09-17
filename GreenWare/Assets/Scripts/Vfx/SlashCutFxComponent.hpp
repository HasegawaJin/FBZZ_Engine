/// @file    SlashCutFxComponent.hpp
/// @brief   斬撃が当たった瞬間の «一閃»。当たり点を斬った向きに横切る光の線を張る
/// @author  Hasegawa Jin
/// @date    2026-09-12
///
/// シーンへは付けない。PlayerComponent が内部モジュールとして持ち、BladeComponent が
/// 当たったときだけ Play を呼ぶ。丸ごと外しても斬撃の芯は全部成立する。
///
/// @note 当たりに «線» が要る。刀の軌跡 (BladeTrail) は «振った»、火花と光条
///       (FX_BLD_SlashHit) は «当たった» を言うが、«斬れた» を言う絵が無かった。
///       当たり点を斬った向きに横切る線が白熱→冷めることで «今そこを斬った» が
///       ヒットストップの間じゅう画面に残る。
/// @note パーティクルでなく帯 (BeamTrailRendererComponent、断面は SlashCut.hlsl) で
///       張る。パーティクルは発生時の回転が乱数で入り «斬った向き» を持てないが、
///       帯は両端をワールド座標で渡せば向きが決まりカメラへ正対する。
/// @note 時計はスケール時間で進める。世界の止め (timeScale 0.05) の間も線を閃光の
///       まま留めたい。実時間で回すと止めの 0.08 秒で閃光が終わり画面に映らない。
#pragma once

#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include <Math/Quaternion.hpp>
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>
#include <Scripts/Utils/BeamTrailRendererComponent.hpp>
#include <algorithm>
#include <string>
#include <vector>

using namespace fbzz::scene;
using namespace fbzz::math;
using fbzz::Time;

namespace sandbox {

class SlashCutFxComponent : public Script {
    FBZZ_SCRIPT(SlashCutFxComponent)

public:
    /// @note フィールド名に cut を付ける。PlayerComponent は全モジュールの Reflect を
    ///       1 つの名前空間へ平らに並べるため、length/width のような汎用名は衝突する。
    FBZZ_GROUP("Slash Cut")
    FBZZ_FIELD_FILE(cutMaterial, "Assets/Materials/Effects/FX_BLD_Cut.mat", "Material", ".mat")
    FBZZ_FIELD_RANGE(float, cutLengthMin, 2.4f, "Length (light)", 0.2f, 8.0f)
    FBZZ_FIELD_RANGE(float, cutLengthMax, 4.2f, "Length (heavy)", 0.2f, 8.0f)
    FBZZ_TOOLTIP("線の長さ [m]。当たりの強さ (段の坂・溜め・拍) で間を取る")
    FBZZ_FIELD_RANGE(float, cutWidthMin, 0.18f, "Width (light)", 0.01f, 1.0f)
    FBZZ_FIELD_RANGE(float, cutWidthMax, 0.36f, "Width (heavy)", 0.01f, 1.0f)
    FBZZ_FIELD_RANGE(float, cutFlashSeconds, 0.05f, "Flash", 0.0f, 0.3f)
    FBZZ_TOOLTIP("白く太い閃光でいる長さ [秒, スケール時間]。止めの間はそのまま留まる")
    FBZZ_FIELD_RANGE(float, cutSeamSeconds, 0.22f, "Seam", 0.02f, 1.0f)
    FBZZ_TOOLTIP("閃光の後、細い切れ目として刀の色へ冷めて消えるまで [秒]")
    FBZZ_FIELD_RANGE(float, cutSeamWidth, 0.3f, "Seam Width", 0.05f, 1.0f)
    FBZZ_TOOLTIP("切れ目の太さ [閃光に対する比]")
    FBZZ_FIELD_RANGE(float, cutGrow, 0.15f, "Grow", 0.0f, 0.6f)
    FBZZ_TOOLTIP("閃光の間に線が伸びる量 [長さ比]。0 で最初から全長。少し伸ばすと «走った» に読める")
    FBZZ_FIELD_RANGE(float, cutFlashIntensity, 6.0f, "Flash Intensity", 0.0f, 30.0f)
    FBZZ_FIELD_RANGE(float, cutSeamIntensity, 2.2f, "Seam Intensity", 0.0f, 30.0f)
    FBZZ_FIELD_RANGE(float, cutFrontOffset, 0.8f, "Front Offset [m]", 0.0f, 3.0f)
    FBZZ_TOOLTIP("当たり点からカメラ側へ寄せる距離。部位の中心で張ると脚の中へ埋まって深度で隠れる")
    FBZZ_FIELD_RANGE(float, cutCrossDegrees, 36.0f, "Cross Angle", 0.0f, 90.0f)
    FBZZ_TOOLTIP("交差斬り (溜め斬り・全段を拍に乗せた締め) の 2 本の開き [度]")
    FBZZ_FIELD_RANGE_INT(int, cutSlots, 4, "Slots", 1, 8)
    FBZZ_TOOLTIP("同時に出せる線の数。超えたら最も古い 1 本を奪う")
    FBZZ_FIELD_READ_ONLY(int, debugLiveCuts, 0, "Live Cuts")

    /// 一閃を出す。axis は線の向き (ワールド)、strength01 は当たりの強さ、
    /// tint は振った刀の色、cross で 2 本を交差させる。
    void Play(const Vector3& point, const Vector3& axis, float strength01,
              const Vector4& tint, bool cross);

    void OnStart()   override;
    void OnUpdate()  override;
    void OnDestroy() override;

private:
    struct Slot {
        EntityRef ref;
        Vector3   center   = Vector3::ZERO;
        Vector3   axis     = Vector3::RIGHT;
        Vector4   tint     = Vector4{ 1.0f, 1.0f, 1.0f, 1.0f };
        float     length   = 0.0f;
        float     width    = 0.0f;
        float     strength = 0.0f;
        float     age      = 0.0f;
        bool      live     = false;
    };

    /// 線 1 本ぶんの GameObject を用意する。DLL リロードを跨いでも名前で拾い直す。
    [[nodiscard]] GameObject* EnsureObject(int index);
    [[nodiscard]] BeamTrailRendererComponent* RendererOf(const Slot& slot) const;
    /// 空いている枠、無ければ最も古い枠。
    [[nodiscard]] Slot& Claim();
    void Launch(const Vector3& center, const Vector3& axis, float strength, const Vector4& tint);

    std::vector<Slot> m_slots;
};

FBZZ_REFLECT(SlashCutFxComponent)


inline GameObject* SlashCutFxComponent::EnsureObject(int index)
{
    const std::string name = "SlashCut_" + std::to_string(index);
    /// @note 先に拾い直す。DLL リロードでこの Script は作り直されるが、線の GameObject
    ///       はシーンに残るため、拾わず作るとリロードのたびに枠が増える。
    GameObject* object = scene.Find(name);
    if (!object) {
        /// @note 誰の子にもしない。LineRenderer はワールド点を所有者のローカルへ引き戻して
        ///       焼くため、親の移動・回転がそのまま端点のずれになる。
        GameObject& created = scene.Create(name);
        created.runtimeGenerated = true;
        object = &created;
    }
    if (!scene.GetScript<BeamTrailRendererComponent>(object))
        object->AddScript<BeamTrailRendererComponent>();
    /// @note Create / AddScript はシーンの配列を伸ばしうる。返すのは名前から引き直した個体。
    return scene.Find(name);
}

inline BeamTrailRendererComponent* SlashCutFxComponent::RendererOf(const Slot& slot) const
{
    GameObject* object = slot.ref.Resolve(scene);
    return object ? scene.GetScript<BeamTrailRendererComponent>(object) : nullptr;
}

inline void SlashCutFxComponent::OnStart()
{
    /// @note 最初に全部作る。実行時に足した Script は、次のフレームに ScriptSystem が
    ///       OnStart を通すまで帯を張れない (IsReady) ため、当たった瞬間に作ると
    ///       いちばん見せたい 1 コマ目が抜ける。
    const int count = std::clamp(cutSlots, 1, 8);
    m_slots.assign(static_cast<std::size_t>(count), Slot{});
    for (int i = 0; i < count; ++i)
        if (GameObject* object = EnsureObject(i))
            m_slots[static_cast<std::size_t>(i)].ref = EntityRef{ object->GetID() };
    debugLiveCuts = 0;
}

inline void SlashCutFxComponent::OnDestroy()
{
    /// @note 枠はルートに置いてあるので、このスクリプトが消えても一緒には消えない。
    for (Slot& slot : m_slots)
        if (GameObject* object = slot.ref.Resolve(scene)) scene.Destroy(*object);
    m_slots.clear();
    debugLiveCuts = 0;
}

inline SlashCutFxComponent::Slot& SlashCutFxComponent::Claim()
{
    Slot* oldest = &m_slots.front();
    for (Slot& slot : m_slots) {
        if (!slot.live) return slot;
        if (slot.age > oldest->age) oldest = &slot;
    }
    return *oldest;
}

inline void SlashCutFxComponent::Launch(const Vector3& center, const Vector3& axis,
                                        float strength, const Vector4& tint)
{
    Slot& slot = Claim();
    slot.center   = center;
    slot.axis     = axis;
    slot.tint     = tint;
    slot.strength = strength;
    slot.length   = Lerp(cutLengthMin, cutLengthMax, strength);
    slot.width    = Lerp(cutWidthMin, cutWidthMax, strength);
    slot.age      = 0.0f;
    slot.live     = true;
}

inline void SlashCutFxComponent::Play(const Vector3& point, const Vector3& axis,
                                      float strength01, const Vector4& tint, bool cross)
{
    if (!enabled || m_slots.empty()) return;

    /// @note カメラ側へ寄せる。部位の中心は脚の中なので、そこで張ると帯が深度で隠れる。
    Vector3 center = point;
    Vector3 view   = Vector3::FORWARD;
    if (GameObject* camera = scene.GetMainCameraObject()) {
        const Vector3 toCamera = camera->transform.worldPosition - point;
        const float   distance = toCamera.Length();
        if (distance > EPSILON) {
            view   = toCamera / distance;
            center = point + view * std::min(Max(cutFrontOffset, 0.0f), distance * 0.5f);
        }
    }

    const Vector3 line     = axis.NormalizedOr(Vector3::RIGHT);
    const float   strength = Clamp01(strength01);
    if (!cross) {
        Launch(center, line, strength, tint);
        return;
    }

    /// @note 交差は視線のまわりで開く。画面の上で «X» に見えることが目的なので、
    ///       ワールドの上軸で回すと、見下ろしたときに 2 本が重なって 1 本に潰れる。
    const float half = ToRad(Clamp(cutCrossDegrees, 0.0f, 90.0f) * 0.5f);
    Launch(center, (Quaternion::FromAxisAngle(view,  half) * line).NormalizedOr(line), strength, tint);
    Launch(center, (Quaternion::FromAxisAngle(view, -half) * line).NormalizedOr(line), strength, tint);
}

inline void SlashCutFxComponent::OnUpdate()
{
    if (!enabled) return;

    const float dt    = Max(Time::deltaTime, 0.0f);
    const float flash = Max(cutFlashSeconds, 0.0f);
    const float seam  = Max(cutSeamSeconds, 0.02f);

    int live = 0;
    for (Slot& slot : m_slots) {
        if (!slot.live) continue;
        BeamTrailRendererComponent* renderer = RendererOf(slot);
        if (!renderer) {
            slot.live = false;
            continue;
        }
        /// @note まだ張れない枠は時計を進めずに待つ。進めると 1 コマ目の閃光を捨てることになる。
        if (!renderer->IsReady()) continue;

        slot.age += dt;
        if (slot.age >= flash + seam) {
            renderer->Hide();
            slot.live = false;
            continue;
        }

        float width     = slot.width;
        float length    = slot.length;
        float intensity = cutFlashIntensity;
        float coreBoost = 3.2f;
        float white     = 0.85f;
        if (slot.age < flash) {
            /// @note 閃光: 一瞬で太り、少しだけ伸びる。
            const float k    = flash > 0.0f ? slot.age / flash : 1.0f;
            const float ease = 1.0f - (1.0f - k) * (1.0f - k);
            width  = slot.width  * Lerp(0.7f, 1.0f, ease);
            length = slot.length * Lerp(1.0f - Clamp01(cutGrow), 1.0f, ease);
        } else {
            /// @note 切れ目: すぐ細り、白熱から刀の色へ冷めながら消える。
            const float s    = Clamp01((slot.age - flash) / seam);
            const float ease = 1.0f - (1.0f - s) * (1.0f - s);
            width     = slot.width * Lerp(1.0f, Clamp01(cutSeamWidth), ease);
            intensity = cutSeamIntensity * (1.0f - s) * (1.0f - s);
            coreBoost = Lerp(2.4f, 1.2f, s);
            white     = Lerp(0.6f, 0.0f, s);
        }
        /// @note 強い当たりほど明るい。形 (長さ・太さ) だけで差を付けると、遠くで見たときに差が消える。
        intensity *= Lerp(0.8f, 1.25f, slot.strength);

        const Vector4& tint = slot.tint;
        BeamTrailStyle style;
        style.materialPath = cutMaterial;
        style.width        = width;
        style.color        = Vector4{ Lerp(tint.x, 1.0f, white) * intensity,
                                      Lerp(tint.y, 1.0f, white) * intensity,
                                      Lerp(tint.z, 1.0f, white) * intensity, 1.0f };
        style.isCore       = true;
        /// @note 火花・光条より手前。後ろへ回ると加算の粒に芯が埋もれる。
        style.orderInLayer = 6;
        style.shape        = LineShape::Ribbon;
        /// @note 直線のまま張る。揺らすと «切った線» ではなく «放電» に見える。
        style.wobble           = 0.0f;
        style.wobbleFrequency  = 0.0f;
        style.segmentsPerMeter = 1.0f;
        style.coreWidth   = 0.32f;
        style.edgeFalloff = 1.6f;
        style.coreBoost   = coreBoost;
        style.muzzleFade  = 0.06f;
        style.tipFade     = 0.06f;
        style.tiling      = 0.6f;
        /// @note 切れ目の揺らぎをゆっくり流し、冷めていく間も «燻っている» ように見せる。
        style.scroll      = slot.age * 0.8f;

        const Vector3 half = slot.axis * (length * 0.5f);
        renderer->Show(slot.center - half, slot.center + half, style);
        ++live;
    }
    debugLiveCuts = live;
}

} // namespace sandbox
