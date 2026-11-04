/// @file    DiagnosticOperators.cpp
/// @brief   メモリ差分の基準と描画計画を画面なしで取得・保存する。
/// @author  Hasegawa Jin
/// @date    2026-11-04
#include <Editor/Op/EditorOperator.hpp>
#include <Editor/Op/OperatorGroups.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/PackageBuildService.hpp>
#include <Editor/Util/StandaloneLauncher.hpp>
#include <Editor/Profiler/ProfilerHistory.hpp>
#include <Editor/Util/MemoryLeakDiff.hpp>
#include <Engine/Renderer/RenderDebugOverlay.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <charconv>
#include <filesystem>
namespace fbzz::editor {
void RegisterDiagnosticOperators(OperatorRegistry& registry)
{
    for (const bool launch : {false, true}) {
        EditorOperator op; op.id = launch ? "build.package_launch" : "build.package_reset"; op.label = op.id; op.category = "Build"; op.kind = OpKind::Action;
        op.desc = "完了したゲーム配布を実行するか、停止中の要求結果を消去する。packageRequestIdで現在の要求を指定できる。";
        OpParam p; p.name = "packageRequestId"; p.required = false; op.params.push_back(p);
        op.exec = [launch](OpContext& context, const OpArgs& args) {
            auto* service = context.ctx.packageBuild;
            if (!service) return OpResult::Err("NO_PACKAGE_SERVICE", "配布ビルドを利用できません。");
            if (args.Has("packageRequestId") && args.GetString("packageRequestId") != service->RequestId()) return OpResult::Err("PACKAGE_REQUEST_NOT_FOUND", "現在の要求を指定してください。");
            if (service->IsRunning()) return OpResult::Err("PACKAGE_RUNNING", "配布ビルドが実行中です。");
            if (!launch) service->Reset();
            else {
                const auto& pipeline = service->Pipeline(); const auto& exe = pipeline.GetOutputExePath();
                if (pipeline.GetState() != BuildPipeline::State::Done || !util::FileSystem::Exists(exe)) return OpResult::Err("NO_PACKAGE", "実行可能な配布がありません。");
                if (!StandaloneLauncher::LaunchExe(exe, "")) return OpResult::Err("LAUNCH_FAILED", "配布ゲームを起動できません。");
            }
            auto data = service->Status(); if (launch) data.Set("launched", OpData(true)); return OpResult::Data(std::move(data));
        }; registry.Register(std::move(op));
    }
    {
        EditorOperator op; op.id = "profiler.memory.set_recording"; op.label = op.id; op.kind = OpKind::Action; op.category = "Profiler";
        OpParam p; p.name = "recording"; p.type = OpParamType::Bool; op.params.push_back(p);
        op.exec = [](OpContext&, const OpArgs& args) { SetMemoryHistoryRecording(args.GetBool("recording")); return OpResult::Ok(); }; registry.Register(std::move(op));
    }
    for (const bool baseline : {false, true}) {
        EditorOperator op; op.id = baseline ? "profiler.memory.baseline" : "profiler.memory.diff"; op.label = op.id; op.category = "Profiler";
        op.kind = baseline ? OpKind::Action : OpKind::Query;
        op.desc = "Rendererリソースの手動基準・現在差分・Play/Stop累積・固定結果を取得する。基準のcapture/clearは共有MemoryLeakDiffに反映される。";
        if (baseline) { OpParam p; p.name = "action"; p.enumValues = {"capture", "clear"}; op.params.push_back(p); }
        op.exec = [baseline](OpContext& context, const OpArgs& args) {
            auto& ctx = context.ctx; if (!ctx.memoryLeakDiff || !ctx.resources) return OpResult::Err("NO_RESOURCES", "リソース台帳がありません。");
            auto& diff = *ctx.memoryLeakDiff;
            if (baseline) {
                if (args.GetString("action") == "capture") diff.CaptureBaseline(*ctx.resources, "Manual");
                else { diff.ClearBaseline(); diff.ClearSessionBaseline(); }
            }
            const auto describe = [](const MemoryLeakReport& report) {
                OpData data = OpData::MakeObject(), rows = OpData::MakeArray(), released = OpData::MakeArray();
                const auto append = [](const auto& source, OpData& output) {
                    for (const auto& row : source) { OpData item = OpData::MakeObject(); item.Set("origin", OpData(row.origin)); item.Set("allocator", OpData(row.allocatorName));
                        item.Set("countDelta", OpData(static_cast<std::int64_t>(row.countDelta))); item.Set("bytesDelta", OpData(static_cast<std::int64_t>(row.bytesDelta))); output.Push(std::move(item)); }
                };
                append(report.rows, rows); append(report.shrunkRows, released); data.Set("rows", std::move(rows)); data.Set("released", std::move(released));
                data.Set("valid", OpData(report.valid)); data.Set("label", OpData(report.label)); data.Set("countDelta", OpData(static_cast<std::int64_t>(report.totalCountDelta)));
                data.Set("bytesDelta", OpData(static_cast<std::int64_t>(report.totalBytesDelta))); data.Set("liveCount", OpData(static_cast<std::int64_t>(report.liveCount))); return data;
            };
            OpData data = OpData::MakeObject(); data.Set("current", describe(diff.Compare(*ctx.resources))); data.Set("session", describe(diff.CompareSession(*ctx.resources)));
            data.Set("pinned", describe(diff.GetPinnedReport())); data.Set("cycleCount", OpData(diff.GetCycleCount())); data.Set("text", OpData(FormatMemoryReport(*ctx.resources, diff)));
            return OpResult::Data(std::move(data));
        }; registry.Register(std::move(op));
    }
    for (const bool save : {false, true}) {
        EditorOperator op; op.id = save ? "profiler.performance.save_render_plan" : "profiler.performance.render_plan"; op.label = op.id; op.category = "Profiler";
        op.kind = save ? OpKind::Action : OpKind::Query;
        for (const char* key : {"captureSessionId", "applicationFrameSerial"}) { OpParam p; p.name = key; p.required = false; op.params.push_back(p); }
        if (save) { OpParam p; p.name = "name"; p.required = false; op.params.push_back(p); }
        op.exec = [save](OpContext& context, const OpArgs& args) {
            std::string text = renderer::RenderDebugOverlay::GetLastSnapshot().planDescription;
            if (args.Has("captureSessionId") || args.Has("applicationFrameSerial")) {
                const auto sessionText = args.GetString("captureSessionId"), serialText = args.GetString("applicationFrameSerial");
                std::uint64_t session = 0, serial = 0;
                const auto first = std::from_chars(sessionText.data(), sessionText.data() + sessionText.size(), session);
                const auto second = std::from_chars(serialText.data(), serialText.data() + serialText.size(), serial);
                if (sessionText.empty() || serialText.empty() || first.ec != std::errc{} || second.ec != std::errc{} || first.ptr != sessionText.data() + sessionText.size() || second.ptr != serialText.data() + serialText.size()) return OpResult::Err("BAD_ARG", "session/serialの組を指定してください。");
                bool found = false; for (const auto& frame : GetPerformanceHistory().frames) if (frame->captureSessionId == session && frame->applicationFrameSerial == serial) { text = frame->rendering.planDescription; found = true; break; }
                if (!found) return OpResult::Err("FRAME_NOT_FOUND", "保持されたフレームがありません。");
            }
            if (text.empty()) return OpResult::Err("NO_PLAN", "描画計画がまだありません。");
            OpData data = OpData::MakeObject(); data.Set("text", OpData(text));
            if (save) {
                const auto name = args.GetString("name", "McpRenderPlan.txt");
                if (context.ctx.projectRoot.empty() || name.empty() || name.size() > 128 || name.find_first_of("/\\:*?\"<>|") != std::string::npos || name.find('\0') != std::string::npos
                    || util::FileSystem::PathFromUtf8(name).extension() != ".txt") return OpResult::Err("BAD_ARG", "nameは.txtファイル名です。");
                const auto directory = util::FileSystem::PathFromUtf8(context.ctx.projectRoot) / "Artifacts/RenderGraph"; const auto path = directory / util::FileSystem::PathFromUtf8(name);
                if (!util::FileSystem::EnsureDirectory(directory) || !util::FileSystem::WriteTextAtomic(util::FileSystem::PathToUtf8(path), text)) return OpResult::Err("SAVE_FAILED", "描画計画を保存できません。");
                data.Set("path", OpData(util::FileSystem::PathToUtf8(path)));
            }
            return OpResult::Data(std::move(data));
        }; registry.Register(std::move(op));
    }
}
} /// @note namespace fbzz::editor
