/// @file    AssetDragPayload.cpp
/// @brief   Hierarchy と Viewport のアセットパスドラッグ読み取り。
/// @author  Hasegawa Jin
/// @date    2026-10-10
#include <Editor/Asset/Browser/AssetDragPayload.hpp>
#include <imgui.h>

namespace fbzz::editor {

bool ReadAssetPayload(const ImGuiPayload* payload, std::string& outPath)
{
    if (!payload || payload->DataSize <= 0) {
        return false;
    }
    outPath.assign(static_cast<const char*>(payload->Data),
                   static_cast<size_t>(payload->DataSize - 1));
    return !outPath.empty();
}

} // namespace fbzz::editor
