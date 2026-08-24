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
///
/// WHY 跡が «置いたあとも動く» か:
///   焼けた瞬間と焼けて 3 秒経った跡が同じ絵だと、どれが今引いた線なのかが読めない。
///   跡は 6.2 の「地形には極性が乗らない」を補う表示なので、新しさが読めなければ
///   «外した場所» の情報にならない。デカールの cbuffer に時刻は無いので、
///   経過時間はここが数えて materialParamOverrides へ毎フレーム流す。
///
/// WHY 火の粉と煙を .vfx に出すか:
///   跡が «静止画» のままだと、当てている最中も当て終わった後も画面が動かない。
///   焼けた点から一定時間だけ粒が出続けると、なぞった軌跡が «まだ熱い点の列» として
///   残り、線を引いた結果が時間の情報として見える。演出の枠管理は
///   VfxManagerComponent が持つので、ここは «焼けた» とだけ言う。
#pragma once

#include <Engine/Scene/Components/DecalComponent.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include <Math/Quaternion.hpp>
#include <Scripts/Game/VfxManagerComponent.hpp>
#include <Scripts/Player/PlayerAimComponent.hpp>
#include <Scripts/Player/PolarityGunComponent.hpp>
#include <Scripts/Utils/PolarityTypes.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
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

    FBZZ_GROUP("Burn")
    // 置いた瞬間から «焼き広がる»。満開で出すと、なぞった線がスタンプの列に見える。
    FBZZ_FIELD_RANGE(float, burnInSeconds, 0.14f, "Burn In", 0.0f, 2.0f)
    FBZZ_TOOLTIP("跡が最終的な大きさまで広がる時間。0 で最初から満開")
    FBZZ_FIELD_RANGE(float, coolSeconds, 1.10f, "Cool Down", 0.0f, 10.0f)
    FBZZ_TOOLTIP("縁の白熱が引いて焦げ色だけになるまでの時間。"
                 "これが Lifetime に近いと、跡がいつまでも «今焼いた» ように見える")
    FBZZ_FIELD_RANGE(float, startCoverage, 0.35f, "Start Coverage", 0.05f, 1.0f)
    FBZZ_TOOLTIP("焼き広がりの初期値。Size に対する割合")

    FBZZ_GROUP("Embers")
    // 「跡が残る」だけでは画面が動かない。焼けた点から一定時間だけ粒を出す。
    FBZZ_FIELD(bool, playEmbers, true, "Play Embers")
    FBZZ_TOOLTIP("焼けた点ごとに FX_BEAM_Scorch.vfx を鳴らす。"
                 "跡と同じ間隔で置かれるので、Spacing を詰めるとそのぶん粒が増える")
    FBZZ_FIELD_RANGE(float, emberSpacingScale, 1.6f, "Ember Spacing", 1.0f, 8.0f)
    FBZZ_TOOLTIP("跡 何枚につき 1 回 粒を出すか (Spacing に対する倍率)。"
                 "1.0 で全部の跡から出るが、なぞりの線が粒で埋まる")

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
    /// 焼けた点から火の粉と煙を出す。跡より粗い間隔で間引く。
    void PlayEmbers(const Vector3& point, const Vector3& normal, Polarity polarity);
    /// リングの次の枠を空ける。埋まっていれば最も古い 1 枚を消す。
    void RecycleOldest();
    /// 生きている跡の «焼き広がり» と «冷め» を 1 フレームぶん進める。
    void TickMarks(float dt);

    /// 跡 1 枚。デカール本体は Scene 側に居るので、ここが持つのは時間だけ。
    ///
    /// WHY 経過時間を自分で数えるか: DecalPass も age を進めているが、材質側からは
    ///     読めない (DecalConstants が渡すのはフェード込みの decalAlpha だけ)。
    ///     焼き広がりと冷めは «消えかけているか» とは別の軸なので、ここで数える。
    struct Mark {
        EntityRef ref;
        float     age = 0.0f;
    };

    PlayerAimComponent*   m_aimOverride = nullptr;
    PolarityGunComponent* m_gunOverride = nullptr;

    // WHY リングで持つか: 寿命が来た跡は DecalPass が破棄するので、ここは
    //     「上限を超えたら古い順に畳む」ためだけに順番を覚えていればよい。
    //     破棄済みの枠は EntityRef が nullptr へ解決されるので掃除も要らない。
    std::array<Mark, kMaxDecals> m_marks{};
    int m_next = 0;

    Vector3 m_lastPoint = Vector3::ZERO;
    bool    m_hasLast   = false;
    float   m_cooldown  = 0.0f;
    /// 前回 粒を出した跡の位置。跡より粗い間隔で出すために別に覚える。
    Vector3 m_lastEmberPoint = Vector3::ZERO;
    bool    m_hasEmber       = false;
    /// 跡ごとに違う種を配るための連番。乱数にすると Play のたびに形が変わり、
    /// 「今の見た目が良かったのか」を判断できなくなる。
    uint32_t m_seedCounter = 1u;
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
    m_marks.fill(Mark{});
    m_next      = 0;
    m_hasLast   = false;
    m_hasEmber  = false;
    m_cooldown  = 0.0f;
    m_seedCounter = 1u;
    debugLiveCount = 0;
}

inline void BeamScorchComponent::OnLateUpdate()
{
    const float dt = Max(Time::deltaTime, 0.0f);
    m_cooldown = Max(0.0f, m_cooldown - dt);

    // 跡は照射をやめた後も生き続ける。焼き広がりと冷めは照射の有無に関係なく進める。
    TickMarks(dt);

    const Polarity polarity = EmittingPolarity();
    auto* aim = Aim();

    // 照射していない / 線が地形に届いていないなら、次に当てた 1 枚目を
    // 必ず置けるよう「前の跡」を忘れる。忘れないと、いったん空へ振ってから
    // 同じ壁へ戻したときに 1 枚目が spacing に食われる。
    if (polarity == Polarity::None || !aim || !aim->HasAim() || !aim->HitGeometry()) {
        m_hasLast  = false;
        m_hasEmber = false;
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

inline void BeamScorchComponent::TickMarks(float dt)
{
    int live = 0;
    for (Mark& mark : m_marks) {
        GameObject* object = mark.ref.Resolve(scene);
        if (!object) {
            // 寿命が来た跡は DecalPass が破棄済み。枠を空けておかないと、
            // 上限を数えるときに «居ない跡» を数え続ける。
            mark.ref = {};
            continue;
        }
        ++live;

        auto* decal = object->GetComponent<DecalComponent>();
        if (!decal) continue;

        mark.age += dt;

        // 焼き «広がる»。満開で出すと、なぞった線が同じ大きさのスタンプの列に見える。
        const float spread = burnInSeconds > 0.0f ? Clamp01(mark.age / burnInSeconds) : 1.0f;
        decal->materialParamOverrides["coverage"] =
            { Lerp(Clamp(startCoverage, 0.05f, 1.0f), 1.0f, spread) };

        // 冷める。0 = 焼けた瞬間 (縁が白熱) / 1 = 焦げ色だけ。
        decal->materialParamOverrides["cooled"] =
            { coolSeconds > 0.0f ? Clamp01(mark.age / coolSeconds) : 1.0f };
    }
    debugLiveCount = live;
}

inline void BeamScorchComponent::RecycleOldest()
{
    // m_next は常に「次に使う = 最も古い」枠を指す。埋まっていればそれを畳む。
    // Inspector で上限を縮めた直後は範囲外を指しうるので、毎回丸めてから触る。
    m_next = m_next % Capacity();
    Mark& slot = m_marks[static_cast<std::size_t>(m_next)];
    if (GameObject* old = slot.ref.Resolve(scene))
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

    // 輪郭の崩し方をこの 1 枚だけずらす。黄金比の刻みは連続する種が最も散らばる。
    // WHY 乱数にしないか: Play のたびに跡の形が変わると、Spacing や Size を触ったとき
    //     «今の見た目が良かったのか» を比べられない。
    const float seed = std::fmod(static_cast<float>(m_seedCounter) * 0.6180339887f, 1.0f);
    ++m_seedCounter;
    decal.materialParamOverrides["seed"] = { seed };
    // 置いた瞬間は «焼けたて»。以降は TickMarks が進める。
    decal.materialParamOverrides["cooled"]   = { 0.0f };
    decal.materialParamOverrides["coverage"] = { Clamp(startCoverage, 0.05f, 1.0f) };

    Mark& slot = m_marks[static_cast<std::size_t>(m_next)];
    slot.ref = EntityRef{ object.GetID() };
    slot.age = 0.0f;
    m_next = (m_next + 1) % Capacity();

    PlayEmbers(point, axis, polarity);
}

inline void BeamScorchComponent::PlayEmbers(const Vector3& point, const Vector3& normal,
                                            Polarity polarity)
{
    if (!playEmbers) return;

    // 跡より粗い間隔で出す。跡と同じ間隔だと、なぞった線が粒で埋まって
    // «焼けた点の列» ではなく «光の帯» になり、どこを通したかが読めなくなる。
    const float step = Max(spacing * Max(emberSpacingScale, 1.0f), 0.01f);
    if (m_hasEmber && (point - m_lastEmberPoint).LengthSq() < step * step) return;

    auto* vfx = VfxManagerComponent::Instance();
    if (!vfx) return;

    vfx->PlayBeamScorch(point, normal, polarity, Max(size, 0.05f));
    m_lastEmberPoint = point;
    m_hasEmber       = true;
}

inline void BeamScorchComponent::OnDestroy()
{
    // ルートに置いた以上、プレイヤーと一緒には消えない。寿命が来る前に
    // Play を止めた跡はここで畳む。
    for (Mark& mark : m_marks) {
        if (GameObject* object = mark.ref.Resolve(scene))
            scene.Destroy(*object);
        mark = {};
    }
    m_next = 0;
    debugLiveCount = 0;
}

} // namespace sandbox
