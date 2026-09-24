/// @file    PlaytestRunner.hpp
/// @brief   Playtest シナリオ (*.playtest.json) をフレームごとに進め、合否とレポートを出す。
/// @author  Hasegawa Jin
/// @date    2026-09-17
/// @see     Docs/design/ai-verification-loop.md «2. Playtest»
#pragma once
#include <Editor/Ai/Json.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace fbzz::editor::playtest {

enum class PlaytestState { IDLE, RUNNING, PASSED, FAILED };

struct PlaytestOptions {
    /// @brief 基準画像を実画像で作り直す。比較はせず updated として記録する。
    bool updateBaselines = false;
    /// @brief compareImage を飛ばす (GPU の無い CI / WARP)。
    bool skipImages = false;
};

/// @brief 実行環境への口。EditorApp が用意する。
struct PlaytestHooks {
    /// @brief NDJSON 要求 1 行 → 応答 1 行。EditorBusDispatcher::Handle と同じ契約。
    std::function<std::string(const std::string& requestLine)> bus;
    /// @brief view ("scene" / "game" / "fluidBaked") の RT を PNG で読む。
    std::function<bool(std::string_view view, std::vector<uint8_t>& png)> capture;
    /// @brief Fluid の撮影設定を読み込み、GPU 再生画面を用意する。失敗時は error を返す。
    std::function<bool(const ai::JsonValue& step, std::string& error)> prepareFluidPlayback;
    /// @brief 隠れているビューポートも描かせ続ける (撮影の数フレーム前から呼ぶ)。
    std::function<void()> keepViewportsRendering;
};

class PlaytestRunner {
public:
    /// @brief シナリオを検証して開始する。実行中なら拒否する。
    /// @param scenarioPath 相対パス (replay の入力列) の基準。インラインなら空。
    /// @param projectRoot  基準画像 (`projectRoot/Tests/Golden`) の置き場の基準。
    /// @param outputDirectory レポートと実画像・差分画像の出力先。
    /// @return 形式が不正・実行中なら false と error。
    bool Start(const ai::JsonValue& scenario, const std::filesystem::path& scenarioPath,
               const std::filesystem::path& projectRoot, const std::filesystem::path& outputDirectory,
               PlaytestOptions options, std::string& error);

    /// @brief 実行中なら失敗として打ち切る。
    void Cancel(const std::string& reason);

    /// @brief 1 フレーム進める。入力の評価前 (IModule::OnInputPolled) に毎フレーム呼ぶ。
    /// @note 待機を伴わない手順は同じフレームで続けて実行する。
    void Tick(const PlaytestHooks& hooks);

    [[nodiscard]] PlaytestState State() const { return m_state; }
    [[nodiscard]] bool IsRunning() const { return m_state == PlaytestState::RUNNING; }
    /// @brief 現在の手順が Fluid 専用 RT の撮影か。
    [[nodiscard]] bool IsFluidPlaybackCaptureStep() const;
    /// @brief 実行中は途中経過、終了後は最終結果。
    [[nodiscard]] ai::JsonValue Report() const;
    [[nodiscard]] const std::filesystem::path& ReportPath() const { return m_reportPath; }

private:
    enum class StepResult { DONE, WAIT, FAIL };

    StepResult RunStep(const ai::JsonValue& step, const PlaytestHooks& hooks);
    StepResult RunQueryCondition(const ai::JsonValue& step, const PlaytestHooks& hooks, bool waiting);
    StepResult RunCompareImage(const ai::JsonValue& step, const PlaytestHooks& hooks, bool captureOnly);
    StepResult RunReplay(const ai::JsonValue& step, const PlaytestHooks& hooks);

    /// @return 応答の ok。result / error はそれぞれへ書く。
    bool CallBus(const PlaytestHooks& hooks, const char* kind, const ai::JsonValue& payload,
                 ai::JsonValue& result, std::string& errorCode, std::string& errorMessage);
    void Finish(PlaytestState state, const PlaytestHooks* hooks);
    /// @param result 非 null なら報告の手順に "result" として残す (bus の query の応答)。
    void RecordStep(bool ok, const std::string& message, const ai::JsonValue* result = nullptr);
    StepResult Fail(const std::string& message);

    PlaytestState         m_state = PlaytestState::IDLE;
    PlaytestOptions       m_options;
    ai::JsonValue         m_scenario;
    std::string           m_name;
    std::filesystem::path m_scenarioDirectory;
    std::filesystem::path m_goldenDirectory;
    std::filesystem::path m_outputDirectory;
    std::filesystem::path m_reportPath;

    size_t   m_stepIndex = 0;
    uint64_t m_frame = 0;
    bool     m_prepared = false;
    /// @note 手順の途中状態。次の手順へ進むときに必ず 0 / 空へ戻す。
    uint64_t m_waitFrames = 0;
    uint64_t m_stepFrame = 0;
    int      m_stepPhase = 0;
    ai::JsonValue m_replayEvents;
    size_t   m_replayCursor = 0;
    uint64_t m_requestSerial = 0;

    ai::JsonValue m_stepLog = ai::JsonValue::MakeArray();
    ai::JsonValue m_images = ai::JsonValue::MakeArray();
    std::string   m_failure;
};

} // namespace fbzz::editor::playtest
