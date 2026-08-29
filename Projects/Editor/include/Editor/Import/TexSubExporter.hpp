/// @file    TexSubExporter.hpp
/// @brief   textures/ フォルダ内の生画像に .meta sidecar を自動生成する。
/// @author  Hasegawa Jin
/// @date    2026-06-18
///
/// MatSubExporter の後に実行する。flipGreen=true の法線マップに対応する。
#pragma once
#include <Editor/Import/IFbxSubExporter.hpp>

namespace fbzz::editor {

class TexSubExporter final : public IFbxSubExporter {
public:
    [[nodiscard]] bool Export(FbxImportContext& ctx) override;
    const char* Name() const override { return "TexSubExporter"; }
};

} // namespace fbzz::editor
