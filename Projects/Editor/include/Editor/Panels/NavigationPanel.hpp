/// @file    NavigationPanel.hpp
/// @brief   シーン内の NavMesh Surface を一覧し、ベイク・診断・オーバーレイ設定をまとめるパネル。
/// @author  Hasegawa Jin
/// @date    2026-08-23
///
/// @note ベイクの入口が Inspector しか無く、「どの GO に Surface が付いているか」を知らないと
///       たどり着けなかった (MCP の navmesh_bake だけが持っていた)。一覧・一括ベイク・古さの検知・
///       失敗理由を 1 か所へ集める。
#pragma once
#include <Editor/Panels/IPanel.hpp>
#include <Engine/Scene/Entity.hpp>
#include <cstdint>
#include <unordered_map>

namespace fbzz::editor {

class NavigationPanel final : public IPanel {
public:
    const char* GetWindowName()        const override { return "Navigation"; }
    bool        GetDefaultVisibility() const override { return false; }

protected:
    void OnRenderContent(EditorContext& ctx) override;

private:
    void DrawToolbar(EditorContext& ctx);
    void DrawOverlaySettings(EditorContext& ctx);
    void DrawSurfaceList(EditorContext& ctx);
    void DrawAgentDiagnostics(EditorContext& ctx);

    /// ベイクソースのハッシュは Terrain の高さ全体を畳むため、パネルが開いている間だけ
    /// 間引いて更新する。キーは EntityID::index。
    void RefreshStaleCache(EditorContext& ctx);

    std::unordered_map<uint32_t, uint64_t> m_sourceHashes;
    double m_lastHashTime = -1.0;
};

} // namespace fbzz::editor
