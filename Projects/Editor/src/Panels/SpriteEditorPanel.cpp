/// @file    SpriteEditorPanel.cpp
/// @brief   Sprite atlas を直接操作する視覚編集ツール。
/// @author  Hasegawa Jin
/// @date    2026-08-12
#include <Editor/Panels/SpriteEditorPanel.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/Util/AssetDirtyRegistry.hpp>
#include <Editor/Util/UndoStack.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/TexDescSerializer.hpp>
#include <Engine/Renderer/IImGuiRenderer.hpp>
#include <Engine/Renderer/ITexture.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Scene/Components/UIImage.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Engine/Util/StringUtils.hpp>
#include <Engine/Util/Uuid.hpp>
#include <imgui.h>
#include <stb_image.h>
#include <algorithm>
#include <bit>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <unordered_map>
#include <utility>

namespace fbzz::editor {
namespace {

// Rendererのopaque pointerをImGuiのTexture IDへ安全に変換する。
ImTextureID ToTextureId(void* ptr)
{
    return static_cast<ImTextureID>(std::bit_cast<std::uintptr_t>(ptr));
}

// ハンドル選択に使うscreen-space距離の二乗を返す。
float DistanceSquared(ImVec2 lhs, ImVec2 rhs)
{
    const float dx = lhs.x - rhs.x;
    const float dy = lhs.y - rhs.y;
    return dx * dx + dy * dy;
}

// 透明部分を判別できる市松模様をcanvas全面へ描く。
void DrawCheckerboard(ImDrawList* drawList, ImVec2 min, ImVec2 max)
{
    constexpr float CELL = 16.0f;
    for (float y = min.y; y < max.y; y += CELL) {
        for (float x = min.x; x < max.x; x += CELL) {
            const int ix = static_cast<int>((x - min.x) / CELL);
            const int iy = static_cast<int>((y - min.y) / CELL);
            const ImU32 color = ((ix + iy) & 1) == 0
                ? IM_COL32(62, 62, 66, 255) : IM_COL32(42, 42, 46, 255);
            drawList->AddRectFilled({ x, y }, { std::min(x + CELL, max.x), std::min(y + CELL, max.y) }, color);
        }
    }
}

// Sprite 設定変更後、同じ Texture を参照する UIImage の UV / 9-slice 再解決を要求する。
void InvalidateSpriteUsers(EditorContext& ctx, const std::string& editedTexturePath)
{
    if (!ctx.activeScene) return;
    const std::filesystem::path editedTexture =
        util::FileSystem::PathFromUtf8(editedTexturePath).lexically_normal();
    for (auto [image] : ctx.activeScene->View<scene::UIImage>()) {
        std::string texturePath;
        std::string spriteToken;
        // 状態別スプライトで差し替え中なら、今描いている絵のほうを見る。
        (void)asset::ParseSpriteReference(
            image.EffectiveTexturePath(), texturePath, spriteToken);
        const std::filesystem::path resolvedTexture = util::FileSystem::PathFromUtf8(
            asset::AssetManager::ResolveAssetPath(texturePath)).lexically_normal();
        std::error_code pathError;
        const bool sameTexture = std::filesystem::equivalent(
            resolvedTexture, editedTexture, pathError);
        if ((!pathError && sameTexture)
            || (pathError && resolvedTexture == editedTexture)) {
            image.loadedTexturePath.clear();
        }
    }
}

} // namespace

// 初回表示を Sprite Sheet 全体と右下プロパティを同時に扱える実用サイズで開く。
void SpriteEditorPanel::OnBeforeBegin(EditorContext&)
{
    if (m_initialSizeRequested) {
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        const ImVec2 workSize = viewport ? viewport->WorkSize : ImVec2{ 1600.0f, 1000.0f };
        const ImVec2 initialSize = {
            std::min(1440.0f, std::max(760.0f, workSize.x * 0.82f)),
            std::min(920.0f, std::max(520.0f, workSize.y * 0.84f))
        };
        ImGui::SetNextWindowSize(initialSize, ImGuiCond_Always);
        if (viewport) {
            ImGui::SetNextWindowPos(
                { viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                  viewport->WorkPos.y + viewport->WorkSize.y * 0.5f },
                ImGuiCond_Always, { 0.5f, 0.5f });
        }
        m_initialSizeRequested = false;
    }
    ImGui::SetNextWindowSizeConstraints(
        { 760.0f, 520.0f }, { FLT_MAX, FLT_MAX });
}

// 指定metaを開き、未適用変更がある場合は意図しない破棄を防ぐ。
void SpriteEditorPanel::Open(const std::string& metaPath)
{
    visible = true;
    m_requestFocus = true;
    if (m_dirty && !m_metaPath.empty() && m_metaPath != metaPath) {
        m_status = "Apply or Revert the current Sprite before opening another Texture.";
        return;
    }
    if (m_metaPath != metaPath && AssetDirtyRegistry::IsDirty(metaPath)) {
        m_status = "Apply or Revert the Inspector Import Settings before opening this Texture.";
        return;
    }
    if (m_metaPath == metaPath) return;
    m_metaPath = metaPath;
    m_texturePath = metaPath.ends_with(".meta")
        ? metaPath.substr(0, metaPath.size() - 5) : metaPath;
    LoadWorkingCopy();
}

// ディスク上のmetaを作業コピーへ読み込み、編集状態を初期化する。
bool SpriteEditorPanel::LoadWorkingCopy()
{
    m_working = {};
    asset::TexDescSerializer serializer;
    if (!serializer.Load(m_metaPath, m_working)) {
        m_status = "Failed to load Texture .meta.";
        return false;
    }
    m_working.sourcePath = m_texturePath;
    m_working.settings.type = asset::TextureType::Sprite;
    if (m_working.settings.sprites.empty()) {
        asset::SpriteRect sprite;
        sprite.id = util::GenerateUUID();
        sprite.name = util::FileSystem::PathToUtf8(
            util::FileSystem::PathFromUtf8(m_texturePath).stem());
        m_working.settings.sprites.push_back(std::move(sprite));
    }
    m_selected = m_working.settings.sprites.empty() ? -1 : 0;
    m_texture = {};
    m_textureResetVersion = 0;
    m_zoom = 1.0f;
    m_pan = {};
    m_dragMode = DragMode::None;
    m_slicePopupOpen = false;
    m_undoPending = false;
    const auto pendingUndo = m_pendingUndoSettings.find(m_metaPath);
    if (pendingUndo != m_pendingUndoSettings.end()) {
        m_working.settings = std::move(pendingUndo->second);
        m_pendingUndoSettings.erase(pendingUndo);
        m_selected = m_working.settings.sprites.empty() ? -1 : 0;
        m_dirty = true;
        m_status = "Restored pending Undo/Redo changes.";
    } else {
        m_dirty = false;
        m_status.clear();
    }
    return true;
}

// 検証済み作業コピーだけをmetaへ保存する。
bool SpriteEditorPanel::Apply(EditorContext& ctx)
{
    asset::TextureAsset persisted = m_working;
    if (persisted.settings.spriteMode == asset::SpriteMode::Single
        && persisted.settings.sprites.size() == 1) {
        asset::SpriteRect& sprite = persisted.settings.sprites.front();
        if (sprite.x == 0 && sprite.y == 0
            && sprite.width == m_textureWidth && sprite.height == m_textureHeight) {
            // Single の全面 Sprite は 0 サイズ表現へ戻し、元画像差し替え後も全面追従させる。
            sprite.width = 0;
            sprite.height = 0;
        }
    }
    asset::TexDescSerializer serializer;
    if (!serializer.Save(persisted, m_metaPath)) {
        m_status = "Apply failed.";
        return false;
    }

    InvalidateSpriteUsers(ctx, m_texturePath);
    AssetDirtyRegistry::MarkClean(m_metaPath);
    m_pendingUndoSettings.erase(m_metaPath);
    ctx.requestAssetBrowserRefresh = true;
    m_dirty = false;
    m_status = "Applied.";
    return true;
}

// 未適用変更を破棄し、ディスク上のmetaへ戻す。
void SpriteEditorPanel::Revert()
{
    m_pendingUndoSettings.erase(m_metaPath);
    LoadWorkingCopy();
    m_status = "Reverted.";
}

// 作業コピーの確定操作を Editor 全体の Undo/Redo 履歴へ積む。
void SpriteEditorPanel::PushUndoSnapshot(
    EditorContext& ctx, const asset::TextureImportSettings& before,
    const asset::TextureImportSettings& after, const std::string& description)
{
    if (before == after || ctx.undoStack == nullptr
        || !ctx.undoStack->IsRecordingEnabled()) {
        return;
    }
    const std::string targetMetaPath = m_metaPath;
    auto applySettings = [this, targetMetaPath](
        const asset::TextureImportSettings& settings) {
        if (m_metaPath == targetMetaPath) {
            m_working.settings = settings;
            m_selected = std::min(
                m_selected, static_cast<int>(m_working.settings.sprites.size()) - 1);
            m_dirty = true;
            m_status = "Modified by Undo/Redo.";
            return;
        }

        // 別 Texture の履歴も失わず保持し、次に対象を開いた時に未適用作業コピーとして復元する。
        // WHY: ディスクへ即時保存すると Inspector の未適用値を上書きするため、Apply までは分離する。
        m_pendingUndoSettings[targetMetaPath] = settings;
        m_status = "Undo/Redo changes queued for another Sprite asset.";
    };
    ctx.undoStack->Push(std::make_unique<LambdaCommand>(
        description,
        [applySettings, after]() { applySettings(after); },
        [applySettings, before]() { applySettings(before); }));
}

// Texture全体を指定行列で均等分割する。
void SpriteEditorPanel::SliceGrid(uint32_t textureWidth, uint32_t textureHeight)
{
    spriteslice::GridParams params;
    params.byCellCount    = m_sliceType == SliceType::CellCount;
    params.columns        = m_gridColumns;
    params.rows           = m_gridRows;
    params.cellWidth      = m_cellWidth;
    params.cellHeight     = m_cellHeight;
    params.offsetX        = m_sliceOffsetX;
    params.offsetY        = m_sliceOffsetY;
    params.paddingX       = m_slicePaddingX;
    params.paddingY       = m_slicePaddingY;
    params.pivotX         = m_slicePivotX;
    params.pivotY         = m_slicePivotY;
    params.keepEmptyRects = m_keepEmptyRects;
    params.baseName       = util::FileSystem::PathToUtf8(
        util::FileSystem::PathFromUtf8(m_texturePath).stem()) + "_";

    const spriteslice::Result generated =
        spriteslice::GenerateGrid(m_texturePath, textureWidth, textureHeight, params);
    if (!generated.error.empty()) {
        m_status = generated.error;
        return;
    }

    m_working.settings.sprites = spriteslice::MergeIntoExisting(
        m_working.settings.sprites, generated.sprites, m_sliceExistingMode);
    m_working.settings.spriteMode = asset::SpriteMode::Multiple;
    m_selected = m_working.settings.sprites.empty() ? -1 : 0;
    m_undoDescription = "Slice Sprite Sheet";
    m_dirty = true;
    m_status = "Slice created " + std::to_string(m_working.settings.sprites.size())
        + " Sprite rects.";
}

// Automatic Slice として、alpha が連結した島ごとに外接矩形を生成する。
void SpriteEditorPanel::AutoTrim(uint32_t textureWidth, uint32_t textureHeight)
{
    spriteslice::AutoTrimParams params;
    params.pivotX   = m_slicePivotX;
    params.pivotY   = m_slicePivotY;
    params.baseName = util::FileSystem::PathToUtf8(
        util::FileSystem::PathFromUtf8(m_texturePath).stem()) + "_";

    const spriteslice::Result generated =
        spriteslice::GenerateAutoTrim(m_texturePath, params);
    if (!generated.error.empty()) {
        m_status = generated.error;
        return;
    }

    m_working.settings.sprites = spriteslice::MergeIntoExisting(
        m_working.settings.sprites, generated.sprites, m_sliceExistingMode);
    m_working.settings.spriteMode = asset::SpriteMode::Multiple;
    m_selected = m_working.settings.sprites.empty() ? -1 : 0;
    m_undoDescription = "Auto Slice Sprite Sheet";
    m_dirty = true;
    m_status = "Automatic Slice created "
        + std::to_string(m_working.settings.sprites.size()) + " SpriteRects.";
    (void)textureWidth;
    (void)textureHeight;
}

// 選択中 SpriteRect を、その範囲内にある非透明ピクセルの外接矩形へ縮める。
void SpriteEditorPanel::TrimSelected(uint32_t textureWidth, uint32_t textureHeight)
{
    if (m_selected < 0
        || m_selected >= static_cast<int>(m_working.settings.sprites.size())) {
        m_status = "Select a SpriteRect to Trim.";
        return;
    }

    int decodedWidth = 0;
    int decodedHeight = 0;
    int channels = 0;
    stbi_uc* pixels = stbi_load(
        m_texturePath.c_str(), &decodedWidth, &decodedHeight, &channels, 4);
    if (pixels == nullptr || decodedWidth <= 0 || decodedHeight <= 0) {
        if (pixels) stbi_image_free(pixels);
        m_status = "Trim could not decode this image.";
        return;
    }

    asset::SpriteRect& sprite =
        m_working.settings.sprites[static_cast<size_t>(m_selected)];
    const uint32_t left = std::min(sprite.x, textureWidth);
    const uint32_t top = std::min(sprite.y, textureHeight);
    const uint32_t right = std::min(
        sprite.x + sprite.width, std::min(textureWidth, static_cast<uint32_t>(decodedWidth)));
    const uint32_t bottom = std::min(
        sprite.y + sprite.height, std::min(textureHeight, static_cast<uint32_t>(decodedHeight)));
    uint32_t minX = right;
    uint32_t minY = bottom;
    uint32_t maxX = left;
    uint32_t maxY = top;
    bool found = false;
    for (uint32_t y = top; y < bottom; ++y) {
        for (uint32_t x = left; x < right; ++x) {
            const size_t pixel =
                (static_cast<size_t>(y) * static_cast<size_t>(decodedWidth) + x) * 4;
            if (pixels[pixel + 3] <= 8) continue;
            minX = std::min(minX, x);
            minY = std::min(minY, y);
            maxX = std::max(maxX, x);
            maxY = std::max(maxY, y);
            found = true;
        }
    }
    stbi_image_free(pixels);
    if (!found) {
        m_status = "Trim found no visible pixels inside the selected SpriteRect.";
        return;
    }

    sprite.x = minX;
    sprite.y = minY;
    sprite.width = maxX - minX + 1;
    sprite.height = maxY - minY + 1;
    sprite.borderLeft = std::min(sprite.borderLeft, static_cast<float>(sprite.width));
    sprite.borderRight = std::min(
        sprite.borderRight, static_cast<float>(sprite.width) - sprite.borderLeft);
    sprite.borderTop = std::min(sprite.borderTop, static_cast<float>(sprite.height));
    sprite.borderBottom = std::min(
        sprite.borderBottom, static_cast<float>(sprite.height) - sprite.borderTop);
    m_undoDescription = "Trim SpriteRect";
    m_dirty = true;
    m_status = "Trimmed selected SpriteRect.";
}

// 名前・矩形範囲・9-slice Borderの保存可能性を検証する。
SpriteEditorPanel::ValidationResult SpriteEditorPanel::Validate(
    uint32_t textureWidth, uint32_t textureHeight) const
{
    ValidationResult result;
    result.duplicateNames.resize(m_working.settings.sprites.size(), false);
    if (m_working.settings.sprites.empty()) {
        result.valid = false;
        result.message = "At least one Sprite is required.";
        return result;
    }
    std::unordered_map<std::string, int> names;
    for (size_t index = 0; index < m_working.settings.sprites.size(); ++index) {
        const asset::SpriteRect& sprite = m_working.settings.sprites[index];
        if (sprite.name.empty()) {
            result.valid = false;
            result.message = "Sprite names cannot be empty.";
        } else if (const auto [it, inserted] = names.emplace(sprite.name, static_cast<int>(index)); !inserted) {
            result.valid = false;
            result.duplicateNames[index] = true;
            result.duplicateNames[static_cast<size_t>(it->second)] = true;
            result.message = "Sprite names must be unique.";
        }
        if (sprite.width == 0 || sprite.height == 0
            || sprite.x >= textureWidth || sprite.y >= textureHeight
            || sprite.width > textureWidth - sprite.x
            || sprite.height > textureHeight - sprite.y) {
            result.valid = false;
            result.message = "Sprite rectangles must stay inside the Texture.";
        }
        if (sprite.borderLeft + sprite.borderRight > static_cast<float>(sprite.width)
            || sprite.borderTop + sprite.borderBottom > static_cast<float>(sprite.height)) {
            result.valid = false;
            result.message = "9-slice borders overlap.";
        }
    }
    return result;
}

void SpriteEditorPanel::RefreshReferrerCount(const EditorContext& ctx)
{
    m_referrerCount = 0;
    m_referrerFileCount = 0;
    if (ctx.projectRoot.empty()) return;

    // ID は .scene / .prefab / .mat のほか、スクリプトへ文字列で埋め込まれることもある
    // (KeyIcons.hpp のような生成表)。テキストとして開けるものは全部見る。
    static const std::vector<std::string> kTextExtensions = {
        ".scene", ".prefab", ".mat", ".animcontroller", ".vfx", ".sequence",
        ".fzdata", ".toml", ".json", ".hpp", ".cpp", ".terrain"
    };
    std::vector<std::string> ids;
    ids.reserve(m_working.settings.sprites.size());
    for (const asset::SpriteRect& sprite : m_working.settings.sprites)
        if (!sprite.id.empty()) ids.push_back(sprite.id);
    if (ids.empty()) return;

    std::error_code ec;
    const std::filesystem::path assetsRoot =
        util::FileSystem::PathFromUtf8(ctx.projectRoot) / "Assets";
    for (std::filesystem::recursive_directory_iterator
             it(assetsRoot, std::filesystem::directory_options::skip_permission_denied, ec), end;
         it != end; it.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        if (!it->is_regular_file(ec)) continue;
        const std::string ext = util::StringUtils::ToLower(
            util::FileSystem::PathToUtf8(it->path().extension()));
        if (std::find(kTextExtensions.begin(), kTextExtensions.end(), ext)
            == kTextExtensions.end()) continue;
        if (it->file_size(ec) > 8u * 1024u * 1024u) continue;

        std::string text;
        if (!util::FileSystem::ReadText(util::FileSystem::PathToUtf8(it->path()), text)) continue;
        int hits = 0;
        for (const std::string& id : ids)
            if (text.find(id) != std::string::npos) ++hits;
        if (hits > 0) {
            m_referrerCount += hits;
            ++m_referrerFileCount;
        }
    }
}

void SpriteEditorPanel::RenameAll(const std::string& prefix, int startIndex)
{
    if (m_working.settings.sprites.empty()) return;
    int index = startIndex;
    for (asset::SpriteRect& sprite : m_working.settings.sprites)
        sprite.name = prefix + std::to_string(index++);
    m_undoDescription = "Rename Sprites";
    m_dirty = true;
    m_status = "Renamed " + std::to_string(m_working.settings.sprites.size()) + " Sprites.";
}

// Unity と同様に、選択中 SpriteRect の情報をキャンバス右下の小パネルへ描画する。
void SpriteEditorPanel::DrawSpriteRectInspector(
    uint32_t textureWidth, uint32_t textureHeight)
{
    const ValidationResult validation = Validate(textureWidth, textureHeight);
    if (m_selected < 0
        || m_selected >= static_cast<int>(m_working.settings.sprites.size())) {
        return;
    }

    asset::SpriteRect& sprite =
        m_working.settings.sprites[static_cast<size_t>(m_selected)];
    static constexpr float PIVOTS[9][2] = {
        {0.0f, 0.0f}, {0.5f, 0.0f}, {1.0f, 0.0f},
        {0.0f, 0.5f}, {0.5f, 0.5f}, {1.0f, 0.5f},
        {0.0f, 1.0f}, {0.5f, 1.0f}, {1.0f, 1.0f}
    };
    static constexpr const char* PIVOT_NAMES[] = {
        "Top Left", "Top", "Top Right", "Left", "Center",
        "Right", "Bottom Left", "Bottom", "Bottom Right", "Custom"
    };
    int pivotPreset = 9;
    for (int index = 0; index < 9; ++index) {
        if (std::abs(sprite.pivotX - PIVOTS[index][0]) < 0.0001f
            && std::abs(sprite.pivotY - PIVOTS[index][1]) < 0.0001f) {
            pivotPreset = index;
            break;
        }
    }

    const float panelHeight = pivotPreset == 9 ? 250.0f : 220.0f;
    ImGui::BeginChild(
        "##sprite_rect_info", { 310.0f, panelHeight }, true,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PushID(sprite.id.c_str());

    char name[256];
    std::snprintf(name, sizeof(name), "%s", sprite.name.c_str());
    ImGui::SetNextItemWidth(-1.0f);
    const bool spriteNameChanged = ImGui::InputText("##Name", name, sizeof(name));
    if (spriteNameChanged) {
        sprite.name = name;
        m_undoDescription = "Rename Sprite";
        m_dirty = true;
    }

    int position[2] = { static_cast<int>(sprite.x), static_cast<int>(sprite.y) };
    int size[2] = { static_cast<int>(sprite.width), static_cast<int>(sprite.height) };
    ImGui::SetNextItemWidth(180.0f);
    if (ImGui::InputInt2("Position", position)) {
        sprite.x = static_cast<uint32_t>(
            std::clamp(position[0], 0, static_cast<int>(textureWidth - 1)));
        sprite.y = static_cast<uint32_t>(
            std::clamp(position[1], 0, static_cast<int>(textureHeight - 1)));
        sprite.width = std::min(sprite.width, textureWidth - sprite.x);
        sprite.height = std::min(sprite.height, textureHeight - sprite.y);
        m_undoDescription = "Move SpriteRect";
        m_dirty = true;
    }
    ImGui::SetNextItemWidth(180.0f);
    if (ImGui::InputInt2("Size", size)) {
        sprite.width = static_cast<uint32_t>(
            std::clamp(size[0], 1, static_cast<int>(textureWidth - sprite.x)));
        sprite.height = static_cast<uint32_t>(
            std::clamp(size[1], 1, static_cast<int>(textureHeight - sprite.y)));
        m_undoDescription = "Resize SpriteRect";
        m_dirty = true;
    }

    float border[4] = {
        sprite.borderLeft, sprite.borderTop, sprite.borderRight, sprite.borderBottom
    };
    ImGui::SetNextItemWidth(180.0f);
    if (ImGui::DragFloat4("Border", border, 1.0f, 0.0f, 16384.0f, "%.0f")) {
        sprite.borderLeft = std::clamp(border[0], 0.0f, static_cast<float>(sprite.width));
        sprite.borderRight = std::clamp(
            border[2], 0.0f, static_cast<float>(sprite.width) - sprite.borderLeft);
        sprite.borderTop = std::clamp(border[1], 0.0f, static_cast<float>(sprite.height));
        sprite.borderBottom = std::clamp(
            border[3], 0.0f, static_cast<float>(sprite.height) - sprite.borderTop);
        m_undoDescription = "Edit Sprite Border";
        m_dirty = true;
    }

    ImGui::SetNextItemWidth(180.0f);
    if (ImGui::Combo("Pivot", &pivotPreset, PIVOT_NAMES, 10) && pivotPreset < 9) {
        sprite.pivotX = PIVOTS[pivotPreset][0];
        sprite.pivotY = PIVOTS[pivotPreset][1];
        m_undoDescription = "Edit Sprite Pivot";
        m_dirty = true;
    }
    if (pivotPreset == 9) {
        float pivot[2] = { sprite.pivotX, sprite.pivotY };
        ImGui::SetNextItemWidth(180.0f);
        if (ImGui::DragFloat2("Custom Pivot", pivot, 0.01f, 0.0f, 1.0f)) {
            sprite.pivotX = std::clamp(pivot[0], 0.0f, 1.0f);
            sprite.pivotY = std::clamp(pivot[1], 0.0f, 1.0f);
            m_undoDescription = "Edit Sprite Pivot";
            m_dirty = true;
        }
    }
    if (!validation.valid) {
        ImGui::TextColored(
            { 1.0f, 0.35f, 0.25f, 1.0f }, "%s", validation.message.c_str());
    }
    ImGui::PopID();
    ImGui::EndChild();
}

// Texture canvasと直接操作ハンドルを描画し、mouse操作をSprite値へ反映する。
void SpriteEditorPanel::DrawCanvas(EditorContext& ctx, void* textureId,
                                   uint32_t textureWidth, uint32_t textureHeight)
{
    ImGui::BeginChild("##sprite_canvas", { 0.0f, 0.0f }, true,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    const ImVec2 canvasMin = ImGui::GetCursorScreenPos();
    const ImVec2 canvasSize = ImGui::GetContentRegionAvail();
    ImGui::InvisibleButton("##canvas_input", canvasSize,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImVec2 canvasMax = { canvasMin.x + canvasSize.x, canvasMin.y + canvasSize.y };
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const bool hasInspectorOverlay = m_selected >= 0
        && m_selected < static_cast<int>(m_working.settings.sprites.size());
    const bool mouseOverInspector = hasInspectorOverlay
        && mouse.x >= std::max(canvasMin.x + 8.0f, canvasMax.x - 318.0f)
        && mouse.y >= std::max(canvasMin.y + 8.0f, canvasMax.y - 258.0f);
    const bool canvasHovered = hovered && !mouseOverInspector;
    drawList->PushClipRect(canvasMin, canvasMax, true);
    DrawCheckerboard(drawList, canvasMin, canvasMax);

    if (canvasHovered && ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
        const ImVec2 delta = ImGui::GetIO().MouseDelta;
        m_pan.x += delta.x;
        m_pan.y += delta.y;
    }
    const float fitScale = std::max(0.001f, std::min(
        std::max(1.0f, canvasSize.x - 40.0f) / static_cast<float>(textureWidth),
        std::max(1.0f, canvasSize.y - 40.0f) / static_cast<float>(textureHeight)));
    if (canvasHovered && ImGui::GetIO().MouseWheel != 0.0f) {
        const float previousScale = fitScale * m_zoom;
        const ImVec2 previousImageMin = {
            canvasMin.x + (canvasSize.x - static_cast<float>(textureWidth) * previousScale)
                * 0.5f + m_pan.x,
            canvasMin.y + (canvasSize.y - static_cast<float>(textureHeight) * previousScale)
                * 0.5f + m_pan.y
        };
        const float textureMouseX = (mouse.x - previousImageMin.x) / previousScale;
        const float textureMouseY = (mouse.y - previousImageMin.y) / previousScale;
        m_zoom = std::clamp(
            m_zoom * std::pow(1.15f, ImGui::GetIO().MouseWheel), 0.1f, 16.0f);
        const float nextScale = fitScale * m_zoom;
        const ImVec2 centeredNext = {
            canvasMin.x + (canvasSize.x - static_cast<float>(textureWidth) * nextScale) * 0.5f,
            canvasMin.y + (canvasSize.y - static_cast<float>(textureHeight) * nextScale) * 0.5f
        };
        m_pan.x = mouse.x - centeredNext.x - textureMouseX * nextScale;
        m_pan.y = mouse.y - centeredNext.y - textureMouseY * nextScale;
    }

    const float scale = fitScale * m_zoom;
    const ImVec2 imageSize = {
        static_cast<float>(textureWidth) * scale,
        static_cast<float>(textureHeight) * scale
    };
    const ImVec2 imageMin = {
        canvasMin.x + (canvasSize.x - imageSize.x) * 0.5f + m_pan.x,
        canvasMin.y + (canvasSize.y - imageSize.y) * 0.5f + m_pan.y
    };
    const ImVec2 imageMax = { imageMin.x + imageSize.x, imageMin.y + imageSize.y };
    drawList->AddImage(ToTextureId(textureId), imageMin, imageMax);
    drawList->AddRect(imageMin, imageMax, IM_COL32(180, 180, 185, 220));

    const auto toScreen = [&](float x, float y) {
        return ImVec2{ imageMin.x + x * scale, imageMin.y + y * scale };
    };
    const auto toTexture = [&](ImVec2 screen) {
        return math::Vector2{
            std::clamp((screen.x - imageMin.x) / scale, 0.0f, static_cast<float>(textureWidth)),
            std::clamp((screen.y - imageMin.y) / scale, 0.0f, static_cast<float>(textureHeight))
        };
    };
    const auto spriteRect = [&](const asset::SpriteRect& sprite) {
        const float width = sprite.width > 0 ? static_cast<float>(sprite.width) : static_cast<float>(textureWidth);
        const float height = sprite.height > 0 ? static_cast<float>(sprite.height) : static_cast<float>(textureHeight);
        return std::pair{ toScreen(static_cast<float>(sprite.x), static_cast<float>(sprite.y)),
                          toScreen(static_cast<float>(sprite.x) + width, static_cast<float>(sprite.y) + height) };
    };

    // Unity と同様に Grid Slice の設定変更を確定前から Texture 上へプレビューする。
    if (m_slicePopupOpen && m_sliceType != SliceType::Automatic) {
        const int offsetX =
            std::clamp(m_sliceOffsetX, 0, static_cast<int>(textureWidth) - 1);
        const int offsetY =
            std::clamp(m_sliceOffsetY, 0, static_cast<int>(textureHeight) - 1);
        const int paddingX = std::max(0, m_slicePaddingX);
        const int paddingY = std::max(0, m_slicePaddingY);
        int columns = 1;
        int rows = 1;
        int cellWidth = std::max(1, m_cellWidth);
        int cellHeight = std::max(1, m_cellHeight);
        int usableWidth = static_cast<int>(textureWidth) - offsetX;
        int usableHeight = static_cast<int>(textureHeight) - offsetY;
        if (m_sliceType == SliceType::CellCount) {
            columns = std::clamp(m_gridColumns, 1, static_cast<int>(textureWidth));
            rows = std::clamp(m_gridRows, 1, static_cast<int>(textureHeight));
            usableWidth -= paddingX * (columns - 1);
            usableHeight -= paddingY * (rows - 1);
        } else {
            columns = std::max(
                1, (usableWidth + paddingX) / (cellWidth + paddingX));
            rows = std::max(
                1, (usableHeight + paddingY) / (cellHeight + paddingY));
        }
        if (usableWidth >= columns && usableHeight >= rows) {
            for (int row = 0; row < rows; ++row) {
                for (int column = 0; column < columns; ++column) {
                    const bool byCount = m_sliceType == SliceType::CellCount;
                    const int x = byCount
                        ? offsetX + (usableWidth * column) / columns + paddingX * column
                        : offsetX + column * (cellWidth + paddingX);
                    const int y = byCount
                        ? offsetY + (usableHeight * row) / rows + paddingY * row
                        : offsetY + row * (cellHeight + paddingY);
                    const int width = byCount
                        ? (usableWidth * (column + 1)) / columns
                            - (usableWidth * column) / columns
                        : std::min(cellWidth, static_cast<int>(textureWidth) - x);
                    const int height = byCount
                        ? (usableHeight * (row + 1)) / rows
                            - (usableHeight * row) / rows
                        : std::min(cellHeight, static_cast<int>(textureHeight) - y);
                    if (x < static_cast<int>(textureWidth)
                        && y < static_cast<int>(textureHeight)
                        && width > 0 && height > 0) {
                        drawList->AddRect(
                            toScreen(static_cast<float>(x), static_cast<float>(y)),
                            toScreen(
                                static_cast<float>(x + width),
                                static_cast<float>(y + height)),
                            IM_COL32(255, 215, 70, 210), 0.0f, 0, 1.5f);
                    }
                }
            }
        }
    }

    for (int index = 0; index < static_cast<int>(m_working.settings.sprites.size()); ++index) {
        const asset::SpriteRect& sprite = m_working.settings.sprites[static_cast<size_t>(index)];
        const auto [rectMin, rectMax] = spriteRect(sprite);
        const bool selected = index == m_selected;
        if (selected) {
            drawList->AddRectFilled(
                rectMin, rectMax, IM_COL32(60, 145, 255, 24));
        }
        drawList->AddRect(rectMin, rectMax,
                          selected ? IM_COL32(255, 255, 255, 255)
                                   : IM_COL32(65, 155, 255, 230),
                          0.0f, 0, selected ? 2.0f : 1.0f);
        const ImVec2 labelSize = ImGui::CalcTextSize(sprite.name.c_str());
        drawList->AddRectFilled(
            { rectMin.x, rectMin.y },
            { rectMin.x + labelSize.x + 8.0f, rectMin.y + labelSize.y + 4.0f },
            selected ? IM_COL32(45, 120, 215, 245) : IM_COL32(35, 95, 175, 225));
        drawList->AddText(
            { rectMin.x + 4.0f, rectMin.y + 2.0f },
            IM_COL32(255, 255, 255, 255), sprite.name.c_str());
    }

    if (canvasHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        const math::Vector2 textureMouse = toTexture(mouse);
        m_dragStartTexture = textureMouse;
        m_dragMode = DragMode::None;
        int hitIndex = -1;
        for (int index = static_cast<int>(m_working.settings.sprites.size()) - 1;
             index >= 0; --index) {
            const auto [rectMin, rectMax] =
                spriteRect(m_working.settings.sprites[static_cast<size_t>(index)]);
            constexpr float SELECTION_MARGIN = 9.0f;
            if (mouse.x >= rectMin.x - SELECTION_MARGIN
                && mouse.x <= rectMax.x + SELECTION_MARGIN
                && mouse.y >= rectMin.y - SELECTION_MARGIN
                && mouse.y <= rectMax.y + SELECTION_MARGIN) {
                hitIndex = index;
                break;
            }
        }

        if (hitIndex < 0) {
            m_selected = -1;
            m_createPreview = {};
            m_createPreview.x = static_cast<uint32_t>(textureMouse.x);
            m_createPreview.y = static_cast<uint32_t>(textureMouse.y);
            const bool insideImage = mouse.x >= imageMin.x && mouse.x <= imageMax.x
                && mouse.y >= imageMin.y && mouse.y <= imageMax.y;
            if (insideImage) m_dragMode = DragMode::Create;
        } else {
            m_selected = hitIndex;
            asset::SpriteRect& sprite =
                m_working.settings.sprites[static_cast<size_t>(m_selected)];
            m_dragStartSprite = sprite;
            const auto [rectMin, rectMax] = spriteRect(sprite);
            const ImVec2 pivot = toScreen(
                static_cast<float>(sprite.x) + sprite.width * sprite.pivotX,
                static_cast<float>(sprite.y) + sprite.height * sprite.pivotY);
            constexpr float HANDLE = 9.0f;
            if (DistanceSquared(mouse, pivot) <= HANDLE * HANDLE) {
                m_dragMode = DragMode::Pivot;
            } else if (DistanceSquared(mouse, rectMin) <= HANDLE * HANDLE) {
                m_dragMode = DragMode::ResizeTopLeft;
            } else if (DistanceSquared(mouse, { rectMax.x, rectMin.y }) <= HANDLE * HANDLE) {
                m_dragMode = DragMode::ResizeTopRight;
            } else if (DistanceSquared(mouse, { rectMin.x, rectMax.y }) <= HANDLE * HANDLE) {
                m_dragMode = DragMode::ResizeBottomLeft;
            } else if (DistanceSquared(mouse, rectMax) <= HANDLE * HANDLE) {
                m_dragMode = DragMode::ResizeBottomRight;
            } else if (std::abs(mouse.x - rectMin.x) <= HANDLE) {
                m_dragMode = DragMode::ResizeLeft;
            } else if (std::abs(mouse.x - rectMax.x) <= HANDLE) {
                m_dragMode = DragMode::ResizeRight;
            } else if (std::abs(mouse.y - rectMin.y) <= HANDLE) {
                m_dragMode = DragMode::ResizeTop;
            } else if (std::abs(mouse.y - rectMax.y) <= HANDLE) {
                m_dragMode = DragMode::ResizeBottom;
            } else {
                const float borderLeft =
                    toScreen(static_cast<float>(sprite.x) + sprite.borderLeft, 0.0f).x;
                const float borderRight = toScreen(
                    static_cast<float>(sprite.x + sprite.width) - sprite.borderRight, 0.0f).x;
                const float borderTop =
                    toScreen(0.0f, static_cast<float>(sprite.y) + sprite.borderTop).y;
                const float borderBottom = toScreen(
                    0.0f, static_cast<float>(sprite.y + sprite.height) - sprite.borderBottom).y;
                const float dl = std::abs(mouse.x - borderLeft);
                const float dr = std::abs(mouse.x - borderRight);
                const float dt = std::abs(mouse.y - borderTop);
                const float db = std::abs(mouse.y - borderBottom);
                const float nearest = std::min({ dl, dr, dt, db });
                if (nearest <= HANDLE) {
                    if (nearest == dl) m_dragMode = DragMode::BorderLeft;
                    else if (nearest == dr) m_dragMode = DragMode::BorderRight;
                    else if (nearest == dt) m_dragMode = DragMode::BorderTop;
                    else m_dragMode = DragMode::BorderBottom;
                } else if (mouse.x >= rectMin.x && mouse.x <= rectMax.x
                    && mouse.y >= rectMin.y && mouse.y <= rectMax.y) {
                    m_dragMode = DragMode::Move;
                }
            }
        }
        if (m_dragMode == DragMode::Create) {
            m_undoDescription = "Create Sprite";
        } else if (m_dragMode == DragMode::Move) {
            m_undoDescription = "Move Sprite";
        } else if (m_dragMode == DragMode::Pivot) {
            m_undoDescription = "Edit Sprite Pivot";
        } else if (m_dragMode == DragMode::BorderLeft
            || m_dragMode == DragMode::BorderTop
            || m_dragMode == DragMode::BorderRight
            || m_dragMode == DragMode::BorderBottom) {
            m_undoDescription = "Edit Sprite Border";
        } else if (m_dragMode != DragMode::None) {
            m_undoDescription = "Resize Sprite";
        }
    }

    if (m_dragMode != DragMode::None && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        const math::Vector2 current = toTexture(mouse);
        if (m_dragMode == DragMode::Create) {
            const float minX = std::min(m_dragStartTexture.x, current.x);
            const float minY = std::min(m_dragStartTexture.y, current.y);
            m_createPreview.x = static_cast<uint32_t>(minX);
            m_createPreview.y = static_cast<uint32_t>(minY);
            m_createPreview.width = static_cast<uint32_t>(std::max(1.0f, std::abs(current.x - m_dragStartTexture.x)));
            m_createPreview.height = static_cast<uint32_t>(std::max(1.0f, std::abs(current.y - m_dragStartTexture.y)));
            const auto [previewMin, previewMax] = spriteRect(m_createPreview);
            drawList->AddRect(previewMin, previewMax, IM_COL32(100, 255, 130, 255), 0.0f, 0, 2.0f);
        } else if (m_selected >= 0 && m_selected < static_cast<int>(m_working.settings.sprites.size())) {
            asset::SpriteRect& sprite = m_working.settings.sprites[static_cast<size_t>(m_selected)];
            const float dx = current.x - m_dragStartTexture.x;
            const float dy = current.y - m_dragStartTexture.y;
            if (m_dragMode == DragMode::Move) {
                const uint32_t moveWidth = std::min(m_dragStartSprite.width, textureWidth);
                const uint32_t moveHeight = std::min(m_dragStartSprite.height, textureHeight);
                sprite.x = static_cast<uint32_t>(std::clamp(
                    static_cast<float>(m_dragStartSprite.x) + dx, 0.0f,
                    static_cast<float>(textureWidth - moveWidth)));
                sprite.y = static_cast<uint32_t>(std::clamp(
                    static_cast<float>(m_dragStartSprite.y) + dy, 0.0f,
                    static_cast<float>(textureHeight - moveHeight)));
            } else if (m_dragMode == DragMode::Pivot) {
                sprite.pivotX = std::clamp(
                    (current.x - static_cast<float>(sprite.x)) / static_cast<float>(std::max<uint32_t>(1, sprite.width)),
                    0.0f, 1.0f);
                sprite.pivotY = std::clamp(
                    (current.y - static_cast<float>(sprite.y)) / static_cast<float>(std::max<uint32_t>(1, sprite.height)),
                    0.0f, 1.0f);
            } else if (m_dragMode == DragMode::BorderLeft) {
                sprite.borderLeft = std::clamp(current.x - static_cast<float>(sprite.x), 0.0f,
                    static_cast<float>(sprite.width) - sprite.borderRight);
            } else if (m_dragMode == DragMode::BorderRight) {
                sprite.borderRight = std::clamp(static_cast<float>(sprite.x + sprite.width) - current.x, 0.0f,
                    static_cast<float>(sprite.width) - sprite.borderLeft);
            } else if (m_dragMode == DragMode::BorderTop) {
                sprite.borderTop = std::clamp(current.y - static_cast<float>(sprite.y), 0.0f,
                    static_cast<float>(sprite.height) - sprite.borderBottom);
            } else if (m_dragMode == DragMode::BorderBottom) {
                sprite.borderBottom = std::clamp(static_cast<float>(sprite.y + sprite.height) - current.y, 0.0f,
                    static_cast<float>(sprite.height) - sprite.borderTop);
            } else {
                float left = static_cast<float>(m_dragStartSprite.x);
                float top = static_cast<float>(m_dragStartSprite.y);
                float right = left + static_cast<float>(m_dragStartSprite.width);
                float bottom = top + static_cast<float>(m_dragStartSprite.height);
                if (m_dragMode == DragMode::ResizeLeft
                    || m_dragMode == DragMode::ResizeTopLeft
                    || m_dragMode == DragMode::ResizeBottomLeft) {
                    left = current.x;
                }
                if (m_dragMode == DragMode::ResizeRight
                    || m_dragMode == DragMode::ResizeTopRight
                    || m_dragMode == DragMode::ResizeBottomRight) {
                    right = current.x;
                }
                if (m_dragMode == DragMode::ResizeTop
                    || m_dragMode == DragMode::ResizeTopLeft
                    || m_dragMode == DragMode::ResizeTopRight) {
                    top = current.y;
                }
                if (m_dragMode == DragMode::ResizeBottom
                    || m_dragMode == DragMode::ResizeBottomLeft
                    || m_dragMode == DragMode::ResizeBottomRight) {
                    bottom = current.y;
                }
                if (right < left) std::swap(right, left);
                if (bottom < top) std::swap(bottom, top);
                sprite.x = static_cast<uint32_t>(std::clamp(left, 0.0f, static_cast<float>(textureWidth - 1)));
                sprite.y = static_cast<uint32_t>(std::clamp(top, 0.0f, static_cast<float>(textureHeight - 1)));
                sprite.width = static_cast<uint32_t>(std::max(1.0f, std::min(right, static_cast<float>(textureWidth)) - sprite.x));
                sprite.height = static_cast<uint32_t>(std::max(1.0f, std::min(bottom, static_cast<float>(textureHeight)) - sprite.y));
            }
            m_dirty = true;
        }
    }

    if (m_dragMode == DragMode::Move)
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
    else if (m_dragMode == DragMode::ResizeLeft
        || m_dragMode == DragMode::ResizeRight)
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    else if (m_dragMode == DragMode::ResizeTop
        || m_dragMode == DragMode::ResizeBottom)
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    else if (m_dragMode == DragMode::ResizeTopLeft
        || m_dragMode == DragMode::ResizeBottomRight)
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNWSE);
    else if (m_dragMode == DragMode::ResizeTopRight
        || m_dragMode == DragMode::ResizeBottomLeft)
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNESW);
    else if (m_dragMode == DragMode::Pivot
        || m_dragMode == DragMode::BorderLeft
        || m_dragMode == DragMode::BorderTop
        || m_dragMode == DragMode::BorderRight
        || m_dragMode == DragMode::BorderBottom)
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

    if (m_dragMode != DragMode::None && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        if (m_dragMode == DragMode::Create
            && (m_createPreview.width > 1 || m_createPreview.height > 1)) {
            const std::string baseName = util::FileSystem::PathToUtf8(
                util::FileSystem::PathFromUtf8(m_texturePath).stem());
            m_createPreview.name =
                baseName + "_" + std::to_string(m_working.settings.sprites.size());
            m_createPreview.id = util::GenerateUUID();
            m_working.settings.sprites.push_back(m_createPreview);
            m_working.settings.spriteMode = asset::SpriteMode::Multiple;
            m_selected = static_cast<int>(m_working.settings.sprites.size()) - 1;
            m_dirty = true;
        }
        m_dragMode = DragMode::None;
    }

    if (m_selected >= 0 && m_selected < static_cast<int>(m_working.settings.sprites.size())) {
        const asset::SpriteRect& sprite = m_working.settings.sprites[static_cast<size_t>(m_selected)];
        const auto [rectMin, rectMax] = spriteRect(sprite);
        constexpr float HANDLE = 4.0f;
        for (const ImVec2 point : {
                 rectMin,
                 ImVec2{(rectMin.x + rectMax.x) * 0.5f, rectMin.y},
                 ImVec2{rectMax.x, rectMin.y},
                 ImVec2{rectMin.x, (rectMin.y + rectMax.y) * 0.5f},
                 ImVec2{rectMax.x, (rectMin.y + rectMax.y) * 0.5f},
                 ImVec2{rectMin.x, rectMax.y},
                 ImVec2{(rectMin.x + rectMax.x) * 0.5f, rectMax.y},
                 rectMax }) {
            drawList->AddRectFilled(
                { point.x - HANDLE, point.y - HANDLE },
                { point.x + HANDLE, point.y + HANDLE },
                IM_COL32(255, 255, 255, 255));
            drawList->AddRect(
                { point.x - HANDLE, point.y - HANDLE },
                { point.x + HANDLE, point.y + HANDLE },
                IM_COL32(30, 30, 30, 255));
        }
        const ImVec2 pivot = toScreen(
            static_cast<float>(sprite.x) + static_cast<float>(sprite.width) * sprite.pivotX,
            static_cast<float>(sprite.y) + static_cast<float>(sprite.height) * sprite.pivotY);
        drawList->AddLine({ pivot.x - 7.0f, pivot.y }, { pivot.x + 7.0f, pivot.y },
                          IM_COL32(255, 80, 100, 255), 2.0f);
        drawList->AddLine({ pivot.x, pivot.y - 7.0f }, { pivot.x, pivot.y + 7.0f },
                          IM_COL32(255, 80, 100, 255), 2.0f);
        drawList->AddCircle(pivot, 5.0f, IM_COL32(255, 80, 100, 255), 16, 2.0f);

        const float left = toScreen(static_cast<float>(sprite.x) + sprite.borderLeft, 0.0f).x;
        const float right = toScreen(static_cast<float>(sprite.x + sprite.width) - sprite.borderRight, 0.0f).x;
        const float top = toScreen(0.0f, static_cast<float>(sprite.y) + sprite.borderTop).y;
        const float bottom = toScreen(0.0f, static_cast<float>(sprite.y + sprite.height) - sprite.borderBottom).y;
        drawList->AddLine({ left, rectMin.y }, { left, rectMax.y }, IM_COL32(100, 255, 150, 220));
        drawList->AddLine({ right, rectMin.y }, { right, rectMax.y }, IM_COL32(100, 255, 150, 220));
        drawList->AddLine({ rectMin.x, top }, { rectMax.x, top }, IM_COL32(100, 255, 150, 220));
        drawList->AddLine({ rectMin.x, bottom }, { rectMax.x, bottom }, IM_COL32(100, 255, 150, 220));
    }

    if (!m_status.empty()) {
        const ImVec2 statusSize = ImGui::CalcTextSize(m_status.c_str());
        const ImVec2 statusMin = { canvasMin.x + 8.0f, canvasMax.y - statusSize.y - 16.0f };
        drawList->AddRectFilled(
            statusMin,
            { statusMin.x + statusSize.x + 12.0f, statusMin.y + statusSize.y + 8.0f },
            IM_COL32(28, 28, 30, 225), 3.0f);
        drawList->AddText(
            { statusMin.x + 6.0f, statusMin.y + 4.0f },
            IM_COL32(225, 225, 225, 255), m_status.c_str());
    }

    drawList->PopClipRect();
    if (m_selected >= 0
        && m_selected < static_cast<int>(m_working.settings.sprites.size())) {
        ImGui::SetCursorScreenPos({
            std::max(canvasMin.x + 8.0f, canvasMax.x - 318.0f),
            std::max(canvasMin.y + 8.0f, canvasMax.y - 258.0f)
        });
        DrawSpriteRectInspector(textureWidth, textureHeight);
    }
    ImGui::EndChild();
    (void)ctx;
}

// toolbar、一覧、canvasをまとめて専用Sprite Editorウィンドウへ描画する。
void SpriteEditorPanel::OnRenderContent(EditorContext& ctx)
{
    if (m_requestFocus) {
        ImGui::SetWindowFocus();
        m_requestFocus = false;
    }
    if (m_metaPath.empty()) {
        ImGui::TextDisabled("Open a Sprite Texture from its Inspector.");
        return;
    }
    if (ctx.resources == nullptr || ctx.imguiRenderer == nullptr) {
        ImGui::TextDisabled("Renderer is unavailable.");
        return;
    }

    const std::uint64_t resetVersion = ctx.resources->GetResetVersion();
    if (!m_texture.IsValid() || m_textureResetVersion != resetVersion) {
        m_texture = ctx.resources->LoadTexture(m_texturePath);
        m_textureResetVersion = resetVersion;
    }
    const renderer::ITexture* texture = ctx.resources->Get(m_texture);
    void* textureId = m_texture.IsValid()
        ? ctx.imguiRenderer->GetImTextureID(m_texture, *ctx.resources) : nullptr;
    if (texture == nullptr || textureId == nullptr) {
        ImGui::TextColored({ 1.0f, 0.3f, 0.25f, 1.0f }, "Texture preview could not be loaded.");
        return;
    }
    const uint32_t textureWidth = texture->GetWidth();
    const uint32_t textureHeight = texture->GetHeight();
    m_textureWidth = textureWidth;
    m_textureHeight = textureHeight;
    if (textureWidth == 0 || textureHeight == 0) {
        ImGui::TextColored({ 1.0f, 0.3f, 0.25f, 1.0f }, "Texture dimensions are invalid.");
        return;
    }
    // width/height=0 はSingle Spriteの「Texture全体」表現なので、視覚編集用作業コピーだけ実寸へ展開する。
    for (asset::SpriteRect& sprite : m_working.settings.sprites) {
        if (sprite.width == 0) sprite.width = textureWidth - std::min(sprite.x, textureWidth);
        if (sprite.height == 0) sprite.height = textureHeight - std::min(sprite.y, textureHeight);
    }
    const asset::TextureImportSettings settingsBeforeRender = m_working.settings;
    const ValidationResult validation = Validate(textureWidth, textureHeight);
    const bool applyShortcut = m_dirty
        && ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_S);
    bool revertedThisFrame = false;

    // Unity Sprite Editor と同じ順序で、左に Sprite Rect 操作、右に表示・保存操作を置く。
    bool openSlicePopup = false;
    ImGui::BeginChild(
        "##sprite_toolbar", { 0.0f, 38.0f }, false,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Sprite Rect");
    ImGui::SameLine();
    if (ImGui::Button("Slice")) {
        openSlicePopup = true;
        m_slicePopupOpen = true;
        RefreshReferrerCount(ctx);
    }
    ImGui::SameLine();
    const bool openRenamePopup = ImGui::Button("Rename");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("全 Sprite を «接頭辞 + 連番» で付け直します。\n"
                          "名前は参照キーを兼ねるので、人と AI が呼べる名前にしておくと\n"
                          "\"Atlas.png::sprite::Key_W\" と書けるようになります");
    ImGui::SameLine();
    const bool hasSelection = m_selected >= 0
        && m_selected < static_cast<int>(m_working.settings.sprites.size());
    if (!hasSelection) ImGui::BeginDisabled();
    if (ImGui::Button("Trim")) TrimSelected(textureWidth, textureHeight);
    if (!hasSelection) ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Frame All")) {
        m_zoom = 1.0f;
        m_pan = {};
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%zu SpriteRects", m_working.settings.sprites.size());
    if (m_dirty) {
        ImGui::SameLine();
        ImGui::TextColored({ 1.0f, 0.72f, 0.2f, 1.0f }, "Modified");
    }

    const float rightControlsWidth = 330.0f;
    if (ImGui::GetContentRegionAvail().x > rightControlsWidth) {
        ImGui::SameLine(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x
            - rightControlsWidth);
    } else {
        ImGui::SameLine();
    }
    ImGui::SetNextItemWidth(110.0f);
    ImGui::SliderFloat("##Zoom", &m_zoom, 0.1f, 16.0f, "%.2fx",
                       ImGuiSliderFlags_Logarithmic);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Zoom");
    ImGui::SameLine();
    if (!m_dirty) ImGui::BeginDisabled();
    if (ImGui::Button("Revert")) {
        Revert();
        revertedThisFrame = true;
    }
    if (!m_dirty) ImGui::EndDisabled();
    ImGui::SameLine();
    if (!validation.valid || !m_dirty) ImGui::BeginDisabled();
    const bool applyClicked = ImGui::Button("Apply");
    if (!validation.valid || !m_dirty) ImGui::EndDisabled();
    if (validation.valid && (applyClicked || applyShortcut)) Apply(ctx);
    ImGui::EndChild();

    if (openRenamePopup) {
        if (m_renamePrefix[0] == '\0') {
            const std::string stem = util::FileSystem::PathToUtf8(
                util::FileSystem::PathFromUtf8(m_texturePath).stem());
            std::snprintf(m_renamePrefix, sizeof(m_renamePrefix), "%s_", stem.c_str());
        }
        ImGui::OpenPopup("##sprite_rename_popup");
    }
    ImGui::SetNextWindowSize({ 340.0f, 0.0f }, ImGuiCond_Appearing);
    if (ImGui::BeginPopup("##sprite_rename_popup")) {
        ImGui::TextUnformatted("Rename All");
        ImGui::Separator();
        ImGui::SetNextItemWidth(200.0f);
        ImGui::InputText("Prefix", m_renamePrefix, sizeof(m_renamePrefix));
        ImGui::SetNextItemWidth(200.0f);
        ImGui::InputInt("Start", &m_renameStartIndex);
        ImGui::TextDisabled("%s%d, %s%d, …",
                            m_renamePrefix, m_renameStartIndex,
                            m_renamePrefix, m_renameStartIndex + 1);
        // ID は変えないので、既存の参照は名前を付け直しても切れない。
        ImGui::TextDisabled("ID は変わりません (既存の参照はそのまま)");
        ImGui::Separator();
        if (ImGui::Button("Rename", { 100.0f, 0.0f })) {
            RenameAll(m_renamePrefix, m_renameStartIndex);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (openSlicePopup) ImGui::OpenPopup("##slice_popup");
    ImGui::SetNextWindowSize({ 370.0f, 0.0f }, ImGuiCond_Appearing);
    if (ImGui::BeginPopup("##slice_popup")) {
        m_slicePopupOpen = true;
        ImGui::TextUnformatted("Slice");
        ImGui::Separator();
        static constexpr const char* kSliceTypeNames[] = {
            "Automatic", "Grid by Cell Size", "Grid by Cell Count"
        };
        int sliceType = static_cast<int>(m_sliceType);
        ImGui::SetNextItemWidth(210.0f);
        if (ImGui::Combo("Type", &sliceType, kSliceTypeNames, 3))
            m_sliceType = static_cast<SliceType>(sliceType);

        static constexpr const char* kExistingModeNames[] = {
            "Delete Existing", "Smart", "Safe"
        };
        int existingMode = static_cast<int>(m_sliceExistingMode);
        ImGui::SetNextItemWidth(210.0f);
        if (ImGui::Combo("Method", &existingMode, kExistingModeNames, 3))
            m_sliceExistingMode = static_cast<SliceExistingMode>(existingMode);

        // Delete Existing だけは既存の ID を全部捨てる。何が壊れるかをここで言う。
        // Smart / Safe は重なりで ID を引き継ぐので、既存の参照は生き残る。
        if (m_sliceExistingMode == SliceExistingMode::DeleteExisting) {
            if (m_referrerCount > 0) {
                ImGui::TextColored({ 1.0f, 0.45f, 0.3f, 1.0f },
                    "既存の Sprite ID を作り直します:\n参照 %d 件 (%d ファイル) が切れます",
                    m_referrerCount, m_referrerFileCount);
                ImGui::TextDisabled("矩形を引き継ぐなら Smart を選んでください");
            } else if (m_referrerCount == 0) {
                ImGui::TextDisabled("この Sprite を参照しているファイルはありません");
            }
        }

        if (m_sliceType == SliceType::CellCount) {
            ImGui::SetNextItemWidth(210.0f);
            if (ImGui::InputInt("Column", &m_gridColumns, 1, 10))
                m_gridColumns = std::clamp(m_gridColumns, 1, 256);
            ImGui::SetNextItemWidth(210.0f);
            if (ImGui::InputInt("Row", &m_gridRows, 1, 10))
                m_gridRows = std::clamp(m_gridRows, 1, 256);
        } else if (m_sliceType == SliceType::CellSize) {
            ImGui::SetNextItemWidth(210.0f);
            if (ImGui::InputInt("Pixel Size X", &m_cellWidth, 1, 16))
                m_cellWidth = std::clamp(
                    m_cellWidth, 1, static_cast<int>(textureWidth));
            ImGui::SetNextItemWidth(210.0f);
            if (ImGui::InputInt("Pixel Size Y", &m_cellHeight, 1, 16))
                m_cellHeight = std::clamp(
                    m_cellHeight, 1, static_cast<int>(textureHeight));
        }

        if (m_sliceType != SliceType::Automatic) {
            ImGui::SetNextItemWidth(210.0f);
            if (ImGui::InputInt("Offset X", &m_sliceOffsetX, 1, 10))
                m_sliceOffsetX = std::max(0, m_sliceOffsetX);
            ImGui::SetNextItemWidth(210.0f);
            if (ImGui::InputInt("Offset Y", &m_sliceOffsetY, 1, 10))
                m_sliceOffsetY = std::max(0, m_sliceOffsetY);
            ImGui::SetNextItemWidth(210.0f);
            if (ImGui::InputInt("Padding X", &m_slicePaddingX, 1, 10))
                m_slicePaddingX = std::max(0, m_slicePaddingX);
            ImGui::SetNextItemWidth(210.0f);
            if (ImGui::InputInt("Padding Y", &m_slicePaddingY, 1, 10))
                m_slicePaddingY = std::max(0, m_slicePaddingY);
            ImGui::Checkbox("Keep Empty Rects", &m_keepEmptyRects);
        }

        static constexpr const char* kPivotNames[] = {
            "Top Left", "Top", "Top Right", "Left", "Center",
            "Right", "Bottom Left", "Bottom", "Bottom Right", "Custom"
        };
        ImGui::SetNextItemWidth(210.0f);
        if (ImGui::Combo("Pivot", &m_slicePivotPreset, kPivotNames, 10)
            && m_slicePivotPreset < 9) {
            static constexpr float kPivotValues[9][2] = {
                {0.0f, 0.0f}, {0.5f, 0.0f}, {1.0f, 0.0f},
                {0.0f, 0.5f}, {0.5f, 0.5f}, {1.0f, 0.5f},
                {0.0f, 1.0f}, {0.5f, 1.0f}, {1.0f, 1.0f}
            };
            m_slicePivotX = kPivotValues[m_slicePivotPreset][0];
            m_slicePivotY = kPivotValues[m_slicePivotPreset][1];
        }
        if (m_slicePivotPreset == 9) {
            float pivot[2] = { m_slicePivotX, m_slicePivotY };
            ImGui::SetNextItemWidth(210.0f);
            if (ImGui::DragFloat2("Custom Pivot", pivot, 0.01f, 0.0f, 1.0f)) {
                m_slicePivotX = std::clamp(pivot[0], 0.0f, 1.0f);
                m_slicePivotY = std::clamp(pivot[1], 0.0f, 1.0f);
            }
        }
        ImGui::Separator();
        if (ImGui::Button("Slice", { 100.0f, 0.0f })) {
            if (m_sliceType == SliceType::Automatic)
                AutoTrim(textureWidth, textureHeight);
            else
                SliceGrid(textureWidth, textureHeight);
            m_slicePopupOpen = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    } else {
        m_slicePopupOpen = false;
    }

    if (!ImGui::IsAnyItemActive() && hasSelection
        && ImGui::IsKeyPressed(ImGuiKey_Delete)) {
        m_working.settings.sprites.erase(
            m_working.settings.sprites.begin() + m_selected);
        m_selected = std::min(
            m_selected, static_cast<int>(m_working.settings.sprites.size()) - 1);
        m_undoDescription = "Remove SpriteRect";
        m_dirty = true;
    }
    if (!ImGui::IsAnyItemActive() && ImGui::IsKeyPressed(ImGuiKey_F)) {
        m_zoom = 1.0f;
        m_pan = {};
    }
    if (!ImGui::IsAnyItemActive() && ImGui::IsKeyPressed(ImGuiKey_Escape))
        m_selected = -1;

    DrawCanvas(ctx, textureId, textureWidth, textureHeight);

    if (!revertedThisFrame
        && m_working.settings != settingsBeforeRender && !m_undoPending) {
        m_undoBefore = settingsBeforeRender;
        m_undoPending = true;
    }
    if (!revertedThisFrame && m_undoPending
        && !ImGui::IsAnyItemActive() && m_dragMode == DragMode::None) {
        PushUndoSnapshot(
            ctx, m_undoBefore, m_working.settings, m_undoDescription);
        m_undoPending = false;
        m_undoDescription = "Edit Sprite";
    }
}

} // namespace fbzz::editor
