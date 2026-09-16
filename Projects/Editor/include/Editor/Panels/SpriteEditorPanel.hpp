/// @file    SpriteEditorPanel.hpp
/// @brief   Texture atlas の Sprite 矩形・Pivot・9-slice Border を視覚編集する専用パネル。
/// @author  Hasegawa Jin
/// @date    2026-08-12
#pragma once
#include <Editor/Panels/IPanel.hpp>
#include <Editor/Util/SpriteSlicer.hpp>
#include <Engine/Asset/TextureAsset.hpp>
#include <Engine/Renderer/ResourceHandle.hpp>
#include <Math/Vector2.hpp>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace fbzz::editor {

class SpriteEditorPanel final : public IPanel {
public:
    const char* GetWindowName() const override { return "Sprite Editor"; }
    const char* GetViewMenuName() const override { return "Sprite Editor"; }
    const char* GetMenuCategory() const override { return "Assets"; }
    bool GetDefaultVisibility() const override { return false; }

    // Texture の .meta sidecar を作業コピーへ読み込み、パネルを前面に開く。
    void Open(const std::string& metaPath);

protected:
    void OnBeforeBegin(EditorContext& ctx) override;
    void OnRenderContent(EditorContext& ctx) override;

private:
    enum class SliceType { Automatic, CellSize, CellCount };
    // 矩形の生成と畳み込みは AI バス (sprite.slice) と共有する (SpriteSlicer.hpp)。
    using SliceExistingMode = spriteslice::ExistingMode;
    enum class DragMode {
        None, Move, ResizeLeft, ResizeTop, ResizeRight, ResizeBottom,
        ResizeTopLeft, ResizeTopRight, ResizeBottomLeft, ResizeBottomRight,
        Pivot, BorderLeft, BorderTop, BorderRight, BorderBottom, Create
    };

    struct ValidationResult {
        bool valid = true;
        std::vector<bool> duplicateNames;
        std::string message;
    };

    bool LoadWorkingCopy();
    bool Apply(EditorContext& ctx);
    void Revert();
    void SliceGrid(uint32_t textureWidth, uint32_t textureHeight);
    void AutoTrim(uint32_t textureWidth, uint32_t textureHeight);
    void TrimSelected(uint32_t textureWidth, uint32_t textureHeight);
    ValidationResult Validate(uint32_t textureWidth, uint32_t textureHeight) const;
    void DrawSpriteRectInspector(uint32_t textureWidth, uint32_t textureHeight);

    /// 今の Sprite ID を参照しているプロジェクト内のファイル数を数える。
    /// WHY: Delete Existing は ID を全部作り直す。何件壊れるかを見ずに押せると、
    ///      «割り当てたはずの絵がアトラス全面に戻る» が後から静かに起きる。
    void RefreshReferrerCount(const EditorContext& ctx);

    /// 連番リネーム。名前は «別名キー» なので、人と AI が呼べる名前にできないと
    /// 参照は UUID を書き写すしかなくなる。
    void RenameAll(const std::string& prefix, int startIndex);

    void DrawCanvas(EditorContext& ctx, void* textureId,
                    uint32_t textureWidth, uint32_t textureHeight);
    void PushUndoSnapshot(
        EditorContext& ctx, const asset::TextureImportSettings& before,
        const asset::TextureImportSettings& after, const std::string& description);

    std::string m_metaPath;
    std::string m_texturePath;
    asset::TextureAsset m_working;
    renderer::ResourceHandle<renderer::TextureTag> m_texture;
    std::uint64_t m_textureResetVersion = 0;
    int m_selected = -1;
    int m_gridColumns = 1;
    int m_gridRows = 1;
    int m_cellWidth = 64;
    int m_cellHeight = 64;
    int m_sliceOffsetX = 0;
    int m_sliceOffsetY = 0;
    int m_slicePaddingX = 0;
    int m_slicePaddingY = 0;
    float m_slicePivotX = 0.5f;
    float m_slicePivotY = 0.5f;
    int m_slicePivotPreset = 4;
    SliceType m_sliceType = SliceType::Automatic;
    SliceExistingMode m_sliceExistingMode = SliceExistingMode::DeleteExisting;
    bool m_keepEmptyRects = true;
    bool m_slicePopupOpen = false;
    float m_zoom = 1.0f;
    math::Vector2 m_pan = {};
    DragMode m_dragMode = DragMode::None;
    math::Vector2 m_dragStartTexture = {};
    asset::SpriteRect m_dragStartSprite;
    asset::SpriteRect m_createPreview;
    asset::TextureImportSettings m_undoBefore;
    bool m_undoPending = false;
    std::string m_undoDescription = "Edit Sprite";
    std::unordered_map<std::string, asset::TextureImportSettings> m_pendingUndoSettings;
    uint32_t m_textureWidth = 0;
    uint32_t m_textureHeight = 0;
    bool m_dirty = false;
    bool m_requestFocus = false;
    bool m_initialSizeRequested = true;
    std::string m_status;
    int m_referrerCount = -1;          ///< -1 は «まだ数えていない»
    int m_referrerFileCount = 0;
    char m_renamePrefix[128] = "";
    int m_renameStartIndex = 0;
};

} // namespace fbzz::editor
