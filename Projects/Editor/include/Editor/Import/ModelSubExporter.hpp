/// @file    ModelSubExporter.hpp
/// @brief   FBX → .fzasset バイナリ (FZMD) + スケルトン埋め込み。
/// @author  Hasegawa Jin
/// @date    2026-06-18
#pragma once
#include <Editor/Import/IFbxSubExporter.hpp>

namespace fbzz::editor {

class ModelSubExporter final : public IFbxSubExporter {
public:
    [[nodiscard]] bool Export(FbxImportContext& ctx) override;
    const char* Name() const override { return "ModelSubExporter"; }
};

} // namespace fbzz::editor
