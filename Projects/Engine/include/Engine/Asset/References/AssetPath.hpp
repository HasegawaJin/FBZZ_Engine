/// @file    AssetPath.hpp
/// @brief   ロード・描画型を公開しないアセット参照のパス解決。
/// @author  Hasegawa Jin
/// @date    2026-10-30
#pragma once
#include <Engine/Api.hpp>
#include <string>

namespace fbzz::asset {
/// @note GUID・プロジェクト・Engine の探索順と未存在時の結果は AssetManager と共通。
[[nodiscard]] FBZZ_ENGINE_API std::string ResolveAssetPath(const std::string& path);
}
