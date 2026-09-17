/// @file    PlaytestHandlers.cpp
/// @brief   Playtest の実行・状態照会、入力の記録、単発の絵の比較 (playtest.* / input.record / visual.compare)。
/// @author  Hasegawa Jin
/// @date    2026-09-17
/// @see     Docs/design/ai-verification-loop.md
#include "BusInternal.hpp"

#include <Editor/PlayModeController.hpp>
#include <Editor/Playtest/ImageCompare.hpp>
#include <Editor/Playtest/InputRecorder.hpp>
#include <Editor/Playtest/PlaytestRunner.hpp>
#include <Engine/Core/Time.hpp>
#include <Engine/Renderer/IRenderer.hpp>
#include <Engine/Renderer/ResourceManager.hpp>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

namespace fbzz::editor::ai::bus {

namespace {

namespace fs = std::filesystem;

/// @brief Playtest の出力先。Library は生成物の置き場なので .meta も付かずコミットもされない。
fs::path PlaytestOutputRoot(const editor::EditorContext& ctx)
{
    return fs::path(ctx.projectRoot) / "Library" / "Playtests";
}

bool ReadOption(const JsonValue& payload, const char* key)
{
    const JsonValue* value = payload.Find(key);
    return value != nullptr && value->AsBool();
}

Outcome DoPlaytestRun(BusCall& call)
{
    editor::EditorContext& ctx = call.ctx;
    if (ctx.playtest == nullptr) return Outcome::Err("NO_PLAYTEST", "Playtest 実行器が未設定です");
    if (ctx.playtest->IsRunning()) return Outcome::Err("PLAYTEST_BUSY", "別の Playtest が実行中です。scenario_status で終了を待つか scenario_cancel");

    JsonValue scenario;
    fs::path scenarioFile;
    std::string relative;
    if (const JsonValue* inlineScenario = call.payload.Find("scenario"); inlineScenario != nullptr && inlineScenario->IsObject()) {
        scenario = *inlineScenario;
    } else {
        const std::string requested = StringField(call.payload, "path");
        if (!ResolveProjectFile(ctx, requested, scenarioFile, relative))
            return Outcome::Err("BAD_PATH", "path (projectRoot 相対の .playtest.json) か scenario が必要です");
        std::ifstream stream(scenarioFile, std::ios::binary);
        if (!stream) return Outcome::Err("NOT_FOUND", "シナリオがありません: " + relative);
        const std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
        std::string parseError;
        std::optional<JsonValue> parsed = ParseJson(text, &parseError);
        if (!parsed.has_value()) return Outcome::Err("BAD_JSON", relative + ": " + parseError);
        scenario = std::move(*parsed);
    }

    playtest::PlaytestOptions options;
    options.updateBaselines = ReadOption(call.payload, "updateBaselines");
    options.skipImages = ReadOption(call.payload, "skipImages");

    const std::string name = [&]() {
        if (const JsonValue* value = scenario.Find("name"); value != nullptr && value->IsString() && !value->AsString().empty())
            return value->AsString();
        return scenarioFile.empty() ? std::string("inline") : scenarioFile.stem().stem().string();
    }();
    std::string safeName = name;
    std::replace_if(safeName.begin(), safeName.end(), [](char character) {
        return !((character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z')
              || (character >= '0' && character <= '9') || character == '_' || character == '-');
    }, '_');
    const fs::path outputDirectory = PlaytestOutputRoot(ctx) / safeName;

    if (call.dryRun) {
        playtest::PlaytestRunner probe;
        std::string error;
        if (!probe.Start(scenario, scenarioFile, ctx.projectRoot, outputDirectory, options, error))
            return Outcome::Err("BAD_SCENARIO", error);
        Outcome preview = DryRunPreview(call.type);
        preview.result.Set("name", JsonValue(name));
        return preview;
    }

    std::string error;
    if (!ctx.playtest->Start(scenario, scenarioFile, ctx.projectRoot, outputDirectory, options, error))
        return Outcome::Err("BAD_SCENARIO", error);

    JsonValue result = JsonValue::MakeObject();
    result.Set("started", JsonValue(true));
    result.Set("name", JsonValue(name));
    result.Set("reportPath", JsonValue((outputDirectory / "report.json").generic_string()));
    /// @note 同期で待てないことを明示する。待ち方を書かないと AI は即座に結果を読みに行く。
    result.Set("async", JsonValue(true));
    result.Set("poll", JsonValue("scenario_status で state が running でなくなるまで確認してください"));
    return Outcome::Ok(std::move(result));
}

Outcome DoPlaytestStatus(BusCall& call)
{
    if (call.ctx.playtest == nullptr) return Outcome::Err("NO_PLAYTEST", "Playtest 実行器が未設定です");
    return Outcome::Ok(call.ctx.playtest->Report());
}

Outcome DoPlaytestCancel(BusCall& call)
{
    if (call.ctx.playtest == nullptr) return Outcome::Err("NO_PLAYTEST", "Playtest 実行器が未設定です");
    if (!call.ctx.playtest->IsRunning()) return Outcome::Err("NOT_RUNNING", "実行中の Playtest はありません");
    if (call.dryRun) return DryRunPreview(call.type);
    call.ctx.playtest->Cancel("playtest.cancel で中断");
    return Outcome::Ok(call.ctx.playtest->Report());
}

Outcome DoPlaytestList(BusCall& call)
{
    const fs::path root = fs::path(call.ctx.projectRoot) / "Tests" / "Playtests";
    JsonValue items = JsonValue::MakeArray();
    std::error_code ec;
    if (fs::is_directory(root, ec)) {
        std::vector<fs::path> files;
        for (fs::recursive_directory_iterator it(root, ec), end; it != end && !ec; it.increment(ec)) {
            const std::string filename = it->path().filename().string();
            if (it->is_regular_file(ec) && filename.size() > 14 && filename.ends_with(".playtest.json")) files.push_back(it->path());
        }
        std::sort(files.begin(), files.end());
        for (const fs::path& file : files) {
            JsonValue item = JsonValue::MakeObject();
            item.Set("path", JsonValue(fs::relative(file, call.ctx.projectRoot, ec).generic_string()));
            items.Push(std::move(item));
        }
    }
    JsonValue result = JsonValue::MakeObject();
    result.Set("playtests", std::move(items));
    result.Set("directory", JsonValue("Tests/Playtests"));
    return Outcome::Ok(std::move(result));
}

Outcome DoInputRecord(BusCall& call)
{
    editor::EditorContext& ctx = call.ctx;
    if (ctx.inputRecorder == nullptr || ctx.playMode == nullptr) return Outcome::Err("NO_RECORDER", "入力記録が未設定です");
    const std::string action = StringField(call.payload, "action");
    if (action == "start") {
        if (ctx.playMode->IsInEditor()) return Outcome::Err("INVALID_PLAY_STATE", "記録は Play 中だけ開始できます");
        if (ctx.inputRecorder->IsRecording()) return Outcome::Err("ALREADY_RECORDING", "既に記録中です");
        if (call.dryRun) return DryRunPreview(call.type + ":start");
        ctx.inputRecorder->Start();
        JsonValue result = JsonValue::MakeObject();
        result.Set("recording", JsonValue(true));
        return Outcome::Ok(std::move(result));
    }
    if (action != "stop") return Outcome::Err("BAD_ARG", "action は start か stop です");
    if (!ctx.inputRecorder->IsRecording()) return Outcome::Err("NOT_RECORDING", "記録していません");

    std::string requested = StringField(call.payload, "path");
    if (requested.empty()) {
        const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        std::tm local{};
        localtime_s(&local, &now);
        char stamp[32] = {};
        std::strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &local);
        requested = std::string("Tests/Playtests/Recordings/") + stamp + ".inputrec.json";
    }
    fs::path file;
    std::string relative;
    if (!ResolveProjectFile(ctx, requested, file, relative)) return Outcome::Err("BAD_PATH", "path は projectRoot 配下: " + requested);
    if (call.dryRun) return DryRunPreview(call.type + ":stop");

    JsonValue recording = ctx.inputRecorder->Stop();
    const std::string text = SerializeJson(recording);
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    std::ofstream stream(file, std::ios::binary | std::ios::trunc);
    if (!stream) return Outcome::Err("WRITE_FAILED", "書けません: " + relative);
    stream << text << '\n';

    JsonValue result = JsonValue::MakeObject();
    result.Set("path", JsonValue(relative));
    const JsonValue* events = recording.Find("events");
    result.Set("events", JsonValue(static_cast<double>(events != nullptr ? events->AsArray().size() : 0)));
    result.Set("frames", *recording.Find("frames"));
    /// @note 記録はロックステップの有無で再生時の進みが変わる。シナリオ側の lockstep と揃えて再生すること。
    result.Set("lockstep", JsonValue(static_cast<double>(fbzz::Time::GetLockstepDelta())));
    return Outcome::Ok(std::move(result));
}

/// @brief 今のビューポートを基準画像と 1 回だけ比べる。シナリオを書く前に閾値を探る用途。
Outcome DoVisualCompare(BusCall& call)
{
    editor::EditorContext& ctx = call.ctx;
    if (ctx.renderer == nullptr || ctx.resources == nullptr) return Outcome::Err("NO_RENDERER", "レンダラーが未初期化です");
    const std::string view = StringField(call.payload, "view").empty() ? std::string("game") : StringField(call.payload, "view");
    if (view != "game" && view != "scene") return Outcome::Err("BAD_ARG", "view は game か scene です");
    const std::string baselineName = StringField(call.payload, "baseline");
    if (baselineName.empty() || baselineName.find("..") != std::string::npos)
        return Outcome::Err("BAD_ARG", "baseline (Tests/Golden 相対、拡張子なし) が必要です");

    ctx.aiViewportRenderUntilFrame = std::max<std::uint64_t>(ctx.aiViewportRenderUntilFrame, fbzz::Time::frameCount + 8);
    const auto target = view == "game" ? call.state.gameViewportRT : call.state.sceneViewportRT;
    if (!target.IsValid()) return Outcome::Err("NO_VIEWPORT", view + " ビューの RT が未生成です");
    std::vector<uint8_t> png;
    uint32_t width = 0;
    uint32_t height = 0;
    if (!ctx.renderer->CaptureRenderTargetToPng(target, *ctx.resources, png, width, height) || png.empty())
        return Outcome::Err("CAPTURE_FAILED", "撮影に失敗しました");

    const fs::path baselinePath = fs::path(ctx.projectRoot) / "Tests" / "Golden" / (baselineName + ".png");
    const fs::path outputDirectory = PlaytestOutputRoot(ctx) / "_visual";
    JsonValue result = JsonValue::MakeObject();
    result.Set("baseline", JsonValue(baselinePath.generic_string()));

    if (ReadOption(call.payload, "updateBaseline")) {
        if (call.dryRun) return DryRunPreview(call.type + ":updateBaseline");
        if (!playtest::WriteBinaryFile(baselinePath, png)) return Outcome::Err("WRITE_FAILED", "基準画像を書けません");
        result.Set("status", JsonValue("updated"));
        return Outcome::Ok(std::move(result));
    }

    playtest::RgbaImage actual;
    playtest::RgbaImage baseline;
    std::vector<uint8_t> baselineBytes;
    if (!playtest::DecodePng(png, actual)) return Outcome::Err("DECODE_FAILED", "撮影した PNG を解読できません");
    if (!playtest::ReadBinaryFile(baselinePath, baselineBytes) || !playtest::DecodePng(baselineBytes, baseline))
        return Outcome::Err("NO_BASELINE", "基準画像がありません: " + baselinePath.generic_string() + " (updateBaseline=true で作る)");

    playtest::ImageDiffSettings settings;
    if (const JsonValue* threshold = call.payload.Find("pixelThreshold"); threshold != nullptr && threshold->IsNumber())
        settings.pixelThreshold = static_cast<float>(threshold->AsNumber());
    const playtest::ImageDiffResult diff = playtest::CompareImages(actual, baseline, settings);
    if (!diff.comparable) return Outcome::Err("SIZE_MISMATCH", "寸法が基準画像と違います");

    result.Set("meanDiff", JsonValue(diff.meanDiff));
    result.Set("badPixelRatio", JsonValue(diff.badPixelRatio));
    result.Set("width", JsonValue(static_cast<double>(width)));
    result.Set("height", JsonValue(static_cast<double>(height)));
    if (diff.badPixels > 0) {
        std::vector<uint8_t> diffPng;
        if (playtest::EncodePng(diff.diff, diffPng)) {
            const fs::path diffPath = outputDirectory / (baselineName + ".diff.png");
            if (!call.dryRun && playtest::WriteBinaryFile(diffPath, diffPng)) result.Set("diffPath", JsonValue(diffPath.generic_string()));
            result.Set("diffBase64", JsonValue(Base64Encode(diffPng)));
        }
    }
    return Outcome::Ok(std::move(result));
}

} // namespace

void RegisterPlaytestHandlers(BusHandlerTable& table)
{
    table.AddQuery("playtest.status", DoPlaytestStatus);
    table.AddQuery("playtest.list", DoPlaytestList);
    table.AddCommand("playtest.run", DoPlaytestRun);
    table.AddCommand("playtest.cancel", DoPlaytestCancel);
    table.AddCommand("input.record", DoInputRecord);
    /// @note 基準画像の書き込み (updateBaseline) と差分画像の出力を伴うので Command 側に置く。
    table.AddCommand("visual.compare", DoVisualCompare);
}

} // namespace fbzz::editor::ai::bus
