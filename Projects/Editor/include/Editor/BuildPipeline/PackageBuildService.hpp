/// @file    PackageBuildService.hpp
/// @brief   パネル・AI・batchで共有する配布ビルドと事前確認。
/// @author  Hasegawa Jin
/// @date    2026-10-04
#pragma once

#include <Editor/BuildPipeline/BuildPipeline.hpp>
#include <Editor/BuildPipeline/BuildSettings.hpp>
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
        Level level = Level::OK; ///< 事前確認の結果の重大度。失敗はビルドの開始を妨げる。
        std::string label; ///< 事前確認一覧へ表示する検査項目の名前。
        std::string detail; ///< 確認結果の理由や調査済みのパスを示すメッセージ。
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
    BuildSettings m_settings; ///< UI と操作が共有する配布設定。開始した要求の正規化済み設定を保持する。
    BuildPipeline m_pipeline; ///< 現在の配布ビルドを進める、サービス所有のパイプライン。
    std::vector<Check> m_checks; ///< 直近の候補設定に対する事前確認の結果一覧。
    std::string m_standaloneExeDir; ///< 事前確認で解決した既存 Standalone 成果物の親ディレクトリ。
    std::string m_lastOutputDir; ///< 現在または直近に開始した要求が指定した配布先の絶対パス。
    std::string m_requestId; ///< 表示・問い合わせで現在の配布要求を識別する ID。未開始なら空。
    std::uint64_t m_nextRequest = 0; ///< 新しい配布要求 ID の発行に使う単調増加カウンター。
    bool m_cancelled = false; ///< 現在の要求が失敗ではなく中止されたことを結果に反映するフラグ。
    bool m_launchAttempted = false; ///< ビルド完了後の exe 起動をすでに試したか。
    bool m_launched = false; ///< 完了後の配布用 exe の起動に成功したか。
    bool m_managedRequired = false; ///< 現在の配布要求で C# のビルド成果物とマニフェストが必要か。
};

/// @note 操作本体はUI非依存。設定変更は開始時の候補として渡す。
void RegisterPackageBuildOperators(OperatorRegistry& registry);

} // namespace fbzz::editor
