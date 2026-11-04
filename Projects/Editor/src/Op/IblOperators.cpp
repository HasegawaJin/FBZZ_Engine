/// @file    IblOperators.cpp
/// @brief   HDRIベイクの設定・要求・完了確認を直接APIへ公開する。
/// @author  Hasegawa Jin
/// @date    2026-11-04
#include <Editor/Panels/IblBakePanel.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/Op/EditorOperator.hpp>
#include <Editor/PlayModeController.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <algorithm>
#include <cstdio>
#include <filesystem>

namespace fbzz::editor {
void IblBakePanel::OnUpdate(EditorContext& ctx, float dt)
{
    static_cast<void>(dt);
    if (!m_bakeRequested) return;
    m_bakeRequested = false;
    DoBake(ctx);
    ctx.requestAssetBrowserRefresh = true;
}
void IblBakePanel::OnRegisterOperators(OperatorRegistry& registry)
{
    for (const char* action : {"inspect", "configure", "bake"}) {
        EditorOperator op;
        const std::string name = action;
        op.id = "ibl." + name; op.label = op.id; op.category = "IBL Bake";
        op.kind = name == "inspect" ? OpKind::Query : OpKind::Action;
        op.desc = "HDRIベイクを画面なしで設定・実行する。bakeはジョブ番号を返し、次の更新でGPU処理を実行する。inspectのstateで完了/失敗を確認する。";
        if (name != "inspect") {
            OpParam settings; settings.name = "settings"; settings.type = OpParamType::Data; settings.required = name == "configure";
            op.params.push_back(std::move(settings));
        }
        op.poll = [name](const OpContext& context, const OpArgs&) { return name == "inspect" || !context.ctx.playMode || context.ctx.playMode->IsInEditor(); };
        op.exec = [this, name](OpContext& context, const OpArgs& args) {
            if (name != "inspect") {
                if (m_status == Status::Baking) return OpResult::Err("IBL_BUSY", "IBLベイクが実行中です。");
                std::string source = m_hdriPath.data(), output = m_outputDir.data(), base = m_baseName.data();
                int env = m_envCubemapSize, irradiance = m_irradianceSize, prefiltered = m_prefilteredSize;
                int mips = m_prefilteredMips, brdf = m_brdfLutSize, samples = m_sampleCount;
                if (const OpData* patch = args.GetData("settings")) {
                    if (!patch->IsObject()) return OpResult::Err("BAD_ARG", "settingsはオブジェクトです。");
                    for (const auto& [key, value] : patch->AsObject()) {
                        if (key == "source" || key == "outputDirectory" || key == "baseName") {
                            if (!value.IsString() || value.AsString().find('\0') != std::string::npos) return OpResult::Err("BAD_ARG", key);
                            if (key == "source") source = value.AsString();
                            else if (key == "outputDirectory") output = value.AsString(); else base = value.AsString();
                        } else {
                            int number = 0;
                            if (!value.TryGetInt32(number)) return OpResult::Err("BAD_ARG", key);
                            if (key == "envCubemapSize") env = number;
                            else if (key == "irradianceSize") irradiance = number;
                            else if (key == "prefilteredSize") prefiltered = number;
                            else if (key == "prefilteredMips") mips = number;
                            else if (key == "brdfLutSize") brdf = number;
                            else if (key == "sampleCount") samples = number;
                            else return OpResult::Err("BAD_ARG", key);
                        }
                    }
                }
                const auto cubeSize = [](int value) { return value == 512 || value == 1024 || value == 2048 || value == 4096; };
                if (!cubeSize(env) || !cubeSize(prefiltered) || (irradiance != 16 && irradiance != 32 && irradiance != 64)
                    || mips < 3 || mips > 8 || brdf < 128 || brdf > 512 || samples < 256 || samples > 4096
                    || source.size() >= m_hdriPath.size() || output.size() >= m_outputDir.size() || base.size() >= m_baseName.size()
                    || base.find_first_of("/\\:*?\"<>|") != std::string::npos || base == "." || base == "..")
                    return OpResult::Err("BAD_ARG", "IBL設定が許容範囲外です。");
                if (name == "bake") {
                    if (!context.ctx.renderer) return OpResult::Err("NO_RENDERER", "描画デバイスがありません。");
                    if (source.empty() || output.empty() || base.empty()) return OpResult::Err("BAD_ARG", "source/outputDirectory/baseNameが必要です。");
                    std::error_code error;
                    auto path = util::FileSystem::PathFromUtf8(source);
                    if (path.is_relative()) path = util::FileSystem::PathFromUtf8(context.ctx.projectRoot) / path;
                    if (!std::filesystem::is_regular_file(path, error)) return OpResult::Err("BAD_PATH", "HDRIが見つかりません。");
                    source = util::FileSystem::PathToUtf8(path);
                    auto destination = util::FileSystem::PathFromUtf8(output);
                    if (destination.is_relative()) destination = util::FileSystem::PathFromUtf8(context.ctx.projectRoot) / destination;
                    destination = std::filesystem::weakly_canonical(destination, error);
                    const auto root = std::filesystem::weakly_canonical(util::FileSystem::PathFromUtf8(context.ctx.projectRoot) / "Assets", error);
                    const auto relative = destination.lexically_relative(root);
                    if (error || relative.empty() || relative.is_absolute() || *relative.begin() == "..") return OpResult::Err("BAD_PATH", "出力先はAssets配下です。");
                    output = util::FileSystem::PathToUtf8(destination);
                    if (source.size() >= m_hdriPath.size() || output.size() >= m_outputDir.size()) return OpResult::Err("BAD_PATH", "パスが長すぎます。");
                }
                std::snprintf(m_hdriPath.data(), m_hdriPath.size(), "%s", source.c_str());
                std::snprintf(m_outputDir.data(), m_outputDir.size(), "%s", output.c_str());
                std::snprintf(m_baseName.data(), m_baseName.size(), "%s", base.c_str());
                m_envCubemapSize = env; m_irradianceSize = irradiance; m_prefilteredSize = prefiltered;
                m_prefilteredMips = mips; m_brdfLutSize = brdf; m_sampleCount = samples;
                if (name == "bake") { ++m_job; m_bakeRequested = true; m_status = Status::Baking; m_statusMsg = "Queued"; }
                else { m_status = Status::Idle; m_statusMsg.clear(); }
            }
            OpData settings = OpData::MakeObject();
            settings.Set("source", OpData(std::string(m_hdriPath.data()))); settings.Set("outputDirectory", OpData(std::string(m_outputDir.data())));
            settings.Set("baseName", OpData(std::string(m_baseName.data()))); settings.Set("envCubemapSize", OpData(m_envCubemapSize));
            settings.Set("irradianceSize", OpData(m_irradianceSize)); settings.Set("prefilteredSize", OpData(m_prefilteredSize));
            settings.Set("prefilteredMips", OpData(m_prefilteredMips)); settings.Set("brdfLutSize", OpData(m_brdfLutSize)); settings.Set("sampleCount", OpData(m_sampleCount));
            OpData data = OpData::MakeObject(); data.Set("settings", std::move(settings)); data.Set("job", OpData(m_job));
            data.Set("state", OpData(m_bakeRequested ? "queued" : m_status == Status::Baking ? "running" : m_status == Status::Done ? "done" : m_status == Status::Error ? "failed" : "idle"));
            data.Set("message", OpData(m_statusMsg)); return OpResult::Data(std::move(data));
        };
        registry.Register(std::move(op));
    }
}
} /// @note namespace fbzz::editor
