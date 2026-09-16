/// @file    AnimSubExporter.hpp
/// @brief   FBX → .anim バイナリ v3。
/// @author  Hasegawa Jin
/// @date    2026-06-18
#pragma once
#include <Editor/Import/IFbxSubExporter.hpp>

namespace fbzz::editor {

class AnimSubExporter final : public IFbxSubExporter {
public:
    [[nodiscard]] bool Export(FbxImportContext& ctx) override;
    const char* Name() const override { return "AnimSubExporter"; }
};

} // namespace fbzz::editor
