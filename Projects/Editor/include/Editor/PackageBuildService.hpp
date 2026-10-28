/// @file    PackageBuildService.hpp
/// @brief   パネル・AI・batchで共有する配布ビルドと事前確認。
/// @author  Hasegawa Jin
/// @date    2026-10-04
#pragma once

#include <Editor/BuildPipeline.hpp>
#include <Editor/BuildSettings.hpp>
#include <Editor/Op/OpData.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace fbzz::editor {

struct EditorContext;
class OperatorRegistry;

/// @note EditorAppが所有し、隠れたパネルも同じ設定と処理結果を参照する。
class PackageBuildService final {
public:
    struct Check {
        enum class Level { OK, WARNING, FAILED };
        Level level = Level::OK;
        std::string label;
        std::string detail;
    };

    [[nodiscard]] BuildSettings& Settings() { return m_settings; }
    [[nodiscard]] const BuildSettings& Settings() const { return m_settings; }
    [[nodiscard]] const BuildPipeline& Pipeline() const { return m_pipeline; }
    [[nodiscard]] const std::vector<Check>& Checks() const { return m_checks; }
    [[nodiscard]] bool IsRunning() const { return m_pipeline.GetState() == BuildPipeline::State::Running; }
    [[nodiscard]] const std::string& RequestId() const { return m_requestId; }
    [[nodiscard]] const std::string& LastOutputDir() const { return m_lastOutputDir; }

    /// @note ディスク事前確認は全入口で共通。候補設定は実行成功時まで保存設定を変えない。
    void RefreshChecks(const EditorContext& ctx, const BuildSettings& settings);
    [[nodiscard]] bool CanStart(const EditorContext& ctx, std::string& code, std::string& reason) const;
    [[nodiscard]] bool Start(EditorContext& ctx, const BuildSettings& settings, bool runAfterBuild,
        std::string& code, std::string& reason);
    /// @note Editor update専用。描画の有無に依存せず進め、Build and Runは完了後一度だけ起動する。
    void Tick();
    void Cancel();
    /// @note 実行中のResetは受理しない。終端要求を消す明示UI操作だけに使う。
    void Reset();
    [[nodiscard]] OpData Status() const;
    [[nodiscard]] OpData CheckStatus() const;

private:
    BuildSettings m_settings;
    BuildPipeline m_pipeline;
    std::vector<Check> m_checks;
    std::string m_standaloneExeDir;
    std::string m_lastOutputDir;
    std::string m_requestId;
    std::uint64_t m_nextRequest = 0;
    bool m_cancelled = false;
    bool m_launchAttempted = false;
    bool m_launched = false;
    bool m_managedRequired = false;
};

/// @note 操作本体はUI非依存。設定変更は開始時の候補として渡す。
void RegisterPackageBuildOperators(OperatorRegistry& registry);

} /// @note namespace fbzz::editor
