/// @file    PolarityRingComponent.hpp
/// @brief   極性の作用半径を地面へ描く環の管理。照準中の «距離の表示» と弾けた放射
/// @author  Hasegawa Jin
/// @date    2026-08-28
///
/// WHY 環が要るか (Docs/weapon-emitter.md「照準中の表示は距離を出す」):
///   本作の芯は «反発 6m / 引力 12m» という距離の違いで、固めると弾け、散らすと集まる。
///   ところがその 6m は 3D の画面からは読めない。中和のプレビューを廃止した代わりに
///   プレイヤーへ渡すのは «結果» ではなく «今どういう配置か» で、その唯一の表示がこれ。
///
/// WHY 1 箇所で持つか:
///   環は 2 つの用途で出る ─ 照準中に出しっぱなしにする 1 枚と、弾けた瞬間に走る数枚。
///   どちらも «地面へ投影した同じ環» で、違うのは寿命と掃きの進み方だけ。持ち主を
///   分けると、材質・寸法・角度フェードの設定が 2 箇所に散る。
///
/// WHY 依存を軽く保つか:
///   照準側 (AimMarkerComponent) と盤面側 (PolarityFieldComponent) の両方から呼ばれる。
///   ここがプレイヤーや盤面を知りに行くと、盤面 → ここ → プレイヤー → 銃 → 戦闘 → 盤面 で
///   include が輪になる。«どこに何色の環を出すか» を決めるのは呼ぶ側、
///   «どう出すか» を決めるのはここ、で切る。
///
/// WHY 枠を開始時に作り切るか:
///   環は毎秒のように出る。そのたびに GameObject を作って捨てると、シーンの
///   GameObject 数が戦闘の激しさに合わせて上下し、EntityID を握っている側の参照が
///   揺さぶられる (ビーム・VFX・撃破コアの枠が同じ理由で寝かせてある)。
#pragma once

#include <Engine/Scene/Components/DecalComponent.hpp>
#include <Engine/Scene/EntityRef.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Scripts/Utils/PolarityTypes.hpp>
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using namespace fbzz::scene;
using namespace fbzz::math;
using fbzz::Time;

namespace sandbox {

class PolarityRingComponent : public Script {
    FBZZ_SCRIPT(PolarityRingComponent)

public:
    FBZZ_GROUP("Material")
    FBZZ_FIELD_FILE(ringMaterial, "Assets/Materials/Decal/DecalPolarityRing.mat",
                    "Ring Material", ".mat")
    FBZZ_TOOLTIP("render_path = \"decal\" の .mat。既定は手続きで環を描く DecalPolarityRing")

    FBZZ_GROUP("Projection")
    // WHY 厚みを持たせるか: デカールは OBB の中に入った受け面にだけ乗る。薄いと
    //     アリーナの起伏や瓦礫の段差で環が途切れ、«測っている» どころか壊れて見える。
    FBZZ_FIELD_RANGE(float, projectionDepth, 4.0f, "Depth", 0.2f, 20.0f)
    FBZZ_TOOLTIP("投影ボリュームの高さ [m]。床の起伏をまたげる厚みを取る")
    FBZZ_FIELD_RANGE(float, angleFadeDegrees, 55.0f, "Angle Fade", 0.0f, 89.0f)
    FBZZ_TOOLTIP("これ以上寝た面には乗せない。壁へ這い上がった環は «距離» を表さない")

    FBZZ_GROUP("Aim Ring")
    FBZZ_FIELD_RANGE(float, holdSpinHz, 0.06f, "Spin", -2.0f, 2.0f)
    FBZZ_TOOLTIP("目盛りが回る速さ [周/秒]。速いと計器ではなく «回転する飾り» になる")
    FBZZ_FIELD_RANGE(float, holdPulseHz, 1.1f, "Pulse Hz", 0.0f, 8.0f)
    FBZZ_FIELD_RANGE(float, holdPulseDepth, 0.22f, "Pulse Depth", 0.0f, 1.0f)
    FBZZ_TOOLTIP("明滅の深さ。出しっぱなしの表示なので、動きが無いと背景に溶ける")
    FBZZ_FIELD_RANGE(float, holdFadeSeconds, 0.12f, "Fade", 0.0f, 1.0f)
    FBZZ_TOOLTIP("照準が外れてから消えるまで。0 だと敵の縁を掠めるたびに点滅する")

    FBZZ_GROUP("Burst Ring")
    FBZZ_FIELD_RANGE_INT(int, burstSlots, 6, "Slots", 1, 24)
    FBZZ_TOOLTIP("同時に走らせる放射の数。超えたら最も古い 1 枚を奪う")
    FBZZ_FIELD_RANGE(float, burstSeconds, 0.18f, "Seconds", 0.05f, 1.5f)
    FBZZ_TOOLTIP("放射が中心から縁まで抜ける時間。押しは毎秒使う手なので、"
                 "画面に残らない長さに収めること")
    FBZZ_FIELD_RANGE(float, burstWidth, 0.22f, "Width", 0.02f, 1.0f)
    FBZZ_TOOLTIP("走る帯の太さ (半径比)")
    FBZZ_FIELD_RANGE(float, burstGain, 1.6f, "Gain", 0.0f, 6.0f)
    FBZZ_TOOLTIP("放射の明るさ。環より明るくしないと «出来事» に見えない")

    FBZZ_GROUP("Debug")
    FBZZ_FIELD_READ_ONLY(int, debugLiveBursts, 0, "Live Bursts")
    FBZZ_FIELD_READ_ONLY(bool, debugAimRing, false, "Aim Ring")

    /// 置き場所に依存しない窓口。照準側と盤面側の両方から呼ばれる。
    [[nodiscard]] static PolarityRingComponent* Instance() { return s_instance; }

    /// 作用半径の環を «今フレームだけ» 出す。出し続けたい側が毎フレーム呼ぶ。
    ///
    /// WHY 消す API を持たないか: «解除» を別に呼ぶ作りにすると、照準が外れた 1 回を
    ///     取りこぼしただけで環が地面に張り付いたまま戻らない。呼ばれなくなった時点が
    ///     解除になる形にする (カメラの Loosen / 移動倍率と同じ約束)。
    /// @param radius 環の «半径» [m]。反発半径をそのまま渡す。
    void ShowRadius(const Vector3& center, float radius, Polarity polarity);

    /// 弾けた瞬間の放射を 1 枚走らせる。以後は自分で消える。
    void Burst(const Vector3& center, float radius, Polarity polarity);

    void OnStart()   override;
    void OnUpdate()  override;
    void OnDestroy() override;

private:
    /// 走っている放射 1 枚。
    struct BurstSlot {
        EntityRef ref;
        float     age  = 0.0f;
        bool      live = false;
    };

    static inline PolarityRingComponent* s_instance = nullptr;

    /// 環 1 枚ぶんの GameObject を作る (または DLL リロード後に拾い直す)。
    [[nodiscard]] GameObject* BuildRing(const std::string& name);
    /// 位置と大きさを合わせる。デカールの投影軸はローカル +Y。
    void PlaceRing(GameObject& object, const Vector3& center, float radius) const;
    /// 極の色を 1 枚だけへ流す。共有 .mat へ書くと、残っている別の極の環まで染まる。
    static void TintRing(DecalComponent& decal, Polarity polarity, float ringAlpha,
                         float fillStrength);

    EntityRef              m_holdRing;
    std::vector<BurstSlot> m_bursts;
    int                    m_burstNext = 0;

    // 今フレーム ShowRadius が呼ばれたか。呼ばれなくなった時点が解除になる。
    bool     m_holdRequested = false;
    Vector3  m_holdCenter    = Vector3::ZERO;
    float    m_holdRadius    = 0.0f;
    Polarity m_holdPolarity  = Polarity::None;
    /// 照準が外れてからの残り。0 で完全に消える。
    float    m_holdFade      = 0.0f;
    /// 目盛りの位相。実時間で進める (ヒットストップ中に止まると «壊れた» に見える)。
    float    m_spin          = 0.0f;
};

FBZZ_REFLECT(PolarityRingComponent)

// ── 実装 (inline) ─────────────────────────────────────────────────────────────

inline void PolarityRingComponent::OnStart()
{
    s_instance = this;

    m_holdRequested = false;
    m_holdFade      = 0.0f;
    m_spin          = 0.0f;
    m_burstNext     = 0;
    m_bursts.clear();

    // 枠を開始時に作り切る。戦闘中に GameObject を増やすと配列の再確保が走る。
    if (GameObject* hold = BuildRing("PolarityRing_Aim")) {
        m_holdRing = EntityRef{ hold->GetID() };
        hold->SetActive(false);
    }

    const int slots = std::max(burstSlots, 1);
    m_bursts.resize(static_cast<std::size_t>(slots));
    for (int i = 0; i < slots; ++i) {
        GameObject* object = BuildRing("PolarityRing_Burst_" + std::to_string(i));
        if (!object) break;
        object->SetActive(false);
        m_bursts[static_cast<std::size_t>(i)].ref = EntityRef{ object->GetID() };
    }
}

inline void PolarityRingComponent::OnDestroy()
{
    // 枠はルートに置いた GameObject なので、このスクリプトと一緒には消えない。
    if (GameObject* hold = m_holdRing.Resolve(scene)) scene.Destroy(*hold);
    for (BurstSlot& slot : m_bursts)
        if (GameObject* object = slot.ref.Resolve(scene)) scene.Destroy(*object);
    m_bursts.clear();

    if (s_instance == this) s_instance = nullptr;
}

inline GameObject* PolarityRingComponent::BuildRing(const std::string& name)
{
    // WHY 名前で拾い直すか: スクリプト DLL をリロードするとこの Script は作り直され、
    //     EntityRef は空に戻る。枠の GameObject は Scene 側に残っているので、
    //     拾わずに作るとリロードのたびに枠が増えていく。
    GameObject* object = scene.Find(name);
    if (!object) {
        GameObject& created = scene.Create(name);
        created.runtimeGenerated = true;
        object = &created;
    }

    DecalComponent* decal = object->GetComponent<DecalComponent>();
    if (!decal) decal = &object->AddComponent<DecalComponent>();

    decal->materialPath = ringMaterial;
    // 寿命はこちらが SetActive で持つ。デカール側に持たせると、枠が勝手に消えて
    // こちらの EntityRef が «居ない相手» を指す。
    decal->lifetime = -1.0f;
    decal->fadeTime = 0.0f;
    // 壁へ這い上がった環は距離を表さない。寝た面では消す。
    decal->angleFadeStrength = 1.0f;
    decal->angleFadeDegrees  = Clamp(angleFadeDegrees, 0.0f, 89.0f);
    return object;
}

inline void PolarityRingComponent::PlaceRing(GameObject& object, const Vector3& center,
                                             float radius) const
{
    // デカールの投影軸はローカル +Y。地面へ落とすので姿勢は無回転でよい
    // (任意の面へ貼るなら法線から LookRotation を組む必要がある)。
    const float diameter = std::max(radius, 0.05f) * 2.0f;

    object.transform.position      = center;
    object.transform.worldPosition = center;
    object.transform.rotation      = Quaternion::Identity();
    object.transform.worldRotation = Quaternion::Identity();
    object.transform.scale = { diameter, std::max(projectionDepth, 0.2f), diameter };
}

inline void PolarityRingComponent::TintRing(DecalComponent& decal, Polarity polarity,
                                            float ringAlpha, float fillStrength)
{
    // 無極は .mat の既定色 (青灰) のまま。極の色を «無極» に割り当てると、
    // 帯電していない相手を狙っているだけで盤面に極が乗っているように見える。
    if (polarity != Polarity::None) {
        const Vector4 color = PolarityColor(polarity);
        decal.materialParamOverrides["ringColor"] =
            { color.x, color.y, color.z, std::max(ringAlpha, 0.0f) };
        decal.materialParamOverrides["fillColor"] =
            { color.x * 0.6f, color.y * 0.6f, color.z * 0.6f, 0.10f };
    } else {
        decal.materialParamOverrides.erase("ringColor");
        decal.materialParamOverrides.erase("fillColor");
    }
    decal.materialParamOverrides["fillStrength"] = { std::max(fillStrength, 0.0f) };
}

inline void PolarityRingComponent::ShowRadius(const Vector3& center, float radius,
                                              Polarity polarity)
{
    if (radius <= 0.0f) return;
    m_holdRequested = true;
    m_holdCenter    = center;
    m_holdRadius    = radius;
    m_holdPolarity  = polarity;
}

inline void PolarityRingComponent::Burst(const Vector3& center, float radius,
                                         Polarity polarity)
{
    if (m_bursts.empty() || radius <= 0.0f) return;

    // 埋まっていれば最も古い枠を奪う。next は常に «次に使う = 最も古い» を指す。
    m_burstNext = m_burstNext % static_cast<int>(m_bursts.size());
    BurstSlot& slot = m_bursts[static_cast<std::size_t>(m_burstNext)];
    m_burstNext = (m_burstNext + 1) % static_cast<int>(m_bursts.size());

    GameObject* object = slot.ref.Resolve(scene);
    if (!object) return;

    PlaceRing(*object, center, radius);
    object->SetActive(true);

    if (auto* decal = object->GetComponent<DecalComponent>()) {
        TintRing(*decal, polarity, 1.0f, 0.0f);
        // 放射は «帯だけ»。環そのものを出すと、押した跡が地面に残って見える。
        decal->materialParamOverrides["ringWidth"]  = { 0.0f };
        decal->materialParamOverrides["tickCount"]  = { 0.0f };
        decal->materialParamOverrides["sweepWidth"] = { std::max(burstWidth, 0.02f) };
        decal->materialParamOverrides["sweep"]      = { 0.0f };
        decal->materialParamOverrides["pulse"]      = { std::max(burstGain, 0.0f) };
    }

    slot.age  = 0.0f;
    slot.live = true;
}

inline void PolarityRingComponent::OnUpdate()
{
    // WHY 実時間で進めるか: 衝突のたびにヒットストップが掛かる。縮んだ時間で回すと、
    //     画面が止まっている間だけ環の目盛りも止まり、表示が壊れたように見える。
    const float dt = std::max(time.UnscaledDeltaTime(), 0.0f);
    m_spin = std::fmod(m_spin + holdSpinHz * dt, 1.0f);

    // ── 照準の環 ──────────────────────────────────────────────────────────
    // 呼ばれなくなった時点が解除。取りこぼしで張り付いたままにならない。
    m_holdFade = m_holdRequested ? std::max(holdFadeSeconds, 0.0f)
                                 : std::max(0.0f, m_holdFade - dt);
    const bool showHold = m_holdRequested || m_holdFade > 0.0f;
    debugAimRing = showHold;

    if (GameObject* hold = m_holdRing.Resolve(scene)) {
        if (!showHold) {
            hold->SetActive(false);
        } else {
            PlaceRing(*hold, m_holdCenter, m_holdRadius);
            hold->SetActive(true);
            if (auto* decal = hold->GetComponent<DecalComponent>()) {
                // 照準が外れた後の余韻ぶんだけ薄める。硬く消すと、敵の縁を掠める
                // たびに環が点滅して «測っている» どころではなくなる。
                const float fade = holdFadeSeconds > 0.0f
                    ? Clamp01(m_holdFade / holdFadeSeconds) : 1.0f;
                const float wave = 0.5f + 0.5f * std::sin(Time::time * holdPulseHz * TWO_PI);
                const float gain = Lerp(1.0f - Clamp01(holdPulseDepth), 1.0f, wave) * fade;

                TintRing(*decal, m_holdPolarity, 0.75f, 0.55f);
                decal->materialParamOverrides["spin"]  = { m_spin };
                decal->materialParamOverrides["pulse"] = { gain };
                // 掃きは出さない。放射は «出来事» の語で、こちらは «状態» の表示。
                decal->materialParamOverrides["sweep"] = { 1.0f };
            }
        }
    }
    m_holdRequested = false;

    // ── 放射 ──────────────────────────────────────────────────────────────
    const float life = std::max(burstSeconds, 0.05f);
    int live = 0;
    for (BurstSlot& slot : m_bursts) {
        if (!slot.live) continue;
        GameObject* object = slot.ref.Resolve(scene);
        if (!object) {
            slot.live = false;
            continue;
        }

        slot.age += dt;
        const float t = slot.age / life;
        if (t >= 1.0f) {
            object->SetActive(false);
            slot.live = false;
            continue;
        }
        ++live;

        if (auto* decal = object->GetComponent<DecalComponent>()) {
            // 帯は中心から縁へ抜ける。シェーダー側が (1 - sweep)^2 で自然に薄まるので、
            // ここは進みだけを渡す。
            decal->materialParamOverrides["sweep"] = { t };
        }
    }
    debugLiveBursts = live;
}

} // namespace sandbox
