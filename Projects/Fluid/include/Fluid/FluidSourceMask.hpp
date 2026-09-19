/// @file    FluidSourceMask.hpp
/// @brief   テクスチャ発生源 (FluidSourceShape::Texture) の濃さマスク。画像を 256×256 の 1 チャンネルへ落として持つ
/// @author  Hasegawa Jin
/// @date    2026-09-12
///
/// @note 固定の 256×256 に落とすのは、GPU が全部のマスクを 1 枚のアトラス (4×4 タイル) にまとめて
///       1 つの SRV で引くため。大きさが揃っていればタイル番号だけで場所が決まり、CPU と GPU で
///       同じ標本を引ける。縦横比は発生源の size.x / size.y が持つ。
/// @note 画像から中身を埋めるのは Engine の FluidSourceMaskLoader。Fluid は素のデータと引き方だけを持つ。
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace fbzz::fluid {

inline constexpr int kFluidSourceMaskSize = 256;
/// GPU のアトラスは kFluidSourceMaskAtlasColumns × kFluidSourceMaskAtlasColumns タイル (= 16 = 発生源の上限)。
inline constexpr int kFluidSourceMaskAtlasColumns = 4;

struct FluidSourceMask {
    /// kFluidSourceMaskSize² 個。行は上から下、値は [0,1] = 輝度 × α。空なら読めていない。
    std::vector<float> values;

    [[nodiscard]] bool IsValid() const
    {
        return values.size() == static_cast<std::size_t>(kFluidSourceMaskSize) * kFluidSourceMaskSize;
    }
};

/// @brief u, v ∈ [0,1] (v は上が 0) を双線形で引く。範囲外は縁の値。
/// @note 標本位置は GPU (線形・クランプのサンプラー) と同じテクセル中心規約:
///       x = u × size − 0.5 (y も同じ)、添字は [0, size − 1] に丸める。
/// @return 無効なマスクは 1 (画像が無くても板の形には湧く — «何も起きない» より原因に気付きやすい)。
[[nodiscard]] float SampleFluidSourceMask(const FluidSourceMask& mask, float u, float v);

/// @brief 発生源の texture 参照からマスクの中身を埋める差し込み口。
/// @note Fluid は画像もアセットデータベースも知らない。Engine
///       (Engine/Asset/FluidSourceMaskLoader.cpp) が起動時に本物を挿す。
using FluidSourceMaskResolver = bool (*)(const std::string& texture, FluidSourceMask& out);

/// @brief 差し込み口を差し替える。nullptr で «誰も埋めない» へ戻す。
/// @warning スレッドセーフではない。挿すのは起動時の 1 回だけ。
void SetFluidSourceMaskResolver(FluidSourceMaskResolver resolver);

/// @brief 差し込まれた読み手でマスクを埋める。
/// @return 読み手が居ない・読めなかったときは false (out は空)。
/// @note 空のマスクは SampleFluidSourceMask が 1 を返すので、板の形にそのまま湧く。
[[nodiscard]] bool ResolveFluidSourceMask(const std::string& texture, FluidSourceMask& out);

} // namespace fbzz::fluid
