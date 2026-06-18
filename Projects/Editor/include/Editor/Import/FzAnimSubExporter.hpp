// FBZZ Engine
// FzAnimSubExporter.hpp | fbzz::editor
// FBX → .anim バイナリ v2
#pragma once
#include <Editor/Import/IFbxSubExporter.hpp>

namespace fbzz::editor {

class FzAnimSubExporter final : public IFbxSubExporter {
public:
    [[nodiscard]] bool Export(FbxImportContext& ctx) override;
    const char* Name() const override { return "FzAnimSubExporter"; }
};

} // namespace fbzz::editor
