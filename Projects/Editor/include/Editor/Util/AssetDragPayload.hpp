/// @file    AssetDragPayload.hpp
/// @brief   アセットパスのドラッグデータ読み取り。
/// @author  Hasegawa Jin
/// @date    2026-10-10
#pragma once
#include <string>

struct ImGuiPayload;

namespace fbzz::editor {

/// @pre payload の Data は終端 NUL を含むアセットパスのバッファ。
/// @return 未受理なら false で outPath は未変更。空の受理データなら false で空文字列。
[[nodiscard]] bool ReadAssetPayload(const ImGuiPayload* payload, std::string& outPath);

} /// @note namespace fbzz::editor
