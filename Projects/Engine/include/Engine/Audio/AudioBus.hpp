/// @file    AudioBus.hpp
/// @brief   ミキサーバスの定義と既定構成。
/// @author  Hasegawa Jin
/// @date    2026-08-23
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace fbzz::audio {

/// バスグラフ内の位置。ApplyBusLayout に渡した配列の添字と一致する。
using BusIndex = uint16_t;

inline constexpr BusIndex kInvalidBus = 0xFFFF;
/// 最終出力。バス配列の先頭は常に Master。
inline constexpr BusIndex kMasterBus = 0;

inline constexpr const char* kMasterBusName = "Master";

/// 1 本のバス。parent が空なら Master 直下 (Master 自身も空)。
struct BusDesc {
    std::string name;
    std::string parent;
    float       volume        = 1.0f;
    /// 0-1 正規化カットオフ。1 で無加工。水中・気絶などの一括加工に使う。
    float       lowPassCutoff = 1.0f;
    /// AudioReverbZone の残響をこのバスへ掛けるか。
    /// WHY 全バスに載せないか: 残響は submix ごとの実 DSP なので、載せた本数ぶん
    ///     CPU を使う。BGM や UI に環境残響が乗るのも音楽的に正しくない。
    bool        reverb        = false;
};

/// ProjectSettings が何も定義していないときの構成。
/// Master / BGM / SE / UI / Voice の 5 本で、Master 以外は Master 直下。
[[nodiscard]] std::vector<BusDesc> DefaultBusLayout();

/// descs を「親が必ず子より前」に並べ替え、Master を先頭へ寄せる。
/// 親が見つからない・循環しているバスは Master 直下として扱う。
/// @ret 並べ替え後の配列。名前が重複するバスは後勝ちで 1 本に畳む。
[[nodiscard]] std::vector<BusDesc> NormalizeBusLayout(const std::vector<BusDesc>& descs);

} // namespace fbzz::audio
