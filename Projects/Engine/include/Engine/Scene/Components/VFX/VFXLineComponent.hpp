/// @file    VFXLineComponent.hpp
/// @brief   雷・ビームの «線» のエフェクト。形 (本流と枝) を毎フレーム作り、自前の帯で描く
/// @author  Hasegawa Jin
/// @date    2026-09-12
///
///
/// @note VFXBeam+Trail は 2 点を結ぶ 1 本の折れ線しか張れず、帯もカメラへ CPU で向けるため Scene
/// @note ビューで紙に見える。ここは複数本の折れ線を 1 メッシュにまとめ、帯を向けるのは VS に
/// @note 任せる (VFXLine.hlsl) —— どのビューでも正対する。見た目の既定は VFXLine.mat、色・明るさ・
/// @note 時刻は VFXLineSystem が paramOverrides へ毎フレーム書く。
#pragma once
#include <Engine/Scene/References/EntityRef.hpp>
#include <Engine/Reflection/Reflection.hpp>
#include <Math/LinearAlgebra/Vector3.hpp>
#include <Math/LinearAlgebra/Vector4.hpp>

#include <cstdint>
#include <string>

namespace fbzz::scene {

enum class VFXLineMode : int { Lightning = 0, Beam = 1 };

/// @note Inspector と生成メニューが使う出発点。値は ApplyVFXLinePreset (VFXLineGeometry.hpp) が入れる。
enum class VFXLinePreset : int { Lightning = 0, ElectricArc, Laser, EnergyBeam, Tether, Count };

struct VFXLineComponent {
    bool enabled = true; ///< この線エフェクトの形状更新と描画を行う。
    VFXLineMode mode = VFXLineMode::Lightning; ///< 線の形状として分岐する雷または連続ビームを選ぶ。

    /// @name 端点
    /// @{
    /// @note 始点の実体。無効なら自分の位置。
    EntityRef fromEntity; ///< 線の始点を追従させるオブジェクト参照。未指定は所有者の位置。
    EntityRef toEntity; ///< 終点の実体。無効なら toPoint (自分から見たローカル座標 = 置いただけで線が見える)。
    math::Vector3 fromOffset = math::Vector3::ZERO; ///< 始点へのワールド オフセット (足元ではなく手や胸から出すとき)。
    math::Vector3 toPoint = { 0.0f, 0.0f, 5.0f }; ///< 終点オブジェクトを使わないときの、所有者ローカルの終点。
    bool disableWhenEndpointMissing = true; ///< 終点の実体が消えたら線を畳む (誰も居ない場所へ伸び続けないように)。
    /// @}

    /// @name 見た目
    /// @{
    std::string materialPath = "Assets/Materials/Effects/VFXLine.mat"; ///< 線を板として描画するシェーダーと材質の参照。
    math::Vector4 color = { 0.35f, 0.6f, 1.0f, 1.0f }; ///< グローの色 (リニア)。A は全体の不透明度。
    math::Vector4 coreColor = { 1.0f, 1.0f, 1.0f, 1.0f }; ///< 芯の色 (リニア HDR)。白熱させるなら 1 より大きく。
    float intensity = 4.0f; ///< 全体の明るさ (HDR)。ブルームが拾う。
    float width = 0.12f; ///< 帯の幅 [m]。
    float coreWidth = 0.25f; ///< 芯の太さ (帯の半幅に対する割合)。
    float endTaper = 0.3f; ///< 終点側の幅 (始点を 1 とした割合)。
    /// @}

    /// @name 雷
    /// @{
    /// @note 折れの細かさ。中点変位の段数で、本流は 2^detail 区間になる。
    int   detail = 6; ///< 雷の折れ線に中点を追加する反復段数。本流を 2 の detail 乗の区間に分ける。
    float chaos = 0.18f; ///< 折れの大きさ (線の長さに対する割合)。
    int   branchCount = 3; ///< 雷の本流から分ける枝の数。
    float branchLength = 0.35f; ///< 枝の長さ (本流の長さに対する割合)。
    float branchAngle = 35.0f; ///< 本流からの開き [度]。
    float branchWidth = 0.55f; ///< 枝の幅と明るさ (本流を 1 とした割合)。
    float branchBrightness = 0.6f; ///< 本流を基準として枝に適用する明るさの倍率。
    float strikeRate = 14.0f; ///< 形を作り直す回数 [回/秒]。0 で形を固定する。
    float flicker = 0.5f; ///< 明滅の強さ [0,1]。打ち直すたびに明るさが揺れ、打った直後が一番明るい。
    float jitter = 0.01f; ///< 打ち直しの間の震え (線の長さに対する割合)。
    /// @}

    /// @name ビーム
    /// @{
    int   beamSegments = 32; ///< ビームの曲線を折れ線に変換する際の区間数。
    float sag = 0.0f; ///< たるみ [m]。中点をこの距離だけ下げる (張った光線と «引っ張られる紐» の違い)。
    float wobble = 0.0f; ///< 横ゆれの振幅 [m]・時間周波数 [Hz]・線に沿った山の数。
    float wobbleFrequency = 3.0f; ///< ビームの横揺れが時間方向に繰り返す周波数（Hz）。
    float wobbleWaves = 2.0f; ///< ビームの端から端までの横揺れの波数。
    /// @}

    /// @name 帯の模様 (シェーダー)
    /// @{
    /// @note 芯の太さのムラ [0,1]。
    float breakup = 0.4f; ///< 線の芯の太さを場所によって変化させる割合（0〜1）。
    float pulseSpeed = 0.0f; ///< 帯を流れる輝点の速さと数。0 で流さない。
    float pulseDensity = 0.0f; ///< 線に沿って配置する光る模様の密度。
    /// @}

    /// @name 時間
    /// @{
    float fadeIn = 0.0f; ///< 再生開始後に線を表示強度まで立ち上げる時間（秒）。
    float duration = 0.0f; ///< 0 以下はずっと出し続ける。
    float fadeOut = 0.2f; ///< 生存時間の末尾で線を消すまでの時間（秒）。
    bool  loop = true; ///< 有限の生存時間を過ぎたときに先頭へ戻して繰り返す。
    int   seed = 1; ///< 雷の形状を打ち直す際の決定論的な乱数シード。
    /// @}

    /// @name ランタイム (保存しない)
    /// @{
    float time = 0.0f; ///< 線の時間変化に使う現在の再生時刻（秒）。
    float strikeClock = 0.0f; ///< 今の形を打ってからの経過 [秒]。
    std::uint32_t strikeIndex = 0; ///< 雷を打ち直すたびに進む形状の連番。乱数入力の一部。

    const char* GetTypeName() const { return "VFX Line"; }

    void Reflect(IReflector& r)
    {
        // Group / 表示条件は Inspector だけの話で、保存キーは常に同じ (モードを戻しても値は残る)。
        r.Field("enabled", enabled);
        int modeValue = static_cast<int>(mode);
        static constexpr const char* kModes[] = { "Lightning", "Beam" };
        r.Enum("mode", modeValue, kModes);
        mode = static_cast<VFXLineMode>(modeValue < 0 || modeValue > 1 ? 0 : modeValue);
        const bool lightning = mode == VFXLineMode::Lightning;

        r.Group("Endpoints");
        r.Field("fromEntity", fromEntity);
        r.Tooltip("始点の実体。空なら自分の位置から出ます。");
        r.Field("toEntity", toEntity);
        r.Tooltip("終点の実体。空なら To Point (自分から見たローカル座標) へ伸びます。");
        r.Field("fromOffset", fromOffset);
        r.Field("toPoint", toPoint);
        r.Field("disableWhenEndpointMissing", disableWhenEndpointMissing);

        r.Group("Look");
        r.FileField("materialPath", materialPath, ".mat");
        r.ColorField("color", color);
        r.ColorField("coreColor", coreColor);
        r.FloatRange("intensity", intensity, 0.0f, 50.0f);
        r.FloatRange("width", width, 0.001f, 5.0f);
        r.FloatRange("coreWidth", coreWidth, 0.0f, 1.0f);
        r.FloatRange("endTaper", endTaper, 0.0f, 1.0f);

        if (lightning) {
            r.Group("Lightning");
        }
        r.FieldIf("detail", detail, lightning, "折れの細かさ (中点変位の段数)。本流は 2^detail 区間になります。");
        r.FieldIf("chaos", chaos, lightning, "折れの大きさ。線の長さに対する割合です。");
        r.FieldIf("branchCount", branchCount, lightning);
        r.FieldIf("branchLength", branchLength, lightning, "枝の長さ。本流の長さに対する割合です。");
        r.FieldIf("branchAngle", branchAngle, lightning, "本流からの開き [度]。");
        r.FieldIf("branchWidth", branchWidth, lightning);
        r.FieldIf("branchBrightness", branchBrightness, lightning);
        r.FieldIf("strikeRate", strikeRate, lightning, "1 秒に形を何回打ち直すか。0 で形を固定します。");
        r.FieldIf("jitter", jitter, lightning, "打ち直しの間の震え。線の長さに対する割合です。");

        if (!lightning) {
            r.Group("Beam");
        }
        r.FieldIf("beamSegments", beamSegments, !lightning);
        r.FieldIf("sag", sag, !lightning, "たるみ [m]。張った光線と «引っ張られる紐» を分けます。");
        r.FieldIf("wobble", wobble, !lightning, "横ゆれの振幅 [m]。");
        r.FieldIf("wobbleFrequency", wobbleFrequency, !lightning);
        r.FieldIf("wobbleWaves", wobbleWaves, !lightning, "線に沿って何山できるか。");

        r.Group("Pattern");
        r.FloatRange("flicker", flicker, 0.0f, 1.0f);
        r.Tooltip("明滅の強さ。雷は打った直後が最も明るく、打つたびに強さが揺れます。");
        r.FloatRange("breakup", breakup, 0.0f, 1.0f);
        r.Tooltip("芯の太さのムラ。0 で均一な芯になります。");
        r.Field("pulseSpeed", pulseSpeed);
        r.Tooltip("帯を流れる輝点の速さ。負で逆向きに流れます。");
        r.Field("pulseDensity", pulseDensity);
        r.Tooltip("輝点の数。0 で流しません。");

        r.Group("Timing");
        r.Field("fadeIn", fadeIn);
        r.Field("duration", duration);
        r.Tooltip("0 以下はずっと出し続けます。");
        r.Field("fadeOut", fadeOut);
        r.Field("loop", loop);
        r.Field("seed", seed);
    }
    /// @}
};

}
