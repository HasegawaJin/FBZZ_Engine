// FBZZ Engine
// FzMatSubExporter.hpp | fbzz::editor
// FBX → .mat TOML + textures/ コピー
#pragma once
#include <Editor/Import/IFbxSubExporter.hpp>

namespace fbzz::editor {

class FzMatSubExporter final : public IFbxSubExporter {
public:
    [[nodiscard]] bool Export(FbxImportContext& ctx) override;
    const char* Name() const override { return "FzMatSubExporter"; }
};

} // namespace fbzz::editor
