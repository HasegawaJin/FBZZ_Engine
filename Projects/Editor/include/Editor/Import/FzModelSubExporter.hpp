// FBZZ Engine
// FzModelSubExporter.hpp | fbzz::editor
// FBX → .model バイナリ (FZMD) + スケルトン埋め込み
#pragma once
#include <Editor/Import/IFbxSubExporter.hpp>

namespace fbzz::editor {

class FzModelSubExporter final : public IFbxSubExporter {
public:
    [[nodiscard]] bool Export(FbxImportContext& ctx) override;
    const char* Name() const override { return "FzModelSubExporter"; }
};

} // namespace fbzz::editor
