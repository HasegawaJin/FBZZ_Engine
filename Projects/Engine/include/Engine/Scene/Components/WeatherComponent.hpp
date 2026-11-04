/// @file    WeatherComponent.hpp
/// @brief   シーン全体の天候 (降雨量と路面の濡れ) を持つコンポーネント
/// @author  Hasegawa Jin
/// @date    2026-08-25
///
/// @note EnvironmentLightComponent と同じ「シーンに 1 つ」パターン。
/// @note 複数ある場合は最初の有効な 1 つを使う。
#pragma once
#include <Engine/Scene/Reflection.hpp>

namespace fbzz::scene {

struct WeatherComponent {
    bool enabled = true;

    /// @note 降雨量 [0, 1]。雨パーティクルの発生量と、下の wetness の到達点を決める。
    float rainIntensity = 0.0f;

    /// @note rainIntensity から wetness を時間積分するか。
    /// @note 雨が降った «あと» に地面が濡れるのが天候で、雨脚と濡れが同じフレームで立ち上がると
    /// @note 書き割りに見える。手付けでカーブを作るときは切って wetness を直接書く。
    bool autoWetness = true;

    /// @note 乾いた状態から rainIntensity ぶんまで濡れるのに要する秒数。
    float wetDuration = 8.0f;
    /// @note 雨が止んでから乾ききるまでの秒数。乾く方が遅いのが自然。
    float dryDuration = 30.0f;

    /// @note 濡れ量 [0, 1]。autoWetness が有効な間は WeatherSystem が毎フレーム書く。
    float wetness = 0.0f;

    /// @note 濡れによる albedo の減衰 [0, 1]。多孔質な表面が水を吸って暗くなる量。
    float darkening = 0.55f;

    /// @note 水平な面に水が溜まる量 [0, 1]。上向きの面ほど強く効く。
    float puddleAmount = 0.6f;

    /// @note rainIntensity = 1 のときの雨粒の発生量 [個/秒]。WeatherSystem がこの GameObject と
    /// @note その子の ParticleEmitter へ rainEmitRate * rainIntensity を毎フレーム書く。
    /// @note emitRate を倍率で触ると元の値の退避が要り、Play/Stop や DLL リロードを挟むと
    /// @note «だんだん弱くなる雨» になる。正本をここ 1 か所にすれば同じ値へ落ち着く。
    float rainEmitRate = 4000.0f;

    const char* GetTypeName() const { return "Weather"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);

        r.Group("Rain");
        r.FloatRange("rainIntensity", rainIntensity, 0.0f, 1.0f);
        r.Tooltip("降雨量。0 では雨も濡れも出ない。\n"
                  "この GameObject と子の Particle Emitter の発生量を決める");
        r.Field("rainEmitRate", rainEmitRate);
        r.Tooltip("rainIntensity = 1 のときの雨粒の発生量 [個/秒]");

        r.Group("Wetness");
        r.Field("autoWetness", autoWetness);
        r.Tooltip("rainIntensity から wetness を時間積分する。\n"
                  "手付けでカーブを作るときは切る");
        r.Field("wetDuration", wetDuration);
        r.Field("dryDuration", dryDuration);
        r.FloatRange("wetness", wetness, 0.0f, 1.0f);
        r.Tooltip("autoWetness が有効な間は毎フレーム上書きされる");
        r.FloatRange("darkening", darkening, 0.0f, 1.0f);
        r.FloatRange("puddleAmount", puddleAmount, 0.0f, 1.0f);
    }
};

} /// @note namespace fbzz::scene
