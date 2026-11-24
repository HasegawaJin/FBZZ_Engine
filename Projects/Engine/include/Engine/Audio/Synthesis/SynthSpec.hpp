/// @file    SynthSpec.hpp
/// @brief   手続き効果音の合成パラメーター。
/// @author  Hasegawa Jin
/// @date    2026-08-23
#pragma once
#include <cstdint>

namespace fbzz::audio {

/// 基本波形。Noise は startFrequency を「ノイズの粗さ」として使う。
enum class SynthWave : uint8_t {
    Sine,
    Square,
    Saw,
    Triangle,
    Noise,
};

/// 効果音 1 つを決めるパラメーター一式。
///
/// このヘッダーは意図的に依存ゼロ (`<cstdint>` のみ) に保つ。
/// @note ScriptProxy ヘッダーから include するため。Engine 実装型を引き込むとスクリプト DLL がエンジン内部へ直接依存することになる。
///
/// 同じ SynthSpec からは常に同じ PCM が出る (Noise も seed で再現する)。
/// 値域外は Synth::Render 側で丸めるため、呼び出し側での事前チェックは不要。
struct SynthSpec {
    SynthWave wave = SynthWave::Square;

    /// @name エンベロープ (秒)
    /// 総再生長 = attack + sustain + decay。
    ///@{
    float attack  = 0.0f;
    float sustain = 0.10f;
    float decay   = 0.20f;
    /// sustain 開始直後の音量ブースト率 (0-1)。sustain の終わりで 1 倍へ戻る。
    float punch   = 0.0f;
    ///@}

    /// @name 音程
    ///@{
    float startFrequency = 440.0f;
    /// スライドの下限 (Hz)。0 で下限なし。
    float minFrequency   = 0.0f;
    /// 1 秒あたりのオクターブ変化。負で下降。
    float slide          = 0.0f;
    /// 0-1。周波数に対する揺れ幅。
    float vibratoDepth   = 0.0f;
    float vibratoRate    = 0.0f;
    ///@}

    /// @name 音色
    ///@{
    /// Square のみ有効。0-1。
    float dutyCycle      = 0.5f;
    /// 1 秒あたりの dutyCycle 変化。
    float dutySweep      = 0.0f;
    /// 0-1 正規化カットオフ。1 で無加工。
    float lowPassCutoff  = 1.0f;
    /// 1 秒あたりの lowPassCutoff 変化。
    float lowPassSweep   = 0.0f;
    /// 0-1 正規化カットオフ。0 で無加工。
    float highPassCutoff = 0.0f;
    /// 0 で無加工、1 に近いほど粗く量子化する。
    float bitCrush       = 0.0f;
    ///@}

    /// @name 反復・アルペジオ
    /// コイン取得やパワーアップの「ピロン」を作る。
    ///@{
    /// 発振器を作り直す頻度 (Hz)。0 で反復なし。
    float repeatRate  = 0.0f;
    /// 反復ごとに基準周波数へ掛ける倍率。0 または 1 で無変化。
    float arpeggioMod = 0.0f;
    ///@}

    /// @name 出力
    ///@{
    float    amplitude  = 0.5f;
    /// Noise の再現性を決める。0 でも決定論的 (既定シードを使う)。
    uint32_t seed       = 0;
    /// 0 なら 44100 を使う。
    uint32_t sampleRate = 0;
    ///@}
};

/// sfxr 由来の定番プリセット。
enum class SynthPreset : uint8_t {
    Pickup,
    Laser,
    Explosion,
    PowerUp,
    Hit,
    Jump,
    Blip,
    Count,
};

} // namespace fbzz::audio
