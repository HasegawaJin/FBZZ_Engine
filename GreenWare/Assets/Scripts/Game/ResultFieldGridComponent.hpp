/// @file    ResultFieldGridComponent.hpp
/// @brief   Result 画面の背景。Title と同じ磁場グリッドを、結果に合わせて鳴らす。
/// @author  Hasegawa Jin
/// @date    2026-08-30
///
/// @note 専用の .mat は作らない。FieldGrid.mat を Title 用と Result 用に割ると格子の
///       間隔も歪みの深さも 2 か所で管理することになるが、見た目の違いは «電極を
///       どう動かすか» と «どれだけ絞るか» だけなので per-instance の上書きで足りる。
/// @note 電極は ElectrodeRig から取らない。あれはカーソル追従の実体で、触れる的が
///       無いのにマウスで背景が動くと押せる物だと誤解させる。«居るふりをする» 2 極を
///       数式で回すだけにして盤面にオブジェクトを増やさない。
/// @note 勝敗で極を変える。CLEAR は＋と−が噛み合った状態、FAILED は片極だけが残って
///       場が閉じない状態にし、背景そのものが結果の説明になるようにする。
#pragma once

#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Script.hpp>
#include <Scripts/Game/GameResultState.hpp>
#include <Scripts/Utils/BladeColors.hpp>
#include <cmath>

using namespace fbzz::scene;
using namespace fbzz::math;

namespace sandbox {

/// FieldGrid.hlsl の cbuffer 名。MaterialPropertyId は名前を畳んだ ID なので、
/// 毎フレーム作り直さずここで 1 度だけ確定させる。
inline constexpr MaterialPropertyId kPZenithColor     { "zenithColor" };
inline constexpr MaterialPropertyId kPHorizonColor    { "horizonColor" };
inline constexpr MaterialPropertyId kPGridColor       { "gridColor" };
inline constexpr MaterialPropertyId kPMajorColor      { "majorColor" };
inline constexpr MaterialPropertyId kPPlusColor       { "plusColor" };
inline constexpr MaterialPropertyId kPMinusColor      { "minusColor" };
inline constexpr MaterialPropertyId kPCellSize        { "cellSize" };
inline constexpr MaterialPropertyId kPMajorEvery      { "majorEvery" };
inline constexpr MaterialPropertyId kPLineWidth       { "lineWidth" };
inline constexpr MaterialPropertyId kPMajorWidth      { "majorWidth" };
inline constexpr MaterialPropertyId kPFadeStart       { "fadeStart" };
inline constexpr MaterialPropertyId kPFadeEnd         { "fadeEnd" };
inline constexpr MaterialPropertyId kPHorizonThickness { "horizonThickness" };
inline constexpr MaterialPropertyId kPGroundHeight    { "groundHeight" };
inline constexpr MaterialPropertyId kPDither          { "dither" };
inline constexpr MaterialPropertyId kPWarpRadius      { "warpRadius" };
inline constexpr MaterialPropertyId kPWarpStrength    { "warpStrength" };
inline constexpr MaterialPropertyId kPHeightFalloff   { "heightFalloff" };
inline constexpr MaterialPropertyId kPPoolRadius      { "poolRadius" };
inline constexpr MaterialPropertyId kPPoolStrength    { "poolStrength" };
inline constexpr MaterialPropertyId kPRingSpacing     { "ringSpacing" };
inline constexpr MaterialPropertyId kPRingSpeed       { "ringSpeed" };
inline constexpr MaterialPropertyId kPRingSharpness   { "ringSharpness" };
inline constexpr MaterialPropertyId kPRingStrength    { "ringStrength" };
inline constexpr MaterialPropertyId kPPhase           { "phase" };
inline constexpr MaterialPropertyId kPIntensity       { "intensity" };
inline constexpr MaterialPropertyId kPElectrode0      { "electrode0" };
inline constexpr MaterialPropertyId kPElectrode1      { "electrode1" };
inline constexpr MaterialPropertyId kPElectrode2      { "electrode2" };
inline constexpr MaterialPropertyId kPElectrode3      { "electrode3" };

class ResultFieldGridComponent : public Script {
    FBZZ_SCRIPT(ResultFieldGridComponent)

public:
    FBZZ_GROUP("Field")
    FBZZ_FIELD(float, orbitRadius, 4.2f, "Orbit Radius")
    FBZZ_TOOLTIP("2 極が回る半径 (m)。大きくすると光の溜まりが画面の外へ逃げる")
    FBZZ_FIELD(float, orbitHeight, 1.35f, "Orbit Height")
    FBZZ_TOOLTIP("極の高さ。床から離すほど溜まりが広く淡くなる (FootprintSpread)")
    FBZZ_FIELD(float, orbitSeconds, 17.0f, "Orbit Seconds")
    FBZZ_TOOLTIP("＋が一周する秒数。−はこれの 1.38 倍で回るので、重なる位相が毎回ずれる")
    FBZZ_FIELD(float, clearIntensity, 0.62f, "Clear Intensity")
    FBZZ_FIELD(float, failIntensity,  0.34f, "Fail Intensity")
    FBZZ_TOOLTIP("負けたときは場が弱い。文字が主役なので、ここは上げすぎないこと")

    FBZZ_GROUP("デバッグ")
    FBZZ_FIELD_READ_ONLY(float, debugPulse, 0.0f, "脈動")

    /// @brief 文字が噛み合う瞬間に、場をひと突きする。ResultPresenter から呼ぶ。
    /// @note static にする。演出の «間» は文字側が持っており、時刻を両方に書くと
    ///       片方だけ直したときに必ずずれるため、呼ぶ側を 1 つにして同期を消す。
    static void Pulse(float strength = 1.0f);

    void OnStart() override;
    void OnDestroy() override;
    void OnUpdate() override;

private:
    static inline ResultFieldGridComponent* s_active = nullptr;

    float m_time  = 0.0f;
    float m_pulse = 0.0f;
};

FBZZ_REFLECT(ResultFieldGridComponent)

inline void ResultFieldGridComponent::Pulse(float strength)
{
    if (s_active) s_active->m_pulse = strength;
}

inline void ResultFieldGridComponent::OnStart()
{
    s_active = this;
    m_time   = 0.0f;
    m_pulse  = 0.0f;
}

inline void ResultFieldGridComponent::OnDestroy()
{
    /// @note 自分が現役のときだけ降ろす。ホットリロードで新旧が一瞬同居しても、
    ///       後から来た方の登録を古い方の破棄が消してしまわない。
    if (s_active == this) s_active = nullptr;
}

inline void ResultFieldGridComponent::OnUpdate()
{
    /// @note ドームは «画面を埋めるための代理» なので、中心をカメラに合わせるだけでよい
    ///       (FieldGrid.hlsl は視線と床平面の交点から絵を作る)。
    if (GameObject* cam = scene.FindWithTag("MainCamera", true))
        transform.position = cam->transform.worldPosition;

    const MaterialInstance mat = material.Instance();
    if (!mat.IsValid()) return;

    const float dt = time.UnscaledDeltaTime();
    m_time += dt;
    /// @note 突きは 1.1 秒で抜ける。指数減衰にすると «鳴った» 感じが頭にだけ残る。
    m_pulse -= m_pulse * (dt * 3.4f);
    if (m_pulse < 1.0e-4f) m_pulse = 0.0f;
    debugPulse = m_pulse;

    const bool  win  = GameResultState::victory;
    /// @note 出だしの 1.2 秒でゆっくり立ち上げる。最初のフレームから満照度だと、
    ///       文字のフェードインより先に背景が完成してしまい、順番が逆に見える。
    const float warm = m_time < 1.2f ? (m_time / 1.2f) * (m_time / 1.2f) : 1.0f;

    /// @name 色
    /// @note 無彩色の格子。Result の地色 (#050608) より少しだけ上に置く。
    mat.SetVector4(kPZenithColor, Vector4{ 0.0196f, 0.0235f, 0.0314f, 1.0f });
    mat.SetVector4(kPHorizonColor, Vector4{ 0.042f, 0.048f, 0.060f, 1.0f });
    mat.SetVector4(kPGridColor, Vector4{ 0.062f, 0.070f, 0.086f, 1.0f });
    mat.SetVector4(kPMajorColor, Vector4{ 0.135f, 0.150f, 0.175f, 1.0f });
    mat.SetVector4(kPPlusColor, kColorRight);
    mat.SetVector4(kPMinusColor, kColorLeft);

    /// @name 格子
    /// @note Title より 1 段細かく、遠くで早く消す。文字の裏に線が残ると読みが落ちる。
    mat.SetFloat(kPCellSize, 1.6f);
    mat.SetFloat(kPMajorEvery, 4.0f);
    mat.SetFloat(kPLineWidth, 0.9f);
    mat.SetFloat(kPMajorWidth, 1.5f);
    mat.SetFloat(kPFadeStart, 9.0f);
    mat.SetFloat(kPFadeEnd, 34.0f);
    mat.SetFloat(kPHorizonThickness, 0.030f);
    mat.SetFloat(kPGroundHeight, 0.0f);
    mat.SetFloat(kPDither, 0.004f);

    /// @name 歪みと光
    const float pulse = m_pulse;
    mat.SetFloat(kPWarpRadius, 4.0f);
    mat.SetFloat(kPWarpStrength, (win ? 1.35f : 0.75f) * (1.0f + pulse * 0.9f));
    mat.SetFloat(kPHeightFalloff, 3.0f);
    mat.SetFloat(kPPoolRadius, 2.4f);
    mat.SetFloat(kPPoolStrength, (win ? 0.42f : 0.26f) * (1.0f + pulse * 1.6f));
    mat.SetFloat(kPRingSpacing, 3.2f);
    mat.SetFloat(kPRingSpeed, win ? 1.5f : 0.7f);
    mat.SetFloat(kPRingSharpness, 14.0f);
    mat.SetFloat(kPRingStrength, (win ? 0.24f : 0.13f) * (1.0f + pulse * 2.2f));

    mat.SetFloat(kPPhase, m_time);
    mat.SetFloat(kPIntensity,
                 (win ? clearIntensity : failIntensity) * warm * (1.0f + pulse * 0.35f));

    /// @name 2 極
    /// @note ＋はキャラクター側 (画面右)、−は文字側 (画面左) を基準にして、
    ///       逆回りでゆっくりすれ違う。同じ向きに回すと «並んで移動する» ように見えて、
    ///       引き合っている感じが出ない。
    const float period = orbitSeconds > 0.1f ? orbitSeconds : 0.1f;
    const float aPlus  = (m_time / period) * 6.2831853f;
    const float aMinus = -(m_time / (period * 1.38f)) * 6.2831853f + 2.4f;

    /// @note キャラクターの少し奥
    const Vector3 hub{ 0.6f, 0.0f, 1.5f };
    const Vector3 plus {
        hub.x + std::cos(aPlus) * orbitRadius,
        orbitHeight + std::sin(aPlus * 0.7f) * 0.35f,
        hub.z + std::sin(aPlus) * orbitRadius * 0.75f,
    };
    const Vector3 minus {
        hub.x + std::cos(aMinus) * orbitRadius * 1.15f,
        orbitHeight + std::sin(aMinus * 0.9f) * 0.35f,
        hub.z + std::sin(aMinus) * orbitRadius * 0.7f,
    };

    /// @note w は極性と «有効か»。負けたときは＋を落として、場が閉じない絵にする。
    mat.SetVector4(kPElectrode0,
                   Vector4{ plus.x,  plus.y,  plus.z,  win ? 1.0f : 0.0f });
    mat.SetVector4(kPElectrode1,
                   Vector4{ minus.x, minus.y, minus.z, -1.0f });
    mat.SetVector4(kPElectrode2, Vector4{ 0.0f, 0.0f, 0.0f, 0.0f });
    mat.SetVector4(kPElectrode3, Vector4{ 0.0f, 0.0f, 0.0f, 0.0f });
}

} // namespace sandbox
