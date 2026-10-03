/// @file    EditorHandlers.cpp
/// @brief   editor.* / console / shader / profiler / build の Query と Command。
/// @author  Hasegawa Jin
/// @date    2026-09-17
#include "BusInternal.hpp"

#include <Editor/Ai/JsonReflector.hpp>
#include <Editor/Ai/OperatorBridge.hpp>
#include <Editor/Op/EditorOperator.hpp>
#include <Editor/Util/BuildConsole.hpp>
#include <Editor/Util/SpriteSlicer.hpp>
#include <Editor/Util/Selection.hpp>
#include <Engine/Renderer/RenderDebugOverlay.hpp>
#include <Engine/Renderer/IShader.hpp>
#include <Engine/Renderer/ShaderCompileDiagnostics.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Core/Time.hpp>
#include <Engine/Profiler/Profiler.hpp>
#include <Engine/Scene/ComponentRegistry.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Editor/PlayModeController.hpp>
#include <Editor/Util/ConsoleSink.hpp>
#include <Engine/Core/Memory/MemorySystem.hpp>

#include <algorithm>
#include <cstdint>
#include <string>
#include <type_traits>
#include <vector>

namespace fbzz::editor::ai::bus {

using scene::GameObject;
using scene::EntityID;

namespace {

const char* ComponentCategoryName(scene::ComponentCategory category)
{
    switch (category) {
    case scene::ComponentCategory::Rendering:   return "Rendering";
    case scene::ComponentCategory::Lighting:    return "Lighting";
    case scene::ComponentCategory::Physics:     return "Physics";
    case scene::ComponentCategory::Animation:   return "Animation";
    case scene::ComponentCategory::Audio:       return "Audio";
    case scene::ComponentCategory::Effects:     return "Effects";
    case scene::ComponentCategory::Environment: return "Environment";
    case scene::ComponentCategory::Navigation:  return "Navigation";
    case scene::ComponentCategory::Terrain:     return "Terrain";
    case scene::ComponentCategory::UI:          return "UI";
    case scene::ComponentCategory::Misc:        return "Misc";
    case scene::ComponentCategory::Internal:    return "Internal";
    }
    return "Misc";
}

const char* ComponentInspectorName(scene::ComponentInspectorMode mode)
{
    switch (mode) {
    case scene::ComponentInspectorMode::Automatic: return "automatic";
    case scene::ComponentInspectorMode::Custom:    return "custom";
    case scene::ComponentInspectorMode::Hidden:    return "hidden";
    }
    return "hidden";
}

/// @brief ComponentRegistry と各 Reflect() を走査し、component.add/set の正確な入力契約を返す。
JsonValue BuildEditorCatalog()
{
    JsonValue components = JsonValue::MakeArray();
    scene::ForEachRegisteredComponent([&]<typename T, typename Reg>() {
        if constexpr (Reg::inspectorMode != scene::ComponentInspectorMode::Hidden) {
            JsonValue entry = JsonValue::MakeObject();
            entry.Set("type", JsonValue(Reg::serializedName));
            entry.Set("displayName", JsonValue(Reg::displayName));
            entry.Set("category", JsonValue(ComponentCategoryName(Reg::category)));
            entry.Set("inspector", JsonValue(ComponentInspectorName(Reg::inspectorMode)));
            constexpr bool addable = Reg::addable
                && std::is_default_constructible_v<T>
                && std::is_move_constructible_v<T>;
            entry.Set("addable", JsonValue(addable));

            JsonValue fields = JsonValue::MakeArray();
            if constexpr (Reg::hasReflect && std::is_default_constructible_v<T>) {
                T component{};
                JsonCatalogReflector reflector;
                component.Reflect(reflector);
                fields = reflector.Result();
            }
            entry.Set("fields", std::move(fields));
            components.Push(std::move(entry));
        }
    });

    JsonValue result = JsonValue::MakeObject();
    result.Set("components", std::move(components));
    return result;
}

Outcome SearchEditorCatalog(const JsonValue& payload)
{
    const std::string query = LowerAscii(StringField(payload, "query"));
    const std::string category = LowerAscii(StringField(payload, "category"));
    const JsonValue* limitValue = payload.Find("limit");
    const int limit = std::clamp(limitValue != nullptr && limitValue->IsNumber() ? limitValue->AsInt() : 25, 1, 100);
    if (query.empty() && category.empty()) return Outcome::Err("BAD_ARG", "query / category のいずれかが必要です");

    JsonValue catalog = BuildEditorCatalog();
    const JsonValue* source = catalog.Find("components");
    JsonValue matches = JsonValue::MakeArray();
    int total = 0;
    if (source != nullptr && source->IsArray()) {
        for (const JsonValue& entry : source->AsArray()) {
            const std::string entryCategory = LowerAscii(StringField(entry, "category"));
            const bool categoryMatches = category.empty() || entryCategory == category;
            const bool queryMatches = query.empty()
                || LowerAscii(SerializeJson(entry)).find(query) != std::string::npos;
            if (!categoryMatches || !queryMatches) continue;
            ++total;
            if (static_cast<int>(matches.AsArray().size()) < limit) matches.Push(entry);
        }
    }
    JsonValue result = JsonValue::MakeObject();
    result.Set("components", std::move(matches));
    result.Set("count", JsonValue(total));
    result.Set("truncated", JsonValue(total > limit));
    return Outcome::Ok(std::move(result));
}

/// @brief シェーダーの変数目録を JSON にする。descriptor が無効なら空配列を返す。
/// @note ShaderDescriptor は PS バイトコードのリフレクション結果で、書ける変数の唯一の正本。
JsonValue ShaderVarsToJson(const renderer::ShaderDescriptor& descriptor)
{
    const auto typeName = [](renderer::ShaderVarType type) -> const char* {
        switch (type) {
        case renderer::ShaderVarType::Int:  return "int";
        case renderer::ShaderVarType::UInt: return "uint";
        case renderer::ShaderVarType::Bool: return "bool";
        case renderer::ShaderVarType::Float: return "float";
        default: return "unsupported";
        }
    };
    JsonValue vars = JsonValue::MakeArray();
    for (const auto& var : descriptor.vars) {
        JsonValue item = JsonValue::MakeObject();
        item.Set("name", JsonValue(var.name));
        /// @note components はパディングを除いた値数。行列も配列も全要素を要求する。
        item.Set("components", JsonValue(static_cast<int>(var.ValueCount())));
        item.Set("class", JsonValue(std::string(var.varClass == renderer::ShaderVarClass::Scalar ? "scalar"
            : var.varClass == renderer::ShaderVarClass::Vector ? "vector"
            : var.varClass == renderer::ShaderVarClass::Matrix ? "matrix" : "unsupported")));
        item.Set("rows", JsonValue(static_cast<int>(var.rows)));
        item.Set("columns", JsonValue(static_cast<int>(var.columns)));
        item.Set("elements", JsonValue(static_cast<int>(var.elements)));
        item.Set("rowMajor", JsonValue(var.rowMajor));
        item.Set("valueOrder", JsonValue(std::string("array-element, row, column; no padding")));
        item.Set("writable", JsonValue(var.IsWritable()));
        item.Set("unsupportedReason", JsonValue(var.unsupportedReason));
        item.Set("type", JsonValue(std::string(typeName(var.varType))));
        item.Set("sizeBytes", JsonValue(static_cast<int>(var.size)));
        vars.Push(std::move(item));
    }
    return vars;
}

/// @brief シェーダーが公開する変数とテクスチャスロットの目録を返す。
/// @note 効くのはコンパイル済みバイトコードのリフレクション結果でありソース宣言ではない (未使用変数は最適化で消える)。存在しない名前は保存は通り実行時に無視される。
Outcome DoShaderInspect(editor::EditorContext& ctx, const JsonValue& payload)
{
    if (ctx.resources == nullptr) return Outcome::Err("NO_RENDERER", "ResourceManager がありません");
    const std::string path = StringField(payload, "path");
    if (path.empty()) return Outcome::Err("BAD_ARG", "path が必要です");

    /// @note `guid:` 参照も含めて AssetManager に解決させる (.mat の shader フィールドは guid 形式)。
    const std::string resolved = asset::AssetManager::ResolveAssetPath(path);
    const auto handle = ctx.resources->LoadShader(resolved.empty() ? path : resolved);
    const renderer::IShader* shader = handle.IsValid() ? ctx.resources->Get(handle) : nullptr;
    if (shader == nullptr)
        return Outcome::Err("SHADER_NOT_FOUND", "シェーダーを読み込めません: " + path);

    const renderer::ShaderDescriptor& descriptor = shader->GetDescriptor();
    JsonValue result = JsonValue::MakeObject();
    result.Set("path", JsonValue(path));
    result.Set("valid", JsonValue(descriptor.IsValid()));
    result.Set("cbufferSize", JsonValue(static_cast<int>(descriptor.cbufferSize)));
    result.Set("vars", ShaderVarsToJson(descriptor));

    renderer::ShaderDescriptor postProcess;
    postProcess.vars = descriptor.postProcessVars;
    result.Set("postProcessVars", ShaderVarsToJson(postProcess));

    JsonValue textures = JsonValue::MakeArray();
    for (const auto& texture : descriptor.textures) {
        JsonValue item = JsonValue::MakeObject();
        item.Set("name", JsonValue(texture.name));
        item.Set("slot", JsonValue(static_cast<int>(texture.slot)));
        textures.Push(std::move(item));
    }
    result.Set("textures", std::move(textures));
    result.Set("hint", JsonValue(std::string(
        "writable=true の変数へ components 個の値を渡してください。配列要素順・行優先でパディング不要です。"
        "構造体メンバーは name の完全名を使います。整数は整数値、bool は 0/1 を指定してください。"
        "Script は SetValues、float4x4 は SetMatrix を使えます。未対応型は unsupportedReason を確認してください。"
        "未使用変数はコンパイル時に消えるため、HLSL に書いてあってもここに出ないことがあります。")));
    return Outcome::Ok(std::move(result));
}

/// @brief Editor の表示と同じ正本から、シェーダーコンパイル診断を AI へ返す。
Outcome DoShaderCompileDiagnostics()
{
    const auto diagnostics = renderer::GetShaderCompileDiagnostics();
    JsonValue result = JsonValue::MakeObject();
    JsonValue items = JsonValue::MakeArray();
    int errorCount = 0;
    int warningCount = 0;
    for (const auto& diagnostic : diagnostics) {
        JsonValue item = JsonValue::MakeObject();
        item.Set("sequence", JsonValue(static_cast<double>(diagnostic.sequence)));
        item.Set("severity", JsonValue(diagnostic.isError ? "error" : "warning"));
        item.Set("path", JsonValue(diagnostic.path));
        item.Set("entryPoint", JsonValue(diagnostic.entryPoint));
        item.Set("target", JsonValue(diagnostic.target));
        item.Set("message", JsonValue(diagnostic.message));
        items.Push(std::move(item));
        if (diagnostic.isError) ++errorCount;
        else ++warningCount;
    }
    result.Set("errorCount", JsonValue(errorCount));
    result.Set("warningCount", JsonValue(warningCount));
    result.Set("diagnostics", std::move(items));
    return Outcome::Ok(std::move(result));
}

Outcome DoEditorState(const editor::EditorContext& ctx)
{
    if (ctx.playMode == nullptr) return Outcome::Err("NO_PLAY_MODE", "PlayModeController が未設定です");
    JsonValue result = JsonValue::MakeObject();
    result.Set("playState", JsonValue(PlayStateName(*ctx.playMode)));
    result.Set("restorePending", JsonValue(ctx.playMode->HasPendingRestore()));
    result.Set("scene", JsonValue(ctx.currentScenePath));
    result.Set("sceneDirty", JsonValue(ctx.sceneDirty));
    result.Set("scriptReloadBusy", JsonValue(ctx.scriptReloadBusy));
    result.Set("frameIndex", JsonValue(static_cast<std::int64_t>(Time::frameCount)));
    return Outcome::Ok(std::move(result));
}

/// @brief Undo/Redo スタックの状態を返す (read+)。
/// @note 自律ループが自分の編集を期待どおりのラベル付きエントリとして検証し、何回 undo すれば戻れるかを判断できるようにする。
Outcome DoUndoHistory(const editor::EditorContext& ctx, const JsonValue& payload)
{
    if (ctx.undoStack == nullptr) return Outcome::Err("NO_UNDOSTACK", "UndoStack が未設定です");
    const JsonValue* limitValue = payload.Find("limit");
    const int limit = std::clamp(
        limitValue != nullptr && limitValue->IsNumber() ? limitValue->AsInt() : 50, 1, 128);

    const auto history = ctx.undoStack->GetHistory();
    const int total = static_cast<int>(history.size());
    /// @note 新しい順 (末尾から) に最大 limit 件返す。index は履歴内の実位置を保つ。
    JsonValue entries = JsonValue::MakeArray();
    for (int i = total - 1; i >= 0 && static_cast<int>(entries.AsArray().size()) < limit; --i) {
        JsonValue entry = JsonValue::MakeObject();
        entry.Set("index", JsonValue(i));
        entry.Set("description", JsonValue(history[static_cast<size_t>(i)].description));
        entry.Set("applied", JsonValue(history[static_cast<size_t>(i)].applied));
        entries.Push(std::move(entry));
    }

    JsonValue result = JsonValue::MakeObject();
    result.Set("canUndo", JsonValue(ctx.undoStack->CanUndo()));
    result.Set("canRedo", JsonValue(ctx.undoStack->CanRedo()));
    result.Set("cursor", JsonValue(static_cast<int>(ctx.undoStack->GetCursor())));
    result.Set("count", JsonValue(total));
    if (ctx.undoStack->CanUndo()) result.Set("undoDescription", JsonValue(ctx.undoStack->GetUndoDescription()));
    if (ctx.undoStack->CanRedo()) result.Set("redoDescription", JsonValue(ctx.undoStack->GetRedoDescription()));
    result.Set("entries", std::move(entries));
    return Outcome::Ok(std::move(result));
}

int LogLevelRank(core::LogLevel level)
{
    return static_cast<int>(level);
}

const char* LogLevelName(core::LogLevel level)
{
    switch (level) {
    case core::LogLevel::DEBUG:     return "debug";
    case core::LogLevel::INFO:      return "info";
    case core::LogLevel::WARNING:   return "warning";
    case core::LogLevel::LOG_ERROR: return "error";
    }
    return "unknown";
}

Outcome DoConsoleLogs(const editor::EditorContext& ctx, const JsonValue& payload)
{
    if (ctx.consoleSink == nullptr) return Outcome::Err("NO_CONSOLE", "ConsoleSink が未設定です");
    const std::string requestedLevel = StringField(payload, "minLevel");
    int minimumRank = 0;
    if (requestedLevel == "info") minimumRank = 1;
    else if (requestedLevel == "warning") minimumRank = 2;
    else if (requestedLevel == "error") minimumRank = 3;
    const std::string contains = LowerAscii(StringField(payload, "contains"));
    const JsonValue* limitValue = payload.Find("limit");
    const int limit = std::clamp(limitValue != nullptr && limitValue->IsNumber() ? limitValue->AsInt() : 100, 1, 512);
    const JsonValue* afterSequenceValue = payload.Find("afterSequence");
    constexpr double MAX_SAFE_JSON_INTEGER = 9007199254740991.0;
    const double requestedAfterSequence = afterSequenceValue != nullptr && afterSequenceValue->IsNumber()
        ? afterSequenceValue->AsNumber() : 0.0;
    const std::uint64_t afterSequence = static_cast<std::uint64_t>(
        std::clamp(requestedAfterSequence, 0.0, MAX_SAFE_JSON_INTEGER));

    JsonValue entries = JsonValue::MakeArray();
    int matched = 0;
    const auto& source = ctx.consoleSink->GetEntries();
    std::uint64_t sequence = ctx.consoleSink->GetLatestSequence();
    for (auto iterator = source.rbegin(); iterator != source.rend(); ++iterator) {
        const std::uint64_t entrySequence = sequence--;
        if (entrySequence <= afterSequence) continue;
        if (LogLevelRank(iterator->level) < minimumRank) continue;
        if (!contains.empty() && LowerAscii(iterator->message).find(contains) == std::string::npos) continue;
        ++matched;
        if (static_cast<int>(entries.AsArray().size()) >= limit) continue;
        JsonValue entry = JsonValue::MakeObject();
        entry.Set("sequence", JsonValue(static_cast<std::int64_t>(entrySequence)));
        entry.Set("level", JsonValue(LogLevelName(iterator->level)));
        entry.Set("message", JsonValue(iterator->message));
        entries.Push(std::move(entry));
    }
    JsonValue result = JsonValue::MakeObject();
    result.Set("entries", std::move(entries));
    result.Set("count", JsonValue(matched));
    result.Set("truncated", JsonValue(matched > limit));
    result.Set("order", JsonValue("newest-first"));
    result.Set("cursor", JsonValue(static_cast<std::int64_t>(ctx.consoleSink->GetLatestSequence())));
    result.Set("oldestSequence", JsonValue(static_cast<std::int64_t>(ctx.consoleSink->GetOldestSequence())));
    result.Set("dropped", JsonValue(afterSequence > 0
        && afterSequence + 1 < ctx.consoleSink->GetOldestSequence()));
    return Outcome::Ok(std::move(result));
}

Outcome DoProfilerSnapshot(editor::EditorContext& ctx, const JsonValue& payload)
{
    const JsonValue* limitValue = payload.Find("limit");
    const int limit = std::clamp(limitValue != nullptr && limitValue->IsNumber() ? limitValue->AsInt() : 20, 1, 100);
    const auto& records = profiler::Profiler::GetLastFrameRecords();
    std::vector<const profiler::ProfileRecord*> sorted;
    sorted.reserve(records.size());
    double frameMs = 0.0;
    for (const auto& record : records) {
        sorted.push_back(&record);
        if (record.depth == 0) frameMs += record.elapsedMs;
    }
    std::sort(sorted.begin(), sorted.end(), [](const auto* left, const auto* right) {
        return left->elapsedMs > right->elapsedMs;
    });
    JsonValue samples = JsonValue::MakeArray();
    for (int index = 0; index < std::min(limit, static_cast<int>(sorted.size())); ++index) {
        JsonValue sample = JsonValue::MakeObject();
        sample.Set("name", JsonValue(sorted[index]->name));
        sample.Set("category", JsonValue(sorted[index]->category));
        sample.Set("elapsedMs", JsonValue(sorted[index]->elapsedMs));
        sample.Set("depth", JsonValue(static_cast<int>(sorted[index]->depth)));
        samples.Push(std::move(sample));
    }
    const auto& rendering = renderer::RenderDebugOverlay::GetLastSnapshot();
    JsonValue result = JsonValue::MakeObject();
    result.Set("profilerEnabled", JsonValue(profiler::Profiler::IsEnabled()));
    result.Set("frameIndex", JsonValue(static_cast<std::int64_t>(profiler::Profiler::GetLastFrameIndex())));
    const double engineFrameMs = static_cast<double>(Time::unscaledDeltaTime) * 1000.0;
    result.Set("frameMs", JsonValue(engineFrameMs));
    result.Set("fps", JsonValue(engineFrameMs > 0.0 ? 1000.0 / engineFrameMs : 0.0));
    const bool lockstep = Time::GetLockstepDelta() > 0.0f;
    result.Set("lockstep", JsonValue(lockstep));
    result.Set("frameMsSource", JsonValue(lockstep ? "lockstep" : "engine-delta-time"));
    result.Set("cpuProfiledMs", JsonValue(frameMs));
    result.Set("drawCalls", JsonValue(rendering.renderStats.drawCalls));
    result.Set("triangles", JsonValue(rendering.renderStats.triangleCount));
    result.Set("vertices", JsonValue(rendering.renderStats.vertexCount));
    result.Set("skinningVertices", JsonValue(
        static_cast<std::int64_t>(rendering.renderStats.skinningVertexCount)));
    /// @note uint32_t は JsonValue の int/int64_t/double と暗黙変換が競合するため、JSON の整数表現へ明示的に昇格させる。
    result.Set("skinningDispatches", JsonValue(
        static_cast<std::int64_t>(rendering.renderStats.skinningDispatchCount)));
    result.Set("totalObjects", JsonValue(rendering.renderStats.totalObjects));
    result.Set("frustumCulled", JsonValue(rendering.renderStats.frustumCulled));
    result.Set("occlusionCulled", JsonValue(rendering.renderStats.occlusionCulled));
    /// @note シャドウマップ描画はカメラ視点の統計と別枠。合計だけで「描画が軽い」と誤判断しないよう分けて返す。
    result.Set("shadowDrawCalls", JsonValue(rendering.renderStats.shadowDrawCalls));
    result.Set("shadowTriangles", JsonValue(rendering.renderStats.shadowTriangleCount));
    /// @note drawCalls / shadowDrawCalls は束ねた後の数。何回束ねて何回ぶん減ったかを併記しないと、
    /// @note «描画が減った» のが束ねによるものかカリングによるものか読み分けられない。
    result.Set("instancedBatches", JsonValue(rendering.renderStats.instancedBatches));
    result.Set("instancedDrawsSaved", JsonValue(rendering.renderStats.instancedDrawsSaved));
    result.Set("visibleObjects", JsonValue(rendering.renderStats.totalObjects
        - rendering.renderStats.frustumCulled - rendering.renderStats.occlusionCulled));
    result.Set("topSamples", std::move(samples));
    JsonValue cpuPasses = JsonValue::MakeArray();
    for (const auto& [name, elapsedMs] : rendering.passTimings) {
        JsonValue pass = JsonValue::MakeObject();
        pass.Set("name", JsonValue(name));
        pass.Set("elapsedMs", JsonValue(elapsedMs));
        cpuPasses.Push(std::move(pass));
    }
    JsonValue gpuPasses = JsonValue::MakeArray();
    const auto metadataJson = [](const renderer::GpuProfilerViewMetadata& metadata) {
        JsonValue value = JsonValue::MakeObject();
        value.Set("applicationFrameSerial", JsonValue(static_cast<std::int64_t>(metadata.applicationFrameSerial)));
        value.Set("viewId", JsonValue(static_cast<std::int64_t>(metadata.viewId)));
        value.Set("sceneGeneration", JsonValue(static_cast<std::int64_t>(metadata.sceneGeneration)));
        value.Set("planGeneration", JsonValue(static_cast<std::int64_t>(metadata.planGeneration)));
        value.Set("resourceEpoch", JsonValue(static_cast<std::int64_t>(metadata.resourceEpoch)));
        value.Set("outputId", JsonValue(static_cast<std::int64_t>(metadata.outputId)));
        value.Set("outputGeneration", JsonValue(static_cast<std::int64_t>(metadata.outputGeneration)));
        value.Set("width", JsonValue(static_cast<std::int64_t>(metadata.width)));
        value.Set("height", JsonValue(static_cast<std::int64_t>(metadata.height)));
        return value;
    };
    for (const auto& profile : rendering.gpuProfiler.passes) {
        JsonValue pass = JsonValue::MakeObject();
        pass.Set("name", JsonValue(profile.name));
        pass.Set("elapsedMs", JsonValue(profile.gpuMs));
        pass.Set("source", metadataJson(profile.metadata));
        pass.Set("physicalFrameSerial", JsonValue(static_cast<std::int64_t>(profile.physicalFrameSerial)));
        pass.Set("deviceEpoch", JsonValue(static_cast<std::int64_t>(profile.deviceEpoch)));
        pass.Set("available", JsonValue(profile.available));
        gpuPasses.Push(std::move(pass));
    }
    result.Set("cpuRenderPasses", std::move(cpuPasses));
    result.Set("gpuRenderPasses", std::move(gpuPasses));
    const auto& gpu = rendering.gpuProfiler;
    JsonValue gpuStatus = JsonValue::MakeObject();
    gpuStatus.Set("supported", JsonValue(gpu.supported));
    gpuStatus.Set("available", JsonValue(gpu.available));
    gpuStatus.Set("complete", JsonValue(gpu.complete));
    gpuStatus.Set("physicalFrameSerial", JsonValue(static_cast<std::int64_t>(gpu.physicalFrameSerial)));
    gpuStatus.Set("deviceEpoch", JsonValue(static_cast<std::int64_t>(gpu.deviceEpoch)));
    gpuStatus.Set("recordedPassCount", JsonValue(static_cast<std::int64_t>(gpu.recordedPassCount)));
    gpuStatus.Set("droppedPassCount", JsonValue(static_cast<std::int64_t>(gpu.droppedPassCount)));
    gpuStatus.Set("viewSampleCount", JsonValue(static_cast<std::int64_t>(gpu.passes.size())));
    gpuStatus.Set("currentView", metadataJson(rendering.gpuCurrentView));
    /// @note 合計・準備・別 queue はまだ未計測。0 ms を返すと高速だと誤認するため unavailable を明示する。
    gpuStatus.Set("totalGpuTimeAvailable", JsonValue(gpu.totalGpuTimeAvailable));
    gpuStatus.Set("totalGpuMs", gpu.totalGpuTimeAvailable ? JsonValue(gpu.totalGpuMs) : JsonValue());
    gpuStatus.Set("preparationGpuTimeAvailable", JsonValue(gpu.preparationGpuTimeAvailable));
    gpuStatus.Set("perQueueGpuTimeAvailable", JsonValue(gpu.perQueueGpuTimeAvailable));
    gpuStatus.Set("queue", JsonValue("direct"));
    gpuStatus.Set("sampleKind", JsonValue("completed-asynchronous-view-passes"));
    result.Set("gpuProfiler", std::move(gpuStatus));
    if (ctx.memorySystem != nullptr) {
        const core::MemoryStats memory = ctx.memorySystem->GetTracker().GetTotalStats();
        JsonValue memoryJson = JsonValue::MakeObject();
        memoryJson.Set("used", JsonValue(static_cast<std::int64_t>(memory.used)));
        memoryJson.Set("peakUsed", JsonValue(static_cast<std::int64_t>(memory.peakUsed)));
        memoryJson.Set("activeAllocations", JsonValue(static_cast<std::int64_t>(memory.activeCount)));
        result.Set("memory", std::move(memoryJson));
    }
    return Outcome::Ok(std::move(result));
}

/// @name Build (スクリプト DLL / HLSL のコンパイル状態)

/// @brief BuildConsole の履歴と診断を返す。shader_get_compile_diagnostics の Script 版。
/// @note コンパイルが通っていないと Play も component_add も無意味な結果になるが、console_get_logs の断片からは何行目で落ちたか組み立て直す必要がある。
Outcome DoBuildStatus(editor::EditorContext& ctx, const JsonValue& payload)
{
    if (ctx.buildConsole == nullptr) return Outcome::Err("NO_BUILD_CONSOLE", "BuildConsole が未設定です");
    const editor::BuildConsole& console = *ctx.buildConsole;

    int limit = 5;
    if (const JsonValue* v = payload.Find("limit"); v != nullptr && v->IsNumber())
        limit = std::clamp(v->AsInt(), 1, 20);

    auto recordJson = [](const editor::BuildRecord& record) {
        JsonValue entry = JsonValue::MakeObject();
        entry.Set("kind", JsonValue(record.kind == editor::BuildRecord::Kind::Hlsl ? "hlsl" : "script"));
        const char* resultName = "building";
        switch (record.result) {
        case editor::BuildRecord::Result::Success:   resultName = "success"; break;
        case editor::BuildRecord::Result::Failed:    resultName = "failed"; break;
        case editor::BuildRecord::Result::Cancelled: resultName = "cancelled"; break;
        case editor::BuildRecord::Result::Building:  resultName = "building"; break;
        }
        entry.Set("result", JsonValue(resultName));
        entry.Set("exitCode", JsonValue(record.exitCode));
        entry.Set("startClock", JsonValue(record.startClock));
        entry.Set("durationSec", JsonValue(record.durationSec));
        entry.Set("errorCount", JsonValue(record.errorCount));
        entry.Set("warnCount", JsonValue(record.warnCount));
        JsonValue diagnostics = JsonValue::MakeArray();
        for (const editor::BuildDiagnostic& diagnostic : record.diagnostics) {
            JsonValue item = JsonValue::MakeObject();
            item.Set("severity", JsonValue(
                diagnostic.severity == editor::BuildDiagnostic::Severity::Warning ? "warning" : "error"));
            item.Set("file", JsonValue(diagnostic.file));
            item.Set("line", JsonValue(diagnostic.line));
            item.Set("column", JsonValue(diagnostic.column));
            item.Set("code", JsonValue(diagnostic.code));
            item.Set("message", JsonValue(diagnostic.message));
            diagnostics.Push(std::move(item));
        }
        entry.Set("diagnostics", std::move(diagnostics));
        return entry;
    };

    JsonValue result = JsonValue::MakeObject();
    result.Set("building", JsonValue(console.IsBuilding()));
    result.Set("currentFile", JsonValue(console.CurrentFile()));
    result.Set("hasActiveFailure", JsonValue(console.HasActiveFailure()));
    /// @note Play 開始が弾かれる理由そのもの。build_run の直後に見るのはここ。
    result.Set("scriptReloadBusy", JsonValue(ctx.scriptReloadBusy));
    if (const editor::BuildRecord* latest = console.Latest())
        result.Set("latest", recordJson(*latest));
    JsonValue history = JsonValue::MakeArray();
    const auto& records = console.History();
    for (auto it = records.rbegin(); it != records.rend() && static_cast<int>(history.AsArray().size()) < limit; ++it)
        history.Push(recordJson(*it));
    result.Set("history", std::move(history));
    return Outcome::Ok(std::move(result));
}

/// @brief スクリプト DLL の再ビルドを要求する。完了は build.status のポーリングで確認する。
/// @note MSBuild は数十秒かかるため同期にすると、バスの drain (メインスレッド) が止まり Editor が固まって応答も返らない。
Outcome DoBuildRun(editor::EditorContext& ctx, const JsonValue& payload, bool dryRun)
{
    const std::string target = LowerAscii(StringField(payload, "target"));
    if (!target.empty() && target != "script")
        return Outcome::Err("BAD_ARG", "target は script のみ対応しています (HLSL はファイル保存で自動コンパイルされます)");
    if (ctx.buildConsole != nullptr && ctx.buildConsole->IsBuilding())
        return Outcome::Err("BUILD_BUSY", "既にビルド中です");
    if (ctx.scriptReloadBusy)
        return Outcome::Err("BUILD_BUSY", "Script DLL の再読み込み中です");
    if (dryRun) return DryRunPreview("build.run");

    /// @note 実体は script.reload operator。Play ツールバーの Reload Scripts と同じ経路を通る (フラグを直に立てるとホットリロード中のコンパイルへもう 1 本重なる)。
    if (const editor::OpResult result = editor::InvokeOperator(ctx, "script.reload"); !result.ok)
        return Outcome::Err(result.errorCode.empty() ? "BUILD_BUSY" : result.errorCode,
                            result.message);

    JsonValue result = JsonValue::MakeObject();
    result.Set("requested", JsonValue("script"));
    /// @note 同期完了を返せないことを明示する。待ち方を書かないと AI は即座に結果を読みに行く。
    result.Set("async", JsonValue(true));
    result.Set("poll", JsonValue("build_get_status で building=false になるまで確認してください"));
    return Outcome::Ok(std::move(result));
}

Outcome DoUndoRedo(editor::EditorContext& ctx, const std::string& type, bool dryRun)
{
    /// @note Operator へ移送済み。2 つ目の実装を残すと「実行できるか」の判定が AI 側だけ別式になるため、応答の形 (applied/description) は互換のまま維持する。
    /// @see Docs/design/editor-operator-model.md
    if (ctx.undoStack == nullptr) return Outcome::Err("NO_UNDOSTACK", "UndoStack が未設定です");
    if (ctx.operators == nullptr) return Outcome::Err("NO_REGISTRY", "Operator レジストリが未初期化です");

    const bool isUndo = (type == "editor.undo");
    const char* operatorId = isUndo ? "edit.undo" : "edit.redo";
    /// @note 説明は実行前に読む (実行するとカーソルが動いて別のエントリを指す)。
    const std::string desc = isUndo ? ctx.undoStack->GetUndoDescription()
                                    : ctx.undoStack->GetRedoDescription();

    OpContext opContext{ ctx, *ctx.undoStack };
    if (!ctx.operators->CanInvoke(operatorId, opContext)) {
        JsonValue result = JsonValue::MakeObject();
        result.Set("applied", JsonValue(false));
        return Outcome::Ok(std::move(result));
    }
    if (dryRun) {
        JsonValue result = JsonValue::MakeObject();
        result.Set("dryRun", JsonValue(true));
        result.Set("would", JsonValue(type));
        result.Set("description", JsonValue(desc));
        return Outcome::Ok(std::move(result));
    }

    const OpResult opResult = ctx.operators->Invoke(operatorId, opContext);
    if (!opResult.ok)
        return Outcome::Err(opResult.errorCode.empty() ? "OP_FAILED" : opResult.errorCode,
                            opResult.message);

    JsonValue result = JsonValue::MakeObject();
    result.Set("applied", JsonValue(true));
    result.Set("description", JsonValue(desc));
    return Outcome::Ok(std::move(result));
}

Outcome DoSelectionSet(editor::EditorContext& ctx, const JsonValue& payload, bool dryRun)
{
    scene::Scene* activeScene = ctx.activeScene;
    if (activeScene == nullptr) return Outcome::Err("NO_SCENE", "アクティブシーンがありません");
    const JsonValue* ids = payload.Find("ids");
    if (ids == nullptr || !ids->IsArray()) return Outcome::Err("BAD_ARG", "ids 配列が必要です");
    std::vector<EntityID> resolved;
    int missing = 0;
    for (const JsonValue& idValue : ids->AsArray()) {
        if (!idValue.IsString()) continue;
        if (GameObject* go = activeScene->FindByGuid(idValue.AsString())) resolved.push_back(go->GetID());
        else ++missing;
    }
    if (dryRun) {
        JsonValue result = JsonValue::MakeObject();
        result.Set("dryRun", JsonValue(true));
        result.Set("resolved", JsonValue(static_cast<int>(resolved.size())));
        result.Set("missing", JsonValue(missing));
        return Outcome::Ok(std::move(result));
    }
    /// @note AI が選んだものは人が確かめる対象なので、畳まれた親を開いてでも Hierarchy に出す。
    editor::SelectEntities(ctx, std::move(resolved));
    JsonValue result = JsonValue::MakeObject();
    result.Set("selected", JsonValue(static_cast<int>(ctx.selectedEntities.size())));
    result.Set("missing", JsonValue(missing));
    return Outcome::Ok(std::move(result));
}

Outcome DoSceneSelectionQuery(editor::EditorContext& ctx)
{
    scene::Scene* activeScene = ctx.activeScene;
    JsonValue ids = JsonValue::MakeArray();
    if (activeScene != nullptr) {
        for (EntityID id : ctx.selectedEntities) {
            if (GameObject* go = activeScene->GetGameObject(id)) ids.Push(JsonValue(go->instanceId));
        }
    }
    JsonValue result = JsonValue::MakeObject();
    result.Set("ids", std::move(ids));
    return Outcome::Ok(std::move(result));
}
} // namespace

void RegisterEditorHandlers(BusHandlerTable& table)
{
    table.AddQuery("editor.catalog", [](BusCall&) { return Outcome::Ok(BuildEditorCatalog()); });
    table.AddQuery("editor.catalog.search", [](BusCall& call) { return SearchEditorCatalog(call.payload); });
    table.AddQuery("editor.state", [](BusCall& call) { return DoEditorState(call.ctx); });
    table.AddQuery("editor.undoHistory", [](BusCall& call) { return DoUndoHistory(call.ctx, call.payload); });
    table.AddQuery("console.logs", [](BusCall& call) { return DoConsoleLogs(call.ctx, call.payload); });
    table.AddQuery("scene.selection", [](BusCall& call) { return DoSceneSelectionQuery(call.ctx); });
    table.AddQuery("shader.inspect", [](BusCall& call) { return DoShaderInspect(call.ctx, call.payload); });
    table.AddQuery("shader.diagnostics", [](BusCall&) { return DoShaderCompileDiagnostics(); });
    table.AddQuery("profiler.snapshot", [](BusCall& call) { return DoProfilerSnapshot(call.ctx, call.payload); });
    table.AddQuery("build.status", [](BusCall& call) { return DoBuildStatus(call.ctx, call.payload); });
    /// @note Operator 登録簿はメニュー・ホットキー・パレットと同じもの。AI だけに見える操作を作れない (Docs/design/editor-operator-model.md)。
    table.AddQuery("editor.op.list", [](BusCall& call) { return FromBridge(ListOperators(call.ctx, call.payload)); });
    /// @note kind=query の Operator は read 権限で呼べるよう Query 側に置く。
    table.AddQuery("editor.op.query", [](BusCall& call) { return FromBridge(QueryOperator(call.ctx, call.payload)); });

    table.AddCommand("editor.op.invoke", [](BusCall& call) { return FromBridge(InvokeOperator(call.ctx, call.payload, call.dryRun)); });
    table.AddCommand("editor.undo", [](BusCall& call) { return DoUndoRedo(call.ctx, call.type, call.dryRun); });
    table.AddCommand("editor.redo", [](BusCall& call) { return DoUndoRedo(call.ctx, call.type, call.dryRun); });
    table.AddCommand("selection.set", [](BusCall& call) { return DoSelectionSet(call.ctx, call.payload, call.dryRun); });
    /// @note シーンの中身の変更ではないため UndoStack へ載せない。
    table.AddCommand("build.run", [](BusCall& call) { return DoBuildRun(call.ctx, call.payload, call.dryRun); });
}

} // namespace fbzz::editor::ai::bus
