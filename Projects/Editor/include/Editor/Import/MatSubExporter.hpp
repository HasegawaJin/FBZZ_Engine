/// @file    MatSubExporter.hpp
/// @brief   FBX → .mat TOML + textures/ コピー。
/// @author  Hasegawa Jin
/// @date    2026-06-18
#pragma once
#include <Editor/Import/IFbxSubExporter.hpp>

namespace fbzz::editor {

class MatSubExporter final : public IFbxSubExporter {
public:
    [[nodiscard]] bool Export(FbxImportContext& ctx) override;
    const char* Name() const override { return "MatSubExporter"; }
};

} // namespace fbzz::editor
