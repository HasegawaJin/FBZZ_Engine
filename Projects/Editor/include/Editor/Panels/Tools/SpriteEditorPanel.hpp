/// @file    SpriteEditorPanel.hpp
/// @brief   Texture atlas の Sprite 矩形・Pivot・9-slice Border を視覚編集する専用パネル。
/// @author  Hasegawa Jin
/// @date    2026-08-12
#pragma once
#include <Editor/Panels/IPanel.hpp>
#include <Editor/Asset/Authoring/SpriteSlicer.hpp>
#include <Engine/Asset/Texture/TextureAsset.hpp>
#include <Graphics/Renderer/Resources/ResourceHandle.hpp>
#include <Math/LinearAlgebra/Vector2.hpp>
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
    HotkeyScope GetHotkeyScope() const override { return m_metaPath.empty() ? HotkeyScope::None : HotkeyScope::SpriteEditor; }
    void OnRegisterOperators(OperatorRegistry& registry) override;

    /// Texture の .meta sidecar を作業コピーへ読み込み、パネルを前面に開く。
    void Open(const std::string& metaPath);

protected:
    void OnBeforeBegin(EditorContext& ctx) override;
    void OnRenderContent(EditorContext& ctx) override;

private:
    enum class SliceType { Automatic, CellSize, CellCount };
    /// 矩形の生成と畳み込みは AI バス (sprite.slice) と共有する (SpriteSlicer.hpp)。
    using SliceExistingMode = spriteslice::ExistingMode;
    enum class DragMode {
        None, Move, ResizeLeft, ResizeTop, ResizeRight, ResizeBottom,
        ResizeTopLeft, ResizeTopRight, ResizeBottomLeft, ResizeBottomRight,
        Pivot, BorderLeft, BorderTop, BorderRight, BorderBottom, Create
    };

    struct ValidationResult {
        bool valid = true; ///< スプライト設定を保存できる状態か。
        std::vector<bool> duplicateNames; ///< スプライト定義と同じ添字に、その名前が重複しているかを保持する。
        std::string message; ///< 設定が無効な場合に表示する理由。
    };

    bool OpenWorkingCopy(const std::string& metaPath);
    bool LoadWorkingCopy();
    bool LoadTextureDimensions(EditorContext& ctx);
    void ExpandFullSpriteRects();
    void FrameAll();
    bool Apply(EditorContext& ctx);
    bool Revert();
    void SliceGrid(uint32_t textureWidth, uint32_t textureHeight);
    void AutoTrim(uint32_t textureWidth, uint32_t textureHeight);
    void TrimSelected(uint32_t textureWidth, uint32_t textureHeight);
    ValidationResult Validate(uint32_t textureWidth, uint32_t textureHeight) const;
    void DrawSpriteRectInspector(uint32_t textureWidth, uint32_t textureHeight);

    /// 今の Sprite ID を参照しているプロジェクト内のファイル数を数える。
    /// @note Delete Existing は ID を全部作り直す。何件壊れるかを見ずに押せると、
    ///       «割り当てたはずの絵がアトラス全面に戻る» が後から静かに起きる。
    void RefreshReferrerCount(const EditorContext& ctx);

    /// 連番リネーム。名前は «別名キー» なので、人と AI が呼べる名前にできないと
    /// 参照は UUID を書き写すしかなくなる。
    void RenameAll(const std::string& prefix, int startIndex);

    void DrawCanvas(EditorContext& ctx, void* textureId,
                    uint32_t textureWidth, uint32_t textureHeight);
    void PushUndoSnapshot(
        EditorContext& ctx, const asset::TextureImportSettings& before,
        const asset::TextureImportSettings& after, const std::string& description);

    std::string m_metaPath; ///< 現在開いているテクスチャの設定を保存する .meta のパス。
    std::string m_texturePath; ///< 切り出し対象のテクスチャ原本のパス。
    std::string m_loadedMeta; ///< 最後に読み込んだか保存した .meta の本文。外部編集を検知するために保持する。
    asset::TextureAsset m_working; ///< 切り出しや名前を編集中のテクスチャアセットの作業コピー。
    renderer::ResourceHandle<renderer::TextureTag> m_texture; ///< テクスチャキャンバスで原本画像を表示するためのハンドル。
    std::uint64_t m_textureResetVersion = 0; ///< 画像を取得した ResourceManager のリセット世代。
    int m_selected = -1; ///< m_working にあるスプライト定義の選択添字。未選択なら -1。
    int m_gridColumns = 1; ///< 行列でテクスチャを切り分けるときの列数。
    int m_gridRows = 1; ///< 行列でテクスチャを切り分けるときの行数。
    int m_cellWidth = 64; ///< 固定セル寸法で切り分けるときの一セルの幅 [px]。
    int m_cellHeight = 64; ///< 固定セル寸法で切り分けるときの一セルの高さ [px]。
    int m_sliceOffsetX = 0; ///< 切り分け開始位置の画像左端からのオフセット [px]。
    int m_sliceOffsetY = 0; ///< 切り分け開始位置の画像上端からのオフセット [px]。
    int m_slicePaddingX = 0; ///< 隣接する切り分け矩形の横方向の間隔 [px]。
    int m_slicePaddingY = 0; ///< 隣接する切り分け矩形の縦方向の間隔 [px]。
    float m_slicePivotX = 0.5f; ///< 新しい矩形の横方向のピボット。矩形内の正規化座標。
    float m_slicePivotY = 0.5f; ///< 新しい矩形の縦方向のピボット。矩形内の正規化座標。
    int m_slicePivotPreset = 4; ///< スライス設定で選んだピボットプリセット。
    SliceType m_sliceType = SliceType::Automatic; ///< 画像自動認識、固定セル寸法、行列指定のどれで切り分けるか。
    SliceExistingMode m_sliceExistingMode = SliceExistingMode::DeleteExisting; ///< 切り分けを再実行するときの既存矩形の扱い。
    bool m_keepEmptyRects = true; ///< 透明画素だけで構成されたグリッドの矩形も残すか。
    bool m_slicePopupOpen = false; ///< 切り出し設定のポップアップを表示するか。
    float m_zoom = 1.0f; ///< テクスチャキャンバスの表示倍率。
    math::Vector2 m_pan = {}; ///< テクスチャキャンバスの表示位置のずれ。
    DragMode m_dragMode = DragMode::None; ///< ドラッグで現在進めている矩形編集の種類。
    math::Vector2 m_dragStartTexture = {}; ///< ドラッグを開始したカーソルのテクスチャ座標 [px]。
    asset::SpriteRect m_dragStartSprite; ///< ドラッグ開始前のスプライト矩形。編集中の差分の基準。
    asset::SpriteRect m_createPreview; ///< ドラッグで新しいスプライトを作る際に表示する未確定矩形。
    asset::TextureImportSettings m_undoBefore; ///< 現在の編集を Undo で戻すために控えた編集前の設定。
    bool m_undoPending = false; ///< 操作が確定したら Undo を積むための開始状態を保持しているか。
    std::string m_undoDescription = "Edit Sprite"; ///< 現在編集中の操作を Undo 履歴へ表示する名前。
    std::unordered_map<std::string, asset::TextureImportSettings> m_pendingUndoSettings; ///< 閉じているアセットへ Undo が適用された際、次回開いたときに復元する設定。キーは .meta パス。
    uint32_t m_textureWidth = 0; ///< 切り分け対象の原本の幅 [px]。
    uint32_t m_textureHeight = 0; ///< 切り分け対象の原本の高さ [px]。
    bool m_dirty = false; ///< 作業コピーに .meta へ書き出していない変更があるか。
    bool m_requestFocus = false; ///< 次にパネルを表示したときにフォーカスする要求。
    bool m_initialSizeRequested = true; ///< 初めて表示するときにパネルの初期サイズを設定するか。
    std::string m_status; ///< 切り出しや保存の結果について表示する診断。
    int m_referrerCount = -1;          ///< -1 は «まだ数えていない»
    int m_referrerFileCount = 0; ///< 選択中のスプライトに対する参照を見つけたファイル数。
    char m_renamePrefix[128] = ""; ///< 一括改名で付けるスプライト名の接頭辞。
    int m_renameStartIndex = 0; ///< 一括改名で最初のスプライト名に付ける番号。
};

} // namespace fbzz::editor
