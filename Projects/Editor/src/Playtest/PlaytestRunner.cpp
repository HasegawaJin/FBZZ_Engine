/// @file    PlaytestRunner.cpp
/// @brief   Playtest シナリオのフレーム駆動実行。手順の語彙は Command Bus の要求そのもの。
/// @author  Hasegawa Jin
/// @date    2026-09-17
#include <Editor/Playtest/PlaytestRunner.hpp>

#include <Editor/Ai/EditorBusProtocol.hpp>
#include <Editor/Playtest/ImageCompare.hpp>
#include <Editor/Playtest/PlaytestJson.hpp>
#include <Engine/Core/Time.hpp>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <optional>
#include <system_error>

namespace fbzz::editor::playtest {

namespace {

using ai::JsonValue;

/// @note 1 フレームに続けて実行する手順の上限。待機の無い手順が延々と並んだシナリオでもフレームを止めないため。
constexpr size_t kMaxStepsPerFrame = 64;
constexpr uint64_t kDefaultWaitTimeoutFrames = 600;

std::string StringOf(const JsonValue& object, const char* key, const std::string& fallback = {})
{
    const JsonValue* value = object.Find(key);
    return (value != nullptr && value->IsString()) ? value->AsString() : fallback;
}

double NumberOf(const JsonValue& object, const char* key, double fallback)
{
    const JsonValue* value = object.Find(key);
    return (value != nullptr && value->IsNumber()) ? value->AsNumber() : fallback;
}

/// @brief 基準画像名はファイル名として安全な文字だけ許す (シナリオから Tests/Golden の外へ書かせない)。
bool IsSafeImageName(const std::string& name)
{
    if (name.empty() || name.size() > 128 || name.find("..") != std::string::npos) return false;
    return std::all_of(name.begin(), name.end(), [](char character) {
        return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z')
            || (character >= '0' && character <= '9') || character == '_' || character == '-' || character == '/';
    });
}

std::optional<JsonValue> ReadJsonFile(const std::filesystem::path& path, std::string& error)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream) { error = "開けない: " + path.generic_string(); return std::nullopt; }
    const std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    std::string parseError;
    std::optional<JsonValue> value = ai::ParseJson(text, &parseError);
    if (!value.has_value()) error = path.generic_string() + ": " + parseError;
    return value;
}

} // namespace

bool PlaytestRunner::Start(const JsonValue& scenario, const std::filesystem::path& scenarioPath,
                           const std::filesystem::path& projectRoot, const std::filesystem::path& outputDirectory,
                           PlaytestOptions options, std::string& error)
{
    if (IsRunning()) { error = "別の Playtest が実行中です"; return false; }
    if (!scenario.IsObject()) { error = "シナリオはオブジェクトである必要があります"; return false; }
    const JsonValue* steps = scenario.Find("steps");
    if (steps == nullptr || !steps->IsArray() || steps->AsArray().empty()) { error = "steps (空でない配列) が必要です"; return false; }
    for (size_t index = 0; index < steps->AsArray().size(); ++index) {
        const JsonValue& step = steps->AsArray()[index];
        if (!step.IsObject() || StringOf(step, "do").empty()) {
            error = "steps[" + std::to_string(index) + "] に do がありません";
            return false;
        }
    }

    m_scenario = scenario;
    m_options = options;
    m_name = StringOf(scenario, "name", scenarioPath.empty() ? std::string("inline") : scenarioPath.stem().stem().string());
    m_scenarioDirectory = scenarioPath.empty() ? projectRoot : scenarioPath.parent_path();
    m_goldenDirectory = projectRoot / "Tests" / "Golden";
    m_outputDirectory = outputDirectory;
    m_reportPath = outputDirectory / "report.json";

    m_state = PlaytestState::RUNNING;
    m_stepIndex = 0;
    m_frame = 0;
    m_prepared = false;
    m_waitFrames = 0;
    m_stepFrame = 0;
    m_stepPhase = 0;
    m_replayEvents = JsonValue::MakeArray();
    m_replayCursor = 0;
    m_stepLog = JsonValue::MakeArray();
    m_images = JsonValue::MakeArray();
    m_failure.clear();
    return true;
}

void PlaytestRunner::Cancel(const std::string& reason)
{
    if (!IsRunning()) return;
    m_failure = reason.empty() ? std::string("キャンセルされました") : reason;
    Finish(PlaytestState::FAILED, nullptr);
}

bool PlaytestRunner::CallBus(const PlaytestHooks& hooks, const char* kind, const JsonValue& payload,
                             JsonValue& result, std::string& errorCode, std::string& errorMessage)
{
    JsonValue request = JsonValue::MakeObject();
    request.Set("protocol", JsonValue(ai::kEditorProtocol));
    request.Set("id", JsonValue("playtest-" + std::to_string(++m_requestSerial)));
    request.Set("kind", JsonValue(kind));
    request.Set("payload", payload);
    request.Set("dryRun", JsonValue(false));

    const std::string responseLine = hooks.bus ? hooks.bus(ai::SerializeJson(request)) : std::string{};
    std::string parseError;
    std::optional<JsonValue> response = ai::ParseJson(responseLine, &parseError);
    if (!response.has_value()) {
        errorCode = "BAD_RESPONSE";
        errorMessage = "バスの応答を解読できません: " + parseError;
        return false;
    }
    const JsonValue* ok = response->Find("ok");
    if (ok != nullptr && ok->AsBool()) {
        const JsonValue* value = response->Find("result");
        result = value != nullptr ? *value : JsonValue::MakeObject();
        return true;
    }
    if (const JsonValue* error = response->Find("error"); error != nullptr && error->IsObject()) {
        errorCode = StringOf(*error, "code", "ERROR");
        errorMessage = StringOf(*error, "message");
    } else {
        errorCode = "ERROR";
    }
    return false;
}

void PlaytestRunner::RecordStep(bool ok, const std::string& message)
{
    const JsonValue* steps = m_scenario.Find("steps");
    JsonValue entry = JsonValue::MakeObject();
    entry.Set("index", JsonValue(static_cast<double>(m_stepIndex)));
    if (steps != nullptr && m_stepIndex < steps->AsArray().size())
        entry.Set("do", JsonValue(StringOf(steps->AsArray()[m_stepIndex], "do")));
    entry.Set("frame", JsonValue(static_cast<double>(m_frame)));
    entry.Set("ok", JsonValue(ok));
    if (!message.empty()) entry.Set("message", JsonValue(message));
    m_stepLog.Push(std::move(entry));
}

PlaytestRunner::StepResult PlaytestRunner::Fail(const std::string& message)
{
    m_failure = message;
    RecordStep(false, message);
    return StepResult::FAIL;
}

void PlaytestRunner::Tick(const PlaytestHooks& hooks)
{
    if (!IsRunning()) return;
    ++m_frame;
    if (hooks.keepViewportsRendering) hooks.keepViewportsRendering();

    if (!m_prepared) {
        m_prepared = true;
        const double lockstep = NumberOf(m_scenario, "lockstep", 1.0 / 60.0);
        fbzz::Time::SetLockstepDelta(static_cast<float>(lockstep));
        if (const std::string scene = StringOf(m_scenario, "scene"); !scene.empty()) {
            JsonValue payload = JsonValue::MakeObject();
            payload.Set("t", JsonValue("scene.open"));
            payload.Set("path", JsonValue(scene));
            payload.Set("discardUnsaved", JsonValue(true));
            JsonValue result;
            std::string code;
            std::string message;
            if (!CallBus(hooks, "command", payload, result, code, message)) {
                m_failure = "シーンを開けません (" + code + "): " + message;
                Finish(PlaytestState::FAILED, &hooks);
                return;
            }
            /// @note 開いた直後のフレームはリソースの作り直し中。2 フレーム置いてから手順に入る。
            m_waitFrames = 2;
        }
    }

    if (m_waitFrames > 0) { --m_waitFrames; return; }

    const JsonValue::Array& steps = m_scenario.Find("steps")->AsArray();
    for (size_t executed = 0; executed < kMaxStepsPerFrame && m_stepIndex < steps.size(); ++executed) {
        const StepResult result = RunStep(steps[m_stepIndex], hooks);
        if (result == StepResult::WAIT) return;
        if (result == StepResult::FAIL) { Finish(PlaytestState::FAILED, &hooks); return; }

        ++m_stepIndex;
        m_stepFrame = 0;
        m_stepPhase = 0;
        m_replayEvents = JsonValue::MakeArray();
        m_replayCursor = 0;
        if (m_waitFrames > 0) return;
    }
    if (m_stepIndex >= steps.size()) Finish(PlaytestState::PASSED, &hooks);
}

PlaytestRunner::StepResult PlaytestRunner::RunStep(const JsonValue& step, const PlaytestHooks& hooks)
{
    const std::string action = StringOf(step, "do");
    JsonValue result;
    std::string code;
    std::string message;

    const auto playControl = [&](const char* playAction, uint64_t settleFrames) {
        JsonValue payload = JsonValue::MakeObject();
        payload.Set("t", JsonValue("play.control"));
        payload.Set("action", JsonValue(playAction));
        if (!CallBus(hooks, "command", payload, result, code, message))
            return Fail(std::string("play.control ") + playAction + " 失敗 (" + code + "): " + message);
        m_waitFrames = settleFrames;
        RecordStep(true, {});
        return StepResult::DONE;
    };

    if (action == "play") return playControl("start", 1);
    /// @note Stop の復元は次フレームの ApplyPendingRestore で起きる。2 フレーム置いてから次へ進む。
    if (action == "stop") return playControl("stop", 2);
    if (action == "pause") return playControl("pause", 0);
    if (action == "resume") return playControl("resume", 0);

    if (action == "frames") {
        const double count = NumberOf(step, "count", 1.0);
        if (count < 0.0 || count > 1000000.0) return Fail("frames.count が範囲外です");
        m_waitFrames = static_cast<uint64_t>(count);
        RecordStep(true, {});
        return StepResult::DONE;
    }

    if (action == "log") {
        RecordStep(true, StringOf(step, "message"));
        return StepResult::DONE;
    }

    if (action == "bus" || action == "input") {
        JsonValue payload;
        if (action == "input") {
            const JsonValue* inject = step.Find("inject");
            if (inject == nullptr || !inject->IsObject()) return Fail("input には inject オブジェクトが必要です");
            payload = *inject;
            payload.Set("t", JsonValue("input.inject"));
        } else {
            const JsonValue* request = step.Find("request");
            if (request == nullptr || !request->IsObject() || StringOf(*request, "t").empty())
                return Fail("bus には t を持つ request が必要です");
            payload = *request;
        }
        const std::string kind = StringOf(step, "kind", "command");
        if (kind != "command" && kind != "query") return Fail("kind は command か query です");
        const bool ok = CallBus(hooks, kind.c_str(), payload, result, code, message);
        const std::string expectError = StringOf(step, "expectError");
        if (!expectError.empty()) {
            if (ok) return Fail(StringOf(payload, "t") + " は " + expectError + " で失敗するはずが成功した");
            if (code != expectError) return Fail(StringOf(payload, "t") + " の失敗コードが " + code + " (期待 " + expectError + ")");
            RecordStep(true, "expected " + code);
            return StepResult::DONE;
        }
        if (!ok) return Fail(StringOf(payload, "t") + " 失敗 (" + code + "): " + message);
        RecordStep(true, {});
        return StepResult::DONE;
    }

    if (action == "assert") return RunQueryCondition(step, hooks, false);
    if (action == "waitUntil") return RunQueryCondition(step, hooks, true);
    if (action == "replay") return RunReplay(step, hooks);
    if (action == "capture") return RunCompareImage(step, hooks, true);
    if (action == "compareImage") return RunCompareImage(step, hooks, false);

    return Fail("未知の手順: " + action);
}

PlaytestRunner::StepResult PlaytestRunner::RunQueryCondition(const JsonValue& step, const PlaytestHooks& hooks, bool waiting)
{
    const JsonValue* query = step.Find("query");
    if (query == nullptr || !query->IsObject() || StringOf(*query, "t").empty())
        return Fail("query (t を持つオブジェクト) が必要です");
    const std::string kind = StringOf(step, "kind", "query");
    const std::string path = StringOf(step, "path");
    const std::string op = StringOf(step, "op", "exists");

    JsonValue result;
    std::string code;
    std::string message;
    std::string detail;
    bool satisfied = false;
    if (CallBus(hooks, kind.c_str(), *query, result, code, message)) {
        satisfied = EvaluateJsonCondition(ResolveJsonPath(result, path), op, step.Find("value"), detail);
    } else {
        detail = StringOf(*query, "t") + " 失敗 (" + code + "): " + message;
    }

    if (satisfied) {
        RecordStep(true, waiting ? "satisfied after " + std::to_string(m_stepFrame) + " frames" : std::string{});
        return StepResult::DONE;
    }
    if (!waiting) return Fail(StringOf(*query, "t") + " " + path + ": " + detail);

    const uint64_t timeout = static_cast<uint64_t>(NumberOf(step, "timeoutFrames", static_cast<double>(kDefaultWaitTimeoutFrames)));
    if (++m_stepFrame > timeout)
        return Fail("waitUntil が " + std::to_string(timeout) + " フレームで成立しない: " + StringOf(*query, "t") + " " + path + ": " + detail);
    return StepResult::WAIT;
}

PlaytestRunner::StepResult PlaytestRunner::RunReplay(const JsonValue& step, const PlaytestHooks& hooks)
{
    if (m_stepPhase == 0) {
        const std::string file = StringOf(step, "file");
        if (file.empty()) return Fail("replay には file が必要です");
        std::string error;
        std::optional<JsonValue> recording = ReadJsonFile(m_scenarioDirectory / file, error);
        if (!recording.has_value()) return Fail("入力列を読めません: " + error);
        const JsonValue* events = recording->Find("events");
        if (events == nullptr || !events->IsArray()) return Fail("入力列に events がありません: " + file);
        m_replayEvents = *events;
        m_replayCursor = 0;
        m_stepFrame = 0;
        m_stepPhase = 1;
    }

    const JsonValue::Array& events = m_replayEvents.AsArray();
    while (m_replayCursor < events.size()) {
        const JsonValue& event = events[m_replayCursor];
        const double frame = NumberOf(event, "frame", 0.0);
        if (frame > static_cast<double>(m_stepFrame)) break;
        const JsonValue* inject = event.Find("inject");
        if (inject != nullptr && inject->IsObject()) {
            JsonValue payload = *inject;
            payload.Set("t", JsonValue("input.inject"));
            JsonValue result;
            std::string code;
            std::string message;
            if (!CallBus(hooks, "command", payload, result, code, message))
                return Fail("replay の注入に失敗 (frame " + std::to_string(static_cast<long long>(frame)) + ", " + code + "): " + message);
        }
        ++m_replayCursor;
    }
    if (m_replayCursor >= events.size()) {
        /// @note 押したまま終わった入力を残すと、後続の手順が «押しっぱなし» の世界を観測する。
        JsonValue clear = JsonValue::MakeObject();
        clear.Set("t", JsonValue("input.inject"));
        clear.Set("kind", JsonValue("clear"));
        JsonValue result;
        std::string code;
        std::string message;
        CallBus(hooks, "command", clear, result, code, message);
        RecordStep(true, std::to_string(events.size()) + " events over " + std::to_string(m_stepFrame + 1) + " frames");
        return StepResult::DONE;
    }
    ++m_stepFrame;
    return StepResult::WAIT;
}

PlaytestRunner::StepResult PlaytestRunner::RunCompareImage(const JsonValue& step, const PlaytestHooks& hooks, bool captureOnly)
{
    const std::string name = StringOf(step, captureOnly ? "name" : "baseline");
    if (!IsSafeImageName(name)) return Fail(std::string(captureOnly ? "name" : "baseline") + " は英数字 / _ / - / / のみ: " + name);
    const std::string view = StringOf(step, "view", "game");
    if (view != "game" && view != "scene") return Fail("view は game か scene です");

    JsonValue image = JsonValue::MakeObject();
    image.Set("name", JsonValue(name));
    image.Set("view", JsonValue(view));

    if (!captureOnly && m_options.skipImages) {
        image.Set("status", JsonValue("skipped"));
        m_images.Push(std::move(image));
        RecordStep(true, "skipped (skipImages)");
        return StepResult::DONE;
    }

    /// @note RT の中身は撮影を要求したフレームにはまだ描かれていない。描き続けさせてから 2 フレーム置く。
    if (m_stepPhase == 0) {
        if (hooks.keepViewportsRendering) hooks.keepViewportsRendering();
        m_stepPhase = 1;
        m_stepFrame = 2;
        return StepResult::WAIT;
    }
    if (m_stepFrame > 0 && --m_stepFrame > 0) return StepResult::WAIT;

    std::vector<uint8_t> png;
    if (!hooks.capture || !hooks.capture(view, png) || png.empty()) return Fail(view + " ビューを撮影できません");
    RgbaImage actual;
    if (!DecodePng(png, actual)) return Fail("撮影した PNG を解読できません");

    const std::filesystem::path actualPath = m_outputDirectory / (name + ".actual.png");
    const std::filesystem::path diffPath = m_outputDirectory / (name + ".diff.png");
    const std::filesystem::path baselinePath = m_goldenDirectory / (name + ".png");
    if (!WriteBinaryFile(actualPath, png)) return Fail("実画像を書けません: " + actualPath.generic_string());
    image.Set("actual", JsonValue(actualPath.generic_string()));
    image.Set("width", JsonValue(static_cast<double>(actual.width)));
    image.Set("height", JsonValue(static_cast<double>(actual.height)));

    if (captureOnly) {
        image.Set("status", JsonValue("captured"));
        m_images.Push(std::move(image));
        RecordStep(true, actualPath.generic_string());
        return StepResult::DONE;
    }

    image.Set("baseline", JsonValue(baselinePath.generic_string()));
    if (m_options.updateBaselines) {
        if (!WriteBinaryFile(baselinePath, png)) return Fail("基準画像を書けません: " + baselinePath.generic_string());
        image.Set("status", JsonValue("updated"));
        m_images.Push(std::move(image));
        RecordStep(true, "baseline updated");
        return StepResult::DONE;
    }

    std::vector<uint8_t> baselineBytes;
    RgbaImage baseline;
    if (!ReadBinaryFile(baselinePath, baselineBytes) || !DecodePng(baselineBytes, baseline)) {
        image.Set("status", JsonValue("missing-baseline"));
        m_images.Push(std::move(image));
        return Fail("基準画像がありません: " + baselinePath.generic_string() + " (updateBaselines で作り、目で確かめてからコミットする)");
    }

    ImageDiffSettings settings;
    settings.pixelThreshold = static_cast<float>(NumberOf(step, "pixelThreshold", 0.1));
    const ImageDiffResult diff = CompareImages(actual, baseline, settings);
    if (!diff.comparable) {
        image.Set("status", JsonValue("size-mismatch"));
        m_images.Push(std::move(image));
        return Fail("寸法が基準画像と違う: " + std::to_string(actual.width) + "x" + std::to_string(actual.height)
            + " vs " + std::to_string(baseline.width) + "x" + std::to_string(baseline.height));
    }

    const double maxMeanDiff = NumberOf(step, "maxMeanDiff", 0.01);
    const double maxBadPixelRatio = NumberOf(step, "maxBadPixelRatio", 0.005);
    const bool pass = diff.meanDiff <= maxMeanDiff && diff.badPixelRatio <= maxBadPixelRatio;
    image.Set("meanDiff", JsonValue(diff.meanDiff));
    image.Set("badPixelRatio", JsonValue(diff.badPixelRatio));
    if (diff.badPixels > 0) {
        std::vector<uint8_t> diffPng;
        if (EncodePng(diff.diff, diffPng) && WriteBinaryFile(diffPath, diffPng))
            image.Set("diff", JsonValue(diffPath.generic_string()));
    }
    image.Set("status", JsonValue(pass ? "passed" : "failed"));
    m_images.Push(std::move(image));

    const std::string summary = "meanDiff=" + std::to_string(diff.meanDiff) + " (max " + std::to_string(maxMeanDiff)
        + "), badPixelRatio=" + std::to_string(diff.badPixelRatio) + " (max " + std::to_string(maxBadPixelRatio) + ")";
    if (!pass) return Fail("絵が基準画像と違う: " + name + " " + summary);
    RecordStep(true, summary);
    return StepResult::DONE;
}

void PlaytestRunner::Finish(PlaytestState state, const PlaytestHooks* hooks)
{
    m_state = state;
    if (hooks != nullptr) {
        JsonValue result;
        std::string code;
        std::string message;
        JsonValue clear = JsonValue::MakeObject();
        clear.Set("t", JsonValue("input.inject"));
        clear.Set("kind", JsonValue("clear"));
        CallBus(*hooks, "command", clear, result, code, message);

        JsonValue stateQuery = JsonValue::MakeObject();
        stateQuery.Set("t", JsonValue("editor.state"));
        if (CallBus(*hooks, "query", stateQuery, result, code, message) && StringOf(result, "playState") != "editor") {
            JsonValue stop = JsonValue::MakeObject();
            stop.Set("t", JsonValue("play.control"));
            stop.Set("action", JsonValue("stop"));
            CallBus(*hooks, "command", stop, result, code, message);
        }
    }
    fbzz::Time::SetLockstepDelta(0.0f);

    std::error_code ec;
    std::filesystem::create_directories(m_outputDirectory, ec);
    std::ofstream stream(m_reportPath, std::ios::binary | std::ios::trunc);
    if (stream) stream << ai::SerializeJson(Report()) << '\n';
}

JsonValue PlaytestRunner::Report() const
{
    JsonValue report = JsonValue::MakeObject();
    report.Set("name", JsonValue(m_name));
    const char* stateName = m_state == PlaytestState::RUNNING ? "running"
        : m_state == PlaytestState::PASSED ? "passed"
        : m_state == PlaytestState::FAILED ? "failed" : "idle";
    report.Set("state", JsonValue(stateName));
    report.Set("frame", JsonValue(static_cast<double>(m_frame)));
    const JsonValue* steps = m_scenario.Find("steps");
    report.Set("stepIndex", JsonValue(static_cast<double>(m_stepIndex)));
    report.Set("stepCount", JsonValue(static_cast<double>(steps != nullptr ? steps->AsArray().size() : 0)));
    if (!m_failure.empty()) report.Set("failure", JsonValue(m_failure));
    report.Set("steps", m_stepLog);
    report.Set("images", m_images);
    if (!m_reportPath.empty()) report.Set("reportPath", JsonValue(m_reportPath.generic_string()));
    return report;
}

} // namespace fbzz::editor::playtest
