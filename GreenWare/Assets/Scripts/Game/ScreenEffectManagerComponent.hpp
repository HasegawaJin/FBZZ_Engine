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
/// WHY 専用シェーダー (PolarityImplode / HitstopFreeze) もここが持つか:
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
#include <Scripts/Utils/ScreenProjection.hpp>
#include <algorithm>

using namespace fbzz::scene;
using namespace fbzz::math;

namespace sandbox {

// 専用パスの名前とシェーダー。名前は customEffects の中の識別にしか使わない。
inline constexpr const char* kImplodeEffectName = "PolarityImplode";
inline constexpr const char* kImplodeShaderPath =
    "assets/shaders/PostProcess/Custom/PolarityImplode.hlsl";
inline constexpr const char* kFreezeEffectName = "HitstopFreeze";
inline constexpr const char* kFreezeShaderPath =
    "assets/shaders/PostProcess/Custom/HitstopFreeze.hlsl";
inline constexpr const char* kOutlineEffectName = "PolarityOutline";
inline constexpr const char* kOutlineShaderPath =
    "assets/shaders/PostProcess/Custom/PolarityOutline.hlsl";

class ScreenEffectManagerComponent : public Script {
    FBZZ_SCRIPT(ScreenEffectManagerComponent)

public:
    FBZZ_GROUP("Flash")
    FBZZ_FIELD_COLOR(defaultFlashColor, (Vector4{ 1.0f, 0.25f, 0.20f, 1.0f }), "Flash Color")
    FBZZ_FIELD_RANGE(float, defaultFlashStrength, 0.45f, "Flash Strength", 0.0f, 1.0f)
    FBZZ_TOOLTIP("強さ 1.0 の要求で画面を覆う濃さ。1.0 にすると一瞬完全に染まる")
    FBZZ_FIELD_RANGE(float, defaultFlashSeconds, 0.22f, "Flash Seconds", 0.0f, 2.0f)

    FBZZ_GROUP("Vignette")
    FBZZ_FIELD_RANGE(float, vignetteIntensity, 0.45f, "Intensity", 0.0f, 1.0f)
    FBZZ_TOOLTIP("スローや被弾中に足すビネットの濃さ")
    FBZZ_FIELD_RANGE(float, vignetteSmoothness, 0.5f, "Smoothness", 0.0f, 1.0f)

    FBZZ_GROUP("Chromatic Aberration")
    FBZZ_FIELD_RANGE(float, aberrationAmount, 0.008f, "Amount", 0.0f, 0.05f)
    FBZZ_TOOLTIP("強さ 1.0 の要求で入れる色収差の量")

    // WHY Flash / Distort と器を分けるか:
    //   被弾と発射はどちらも «画面の縁» を使うが、向きが逆でなければならない。被弾は
    //   画面を塗って縁を «暗く» し、発射は縁だけを極の色で «明るく» する。同じ器で
    //   強さだけ変えると、＋ (赤) を撃った瞬間と被弾が同じ絵になり、撃つたびに手が止まる。
    FBZZ_GROUP("Surge")
    FBZZ_FIELD_RANGE(float, surgeRim, 0.45f, "Rim", 0.0f, 1.0f)
    FBZZ_TOOLTIP("強さ 1.0 の要求で画面の縁を極の色に染める濃さ")
    FBZZ_FIELD_RANGE(float, surgeRimSmoothness, 0.6f, "Rim Smoothness", 0.0f, 1.0f)
    FBZZ_TOOLTIP("1 に近いほど内側まで色が入る。下げると額縁のように縁だけが光る")
    FBZZ_FIELD_RANGE(float, surgeAberration, 0.012f, "Aberration", 0.0f, 0.05f)
    FBZZ_FIELD_RANGE(float, surgePull, 0.04f, "Lens Pull", 0.0f, 0.2f)
    FBZZ_TOOLTIP("画面が内側へ吸い込まれる量。外へ膨らませる方向には動かさない "
                 "(Composite が画面外の uv を黒で返すため四隅が欠ける)")

    // 7.9 の集束。起点へ画面ごと引き込む渦を PolarityImplode.hlsl が描く。
    //
    // WHY ビネットや Surge の «縁» ではなく画面の中を歪めるか:
    //   集束は盤面の 1 点で起きる出来事で、画面のどこで起きたかに意味がある。
    //   縁を光らせる表現は方向を持てないので、4 体を巻き込んだ大技も
    //   足元の 1 体も同じ絵になる。集束で画角を広げるのと同じ動機。
    FBZZ_GROUP("Implode (7.9)")
    FBZZ_FIELD_RANGE(float, implodePull, 0.045f, "Pull", 0.0f, 0.25f)
    FBZZ_TOOLTIP("強さ 1.0 のときに画面が起点へ寄る最大量。画面の «高さ» に対する比。"
                 "尾のぼけも同じ量で伸びる")
    FBZZ_FIELD_RANGE(float, implodeReach, 0.60f, "Reach", 0.05f, 1.5f)
    FBZZ_TOOLTIP("渦が効く半径。画面の高さに対する比で、外側は素通しになる")
    FBZZ_FIELD_RANGE(float, implodeAttack, 0.06f, "Attack", 0.0f, 0.5f)
    FBZZ_TOOLTIP("最大まで開くまでの時間。0 にすると 1 フレームで飛ぶ")

    // 12.6 の衝突。止まっている数フレームだけ画面を固める。
    FBZZ_GROUP("Freeze (12.6)")
    FBZZ_FIELD_RANGE(float, freezeFloor, 0.45f, "Floor", 0.0f, 1.0f)
    FBZZ_TOOLTIP("最も軽い当たりでの強さ。1 に近づけるほど、軽い接触も全力の激突も同じ絵になる")
    FBZZ_FIELD_RANGE(float, freezeLevels, 7.0f, "Levels", 2.0f, 32.0f)
    FBZZ_TOOLTIP("明るさを落とす段数。小さいほど硬い。色相は動かないので極の赤青は保たれる")
    FBZZ_FIELD_RANGE(float, freezeAberration, 2.4f, "Aberration", 0.0f, 12.0f)
    FBZZ_TOOLTIP("画面の縁での色収差 [px]。中央は割らない")
    FBZZ_FIELD_RANGE(float, freezeContrast, 0.18f, "Contrast", 0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, freezeGrain, 0.045f, "Grain", 0.0f, 0.3f)
    FBZZ_TOOLTIP("止めている間だけ乗る粒。動かないので «時間が止まった» 側に働く")
    FBZZ_FIELD_RANGE(float, freezeRelease, 0.07f, "Release", 0.0f, 0.5f)
    FBZZ_TOOLTIP("止めが解けてから抜けきるまでの時間。掛かる側は常に 1 フレーム")

    // 極を帯びた対象へ掛かる放電の輪郭。誰を縁取るかは
    // PolarityTargetComponent が輪郭マスクへ申告し、ここは «縁取り方» だけを持つ。
    //
    // WHY 縁取るパスをここが載せるか:
    //   customEffects は «まるごと差し替え» の対象 (このファイルの WHY)。対象側が
    //   自分で AddCustom しても、次にこのマネージャーが書いた瞬間に消える。
    FBZZ_GROUP("Polarity Outline")
    FBZZ_FIELD_RANGE(float, outlineWidth, 5.0f, "Width", 1.0f, 16.0f)
    FBZZ_TOOLTIP("輪郭の最大の太さ [px]。対象ごとの太さはこれに対する比で効く")
    FBZZ_FIELD_RANGE(float, outlineCrackle, 0.55f, "Crackle", 0.0f, 1.0f)
    FBZZ_TOOLTIP("方向ごとに届く距離を揺らす量。0 で等幅の滑らかな輪郭")
    FBZZ_FIELD_RANGE(float, outlineSpeed, 6.0f, "Speed", 0.0f, 30.0f)
    FBZZ_TOOLTIP("明滅と放電の走る速さ [Hz]")
    FBZZ_FIELD_RANGE(float, outlineGain, 2.5f, "Gain", 0.0f, 8.0f)
    FBZZ_TOOLTIP("足す明るさ。HDR へ加算するので 1 を超えた分をブルームが拾う。"
                 "上げすぎると芯が白く飛んで極の色が読めなくなる")

    FBZZ_GROUP("Debug")
    FBZZ_FIELD_READ_ONLY(float, debugFade, 0.0f, "Fade")
    FBZZ_FIELD_READ_ONLY(float, debugFlash, 0.0f, "Flash")
    FBZZ_FIELD_READ_ONLY(float, debugDistortion, 0.0f, "Distortion")
    FBZZ_FIELD_READ_ONLY(float, debugSurge, 0.0f, "Surge")
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

    // --- サージ: 縁が色付きで光る一撃 (時間で自動的に消える) ---
    // color は «何が起きたか» を表す色をそのまま渡す (極性なら PolarityColor)。
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
        fbzz::renderer::BlendMode blendMode = fbzz::renderer::BlendMode::OPAQUE_BLEND);

    /// 集束の起点を今フレームの画面 UV へ落とす。カメラの後ろなら false。
    [[nodiscard]] bool ResolveImplodeUv(Vector2& outUv);

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
    fbzz::renderer::CustomPassStage stage, fbzz::renderer::BlendMode blendMode)
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

inline void ScreenEffectManagerComponent::OnLateUpdate()
{
    // WHY 実時間か: 画面効果はヒットストップ中こそ見せたい。止めると被弾の赤が
    //     停止解除まで出ないので、当たった瞬間の情報が遅れて届く。
    const float dt = std::max(time.UnscaledDeltaTime(), 0.0f);

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

    const bool active = m_fadeAlpha > 0.0f || flash > 0.0f || distortion > 0.0f
                     || surge > 0.0f || implode > 0.0f || m_freeze > 0.0f || outlineOn;
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

    // 縁は «暗く締める側» (被弾・スロー) と «極の色で光る側» (照射) が取り合う。
    // ビネットは色を 1 つしか持てないので、濃さで重み付けした 1 色へ畳む。
    // どちらか一方を優先すると、被弾しながら撃ったときに片方が丸ごと消える。
    const float dim = vignetteIntensity * distortion;
    const float rim = surgeRim * surge;
    if (dim > 0.0f || rim > 0.0f) {
        const float mix = rim / (dim + rim);
        pp.vignette.enabled    = true;
        pp.vignette.intensity  = std::max(pp.vignette.intensity, Clamp01(dim + rim));
        pp.vignette.smoothness = Lerp(vignetteSmoothness, surgeRimSmoothness, mix);
        pp.vignette.color[0]   = Lerp(pp.vignette.color[0], m_surgeColor.x, mix);
        pp.vignette.color[1]   = Lerp(pp.vignette.color[1], m_surgeColor.y, mix);
        pp.vignette.color[2]   = Lerp(pp.vignette.color[2], m_surgeColor.z, mix);
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
        PushCustomEffect(pp, kOutlineEffectName, kOutlineShaderPath, 1.0f,
                         { outlineWidth, outlineCrackle, outlineSpeed, outlineGain },
                         fbzz::renderer::CustomPassStage::SceneHDR,
                         fbzz::renderer::BlendMode::ADDITIVE);
    }
    if (implode > 0.0f) {
        PushCustomEffect(pp, kImplodeEffectName, kImplodeShaderPath, implode,
                         { implodeUv.x, implodeUv.y, implodePull, implodeReach });
    }
    if (m_freeze > 0.0f) {
        PushCustomEffect(pp, kFreezeEffectName, kFreezeShaderPath, m_freeze,
                         { freezeLevels, freezeAberration, freezeGrain, freezeContrast });
    }

    postprocess.Set(pp);
    m_writing = true;
}

} // namespace sandbox
