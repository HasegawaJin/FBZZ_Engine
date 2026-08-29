/// @file    IPreviewPanel.hpp
/// @brief   アセット種別ごとのプレビューパネルが共通して実装する描画契約。
/// @author  Hasegawa Jin
/// @date    2026-08-19
///
/// WHY: Animation / Material / VFX を 1 枚のパネルへ集約すると、アセット種別の追加たびに
/// IPanel のライフサイクルや Inspector の呼び出し規約まで変更する必要がある。
/// 種別判定と描画だけを小さなインターフェースへ切り出し、PreviewPanel はルーターとして保つ。
#pragma once

#include <string_view>

namespace fbzz::editor {

struct EditorContext;

// 共通 Preview の描画契約。
// WHAT: 拡張子の対応可否と、指定領域への 1 フレーム分の描画を抽象化する。
class IPreviewPanel {
public:
    virtual ~IPreviewPanel() = default;

    [[nodiscard]] virtual const char* GetPreviewName() const = 0;
    [[nodiscard]] virtual bool Supports(std::string_view extension) const = 0;
    [[nodiscard]] virtual bool DrawPreview(EditorContext& ctx,
                                           std::string_view assetPath,
                                           float previewHeight) = 0;
};

} // namespace fbzz::editor
