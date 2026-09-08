/// @file    ScreenEffectManagerComponent.hpp
/// @brief   画面効果 (フェード・フラッシュ・ビネット・色収差・専用パス) の唯一の書き手
/// @author  Hasegawa Jin
/// @date    2026-08-22
///
/// WHY 1 箇所に集めるか:
///   ランタイムの PostProcessSettings は「まるごと差し替える」器で、TimeManager が扱う
///   Time::timeScale と同じ性質を持つ。書き手が 2 つになると、後から書いた側が
///   前の効果を消す。実際 SceneManagerScript はフェードのために TryGet() で読んで
///   書き戻しており、そこへ被弾フラッシュが割り込めば、どちらかが必ず消える。
///
/// WHY 毎フレーム読み戻さないか:
///   自分が書いた結果を次のフレームの基準として読むと、効果が積み重なって発散する。
///   効果が 1 つも無い状態の設定を基準として保持し、常にそこから作り直す。
///
/// WHY 専用シェーダー (Implode / HitstopFreeze) もここが持つか:
///   customEffects は PostProcessSettings の一部なので、上と同じ「まるごと差し替え」の
///   対象になる。他所で postprocess.AddCustom() したものは、このマネージャーが
///   1 フレーム書いた瞬間に消える。器を握っている側が自分の効果も一緒に載せる以外に、
///   両方を生かす置き方が無い。
#pragma once

#include <Engine/Renderer/RenderSettings.hpp>
// WHY Scene.hpp まで要るか: GetMainCameraObject / GetComponent の実体が末尾にある。
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include <Math/Vector2.hpp>
#include <Math/Vector3.hpp>
#include <Scripts/Game/HitstopManagerComponent.hpp>
#include <Scripts/Game/TimeManagerComponent.hpp>
#include <Scripts/Utils/SceneTransition.hpp>
#include <Scripts/Utils/ScreenProjection.hpp>
#include <algorithm>
#include <cmath>

using namespace fbzz::scene;
using namespace fbzz::math;

namespace sandbox {

// 専用パスの名前とシェーダー。名前は customEffects の中の識別にしか使わない。
inline constexpr const char* kImplodeEffectName = "Implode";
inline constexpr const char* kImplodeShaderPath =
    "assets/shaders/PostProcess/Custom/Implode.hlsl";
inline constexpr const char* kFreezeEffectName = "HitstopFreeze";
inline constexpr const char* kFreezeShaderPath =
    "assets/shaders/PostProcess/Custom/HitstopFreeze.hlsl";
inline constexpr const char* kOutlineEffectName = "Outline";
inline constexpr const char* kOutlineShaderPath =
    "assets/shaders/PostProcess/Custom/Outline.hlsl";
inline constexpr const char* kVignetteEffectName = "GameVignette";
inline constexpr const char* kVignetteShaderPath =
    "assets/shaders/PostProcess/Custom/GameVignette.hlsl";
inline constexpr const char* kSlowEffectName = "SlowMotion";
inline constexpr const char* kSlowShaderPath =
    "assets/shaders/PostProcess/Custom/SlowMotion.hlsl";

class ScreenEffectManagerComponent : public Script {
    FBZZ_SCRIPT(ScreenEffectManagerComponent)

public:
    FBZZ_GROUP("閃光")
    FBZZ_FIELD_COLOR(defaultFlashColor, (Vector4{ 1.0f, 0.25f, 0.20f, 1.0f }), "Flash Color")
    FBZZ_FIELD_RANGE(float, defaultFlashStrength, 0.45f, "Flash Strength", 0.0f, 1.0f)
    FBZZ_TOOLTIP("強さ 1.0 の要求で画面を覆う濃さ。1.0 にすると一瞬完全に染まる")
    FBZZ_FIELD_RANGE(float, defaultFlashSeconds, 0.22f, "閃光の長さ [秒]", 0.0f, 2.0f)

    // 被弾・溜め・危機の縁。エンジンの楕円ビネットではなく GameVignette.hlsl が描く
    // (方向・鼓動・縁取り・繊維を持つ。理由はシェーダーの冒頭)。
    // Surge (極の色で縁が光る側) だけは今までどおりエンジンのビネットに乗せる。
    FBZZ_GROUP("Vignette")
    FBZZ_FIELD_RANGE(float, vignetteIntensity, 0.55f, "強さ", 0.0f, 1.0f)
    FBZZ_TOOLTIP("被弾 (Distort) 強さ 1.0 で縁が落ちる暗さ")
    FBZZ_FIELD_RANGE(float, vignetteRadius, 0.62f, "半径", 0.2f, 1.2f)
    FBZZ_TOOLTIP("ここより内側は素通し。画面の高さの半分に対する比")
    FBZZ_FIELD_COLOR(vignetteRimColor, (Vector4{ 1.0f, 0.30f, 0.22f, 1.0f }), "Rim Color")
    FBZZ_TOOLTIP("暗い縁の内側に走る細い光の帯。極の＋ (純赤) と紛れないよう彩度を落としてある")
    FBZZ_FIELD_RANGE(float, vignetteLobe, 0.9f, "Direction Lobe", 0.0f, 1.0f)
    FBZZ_TOOLTIP("被弾した向きへ縁が張り出す量。0 で全周が均等")
    // 危機 (残り HP が Last Stand 以下) の鼓動。プレイヤー側が毎フレーム SetDanger で申告する。
    FBZZ_FIELD_RANGE(float, dangerDim, 0.42f, "Danger Dim", 0.0f, 1.0f)
    FBZZ_TOOLTIP("危機の間ずっと掛かる縁の暗さ。被弾の暗さより下に置く")
    FBZZ_FIELD_RANGE(float, dangerPulseHz, 1.4f, "Danger Pulse (Hz)", 0.2f, 5.0f)
    FBZZ_TOOLTIP("鼓動の速さ。速いほど焦る")

    FBZZ_GROUP("Chromatic Aberration")
    FBZZ_FIELD_RANGE(float, aberrationAmount, 0.008f, "Amount", 0.0f, 0.05f)
    FBZZ_TOOLTIP("強さ 1.0 の要求で入れる色収差の量")

    // WHY Flash / Distort と器を分けるか:
    //   被弾と発射はどちらも «画面の縁» を使うが、向きが逆でなければならない。被弾は
    //   画面を塗って縁を «暗く» し、発射は縁だけを極の色で «明るく» する。同じ器で
    //   強さだけ変えると、＋ (赤) を撃った瞬間と被弾が同じ絵になり、撃つたびに手が止まる。
    FBZZ_GROUP("サージ")
    FBZZ_FIELD_RANGE(float, surgeRim, 0.45f, "Rim", 0.0f, 1.0f)
    FBZZ_TOOLTIP("強さ 1.0 の要求で画面の縁を極の色に染める濃さ")
    FBZZ_FIELD_RANGE(float, surgeRimSmoothness, 0.6f, "Rim Smoothness", 0.0f, 1.0f)
    FBZZ_TOOLTIP("1 に近いほど内側まで色が入る。下げると額縁のように縁だけが光る")
    FBZZ_FIELD_RANGE(float, surgeAberration, 0.012f, "色収差", 0.0f, 0.05f)
    FBZZ_FIELD_RANGE(float, surgePull, 0.04f, "Lens Pull", 0.0f, 0.2f)
    FBZZ_TOOLTIP("画面が内側へ吸い込まれる量。外へ膨らませる方向には動かさない "
                 "(Composite が画面外の uv を黒で返すため四隅が欠ける)")

    // 起点へ画面ごと引き込む渦を Implode.hlsl が描く。
    //
    // WHY ビネットや Surge の «縁» ではなく画面の中を歪めるか:
    //   集束は盤面の 1 点で起きる出来事で、画面のどこで起きたかに意味がある。
    //   縁を光らせる表現は方向を持てないので、4 体を巻き込んだ大技も
    //   足元の 1 体も同じ絵になる。集束で画角を広げるのと同じ動機。
    FBZZ_GROUP("Implode (7.9)")
    FBZZ_FIELD_RANGE(float, implodePull, 0.045f, "Pull", 0.0f, 0.25f)
    FBZZ_TOOLTIP("強さ 1.0 のときに画面が起点へ寄る最大量。画面の «高さ» に対する比。"
                 "尾のぼけも同じ量で伸びる")
    FBZZ_FIELD_RANGE(float, implodeReach, 0.60f, "届く距離", 0.05f, 1.5f)
    FBZZ_TOOLTIP("渦が効く半径。画面の高さに対する比で、外側は素通しになる")
    FBZZ_FIELD_RANGE(float, implodeAttack, 0.06f, "攻撃", 0.0f, 0.5f)
    FBZZ_TOOLTIP("最大まで開くまでの時間。0 にすると 1 フレームで飛ぶ")

    // 12.6 の衝突。止まっている数フレームだけ画面を固める。
    FBZZ_GROUP("Freeze (12.6)")
    FBZZ_FIELD_RANGE(float, freezeFloor, 0.45f, "Floor", 0.0f, 1.0f)
    FBZZ_TOOLTIP("最も軽い当たりでの強さ。1 に近づけるほど、軽い接触も全力の激突も同じ絵になる")
    FBZZ_FIELD_RANGE(float, freezeLevels, 7.0f, "Levels", 2.0f, 32.0f)
    FBZZ_TOOLTIP("明るさを落とす段数。小さいほど硬い。色相は動かないので極の赤青は保たれる")
    FBZZ_FIELD_RANGE(float, freezeAberration, 2.4f, "色収差", 0.0f, 12.0f)
    FBZZ_TOOLTIP("画面の縁での色収差 [px]。中央は割らない")
    FBZZ_FIELD_RANGE(float, freezeContrast, 0.18f, "Contrast", 0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, freezeGrain, 0.045f, "Grain", 0.0f, 0.3f)
    FBZZ_TOOLTIP("止めている間だけ乗る粒。動かないので «時間が止まった» 側に働く")
    FBZZ_FIELD_RANGE(float, freezeRelease, 0.07f, "解放", 0.0f, 0.5f)
    FBZZ_TOOLTIP("止めが解けてから抜けきるまでの時間。掛かる側は常に 1 フレーム")

    // スロー (TimeManager の下位層) の «時間が伸びている» 見え方。止め (Freeze) とは別。
    //
    // WHY 要求を受けずにスローを覗くか (Freeze と同じ理由):
    //   回避・とどめ・演出、スローを掛ける場所は増える。掛けた側が絵も頼む形にすると、
    //   頼み忘れた場所だけ «世界は遅いのに画面は普段どおり» になる。深さを写すだけなら
    //   定義から一致する。
    FBZZ_GROUP("Slow Motion")
    FBZZ_FIELD(bool, slowLookEnabled, true, "有効")
    FBZZ_FIELD_RANGE(float, slowDesaturate, 0.55f, "Desaturate (edge)", 0.0f, 1.0f)
    FBZZ_TOOLTIP("縁の彩度をどれだけ抜くか。中央は保つ (極の赤青を数える場所)")
    FBZZ_FIELD_RANGE(float, slowStreak, 0.035f, "Radial Streak", 0.0f, 0.15f)
    FBZZ_TOOLTIP("縁の画素が外へ流れる長さ (画面比)。0.05 を超えると «ズーム» に見える")
    FBZZ_FIELD_RANGE(float, slowAberration, 3.0f, "Aberration (px)", 0.0f, 12.0f)
    FBZZ_FIELD_RANGE(float, slowDarken, 0.35f, "Darken (edge)", 0.0f, 1.0f)
    FBZZ_FIELD_COLOR(slowHeatColor, (Vector4{ 0.40f, 1.00f, 0.60f, 1.0f }), "Center Heat")
    FBZZ_TOOLTIP("中央にごく薄く乗る色。プレイヤー色。«自分の時間» の印")
    FBZZ_FIELD_RANGE(float, slowPulseHz, 1.2f, "Pulse (Hz)", 0.0f, 6.0f)
    FBZZ_FIELD_RANGE(float, slowGamma, 0.7f, "追従", 0.2f, 2.0f)
    FBZZ_TOOLTIP("深さ→強さの曲線。1 未満で浅いスローでも絵が出る")
    FBZZ_FIELD_READ_ONLY(float, debugSlow, 0.0f, "スロー")
    FBZZ_FIELD_READ_ONLY(float, debugWipe, 0.0f, "Wipe")

    // 極を帯びた対象へ掛かる放電の輪郭。誰を縁取るかは
    // 輪郭マスクへ申告するのは描く側で、ここは «縁取り方» だけを持つ。
    //
    // WHY 縁取るパスをここが載せるか:
    //   customEffects は «まるごと差し替え» の対象 (このファイルの WHY)。対象側が
    //   自分で AddCustom しても、次にこのマネージャーが書いた瞬間に消える。
    FBZZ_GROUP("BladeSide Outline")
    FBZZ_FIELD_RANGE(float, outlineWidth, 5.0f, "幅", 1.0f, 16.0f)
    FBZZ_TOOLTIP("輪郭の最大の太さ [px]。対象ごとの太さはこれに対する比で効く")
    FBZZ_FIELD_RANGE(float, outlineCrackle, 0.55f, "はぜる音", 0.0f, 1.0f)
    FBZZ_TOOLTIP("方向ごとに届く距離を揺らす量。0 で等幅の滑らかな輪郭")
    FBZZ_FIELD_RANGE(float, outlineSpeed, 6.0f, "速さ", 0.0f, 30.0f)
    FBZZ_TOOLTIP("明滅と放電の走る速さ [Hz]")
    FBZZ_FIELD_RANGE(float, outlineGain, 2.5f, "Gain", 0.0f, 8.0f)

    // 放電 (びりびり)。輪郭の «上へ» 細い筋を足す層で、帯そのものは削らない。
    //
    // WHY 削らないか: 電気で帯を欠けさせると、強くするほど輪郭が虫食いになって
    //     «どの脚が何極か» が読めなくなる。芯を成立させてから乗せる
    //     (Shaders/Material/Effects/WeaponTrail.hlsl で同じ結論に至っている)。
    FBZZ_FIELD_RANGE(float, outlineArcRate, 14.0f, "Arc Rate (Hz)", 0.0f, 60.0f)
    FBZZ_TOOLTIP("放電を引き直す速さ。速すぎると «放電» ではなく «画面のちらつき» になる。"
                 "2〜3 フレーム保つ 10〜20 あたりが電気に見える")
    FBZZ_FIELD_RANGE(float, outlineArcChance, 0.35f, "Arc Chance", 0.0f, 1.0f)
    FBZZ_TOOLTIP("同時に放電する方向の割合。1 に近づけると全周が伸びて «太い輪郭» に戻る")
    FBZZ_FIELD_RANGE(float, outlineArcReach, 1.9f, "Arc Reach", 1.0f, 4.0f)
    FBZZ_TOOLTIP("放電した方向がどこまで伸びるか。1 で伸びない")
    FBZZ_FIELD_RANGE(float, outlineArcSharp, 9.0f, "Arc Sharpness", 1.0f, 24.0f)
    FBZZ_TOOLTIP("筋の細さ。低いと帯が明るくなるだけで «線» に見えない")
    FBZZ_TOOLTIP("足す明るさ。HDR へ加算するので 1 を超えた分をブルームが拾う。"
                 "上げすぎると芯が白く飛んで極の色が読めなくなる")

    FBZZ_GROUP("デバッグ")
    FBZZ_FIELD_READ_ONLY(float, debugFade, 0.0f, "フェード")
    FBZZ_FIELD_READ_ONLY(float, debugFlash, 0.0f, "閃光")
    FBZZ_FIELD_READ_ONLY(float, debugDistortion, 0.0f, "歪み")
    FBZZ_FIELD_READ_ONLY(float, debugSurge, 0.0f, "サージ")
    FBZZ_FIELD_READ_ONLY(float, debugImplode, 0.0f, "Implode")
    FBZZ_FIELD_READ_ONLY(float, debugFreeze, 0.0f, "Freeze")
    FBZZ_FIELD_READ_ONLY(int, debugOutline, 0, "Outline")

    [[nodiscard]] static ScreenEffectManagerComponent* Instance() { return s_instance; }

    // --- フェード (シーン遷移が持ち続ける値。自動では減らない) ---
    void SetFade(float alpha, const Vector4& color = { 0.0f, 0.0f, 0.0f, 1.0f });
    void ClearFade() { m_fadeAlpha = 0.0f; }
    [[nodiscard]] float FadeAlpha() const { return m_fadeAlpha; }

    // --- フラッシュ (時間で自動的に消える) ---
    void Flash(float strength01);
    void Flash(const Vector4& color, float strength01, float seconds);

    // --- 歪み: ビネット + 色収差 (時間で自動的に消える) ---
    void Distort(float strength01, float seconds);
    // 解除するまで維持する版。スロー中に掛けっぱなしにする用途。
    void SetSustainedDistortion(float strength01) { m_sustainedDistortion = Clamp01(strength01); }
    /// 直近の被弾が «どこから» 来たか。Distort に向きを与える。
    /// 縁はこの点をカメラの右・上へ落とした角度へ張り出す。Distort が消えれば忘れる。
    void SetHurtSource(const Vector3& worldPoint)
    {
        m_hurtPoint = worldPoint;
        m_hurtValid = true;
    }
    /// 危機 (残り体力が僅か) を申告する。0 で解除。鼓動する縁が掛かり続ける。
    ///
    /// WHY «秒» ではなく «状態» で受けるか: 危機は出来事ではなく続く状態で、
    ///     解除されるのは回復か死のとき。申告側が毎フレーム書く形にすると、
    ///     解除の呼び忘れが «鼓動が止まらない» になる。値で持てば 0 を書けば消える。
    void SetDanger(float strength01) { m_danger = Clamp01(strength01); }

    // --- サージ: 縁が色付きで光る一撃 (時間で自動的に消える) ---
    // color は «何が起きたか» を表す色をそのまま渡す。
    void Surge(const Vector4& color, float strength01, float seconds);

    // --- 集束: 起点へ画面ごと引き込む (時間で自動的に消える) ---
    //
    // WHY UV ではなくワールド座標を受け取るか:
    //   効いている 0.3 秒の間、カメラは動き続けるし、起点になった敵自身も動く
    //   (Roller は転がり、ボスは歩く)。呼び出した瞬間の UV を握ると、渦だけが
    //   画面に貼り付いて盤面から剥がれる。毎フレーム投影し直せば、渦は «その場所»
    //   に留まり、カメラの後ろへ回った時点で自然に畳める。
    void Implode(const Vector3& worldPoint, float strength01, float seconds);

    // --- 輪郭: 誰かが輪郭マスクへ申告したことを知らせる (毎フレーム呼ぶ) ---
    //
    // WHY 強さも色も受け取らないか: 縁取る «相手» はマスクが持っていて、色も
    //     太さも対象ごとに違う。ここが受け取れるのは 1 組の値だけなので、
    //     受け取った時点で «最後に言った 1 体» の見た目に全員が揃ってしまう。
    //     こちらが要るのは「今フレーム誰か居るか」だけ。
    void KeepOutline() { m_outlineRequested = true; }

    void OnStart() override;
    void OnLateUpdate() override;
    void OnDestroy() override;

private:
    static inline ScreenEffectManagerComponent* s_instance = nullptr;

    // 効果が 1 つも無い状態の設定。ここから毎フレーム作り直す。
    void CaptureBase();

    /// 専用パスを 1 本、今フレームの設定へ載せる。
    ///
    /// 既定は «Composite の後で画面を作り直す» 従来のカスタムパス。stage / blendMode を
    /// 渡すと Composite の前 (HDR) で加算する «盤面の中で光るもの» にできる。
    static void PushCustomEffect(
        fbzz::renderer::PostProcessSettings& settings,
        const char* name, const char* shaderPath,
        float intensity, const Vector4& parameters,
        fbzz::renderer::CustomPassStage stage = fbzz::renderer::CustomPassStage::PostProcess,
        fbzz::renderer::BlendMode blendMode = fbzz::renderer::BlendMode::OPAQUE_BLEND,
        const Vector4& parameters2 = Vector4{});

    /// 集束の起点を今フレームの画面 UV へ落とす。カメラの後ろなら false。
    [[nodiscard]] bool ResolveImplodeUv(Vector2& outUv);
    /// 被弾の出どころを画面上の角度 [rad] へ落とす (右 = 0、上 = +π/2)。
    /// 出どころが無ければ false。カメラの後ろでも角度は取れる (縁の話なので)。
    [[nodiscard]] bool ResolveHurtAngle(float& outAngle);

    fbzz::renderer::PostProcessSettings m_base{};
    bool    m_hasBase = false;
    bool    m_writing = false;

    float   m_fadeAlpha = 0.0f;
    Vector4 m_fadeColor{ 0.0f, 0.0f, 0.0f, 1.0f };

    Vector4 m_flashColor{ 1.0f, 1.0f, 1.0f, 1.0f };
    float   m_flashStrength = 0.0f;
    float   m_flashSeconds  = 0.0f;
    float   m_flashRemaining = 0.0f;

    float   m_distortStrength  = 0.0f;
    float   m_distortSeconds   = 0.0f;
    float   m_distortRemaining = 0.0f;
    float   m_sustainedDistortion = 0.0f;

    Vector4 m_surgeColor{ 1.0f, 1.0f, 1.0f, 1.0f };
    float   m_surgeStrength  = 0.0f;
    float   m_surgeSeconds   = 0.0f;
    float   m_surgeRemaining = 0.0f;

    Vector3 m_implodePoint{ 0.0f, 0.0f, 0.0f };
    float   m_implodeStrength  = 0.0f;
    float   m_implodeSeconds   = 0.0f;
    float   m_implodeRemaining = 0.0f;

    // 止めは «掛かる» ではなく «掛かっている» 状態なので、要求ではなく現在値を持つ。
    float   m_freeze = 0.0f;

    // 被弾の出どころと、危機の申告。鼓動の時計は実時間で進める。
    Vector3 m_hurtPoint{ 0.0f, 0.0f, 0.0f };
    bool    m_hurtValid  = false;
    float   m_danger     = 0.0f;
    float   m_pulseClock = 0.0f;

    // 今フレーム輪郭マスクへ申告した対象が居たか。OnLateUpdate が読んで倒す。
    bool    m_outlineRequested = false;
};

FBZZ_REFLECT(ScreenEffectManagerComponent)

inline void ScreenEffectManagerComponent::OnStart()
{
    if (s_instance && s_instance != this) {
        debug.LogWarning("ScreenEffectManagerComponent: another instance is already active. "
                         "The most recently started one takes over.");
    }
    s_instance = this;

    m_hasBase = false;
    m_writing = false;
    m_fadeAlpha = 0.0f;
    m_flashRemaining = 0.0f;
    m_distortRemaining = 0.0f;
    m_sustainedDistortion = 0.0f;
    m_surgeRemaining = 0.0f;
    m_implodeRemaining = 0.0f;
    m_freeze = 0.0f;
    m_outlineRequested = false;
    m_hurtValid  = false;
    m_danger     = 0.0f;
    m_pulseClock = 0.0f;
}

inline void ScreenEffectManagerComponent::OnDestroy()
{
    // 効果を掛けたままシーンを抜けると、次のシーンが暗転や赤染めのまま始まる。
    if (m_writing) postprocess.Clear();
    if (s_instance == this) s_instance = nullptr;
}

inline void ScreenEffectManagerComponent::CaptureBase()
{
    if (m_hasBase) return;
    // 何も書いていない今の設定を基準にする。効果が終わればここへ戻る。
    m_base = postprocess.TryGet() ? *postprocess.TryGet() : fbzz::renderer::PostProcessSettings{};
    m_hasBase = true;
}

inline void ScreenEffectManagerComponent::SetFade(float alpha, const Vector4& color)
{
    CaptureBase();
    m_fadeAlpha = Clamp01(alpha);
    m_fadeColor = color;
}

inline void ScreenEffectManagerComponent::Flash(float strength01)
{
    Flash(defaultFlashColor, strength01 * defaultFlashStrength, defaultFlashSeconds);
}

inline void ScreenEffectManagerComponent::Flash(const Vector4& color, float strength01, float seconds)
{
    const float strength = Clamp01(strength01);
    if (strength <= 0.0f || seconds <= 0.0f) return;
    CaptureBase();

    // 強い方を採る。足すと連続被弾で画面が真っ赤のまま戻らなくなる。
    if (strength >= m_flashStrength || m_flashRemaining <= 0.0f) {
        m_flashColor    = color;
        m_flashStrength = strength;
        m_flashSeconds  = seconds;
    }
    m_flashRemaining = std::max(m_flashRemaining, seconds);
}

inline void ScreenEffectManagerComponent::Distort(float strength01, float seconds)
{
    const float strength = Clamp01(strength01);
    if (strength <= 0.0f || seconds <= 0.0f) return;
    CaptureBase();

    if (strength >= m_distortStrength || m_distortRemaining <= 0.0f) {
        m_distortStrength = strength;
        m_distortSeconds  = seconds;
    }
    m_distortRemaining = std::max(m_distortRemaining, seconds);
}

inline void ScreenEffectManagerComponent::Surge(const Vector4& color, float strength01,
                                                float seconds)
{
    const float strength = Clamp01(strength01);
    if (strength <= 0.0f || seconds <= 0.0f) return;
    CaptureBase();

    // WHY 同じ強さで来たら «先に立った色» を残すか: 左右の銃を同時に撃つと同じ
    //     フレームで 2 回呼ばれる。後勝ちにすると押し順で縁の色が入れ替わり、
    //     何を撃ったのかが縁から読めなくなる。赤と青を混ぜるのは 12.2 が禁じている
    //     (混ざった色は «第 3 の極» に見えてしまう) ので、先に立った側が消えるまで持たせる。
    if (strength > m_surgeStrength || m_surgeRemaining <= 0.0f) {
        m_surgeColor    = color;
        m_surgeStrength = strength;
        m_surgeSeconds  = seconds;
    }
    m_surgeRemaining = std::max(m_surgeRemaining, seconds);
}

inline void ScreenEffectManagerComponent::Implode(const Vector3& worldPoint, float strength01,
                                                  float seconds)
{
    const float strength = Clamp01(strength01);
    if (strength <= 0.0f || seconds <= 0.0f) return;
    CaptureBase();

    // WHY 弱い要求を «残り時間だけ伸ばす» 形で混ぜないか (Flash / Surge との違い):
    //   起点は 1 点しか持てない。中心を混ぜると誰も居ない場所が歪むし、残りだけ
    //   伸ばすと開き方を測っている経過時間が要求の長さと食い違って、伸ばした
    //   ぶんの渦が出なくなる。強い方が丸ごと勝ち、弱い方は爆発と音の側で拾わせる。
    if (m_implodeRemaining > 0.0f && strength < m_implodeStrength) return;

    // 同じ強さでも作り直す。より大きな集束が続けて起きたなら、開き直すのが正しい。
    m_implodePoint     = worldPoint;
    m_implodeStrength  = strength;
    m_implodeSeconds   = seconds;
    m_implodeRemaining = seconds;
}

inline void ScreenEffectManagerComponent::PushCustomEffect(
    fbzz::renderer::PostProcessSettings& settings, const char* name, const char* shaderPath,
    float intensity, const Vector4& parameters,
    fbzz::renderer::CustomPassStage stage, fbzz::renderer::BlendMode blendMode,
    const Vector4& parameters2)
{
    auto& custom = settings.customEffects.emplace_back();
    custom.name          = name;
    custom.enabled       = true;
    custom.shaderPath    = shaderPath;
    custom.intensity     = intensity;
    custom.blend         = 1.0f;
    custom.parameters[0] = parameters.x;
    custom.parameters[1] = parameters.y;
    custom.parameters[2] = parameters.z;
    custom.parameters[3] = parameters.w;
    custom.parameters[4] = parameters2.x;
    custom.parameters[5] = parameters2.y;
    custom.parameters[6] = parameters2.z;
    custom.parameters[7] = parameters2.w;
    custom.stage         = stage;
    custom.blendMode     = blendMode;
}

inline bool ScreenEffectManagerComponent::ResolveImplodeUv(Vector2& outUv)
{
    GameObject* cameraObject = scene.GetMainCameraObject();
    if (!cameraObject) return false;
    const auto* camera = scene.GetComponent<CameraComponent>(cameraObject);
    if (!camera) return false;
    return screenproj::WorldToUv(*cameraObject, *camera, m_implodePoint, outUv);
}

inline bool ScreenEffectManagerComponent::ResolveHurtAngle(float& outAngle)
{
    if (!m_hurtValid) return false;
    GameObject* cameraObject = scene.GetMainCameraObject();
    if (!cameraObject) return false;

    const Vector3 relative = m_hurtPoint - cameraObject->transform.worldPosition;
    const float x = Vector3::Dot(relative, cameraObject->transform.right);
    const float y = Vector3::Dot(relative, cameraObject->transform.up);
    // 真正面 (右にも上にも成分が無い) は向きを持たない。全周へ落とす。
    if (x * x + y * y < 1.0e-4f) return false;
    outAngle = std::atan2(y, x);
    return true;
}

inline void ScreenEffectManagerComponent::OnLateUpdate()
{
    // WHY 実時間か: 画面効果はヒットストップ中こそ見せたい。止めると被弾の赤が
    //     停止解除まで出ないので、当たった瞬間の情報が遅れて届く。
    const float dt = std::max(time.UnscaledDeltaTime(), 0.0f);

    // シーン遷移の扉は実時間で進める (スローやヒットストップの最中に切り替わっても止まらない)。
    transition::Tick(dt, scene);
    const float wipe = transition::Coverage();

    // スローの深さ。止め (Override) は含まない ─ あちらは Freeze が描く。
    float slow = 0.0f;
    if (slowLookEnabled)
        if (auto* timeManager = TimeManagerComponent::Instance())
            slow = std::pow(Clamp01(timeManager->Slow01()), std::max(slowGamma, 0.05f));

    float flash = 0.0f;
    if (m_flashRemaining > 0.0f && m_flashSeconds > EPSILON) {
        m_flashRemaining = std::max(0.0f, m_flashRemaining - dt);
        flash = m_flashStrength * Clamp01(m_flashRemaining / m_flashSeconds);
        if (m_flashRemaining <= 0.0f) m_flashStrength = 0.0f;
    }

    float distortion = m_sustainedDistortion;
    if (m_distortRemaining > 0.0f && m_distortSeconds > EPSILON) {
        m_distortRemaining = std::max(0.0f, m_distortRemaining - dt);
        distortion = std::max(distortion,
                              m_distortStrength * Clamp01(m_distortRemaining / m_distortSeconds));
        if (m_distortRemaining <= 0.0f) m_distortStrength = 0.0f;
    }
    // 被弾の向きは被弾の縁と一緒に消える。次の被弾が向きを持たなければ全周に戻る。
    if (m_distortRemaining <= 0.0f) m_hurtValid = false;

    // 危機の鼓動。心拍のように «ドッ» と縮んで、ゆっくり戻る (sin の 3 乗)。
    float pulse = 0.0f;
    if (m_danger > 0.0f) {
        m_pulseClock += dt * std::max(dangerPulseHz, 0.2f);
        if (m_pulseClock > 64.0f) m_pulseClock -= 64.0f;
        const float wave = 0.5f + 0.5f * std::sin(m_pulseClock * TWO_PI);
        pulse = wave * wave * wave * m_danger;
    } else {
        m_pulseClock = 0.0f;
    }

    float surge = 0.0f;
    if (m_surgeRemaining > 0.0f && m_surgeSeconds > EPSILON) {
        m_surgeRemaining = std::max(0.0f, m_surgeRemaining - dt);
        // WHY 二乗で落とすか: 放電は «立ち上がった瞬間が最大で、すぐ痩せる»。線形だと
        //     縁が居座り、長押し照射を切り替えるたびに前の残りへ次が乗って画面が
        //     染まったままになる。前半で一気に落とせば、点火の «パチッ» が絵に出る。
        const float t = Clamp01(m_surgeRemaining / m_surgeSeconds);
        surge = m_surgeStrength * t * t;
        if (m_surgeRemaining <= 0.0f) m_surgeStrength = 0.0f;
    }

    // 集束の渦。立ち上がりを短く、引きずるのを長く取る。
    //
    // WHY 立ち上がりを «残り» ではなく «経過» で測るか: 集束は、成立した瞬間に
    //     全員が動き出して、あとは飛んでいくだけの出来事。開き方は集束の長さに
    //     関係なく一定でないと、巻き込んだ数で «食い付き» が変わってしまう。
    float implode = 0.0f;
    Vector2 implodeUv{ 0.5f, 0.5f };
    if (m_implodeRemaining > 0.0f && m_implodeSeconds > EPSILON) {
        m_implodeRemaining = std::max(0.0f, m_implodeRemaining - dt);
        const float elapsed = m_implodeSeconds - m_implodeRemaining;
        const float attack  = implodeAttack <= EPSILON
            ? 1.0f
            : Clamp01(elapsed / std::max(implodeAttack, EPSILON));
        // WHY 減衰を «開ききった時点» から測るか: 全体の残り時間で測ると、開ききった
        //     瞬間には既に減衰が始まっていて、Inspector の最大値が画面に一度も出ない。
        const float decaySpan = std::max(m_implodeSeconds - implodeAttack, EPSILON);
        const float decay     = Clamp01(m_implodeRemaining / decaySpan);
        implode = m_implodeStrength * attack * decay * decay;
        if (m_implodeRemaining <= 0.0f) m_implodeStrength = 0.0f;

        // 起点がカメラの後ろへ回ったら渦も畳む。前を歪ませても、そこでは何も起きていない。
        if (implode > 0.0f && !ResolveImplodeUv(implodeUv)) implode = 0.0f;
    }

    // 止めの絵。掛かるのは 1 フレーム目、抜けるときだけ滑らかに (HitstopFreeze.hlsl の WHY)。
    //
    // WHY 要求を受けずに止めを覗くか: 止めと絵が別々の要求で動くと、Option で止めを
    //     短くしたときや、ImpactFeedbackManager を通さずに Request() した場所で、
    //     画面だけが残る / 画面だけが出ない、というずれ方をする。止めの «状態» を
    //     そのまま写せば、長さも有無も定義から一致する。
    float freezeTarget = 0.0f;
    if (auto* hitstop = HitstopManagerComponent::Instance(); hitstop && hitstop->IsActive())
        freezeTarget = Lerp(Clamp01(freezeFloor), 1.0f, hitstop->Weight01());

    if (freezeTarget >= m_freeze || freezeRelease <= EPSILON) {
        m_freeze = freezeTarget;
    } else {
        m_freeze = std::max(freezeTarget, m_freeze - dt / std::max(freezeRelease, EPSILON));
    }

    // 申告は毎フレーム来る前提なので、読んだ時点で倒す。倒さないと、最後の 1 体が
    // 消えた後も «誰か居る» が残って全画面パスが 1 本鳴り続ける。
    const bool outlineOn = m_outlineRequested;
    m_outlineRequested   = false;

    debugFade       = m_fadeAlpha;
    debugFlash      = flash;
    debugDistortion = distortion;
    debugSurge      = surge;
    debugImplode    = implode;
    debugFreeze     = m_freeze;
    debugOutline    = outlineOn ? 1 : 0;
    debugSlow       = slow;
    debugWipe       = wipe;

    const bool active = m_fadeAlpha > 0.0f || flash > 0.0f || distortion > 0.0f
                     || surge > 0.0f || implode > 0.0f || m_freeze > 0.0f || outlineOn
                     || m_danger > 0.0f || slow > 0.0f || wipe > 0.0f;
    if (!active) {
        // 何も掛かっていない間はランタイム上書きを外す。載せっぱなしにすると
        // シーンの PostProcessVolume が効かなくなる。
        if (m_writing) {
            postprocess.Clear();
            m_writing = false;
            m_hasBase = false;
        }
        return;
    }

    CaptureBase();
    fbzz::renderer::PostProcessSettings pp = m_base;

    // 縁は «暗く締める側» (被弾・スロー・危機) と «極の色で光る側» (照射) で器を分ける。
    // 暗く締める側は GameVignette.hlsl (下の専用パス) が描き、エンジンのビネットは
    // 光る側だけが使う。同じ器に畳んでいた頃は、被弾しながら撃つと片方が消えていた。
    const float rim = surgeRim * surge;
    if (rim > 0.0f) {
        pp.vignette.enabled    = true;
        pp.vignette.intensity  = std::max(pp.vignette.intensity, Clamp01(rim));
        pp.vignette.smoothness = surgeRimSmoothness;
        pp.vignette.color[0]   = m_surgeColor.x;
        pp.vignette.color[1]   = m_surgeColor.y;
        pp.vignette.color[2]   = m_surgeColor.z;
    }

    if (distortion > 0.0f || surge > 0.0f) {
        pp.lens.chromaticAberrationEnabled = true;
        pp.lens.chromaticAberration = std::max(pp.lens.chromaticAberration,
                                               std::max(aberrationAmount * distortion,
                                                        surgeAberration * surge));
    }

    if (surge > 0.0f) {
        // WHY 内側へしか歪めないか: 外へ膨らませると uv が画面外へ出て、Composite が
        //     そこを黒で返す (Composite.hlsl の PSMain 冒頭)。撃つたびに四隅が
        //     欠けることになる。引く向きなら常に画面の内側を舐めるので破綻しない。
        pp.lens.distortionEnabled = true;
        pp.lens.distortion -= surgePull * surge;
    }

    // フラッシュとフェードは同じ画面塗りの器を共有する。両方出ているときは、
    // 濃い方を採ったうえで色を混ぜる。遷移中の暗転がフラッシュで薄まると事故に見える。
    float  paintAlpha = m_fadeAlpha;
    Vector4 paintColor = m_fadeColor;
    if (flash > m_fadeAlpha) {
        paintAlpha = flash;
        paintColor = m_flashColor;
    } else if (flash > 0.0f && m_fadeAlpha > 0.0f) {
        const float t = flash / std::max(m_fadeAlpha, EPSILON);
        paintColor = m_fadeColor + (m_flashColor - m_fadeColor) * Clamp01(t) * 0.5f;
    }

    pp.screenFadeAlpha    = paintAlpha;
    pp.screenFadeColor[0] = paintColor.x;
    pp.screenFadeColor[1] = paintColor.y;
    pp.screenFadeColor[2] = paintColor.z;

    // 専用パスは «掛かっている間だけ» 載せる。
    //
    // WHY 常駐させて enabled で切らないか: 有効な効果 1 つにつき全画面が 1 パス増える。
    //     止めも集束も一瞬の出来事なので、出ていない間の 2 パスは丸ごと無駄になる。
    //
    // WHY 渦を先に置くか: チェーンは登録順に走る。渦は画素を «動かす» 変換、止めは
    //     動いた結果を «染める» 変換で、逆に並べると染めた色を渦が引き伸ばして
    //     階調の段が尾を引く。動かしてから染める方が、段が画面に対して素直に並ぶ。
    // 輪郭だけは SceneHDR 段 (Composite の前) で加算する。
    //
    // WHY 他の効果と段を分けるか:
    //   これは «画面の効果» ではなく «盤面の中で光っているもの»。トーンマップ後に
    //   足すと 1 で頭打ちになり、どれだけ Gain を上げてもブルームが拾わず、
    //   露出とも噛み合わない (明るい場所でも同じ濃さで浮く)。
    //
    // WHY 渦との前後を気にしなくてよくなるか:
    //   HDR 段で焼き込んでしまえば、後段の渦は «輪郭も含んだ画面» を歪める。
    //   LDR で並べていた頃のように、順番を間違えると輪郭だけ体から剥がれる、
    //   という関係が無くなる。
    if (outlineOn) {
        // PostProcess 段 (LDR)。エディタの選択輪郭と同じ «最後に描き直す» 経路で、
        // これが実際に画面へ出る唯一の形だった。
        //
        // WHY blendMode を渡さないか: LDR 段は blendMode を見ない。前段の画を t5 で
        //     受け取って全画面を描き直す実装なので、合成はシェーダーが自分で行う
        //     (Outline.hlsl の NOTE)。加算を指定しても効かない。
        PushCustomEffect(pp, kOutlineEffectName, kOutlineShaderPath, 1.0f,
                         { outlineWidth, outlineCrackle, outlineSpeed, outlineGain },
                         fbzz::renderer::CustomPassStage::PostProcess,
                         fbzz::renderer::BlendMode::OPAQUE_BLEND,
                         { outlineArcRate, outlineArcChance,
                           outlineArcReach, outlineArcSharp });
    }
    if (implode > 0.0f) {
        PushCustomEffect(pp, kImplodeEffectName, kImplodeShaderPath, implode,
                         { implodeUv.x, implodeUv.y, implodePull, implodeReach });
    }
    // スローの見え方。渦の後 (動かした結果を流す)、縁と止めの前 (流した上から締める)。
    if (slow > 0.0f) {
        PushCustomEffect(pp, kSlowEffectName, kSlowShaderPath, slow,
                         { slowDesaturate, slowStreak, slowAberration, slowDarken },
                         fbzz::renderer::CustomPassStage::PostProcess,
                         fbzz::renderer::BlendMode::OPAQUE_BLEND,
                         { slowHeatColor.x, slowHeatColor.y, slowHeatColor.z, slowPulseHz });
    }
    // 被弾・溜め・危機の縁。暗さは «被弾» と «危機» の強い方、向きは被弾だけが持つ。
    //
    // WHY 渦より後・止めより前か: 渦は画素を動かすので、縁を先に描くと縁ごと
    //     引き込まれて楕円が歪む。止めは «染める» だけなので、縁の上から掛かっても
    //     形は崩れない (むしろ止めの粒が縁にも乗って 1 枚の絵になる)。
    const float dim = std::max(vignetteIntensity * distortion, dangerDim * m_danger);
    if (dim > 0.0f) {
        float angle = -10.0f;
        if (distortion > 0.0f && !ResolveHurtAngle(angle)) angle = -10.0f;
        PushCustomEffect(pp, kVignetteEffectName, kVignetteShaderPath, 1.0f,
                         { Clamp01(dim), vignetteRadius, Clamp01(pulse), angle },
                         fbzz::renderer::CustomPassStage::PostProcess,
                         fbzz::renderer::BlendMode::OPAQUE_BLEND,
                         { vignetteRimColor.x, vignetteRimColor.y, vignetteRimColor.z,
                           Clamp01(vignetteLobe * distortion) });
    }
    if (m_freeze > 0.0f) {
        PushCustomEffect(pp, kFreezeEffectName, kFreezeShaderPath, m_freeze,
                         { freezeLevels, freezeAberration, freezeGrain, freezeContrast });
    }
    // 扉は最後。塗られた上に何かが乗ると «扉の向こうで何かが起きている» に見える。
    transition::PushWipe(pp);

    postprocess.Set(pp);
    m_writing = true;
}

} // namespace sandbox
