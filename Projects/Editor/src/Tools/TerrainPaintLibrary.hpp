/// @file    TerrainPaintLibrary.hpp
/// @brief   TerrainTool の素材 Browser、3D サムネイルと作成・設定画面。
/// @author  Hasegawa Jin
/// @date    2026-11-05
#pragma once
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace fbzz::editor {
struct EditorContext;

/// @note GPU 資源より先に破棄する。選択は Hierarchy / Asset Browser の選択から独立する。
class TerrainPaintLibrary {
public:
    TerrainPaintLibrary();
    ~TerrainPaintLibrary();
    void OpenCreate(const std::string& directory);
    [[nodiscard]] bool Draw(EditorContext& context, std::span<const std::string> selected, std::vector<std::string>& next);
    void DrawInspector(EditorContext& context, const std::string& path);
    [[nodiscard]] void* Thumbnail(EditorContext& context, const std::string& reference);
    [[nodiscard]] void* MixThumbnail(EditorContext& context, const std::string& key,
        std::span<const std::string> sources, std::span<const float> weights);
    [[nodiscard]] void* GroundThumbnail(EditorContext& context, const std::string& reference);
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

void DrawTerrainPaintAssetInspector(EditorContext& context, const std::string& path);
void DrawMeshInstancesAssetInspector(EditorContext& context, const std::string& path);
} /// @note namespace fbzz::editor
