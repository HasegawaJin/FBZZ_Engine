/// @file    ManagedSettingsReflector.hpp
/// @brief   C# 設定の ImGui 編集を非公開 draft に集める。
/// @author  Hasegawa Jin
/// @date    2026-10-04
#pragma once
#include <Editor/ImGuiReflector.hpp>
#include <Editor/Util/ManagedSettingsCodec.hpp>
#include <Editor/Util/ManagedScriptSettings.hpp>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace fbzz::scene { class ManagedScriptAdapter; }
namespace fbzz::tests { struct ManagedSettingsReflectorTestAccess; }
namespace fbzz::editor {
/// @note 短い状態を常設し、長い診断とコピー操作は明示的に展開したときだけ描く。
void DrawManagedDiagnostic(const char* title, std::string_view message, std::string_view secondary = {});
struct ManagedSettingsDraft {
    ~ManagedSettingsDraft();
    ManagedSettingsTarget target;
    std::vector<ManagedSettingsTarget> targets;
    std::vector<std::string> targetRevisions;
    std::vector<uint64_t> targetCounters;
    std::unordered_set<std::string> mixedPaths;
    std::unordered_set<std::string> adoptionPaths;
    std::unordered_map<std::string, OpData> values;
    std::unordered_set<std::string> rebindReferences;
    OpData createPatches = OpData::MakeArray();
    unsigned int activeId = 0;
    std::string diagnostic;
    std::string staleReason;
    std::string revision;
    OpData catalog;
    std::uint64_t settingsRevision = 0;
    std::uint64_t lastUsed = 0;
    uint64_t editSession = 0;
    std::string registryKey;
    std::string projectRoot;
    std::unordered_map<std::string, OpData> pages;
    std::unordered_map<std::string, OpData> schemas;
    std::unordered_map<std::string, ManagedSettingsPageCursor> pageCursors;
    std::unordered_map<std::string, float> fieldHeights;
    std::string activeFieldPath;
    unsigned int layoutActiveId = 0;
    float layoutWidth = 0.0f;
    float layoutFontSize = 0.0f;
    bool layoutAdoption = false;
    std::unordered_set<std::string> invalidatedPagePaths;
    std::vector<std::shared_ptr<ManagedSettingsDraft>> staleDrafts;
    mutable std::string staleReviewJson;
    mutable bool staleReviewAttempted = false;
    bool revisionConflict = false;
    bool commitAttempted = false;
    bool selectAdoptionPaths = false;
};
/// @note Schema と JSON snapshot を描画し、保存用 IReflector の scalar dispatch を使わない。
/// @note 編集の終了時に exact instance と変更 key を共通 Configure Op へ渡す。
class ManagedSettingsReflector final : public ImGuiReflector {
public:
    ManagedSettingsReflector(EditorContext& context, scene::GameObject& owner, const scene::ScriptEntry& entry);
    /// @pre Selection is nonempty and contains exact instances of the same schema type/generation.
    ManagedSettingsReflector(EditorContext& context, const std::vector<std::pair<scene::GameObject*, const scene::ScriptEntry*>>& selection);
    void Draw();
    void Finish();
    void DrawActions();
    /// @brief Explicitly discards preserved input from an older Scene or managed generation.
    void DiscardStaleDrafts();
    /// @return JSON for copying preserved stale input; empty when serialization fails.
    [[nodiscard]] std::string CopyStaleDrafts() const;
private:
    /// @note TestAccess checks closed observation reads and exact reference labels that saved snapshots cannot expose; it stays local to Inspector tests.
    friend struct fbzz::tests::ManagedSettingsReflectorTestAccess;
    void DrawFields(bool settings);
    std::string ComponentReferenceLabel(const OpData& schema, const OpData& address) const;
    const OpData* ReadPage(std::string_view path, uint64_t start, uint32_t count);
    /// @note 非表示の値は取得せず実測高さだけ進める。編集中の入力とフォーカス先は保持する。
    bool BeginVisiblePath(const std::string& path);
    void EndVisiblePath(const std::string& path, float startY, unsigned int activeBefore);
    void DrawPath(std::string_view path, const char* label, const OpData& schema, const OpData& summary, bool setting, size_t depth = 0);
    bool BeginEdit();
    bool DrawComponentReference(ImGuiReflector& ui, const char* name, const OpData& schema, OpData& value);
    bool DrawTypedAssetReference(ImGuiReflector& ui, const char* name, int assetKind, OpData& value);
    void QueuePatch(OpData patch);
    EditorContext& m_context;
    std::shared_ptr<ManagedSettingsDraft> m_draft;
    std::unordered_set<std::string> m_settingKeys;
    std::string m_group;
    scene::ManagedScriptAdapter* m_adapter = nullptr;
    std::vector<scene::ManagedScriptAdapter*> m_adapters;
};
} /// @note namespace fbzz::editor
