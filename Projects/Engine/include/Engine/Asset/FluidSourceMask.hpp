/// @file    FluidSourceMask.hpp
/// @brief   テクスチャ発生源 (FluidSourceShape::Texture) の濃さマスク。画像を 256×256 の 1 チャンネルへ落として持つ
/// @author  Hasegawa Jin
/// @date    2026-09-12
///
/// WHY 固定の 256×256 に落とすか: GPU は全部のマスクを 1 枚のアトラス (4×4 タイル) にまとめて 1 つの SRV で引く。
///     大きさが揃っていればタイル番号だけで場所が決まり、CPU と GPU で同じ標本を引ける。
///     縦横比は発生源の size.x / size.y が持つ。
#pragma once

#include <string>
#include <vector>

namespace fbzz::asset {

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

/// 画像 (PNG / TGA / JPG) を読み、kFluidSourceMaskSize² へ双線形で縮めて «輝度 × α» のマスクにする。
/// path は Assets 相対・guid:・実パスのどれでもよい。どのスレッドから呼んでもよい (WIC を使わない)。
/// Sprite 参照 ("<画像>::sprite::<ID または名前>") ならその 1 コマだけを切り抜いて縮める。
/// 失敗したら false (out は空のまま)。
[[nodiscard]] bool LoadFluidSourceMask(const std::string& path, FluidSourceMask& out,
                                       std::string* outError = nullptr);

/// u, v ∈ [0,1] (v は上が 0) を双線形で引く。範囲外は縁の値。
/// 標本位置は GPU (線形・クランプのサンプラー) と同じテクセル中心規約: x = u × size − 0.5 (y も同じ)、添字は [0, size − 1] に丸める。
/// 無効なマスクは 1 を返す (画像が無くても板の形には湧く — «何も起きない» より原因に気付きやすい)。
[[nodiscard]] float SampleFluidSourceMask(const FluidSourceMask& mask, float u, float v);

} // namespace fbzz::asset
