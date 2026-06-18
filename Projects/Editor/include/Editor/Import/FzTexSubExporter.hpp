// FBZZ Engine
// FzTexSubExporter.hpp | fbzz::editor
// textures/ フォルダ内の生画像に .tex descriptor を自動生成する
// FzMatSubExporter の後に実行する。flipGreen=true の法線マップに対応する。
#pragma once
#include <Editor/Import/IFbxSubExporter.hpp>

namespace fbzz::editor {

class FzTexSubExporter final : public IFbxSubExporter {
public:
    [[nodiscard]] bool Export(FbxImportContext& ctx) override;
    const char* Name() const override { return "FzTexSubExporter"; }
};

} // namespace fbzz::editor
