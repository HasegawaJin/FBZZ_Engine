// FBZZ Engine
// ModelSubExporter.hpp | fbzz::editor
// FBX → .fzasset バイナリ (FZMD) + スケルトン埋め込み
#pragma once
#include <Editor/Import/IFbxSubExporter.hpp>

namespace fbzz::editor {

class ModelSubExporter final : public IFbxSubExporter {
public:
    [[nodiscard]] bool Export(FbxImportContext& ctx) override;
    const char* Name() const override { return "ModelSubExporter"; }
};

} // namespace fbzz::editor
