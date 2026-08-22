/// @file BeamScorchComponent.hpp
/// @brief 極性レーザーが地形に当たった点へ、一定時間だけ残る焼け跡を落とす
/// @author Hasegawa Jin
/// @date 2026-08-23
///
/// WHY 跡を残すか:
///   6.2 は「対象は敵のみ。地形には極性が乗らない」と決めている。つまり壁や床を
///   なぞっている間、画面には何の変化も起きない。照射しているのに手応えが無い状態は
///   12.1 が最優先で潰そうとしているもので、しかもここは「当たっていないのか、
///   当たっているが乗らないのか」の区別が付かないという形で出る。
///   跡が残れば、線が地形のどこを通ったかが結果として見え、外したことが分かる。
///
/// WHY 敵には落とさないか:
///   敵はスキンメッシュで動き続けるため、ワールドへ投影したデカールは体から
///   ずり落ちる。敵側の表示は 6.5 のプレビューと 12.4 の明滅が既に持っている。
///
/// WHY 間隔を空けて置くか:
///   なぞりは 1 本の線を引く操作なので、毎フレーム置くと 1 回の照射で
///   100 枚を超える。デカールは 1 枚が 1 ドローコールなので、そのまま描画負荷になる。
///   前の跡から一定距離離れるまで置かないことにすると、枚数が「なぞった長さ」に
///   比例するようになり、速く振るほど疎になる = 塗り残しと同じ密度で残る。
///
/// WHY 寿命の管理を自分でしないか:
///   DecalPass が age を進め、寿命を過ぎた GameObject を自分で破棄する。
///   ここが持つのは「いつどこへ置くか」だけで、消し方はエンジンに任せる。
#pragma once

#include <Engine/Scene/Components/DecalComponent.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include <Math/Quaternion.hpp>
#include <Scripts/Player/PlayerAimComponent.hpp>
#include <Scripts/Player/PolarityGunComponent.hpp>
#include <Scripts/Utils/PolarityTypes.hpp>
#include <algorithm>
#include <array>
#include <cstddef>
#include <string>

using namespace fbzz::scene;
using namespace fbzz::math;
using fbzz::Time;

namespace sandbox {

class BeamScorchComponent : public Script {
    FBZZ_SCRIPT(BeamScorchComponent)

    /// リングの容量。Inspector の Max Count はこの範囲でしか動かせない。
    /// WHY 先に宣言するか: 下の Max Count の範囲指定がこの値を参照する。
    static constexpr int kMaxDecals = 64;

public:
    FBZZ_GROUP("Decal")
    FBZZ_FIELD_FILE(decalMaterial, "Assets/Materials/Decal/DecalScorch.mat",
                    "Decal Material", ".mat")
    FBZZ_TOOLTIP("render_path = \"decal\" の .mat。既定は手続きで焦げを描く DecalScorch")
    FBZZ_FIELD_RANGE(float, size, 0.55f, "Size", 0.05f, 5.0f)
    FBZZ_TOOLTIP("焦げの直径 (m)。6.2 のビーム半径 0.6m より少し小さいと、"
                 "線の中心を通った跡に見える")
    // WHY 厚みを持たせるか: デカールは OBB の中に入った面へ投影する。薄すぎると
    //     床のわずかな起伏で跡が切れ、厚すぎると壁の裏側の面にも回り込む。
    FBZZ_FIELD_RANGE(float, depth, 0.35f, "Projection Depth", 0.02f, 3.0f)
    FBZZ_FIELD_RANGE(float, lifetime, 3.5f, "Lifetime", 0.1f, 30.0f)
    FBZZ_TOOLTIP("跡が残る秒数。長くすると盤面が焦げだらけになり、線の新しさが読めなくなる")
    FBZZ_FIELD_RANGE(float, fadeTime, 1.2f, "Fade Time", 0.0f, 10.0f)
    FBZZ_TOOLTIP("消える前に薄くなる時間。Lifetime の内数")

    FBZZ_GROUP("Placement")
    FBZZ_FIELD_RANGE(float, spacing, 0.30f, "Spacing", 0.01f, 3.0f)
    FBZZ_TOOLTIP("前の跡からこの距離だけ離れるまで新しく置かない。小さいほど線が濃く連続する")
    FBZZ_FIELD_RANGE(float, minInterval, 0.04f, "Min Interval", 0.0f, 1.0f)
    FBZZ_TOOLTIP("連続で置ける最短間隔 (秒)。その場に留まって撃ち続けたときの上限になる")
    FBZZ_FIELD_RANGE_INT(int, maxCount, 24, "Max Count", 1, kMaxDecals)
    FBZZ_TOOLTIP("同時に残せる枚数。超えたら最も古い跡から消す")

    FBZZ_GROUP("Color")
    // 12.2 の配色に従い、縁の熱だけを極の色にする。焦げ本体まで赤青にすると、
    // 地形が帯電しているように見えて 7 章のルールと食い違う。
    FBZZ_FIELD(bool, tintByPolarity, true, "Tint By Polarity")
    FBZZ_FIELD_RANGE(float, rimIntensity, 1.1f, "Rim Intensity", 0.0f, 4.0f)

    FBZZ_GROUP("Debug")
    FBZZ_FIELD_READ_ONLY(int, debugLiveCount, 0, "Live Decals")

    /// PlayerComponent が内部モジュールとして持つときに、同じ参照を渡す。
    void SetAimComponent(PlayerAimComponent* aim) { m_aimOverride = aim; }
    void SetGun(PolarityGunComponent* gun) { m_gunOverride = gun; }

    void OnStart() override;
    void OnLateUpdate() override;
    void OnDestroy() override;

private:
    /// 今の上限。Inspector の値をリングの容量へ収めたもの。
    [[nodiscard]] int Capacity() const { return std::clamp(maxCount, 1, kMaxDecals); }

    [[nodiscard]] PlayerAimComponent*   Aim() const;
    [[nodiscard]] PolarityGunComponent* Gun() const;
    /// 今フレーム照射している極。両方なら Plus を採る (色は 1 つしか選べない)。
    /// 撃っていなければ None。
    [[nodiscard]] Polarity EmittingPolarity() const;

    void Place(const Vector3& point, const Vector3& normal, Polarity polarity);
    /// リングの次の枠を空ける。埋まっていれば最も古い 1 枚を消す。
    void RecycleOldest();

    PlayerAimComponent*   m_aimOverride = nullptr;
    PolarityGunComponent* m_gunOverride = nullptr;

    // WHY リングで持つか: 寿命が来た跡は DecalPass が破棄するので、ここは
    //     「上限を超えたら古い順に畳む」ためだけに順番を覚えていればよい。
    //     破棄済みの枠は EntityRef が nullptr へ解決されるので掃除も要らない。
    std::array<EntityRef, kMaxDecals> m_decals{};
    int m_next = 0;

    Vector3 m_lastPoint = Vector3::ZERO;
    bool    m_hasLast   = false;
    float   m_cooldown  = 0.0f;
};

FBZZ_REFLECT(BeamScorchComponent)

// ── 実装 (inline) ─────────────────────────────────────────────────────────────

inline PlayerAimComponent* BeamScorchComponent::Aim() const
{
    return m_aimOverride ? m_aimOverride : scene.GetScript<PlayerAimComponent>();
}

inline PolarityGunComponent* BeamScorchComponent::Gun() const
{
    return m_gunOverride ? m_gunOverride : scene.GetScript<PolarityGunComponent>();
}

inline Polarity BeamScorchComponent::EmittingPolarity() const
{
    auto* gun = Gun();
    if (!gun) return Polarity::None;
    if (gun->IsEmitting(Polarity::Plus))  return Polarity::Plus;
    if (gun->IsEmitting(Polarity::Minus)) return Polarity::Minus;
    return Polarity::None;
}

inline void BeamScorchComponent::OnStart()
{
    m_decals.fill(EntityRef{});
    m_next      = 0;
    m_hasLast   = false;
    m_cooldown  = 0.0f;
    debugLiveCount = 0;
}

inline void BeamScorchComponent::OnLateUpdate()
{
    const float dt = Max(Time::deltaTime, 0.0f);
    m_cooldown = Max(0.0f, m_cooldown - dt);

    const Polarity polarity = EmittingPolarity();
    auto* aim = Aim();

    // 照射していない / 線が地形に届いていないなら、次に当てた 1 枚目を
    // 必ず置けるよう「前の跡」を忘れる。忘れないと、いったん空へ振ってから
    // 同じ壁へ戻したときに 1 枚目が spacing に食われる。
    if (polarity == Polarity::None || !aim || !aim->HasAim() || !aim->HitGeometry()) {
        m_hasLast = false;
        return;
    }

    const Vector3 point = aim->AimPoint();
    if (m_cooldown > 0.0f) return;
    if (m_hasLast && (point - m_lastPoint).LengthSq() < spacing * spacing) return;

    Place(point, aim->SurfaceNormal(), polarity);
    m_lastPoint = point;
    m_hasLast   = true;
    m_cooldown  = Max(minInterval, 0.0f);
}

inline void BeamScorchComponent::RecycleOldest()
{
    // m_next は常に「次に使う = 最も古い」枠を指す。埋まっていればそれを畳む。
    // Inspector で上限を縮めた直後は範囲外を指しうるので、毎回丸めてから触る。
    m_next = m_next % Capacity();
    EntityRef& slot = m_decals[static_cast<std::size_t>(m_next)];
    if (GameObject* old = slot.Resolve(scene))
        scene.Destroy(*old);
    slot = {};
}

inline void BeamScorchComponent::Place(const Vector3& point, const Vector3& normal,
                                       Polarity polarity)
{
    Vector3 axis = normal;
    if (axis.LengthSq() <= EPSILON) axis = Vector3::UP;
    axis = axis.Normalized();

    RecycleOldest();

    GameObject& object = scene.Create("BeamScorch");
    object.runtimeGenerated = true;

    // デカールの投影軸はローカル +Y (DecalPass が transform.up を法線として使う)。
    // LookRotation が向けるのは +Z なので、X 軸まわりに 90 度回して +Y を +Z へ
    // 送ってから、その +Z を面の法線へ合わせる。
    // WHY 真上 / 真下の面だけ up を替えるか: LookRotation の up と forward が
    //     平行になると基底が作れず、床と天井でだけ姿勢が飛ぶ。
    const Vector3 up = Abs(Vector3::Dot(axis, Vector3::UP)) > 0.99f
        ? Vector3::FORWARD : Vector3::UP;
    const Quaternion rotation =
        Quaternion::LookRotation(axis, up) * Quaternion::FromAxisAngle(Vector3::RIGHT, HALF_PI);

    // OBB の中心を面の上に置く。厚みの半分が表と裏へ均等に出るので、
    // わずかな起伏があっても跡が切れない。
    object.transform.position      = point;
    object.transform.worldPosition = point;
    object.transform.rotation      = rotation;
    object.transform.worldRotation = rotation;
    object.transform.scale         = { Max(size, 0.01f), Max(depth, 0.01f), Max(size, 0.01f) };

    DecalComponent& decal = object.AddComponent<DecalComponent>();
    decal.materialPath = decalMaterial;
    decal.lifetime     = Max(lifetime, 0.0f);
    decal.fadeTime     = Clamp(fadeTime, 0.0f, decal.lifetime);
    // 斜めから当てた跡が長い筋に伸びるのを防ぐ (DecalComponent の角度フェード)。
    decal.angleFadeStrength = 1.0f;
    decal.angleFadeDegrees  = 65.0f;

    // 縁の熱だけを極の色にする。共有 .mat へ書くと ＋ の跡を出した瞬間に
    // 残っている − の跡まで赤くなるので、このデカールだけの上書きで乗せる。
    if (tintByPolarity) {
        const Vector4 color = PolarityColor(polarity);
        decal.materialParamOverrides["rimColor"] = { color.x, color.y, color.z, 1.0f };
    }
    decal.materialParamOverrides["rimIntensity"] = { Max(rimIntensity, 0.0f) };

    m_decals[static_cast<std::size_t>(m_next)] = EntityRef{ object.GetID() };
    m_next = (m_next + 1) % Capacity();

    int live = 0;
    for (const EntityRef& ref : m_decals)
        if (ref.Resolve(scene)) ++live;
    debugLiveCount = live;
}

inline void BeamScorchComponent::OnDestroy()
{
    // ルートに置いた以上、プレイヤーと一緒には消えない。寿命が来る前に
    // Play を止めた跡はここで畳む。
    for (EntityRef& ref : m_decals) {
        if (GameObject* object = ref.Resolve(scene))
            scene.Destroy(*object);
        ref = {};
    }
    m_next = 0;
}

} // namespace sandbox
