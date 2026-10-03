/// @file    RuntimePackageValidation.hpp
/// @brief   描画ホスト EXE と同梱ランタイムの配布契約検証。
/// @author  Hasegawa Jin
/// @date    2026-10-03
#pragma once
#include <filesystem>
#include <string>
namespace fbzz::renderer {
/// @return 契約違反なら false とファイル名付き reason。DX12 無効ビルドでは true。
/// @note DLL をロードせず EXE データ export と x64 ファイル版を確認する。
[[nodiscard]] bool ValidateGraphicsRuntimePackage(const std::filesystem::path& exePath, std::string& reason);
}
