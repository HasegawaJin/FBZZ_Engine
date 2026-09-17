/// @file    VFXBeamComponent.hpp
/// @brief   2 つの実体 (または任意のワールド 2 点) を結ぶビーム。経路だけを決める。
/// @author  Hasegawa Jin
/// @date    2026-08-26
///
/// @note 描画は既に TrailComponent が持つ (幅の先細り・色・マテリアル・UV スクロール・カメラ追従)。
///       このコンポーネントは同じ GameObject の TrailComponent へ毎フレーム経路を書くだけにする。
/// @note beamStart/beamEnd (手で置いた固定値) とは別に持つ。同じフィールドに「毎フレーム引き直す
///       今の位置」も持たせると、いつ上書きされるか読めなくなる。
#pragma once
#include <Engine/Scene/EntityRef.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/Vector3.hpp>

namespace fbzz::scene {

struct VFXBeamComponent {
    bool enabled = true;

    /// 始点・終点の実体。無効なら下の fallback 座標を使う。
    EntityRef fromEntity;
    EntityRef toEntity;

    /// 実体が無いときのワールド座標。片側だけ実体に繋ぐ使い方ができる
    /// (企画書 12.5 の「全員から起爆点へ」は、起爆点が実体を持たない)。
    math::Vector3 fromPoint = math::Vector3::ZERO;
    math::Vector3 toPoint   = { 0.0f, 0.0f, 5.0f };

    /// 端点へのワールド オフセット。足元ではなく体の中心から出したいときに使う。
    math::Vector3 fromOffset = math::Vector3::ZERO;
    math::Vector3 toOffset   = math::Vector3::ZERO;

    /// 経路の分割数。1 で直線。sag / jitter を効かせるには 8 以上が要る。
    int segments = 12;

    /// たるみ [m]。中点をこの距離だけ下げる。引力の線は張らずに «垂れる» 方が、
    /// 撃った線 (レーザー) と区別がつく。
    float sag = 0.0f;

    /// 横ゆれの振幅 [m]。電弧らしさ。0 で滑らかな曲線。
    float jitter = 0.0f;
    /// 横ゆれの時間周波数 [Hz]。
    float jitterFrequency = 12.0f;
    /// 横ゆれの空間周波数。線に沿って何山できるか。
    float jitterWaves = 3.0f;

    /// 端点の実体が消えたらビームを畳む。
    /// @note 既定 true: 敵が死んだ後も線が誰も居ない場所へ伸び続けると、盤面に無い関係を描くことになる。
    bool disableWhenEndpointMissing = true;

    /// @name ランタイム
    /// @{
    float phase = 0.0f;

    const char* GetTypeName() const { return "VFX Beam"; }

    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        r.Field("fromEntity", fromEntity);
        r.Field("toEntity", toEntity);
        r.Field("fromPoint", fromPoint);
        r.Field("toPoint", toPoint);
        r.Field("fromOffset", fromOffset);
        r.Field("toOffset", toOffset);
        r.IntRange("segments", segments, 1, 64);
        r.Field("sag", sag);
        r.Field("jitter", jitter);
        r.Field("jitterFrequency", jitterFrequency);
        r.Field("jitterWaves", jitterWaves);
        r.Field("disableWhenEndpointMissing", disableWhenEndpointMissing);
    }
    /// @}
};

} // namespace fbzz::scene
