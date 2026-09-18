/// @file    VectorFieldFile.hpp
/// @brief   速度場 PNG / .fga の読み書き。
/// @author  Hasegawa Jin
/// @date    2026-09-16
///
/// @note 格子そのものと標本化・ベイクは Fluid/VectorFieldAsset.hpp (FBZZFluid)。ここは入口だけ。
/// @see Docs/design/fluid-library.md
#pragma once

#include <Fluid/VectorFieldAsset.hpp>
#include <string>

namespace fbzz::asset {

/// @brief PNG + .meta を書く。PNG 以外の拡張子は拒否する。
/// @return 場が空、または書けなければ false。
/// @note TOML にしないのは 32³ でも 32768 ベクトルあるため。
[[nodiscard]] bool SaveVectorField(const std::string& absPath, const fluid::VectorFieldAsset& field);

/// @brief PNG + .meta を読む。PNG 以外の拡張子は拒否する。
/// @return 読めなければ false (outField は未変更)。
[[nodiscard]] bool LoadVectorFieldFile(const std::string& absPath, fluid::VectorFieldAsset& outField);

/// @brief PNG の .meta に速度場の格子情報があるか判定する。
[[nodiscard]] bool IsVectorFieldPng(const std::string& absPath);

/// @brief Unreal の .fga (ASCII) を取り込む。
/// @return 読めなければ false (outField は未変更)。
/// @note 既存の資産をそのまま持ち込めるようにするためだけの経路。
[[nodiscard]] bool ImportFgaFile(const std::string& absPath, fluid::VectorFieldAsset& outField);

} // namespace fbzz::asset
