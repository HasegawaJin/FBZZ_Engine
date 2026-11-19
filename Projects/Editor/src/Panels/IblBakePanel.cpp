/// @file    IblBakePanel.cpp
/// @brief   HDRI → IBL ベイクパネル ImGui UI 実装。
/// @author  Hasegawa Jin
/// @date    2026-06-23
#include <Editor/Util/PropertyControls.hpp>
#include <Editor/Op/EditorOperator.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Editor/Panels/IblBakePanel.hpp>
#include <Editor/Import/HdriLoader.hpp>
#include <Editor/Util/FileDialog.hpp>
#include <Editor/EditorContext.hpp>
#include <Engine/Renderer/IRenderer.hpp>
#include <Engine/Renderer/IIblBaker.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Asset/IblDescriptorWriter.hpp>
#include <filesystem>
#include <imgui.h>
#include <Windows.h>
#include <shlobj.h>
#include <cstdio>
#include <string>
#include <string_view>

namespace fbzz::editor {

namespace {

/// @note フォルダ選択ダイアログ (Win32 SHBrowseForFolder)。
/// @note FileDialog は OpenFile/SaveFile のみ提供するため、フォルダ選択は Win32 API を直接呼ぶ。
bool BrowseForFolder(std::string& outPath)
{
    BROWSEINFOW bi{};
    bi.hwndOwner = GetForegroundWindow();
    bi.ulFlags   = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return false;

    wchar_t path[MAX_PATH]{};
    SHGetPathFromIDListW(pidl, path);
    CoTaskMemFree(pidl);

    /// @note wchar_t → UTF-8
    const int size = WideCharToMultiByte(CP_UTF8, 0, path, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 0) return false;
    outPath.assign(static_cast<size_t>(size - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, path, -1, outPath.data(), size, nullptr, nullptr);
    return true;
}

} /// @note namespace

void IblBakePanel::OnRenderContent(EditorContext& ctx)
{
    ImGui::BeginDisabled(m_status == Status::Baking);
    /// @name HDRI ファイル
    ImGui::Text("HDRI File (.hdr / .exr)");
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 88.0f);
    ImGui::InputText("##hdriPath", m_hdriPath.data(), m_hdriPath.size());
    ImGui::SameLine();
    if (ImGui::Button("Browse##hdri")) {
        std::string chosen;
        if (FileDialog::OpenFile(
                GetForegroundWindow(),
                { { "HDRI Files", "*.hdr;*.exr" },
                  { "Radiance HDR", "*.hdr" },
                  { "OpenEXR",      "*.exr"  } },
                chosen))
        {
            snprintf(m_hdriPath.data(), m_hdriPath.size(), "%s", chosen.c_str());
            /// @note ステータスをリセット
            m_status    = Status::Idle;
            m_statusMsg = "";
        }
    }
    ImGui::Spacing();

    /// @name 出力ディレクトリ
    ImGui::Text("Output Directory");
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 88.0f);
    ImGui::InputText("##outDir", m_outputDir.data(), m_outputDir.size());
    ImGui::SameLine();
    if (ImGui::Button("Browse##dir")) {
        std::string chosen;
        if (BrowseForFolder(chosen))
            snprintf(m_outputDir.data(), m_outputDir.size(), "%s", chosen.c_str());
    }
    ImGui::Spacing();

    /// @name ベース名
    ImGui::Text("Base Name");
    ImGui::SetNextItemWidth(200.0f);
    ImGui::InputText("##baseName", m_baseName.data(), m_baseName.size());
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        ImGui::SetTooltip("例: \"sky\" → sky.ibl / sky_env.dds / sky_irr.dds / sky_prefilter.dds / sky_brdf.dds");

    ImGui::Separator();

    /// @name 詳細設定
    if (ImGui::CollapsingHeader("Advanced Settings")) {
        static const char* s_cubeSizeLabels[] = { "512", "1024", "2048", "4096" };
        static const int   s_cubeSizeVals[]   = { 512, 1024, 2048, 4096 };
        static const char* s_irrSizeLabels[]  = { "16", "32", "64" };
        static const int   s_irrSizeVals[]    = { 16, 32, 64 };

        /// @note Env Cubemap サイズ
        {
            /// @note デフォルト 2048
            int sel = 2;
            for (int i = 0; i < 4; ++i) if (s_cubeSizeVals[i] == m_envCubemapSize) sel = i;
            ImGui::SetNextItemWidth(100.0f);
            if (property::Combo("Env Cubemap Size", &sel, s_cubeSizeLabels, 4))
                m_envCubemapSize = s_cubeSizeVals[sel];
        }
        /// @note Irradiance サイズ
        {
            /// @note デフォルト 32
            int sel = 1;
            for (int i = 0; i < 3; ++i) if (s_irrSizeVals[i] == m_irradianceSize) sel = i;
            ImGui::SetNextItemWidth(100.0f);
            if (property::Combo("Irradiance Size", &sel, s_irrSizeLabels, 3))
                m_irradianceSize = s_irrSizeVals[sel];
        }
        /// @note Prefiltered サイズ
        {
            /// @note デフォルト 512
            int sel = 1;
            for (int i = 0; i < 4; ++i) if (s_cubeSizeVals[i] == m_prefilteredSize) sel = i;
            ImGui::SetNextItemWidth(100.0f);
            if (property::Combo("Prefiltered Size", &sel, s_cubeSizeLabels, 4))
                m_prefilteredSize = s_cubeSizeVals[sel];
        }
        ImGui::SetNextItemWidth(100.0f);
        property::DragInt("Prefiltered Mips", &m_prefilteredMips, 1.0f, 3, 8);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
            ImGui::SetTooltip("Mip 0 = roughness 0 (mirror), Mip N-1 = roughness 1 (diffuse-like)");
        ImGui::SetNextItemWidth(100.0f);
        property::DragInt("BRDF LUT Size", &m_brdfLutSize, 1.0f, 128, 512);
        ImGui::SetNextItemWidth(100.0f);
        property::DragInt("Sample Count",  &m_sampleCount, 4.0f, 256, 4096);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
            ImGui::SetTooltip("GGX 重点サンプリング数。大きいほど高品質だがベイクが遅くなる");
    }

    ImGui::Separator();
    ImGui::Spacing();

    ImGui::EndDisabled();

    /// @name ベイクボタン
    const bool canBake = m_hdriPath[0] != '\0'
                      && m_outputDir[0] != '\0'
                      && m_baseName[0]  != '\0'
                      && ctx.renderer   != nullptr
                      && m_status       != Status::Baking;

    ImGui::BeginDisabled(!canBake);
    if (ImGui::Button(LOCT("Bake IBL"), ImVec2(120.0f, 0.0f))) {
        const auto result = InvokeOperator(ctx, "ibl.bake");
        if (!result.ok) {
            m_status = Status::Error;
            m_statusMsg = result.message;
        }
    }
    ImGui::EndDisabled();
    if (m_status == Status::Baking) {
        const int progress = m_progress.load();
        ImGui::ProgressBar(std::max(0, progress) / 100.0f, { -1.0f, 0.0f },
            progress < 0 ? LOCT("Cancelling...") : progress >= SAVE_PROGRESS ? LOCT("Saving files...")
            : progress >= 25 ? LOCT("Baking IBL...") : LOCT("Preparing HDRI..."));
        ImGui::BeginDisabled(progress < 0 || progress >= SAVE_PROGRESS);
        if (ImGui::Button(LOCT("Cancel"))) (void)InvokeOperator(ctx, "ibl.cancel");
        ImGui::EndDisabled();
    }

    /// @note ステータス表示
    if (!m_statusMsg.empty()) {
        ImGui::SameLine();
        const ImVec4 green = ImVec4(0.3f, 1.0f, 0.3f, 1.0f);
        const ImVec4 red   = ImVec4(1.0f, 0.4f, 0.4f, 1.0f);
        switch (m_status) {
        case Status::Done:   ImGui::TextColored(green, "%s", m_statusMsg.c_str()); break;
        case Status::Error:  ImGui::TextColored(red,   "%s", m_statusMsg.c_str()); break;
        default:             ImGui::TextDisabled("%s", m_statusMsg.c_str());       break;
        }
    }
}

void IblBakePanel::StartBake(EditorContext& ctx)
{
    auto baker = ctx.renderer ? ctx.renderer->CreateIblBaker() : nullptr;
    if (!baker) {
        m_status = Status::Error;
        m_statusMsg = LOCT("IBL Baker is unavailable.");
        return;
    }

    renderer::IblBakeInput input;
    input.envCubemapSize = static_cast<std::uint32_t>(m_envCubemapSize);
    input.irradianceSize = static_cast<std::uint32_t>(m_irradianceSize);
    input.prefilteredSize = static_cast<std::uint32_t>(m_prefilteredSize);
    input.prefilteredMipCount = static_cast<std::uint32_t>(m_prefilteredMips);
    input.sampleCount = static_cast<std::uint32_t>(m_sampleCount);
    input.brdfLutSize = static_cast<std::uint32_t>(m_brdfLutSize);
    input.compiledShadersDir = ctx.hlslSourceDir + "/compiled_dx12/";
    input.reportProgress = [this](int progress) {
        int observed = m_progress.load();
        while (observed >= 0) {
            if (m_progress.compare_exchange_weak(observed, progress)) return true;
        }
        return false;
    };

    m_statusMsg.clear();
    m_bake = std::async(std::launch::async, [baker = std::move(baker), input = std::move(input),
        path = std::string(m_hdriPath.data()), directory = std::string(m_outputDir.data()),
        name = std::string(m_baseName.data())]() mutable {
        BakeResult result;
        if (!input.reportProgress(5)) return result;
        HdriPixels pixels = HdriLoader::Load(path);
        if (!input.reportProgress(18)) return result;
        if (!pixels.IsValid()) {
            result.message = "Could not read the HDRI file.";
            return result;
        }
        input.pixels = pixels.Pixels();
        input.equirectW = pixels.width;
        input.equirectH = pixels.height;
        if (!baker->Bake(input, directory, name, result.output)) {
            result.message = "IBL bake failed. See Console for details.";
            return result;
        }
        const auto descriptor = util::FileSystem::PathFromUtf8(directory) / (name + ".ibl");
        result.success = asset::WriteIblDescriptor(descriptor, result.output);
        result.message = result.success ? name + ".ibl" : "Could not save the IBL descriptor.";
        return result;
    });
}

bool IblBakePanel::CancelBake()
{
    if (m_bakeRequested) {
        m_bakeRequested = false;
        m_status = Status::Cancelled;
        m_statusMsg = LOCT("IBL bake cancelled.");
        return true;
    }
    if (m_status != Status::Baking) return false;
    int observed = m_progress.load();
    while (observed >= 0 && observed < SAVE_PROGRESS) {
        if (m_progress.compare_exchange_weak(observed, -1)) return true;
    }
    return false;
}

void IblBakePanel::FinishBake(EditorContext& ctx, const BakeResult& result)
{
    if (m_progress.load() < 0) {
        m_status = Status::Cancelled;
        m_statusMsg = LOCT("IBL bake cancelled.");
        return;
    }
    if (!result.success) {
        m_status = Status::Error;
        m_statusMsg = LOCT(result.message.c_str());
        return;
    }

    /// @note 描画リソースの更新は Editor フレームで行う。
    if (ctx.resources) {
        ctx.resources->ReloadTexture(result.output.envCubemapPath);
        ctx.resources->ReloadTexture(result.output.irradiancePath);
        ctx.resources->ReloadTexture(result.output.prefilteredPath);
        ctx.resources->ReloadTexture(result.output.brdfLutPath);
    }
    m_progress = 100;
    m_status = Status::Done;
    m_statusMsg = std::string(LOCT("Saved")) + " " + result.message;
    ctx.requestAssetBrowserRefresh = true;
}

void IblBakePanel::OnShutdown()
{
    (void)CancelBake();
    if (m_bake.valid()) m_bake.wait();
}

} /// @note namespace fbzz::editor
