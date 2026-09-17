/// @file    FluidSourceMaskLoader.hpp
/// @brief   画像 (PNG / TGA / JPG) から流体のテクスチャ発生源マスクを埋める。
/// @author  Hasegawa Jin
/// @date    2026-09-16
///
/// @note 素のデータと引き方は Fluid/FluidSourceMask.hpp (FBZZFluid)。ここは «入口» だけを持つ。
/// @see Docs/design/fluid-library.md
#pragma once

#include <Fluid/FluidSourceMask.hpp>
#include <string>

namespace fbzz::asset {

/// @brief 画像を読み、kFluidSourceMaskSize² へ縮めて «輝度 × α» のマスクにする。
/// @param path Assets 相対・guid:・実パスのどれでもよい。Sprite 参照
///             ("<画像>::sprite::<ID または名前>") ならその 1 コマだけを切り抜いて縮める。
/// @return 失敗したら false (out は空のまま)。理由は outError へ。
/// @note どのスレッドから呼んでもよい (WIC を使わない)。
[[nodiscard]] bool LoadFluidSourceMask(const std::string& path, fluid::FluidSourceMask& out,
                                       std::string* outError = nullptr);

} // namespace fbzz::asset
