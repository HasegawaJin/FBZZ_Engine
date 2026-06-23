// FBZZ Engine
// AnimSubExporter.hpp | fbzz::editor
// FBX → .anim バイナリ v2
#pragma once
#include <Editor/Import/IFbxSubExporter.hpp>

namespace fbzz::editor {

class AnimSubExporter final : public IFbxSubExporter {
public:
    [[nodiscard]] bool Export(FbxImportContext& ctx) override;
    const char* Name() const override { return "AnimSubExporter"; }
};

} // namespace fbzz::editor
