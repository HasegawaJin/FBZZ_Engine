// FBZZ Engine
// MatSubExporter.hpp | fbzz::editor
// FBX → .mat TOML + textures/ コピー
#pragma once
#include <Editor/Import/IFbxSubExporter.hpp>

namespace fbzz::editor {

class MatSubExporter final : public IFbxSubExporter {
public:
    [[nodiscard]] bool Export(FbxImportContext& ctx) override;
    const char* Name() const override { return "MatSubExporter"; }
};

} // namespace fbzz::editor
