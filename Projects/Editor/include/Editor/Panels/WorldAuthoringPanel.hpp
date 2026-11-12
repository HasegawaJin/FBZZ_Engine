/// @file    WorldAuthoringPanel.hpp
/// @brief   World cell and PCG editing UI backed exclusively by registered operations.
/// @author  Hasegawa Jin
/// @date    2026-11-07
#pragma once
#include <Editor/Panels/EditorToolPanel.hpp>
#include <Editor/Op/EditorOperator.hpp>
#include <Engine/Scene/WorldPcg.hpp>
#include <cstdint>
#include <optional>
#include <string>

namespace fbzz::editor {

class WorldAuthoringPanel final : public EditorToolPanel {
public:
    const char* GetWindowName() const override { return "World / PCG"; }
    bool GetDefaultVisibility() const override { return false; }
    void OnUpdate(EditorContext& context, float deltaTime) override;
    void DrawSceneDebug(EditorContext& context, const ImVec2& origin, const ImVec2& size) const;

protected:
    const char* GetEditorType() const override { return "World / PCG"; }
    void OnRenderContent(EditorContext& context) override;

private:
    void SyncContext(EditorContext& context);
    void InspectPartition(EditorContext& context);
    void RefreshStreaming(EditorContext& context);
    void ReadPartitionSettings(const OpData& data);
    void DrawPartition(EditorContext& context);
    void DrawPcg(EditorContext& context);
    void DrawPcgProfile(EditorContext& context);
    bool DrawPcgProfileRules(EditorContext& context, scene::WorldPcgProfile& profile);
    bool ReadPcgProfileDraft(const OpData& data);
    void DrawStreaming(EditorContext& context);
    void ClearPreview();
    void RefreshPcgPreview(EditorContext& context, bool automatic);
    void DrawPcgDebugControls(EditorContext& context);
    OpArgs PartitionArgs() const;
    OpArgs PreviewArgs() const;
    bool Run(EditorContext& context, const char* operation, const OpArgs& args, OpData* output = nullptr);
    bool ActionButton(EditorContext& context, const char* label, const char* operation,
                      const OpArgs& args, OpData* output = nullptr);

    std::string m_project;
    std::uint64_t m_sceneGeneration = 0;
    bool m_initialized = false;
    float m_refreshClock = 0;
    char m_partition[64]{};
    char m_volume[64]{};
    char m_manifest[512] = "Assets/World/World.worldcells";
    std::string m_profile;
    std::string m_profileFile;
    std::string m_profileLoadedPath;
    std::optional<scene::WorldPcgProfile> m_profileDraft;
    char m_profileText[65536]{};
    std::size_t m_selectedRule = 0;
    bool m_editProfileText = false;
    bool m_profileDirty = false;
    bool m_volumeDraft = false;
    std::string m_rockSource;
    std::string m_pebbleSource;
    std::string m_profileExpected;
    std::string m_expected;
    std::string m_message;
    std::string m_error;
    std::string m_streamError;
    std::string m_details;
    OpData m_partitionData;
    OpData m_previewData;
    OpData m_volumeBounds;
    std::uint64_t m_previewUndoRevision = 0;
    bool m_livePreview = true;
    bool m_previewPending = false;
    float m_previewDelay = 0.0f;
    std::string m_previewStatus;
    bool m_showPcgBounds = true;
    bool m_showBiomeBounds = false;
    bool m_showAccepted = true;
    bool m_showRejected = false;
    std::string m_debugRule;
    std::string m_debugReason;
    OpData m_streamData;
    float m_origin[3]{};
    float m_cellSize = 256;
    float m_nearDistance = 384;
    float m_hlodDistance = 2048;
    float m_hysteresis = 64;
    int m_reads = 4;
    int m_commits = 2;
    bool m_streamInEditor = false;
    float m_boundsMin[3]{};
    float m_boundsMax[3] = {256, 0, 256};
    bool m_overrideBounds = false;
    bool m_partialUpdate = false;
    float m_updateBoundsMin[3]{};
    float m_updateBoundsMax[3] = {256, 0, 256};
    int m_seed = -1;
    bool m_protectManual = true;
    bool m_enabled = true;
    int m_maxPoints = 256;
    bool m_includeScene = false;
    bool m_includeTerrain = false;
    bool m_includeLightProbes = false;
    bool m_omitTerrainCollisionInHlod = false;
    bool m_buildHlod = true;
    int m_cell[2]{};
    float m_source[3]{};
    bool m_overrideSource = false;
};

} /// @note namespace fbzz::editor
