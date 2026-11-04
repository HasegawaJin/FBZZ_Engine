/// @file    RenderPassOperators.cpp
/// @brief   描画パスと途中画像の設定・照会・PNG取得を直接APIへ公開する。
/// @author  Hasegawa Jin
/// @date    2026-11-04
#include <Editor/Panels/RenderPassViewerPanel.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/Op/EditorOperator.hpp>
#include <Engine/Renderer/IRenderer.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <cmath>
#include <filesystem>

namespace fbzz::editor {
namespace {
using Capture = scene::RenderPassCapture;
constexpr const char* MODES[] = {"auto", "rgb", "red", "green", "blue", "alpha", "signed_vector", "linear_depth"};
bool Number(const OpData& data, float& value)
{
    if (data.IsFloat()) return data.TryGetFloat(value);
    if (!data.IsInt() || data.AsInt64() < -1000000 || data.AsInt64() > 1000000) return false;
    value = static_cast<float>(data.AsInt64()); return true;
}
bool PatchView(const OpData& data, Capture::ViewSettings& view)
{
    if (!data.IsObject()) return false;
    for (const auto& [key, value] : data.AsObject()) {
        if (key == "mode") {
            if (!value.IsString()) return false;
            int found = -1; for (int i = 0; i < 8; ++i) if (value.AsString() == MODES[i]) found = i;
            if (found < 0) return false; view.mode = static_cast<Capture::DisplayMode>(found);
        } else if (key == "gamma") { if (!value.IsBool()) return false; view.gamma = value.AsBool(); }
        else if (key == "exposure") { if (!Number(value, view.exposure) || view.exposure < -16 || view.exposure > 16) return false; }
        else if (key == "rangeMin") { if (!Number(value, view.rangeMin)) return false; }
        else if (key == "rangeMax") { if (!Number(value, view.rangeMax)) return false; }
        else return false;
    }
    return view.rangeMax > view.rangeMin;
}
OpData DescribeView(const Capture::ViewSettings& view)
{
    OpData data = OpData::MakeObject(); data.Set("mode", OpData(MODES[static_cast<int>(view.mode)]));
    data.Set("exposure", OpData(view.exposure)); data.Set("rangeMin", OpData(view.rangeMin));
    data.Set("rangeMax", OpData(view.rangeMax)); data.Set("gamma", OpData(view.gamma)); return data;
}
} /// @note namespace
void RenderPassViewerPanel::OnRegisterOperators(OperatorRegistry& registry)
{
    for (const char* action : {"inspect", "configure", "capture"}) {
        EditorOperator op; const std::string name = action;
        op.id = "renderpass." + name; op.label = op.id; op.category = "Render Pass";
        op.kind = name == "inspect" ? OpKind::Query : OpKind::Action;
        op.desc = "描画パスと資源を画面なしで調べる。configureでenabled/view/pass/occurrence/output/viewSettings/gallery/galleryViewを部分指定し、次の描画後にinspect/captureする。captureは生成画像をLibrary/Capturesへ保存する。";
        if (name == "configure") { OpParam settings; settings.name = "settings"; settings.type = OpParamType::Data; op.params.push_back(std::move(settings)); }
        if (name == "capture") {
            for (const char* field : {"name", "resource"}) { OpParam value; value.name = field; value.type = OpParamType::String; value.required = false; op.params.push_back(std::move(value)); }
        }
        op.exec = [this, name](OpContext& context, const OpArgs& args) {
            if (name == "configure") {
                const OpData* patch = args.GetData("settings");
                if (!patch || !patch->IsObject()) return OpResult::Err("BAD_ARG", "settingsはオブジェクトです。");
                auto request = m_capture.request; auto galleryView = m_capture.galleryView;
                bool enabled = true, game = m_gameView, gallery = m_apiGallery;
                for (const auto& [key, value] : patch->AsObject()) {
                    if (key == "enabled" || key == "gallery" || key == "outputIsPreference") {
                        if (!value.IsBool()) return OpResult::Err("BAD_ARG", key);
                        if (key == "enabled") enabled = value.AsBool(); else if (key == "gallery") gallery = value.AsBool(); else request.outputIsPreference = value.AsBool();
                    } else if (key == "view" || key == "pass" || key == "output") {
                        if (!value.IsString()) return OpResult::Err("BAD_ARG", key);
                        if (key == "view") { if (value.AsString() != "scene" && value.AsString() != "game") return OpResult::Err("BAD_ARG", "viewはscene/gameです。"); game = value.AsString() == "game"; }
                        else if (key == "pass") request.passName = value.AsString(); else request.outputName = value.AsString();
                    } else if (key == "occurrence") {
                        if (!value.IsInt() || value.AsInt64() < 0 || value.AsInt64() > 65535) return OpResult::Err("BAD_ARG", key); request.occurrence = static_cast<std::size_t>(value.AsInt64());
                    } else if (key == "viewSettings") { if (!PatchView(value, request.view)) return OpResult::Err("BAD_ARG", key); }
                    else if (key == "galleryView") { if (!PatchView(value, galleryView)) return OpResult::Err("BAD_ARG", key); }
                    else return OpResult::Err("BAD_ARG", key);
                }
                if (request.passName.empty()) return OpResult::Err("BAD_ARG", "passが空です。");
                m_apiCapture = enabled; m_apiGallery = gallery; m_gameView = game;
                m_capture.request = request; m_capture.galleryView = galleryView; m_capture.galleryEnabled = enabled && gallery;
            }
            if (name == "capture") {
                auto& ctx = context.ctx;
                if (!ctx.renderer || !ctx.resources || ctx.projectRoot.empty()) return OpResult::Err("NO_RENDERER", "描画デバイスまたはプロジェクトがありません。");
                const std::string resource = args.GetString("resource");
                auto target = m_capture.Preview(); bool available = m_capture.HasPreview();
                const auto& captured = m_capture.CapturedRequest(); const auto& request = m_capture.request;
                available = available && m_capturedGameView == m_gameView && captured.passName == request.passName
                    && captured.occurrence == request.occurrence && captured.outputName == request.outputName
                    && captured.outputIsPreference == request.outputIsPreference && captured.view == request.view;
                if (!resource.empty()) {
                    available = false;
                    if (m_capture.galleryEnabled && !m_capture.Passes().empty()
                        && m_capture.CapturedGalleryView() == m_capture.galleryView && m_capturedGameView == m_gameView)
                        for (const auto& tile : m_capture.Gallery()) if (tile.label == resource && tile.hasImage) {
                            target = tile.image.Handle(); available = true; break;
                        }
                }
                if (!available) return OpResult::Err("CAPTURE_NOT_READY", "指定ビュー・パス・表示設定の描画を待ってください。resourceはinspectのgallery.labelです。");
                const std::string filename = args.GetString("name", "RenderPass.png");
                if (filename.empty() || filename.size() > 128 || filename.find_first_of("/\\:*?\"<>|") != std::string::npos
                    || util::FileSystem::PathFromUtf8(filename).extension() != ".png") return OpResult::Err("BAD_ARG", "nameはPNGファイル名です。");
                std::vector<uint8_t> png; uint32_t width = 0, height = 0;
                if (!ctx.renderer->CaptureRenderTargetToPng(target, *ctx.resources, png, width, height) || png.empty()) return OpResult::Err("CAPTURE_FAILED", "画像を読み戻せません。");
                const auto directory = util::FileSystem::PathFromUtf8(ctx.projectRoot) / "Library/Captures";
                if (!util::FileSystem::EnsureDirectory(directory) || !util::FileSystem::WriteBinaryAtomic(directory / util::FileSystem::PathFromUtf8(filename), png.data(), png.size()))
                    return OpResult::Err("CAPTURE_WRITE_FAILED", "PNGを保存できません。");
                OpData data = OpData::MakeObject(); data.Set("path", OpData(util::FileSystem::PathToUtf8(directory / util::FileSystem::PathFromUtf8(filename))));
                data.Set("width", OpData(width)); data.Set("height", OpData(height)); return OpResult::Data(std::move(data));
            }
            SyncSnapshot();
            OpData settings = OpData::MakeObject(); settings.Set("enabled", OpData(m_apiCapture)); settings.Set("view", OpData(m_gameView ? "game" : "scene"));
            settings.Set("pass", OpData(m_capture.request.passName)); settings.Set("occurrence", OpData(static_cast<std::int64_t>(m_capture.request.occurrence)));
            settings.Set("output", OpData(m_capture.request.outputName)); settings.Set("outputIsPreference", OpData(m_capture.request.outputIsPreference));
            settings.Set("viewSettings", DescribeView(m_capture.request.view)); settings.Set("gallery", OpData(m_apiGallery)); settings.Set("galleryView", DescribeView(m_capture.galleryView));
            OpData passes = OpData::MakeArray();
            for (const auto& pass : m_passes) {
                OpData row = OpData::MakeObject(); row.Set("name", OpData(pass.name)); row.Set("occurrence", OpData(static_cast<std::int64_t>(pass.occurrence)));
                row.Set("graphIndex", OpData(static_cast<std::int64_t>(pass.graphIndex))); row.Set("culled", OpData(pass.culled));
                row.Set("cpuMs", OpData(pass.cpuMs)); row.Set("gpuMs", OpData(pass.gpuMs)); OpData accesses = OpData::MakeArray();
                for (const auto& access : pass.accesses) { OpData value = OpData::MakeObject(); value.Set("name", OpData(access.name));
                    value.Set("usage", OpData(access.usage == renderer::RenderResourceUsage::Read ? "read" : access.usage == renderer::RenderResourceUsage::Write ? "write" : "read_write")); accesses.Push(std::move(value)); }
                row.Set("accesses", std::move(accesses)); passes.Push(std::move(row));
            }
            OpData outputs = OpData::MakeArray(), gallery = OpData::MakeArray();
            for (const auto& output : m_capture.Outputs()) { OpData row = OpData::MakeObject(); row.Set("name", OpData(output.name));
                row.Set("width", OpData(output.width)); row.Set("height", OpData(output.height)); row.Set("depth", OpData(output.depth)); outputs.Push(std::move(row)); }
            for (const auto& tile : m_capture.Gallery()) { OpData row = OpData::MakeObject(); row.Set("resource", OpData(tile.resource)); row.Set("label", OpData(tile.label));
                row.Set("issue", OpData(static_cast<int>(tile.issue))); row.Set("hasImage", OpData(tile.hasImage));
                row.Set("width", OpData(tile.sourceWidth)); row.Set("height", OpData(tile.sourceHeight)); gallery.Push(std::move(row)); }
            OpData data = OpData::MakeObject(); data.Set("settings", std::move(settings)); data.Set("passes", std::move(passes));
            data.Set("outputs", std::move(outputs)); data.Set("gallery", std::move(gallery)); data.Set("live", OpData(m_live));
            data.Set("status", OpData(m_capture.Status())); data.Set("hasPreview", OpData(m_capture.HasPreview())); data.Set("resolvedOutput", OpData(m_capture.ResolvedOutputName()));
            return OpResult::Data(std::move(data));
        }; registry.Register(std::move(op));
    }
}
} /// @note namespace fbzz::editor
