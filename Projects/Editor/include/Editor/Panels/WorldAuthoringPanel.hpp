/// @file    WorldAuthoringPanel.hpp
/// @brief   World cell and PCG editing UI backed exclusively by registered operations.
/// @author  Hasegawa Jin
/// @date    2026-11-07
#pragma once
#include <Editor/Panels/EditorToolPanel.hpp>
#include <Editor/Op/EditorOperator.hpp>
#include <cstdint>
#include <string>

namespace fbzz::editor {

class WorldAuthoringPanel final : public EditorToolPanel {
public:
    const char* GetWindowName() const override { return "World / PCG"; }
    bool GetDefaultVisibility() const override { return false; }
    void OnUpdate(EditorContext& context, float deltaTime) override;

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
    void DrawStreaming(EditorContext& context);
    void ClearPreview();
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
    char m_profile[512]{};
    char m_profileText[65536]{};
    std::string m_profileExpected;
    std::string m_expected;
    std::string m_message;
    std::string m_error;
    std::string m_streamError;
    std::string m_details;
    OpData m_partitionData;
    OpData m_previewData;
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
    int m_seed = -1;
    bool m_protectManual = true;
    bool m_enabled = true;
    int m_maxPoints = 256;
    bool m_includeScene = false;
    bool m_buildHlod = true;
    int m_cell[2]{};
    float m_source[3]{};
    bool m_overrideSource = false;
};

} /// @note namespace fbzz::editor
