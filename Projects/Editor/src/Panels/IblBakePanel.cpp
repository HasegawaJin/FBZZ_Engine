// FBZZ Engine
// IblBakePanel.cpp | fbzz::editor
// HDRI → IBL ベイクパネル ImGui UI 実装
#include <Editor/Panels/IblBakePanel.hpp>
#include <Editor/Import/HdriLoader.hpp>
#include <Editor/Util/FileDialog.hpp>
#include <Editor/EditorContext.hpp>
#include <Engine/Renderer/IRenderer.hpp>
#include <Engine/Renderer/IIblBaker.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Core/Logger.hpp>
#include <imgui.h>
#include <Windows.h>
#include <shlobj.h>
#include <cstdio>
#include <string>
#include <string_view>

namespace fbzz::editor {

namespace {

// フォルダ選択ダイアログ (Win32 SHBrowseForFolder)
// WHY: FileDialog は OpenFile/SaveFile のみ提供しているため、
//      フォルダ選択は Win32 API を直接呼ぶ必要がある。
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

    // wchar_t → UTF-8
    const int size = WideCharToMultiByte(CP_UTF8, 0, path, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 0) return false;
    outPath.assign(static_cast<size_t>(size - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, path, -1, outPath.data(), size, nullptr, nullptr);
    return true;
}

} // namespace

void IblBakePanel::OnRenderContent(EditorContext& ctx)
{
    // ── HDRI ファイル ───────────────────────────────────────────────────────
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
            // ステータスをリセット
            m_status    = Status::Idle;
            m_statusMsg = "";
        }
    }
    ImGui::Spacing();

    // ── 出力ディレクトリ ────────────────────────────────────────────────────
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

    // ── ベース名 ────────────────────────────────────────────────────────────
    ImGui::Text("Base Name");
    ImGui::SetNextItemWidth(200.0f);
    ImGui::InputText("##baseName", m_baseName.data(), m_baseName.size());
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        ImGui::SetTooltip("例: \"sky\" → sky.ibl / sky_env.dds / sky_irr.dds / sky_prefilter.dds / sky_brdf.dds");

    ImGui::Separator();

    // ── 詳細設定 ────────────────────────────────────────────────────────────
    if (ImGui::CollapsingHeader("Advanced Settings")) {
        static const char* s_cubeSizeLabels[] = { "512", "1024", "2048", "4096" };
        static const int   s_cubeSizeVals[]   = { 512, 1024, 2048, 4096 };
        static const char* s_irrSizeLabels[]  = { "16", "32", "64" };
        static const int   s_irrSizeVals[]    = { 16, 32, 64 };

        // Env Cubemap サイズ
        {
            int sel = 2; // デフォルト 2048
            for (int i = 0; i < 4; ++i) if (s_cubeSizeVals[i] == m_envCubemapSize) sel = i;
            ImGui::SetNextItemWidth(100.0f);
            if (ImGui::Combo("Env Cubemap Size", &sel, s_cubeSizeLabels, 4))
                m_envCubemapSize = s_cubeSizeVals[sel];
        }
        // Irradiance サイズ
        {
            int sel = 1; // デフォルト 32
            for (int i = 0; i < 3; ++i) if (s_irrSizeVals[i] == m_irradianceSize) sel = i;
            ImGui::SetNextItemWidth(100.0f);
            if (ImGui::Combo("Irradiance Size", &sel, s_irrSizeLabels, 3))
                m_irradianceSize = s_irrSizeVals[sel];
        }
        // Prefiltered サイズ
        {
            int sel = 1; // デフォルト 512
            for (int i = 0; i < 4; ++i) if (s_cubeSizeVals[i] == m_prefilteredSize) sel = i;
            ImGui::SetNextItemWidth(100.0f);
            if (ImGui::Combo("Prefiltered Size", &sel, s_cubeSizeLabels, 4))
                m_prefilteredSize = s_cubeSizeVals[sel];
        }
        ImGui::SetNextItemWidth(100.0f);
        ImGui::DragInt("Prefiltered Mips", &m_prefilteredMips, 1.0f, 3, 8);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
            ImGui::SetTooltip("Mip 0 = roughness 0 (mirror), Mip N-1 = roughness 1 (diffuse-like)");
        ImGui::SetNextItemWidth(100.0f);
        ImGui::DragInt("BRDF LUT Size", &m_brdfLutSize, 1.0f, 128, 512);
        ImGui::SetNextItemWidth(100.0f);
        ImGui::DragInt("Sample Count",  &m_sampleCount, 4.0f, 256, 4096);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
            ImGui::SetTooltip("GGX 重点サンプリング数。大きいほど高品質だがベイクが遅くなる");
    }

    ImGui::Separator();
    ImGui::Spacing();

    // ── ベイクボタン ────────────────────────────────────────────────────────
    const bool canBake = m_hdriPath[0] != '\0'
                      && m_outputDir[0] != '\0'
                      && m_baseName[0]  != '\0'
                      && ctx.renderer   != nullptr
                      && m_status       != Status::Baking;

    ImGui::BeginDisabled(!canBake);
    if (ImGui::Button("Bake IBL", ImVec2(120.0f, 0.0f)))
        DoBake(ctx);
    ImGui::EndDisabled();

    // ステータス表示
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

void IblBakePanel::DoBake(EditorContext& ctx)
{
    m_status    = Status::Baking;
    m_statusMsg = "ベイク中...";

    // ── Step 1: HDRI 読み込み ───────────────────────────────────────────────
    HdriPixels pixels = HdriLoader::Load(m_hdriPath.data());
    if (!pixels.IsValid()) {
        FBZZ_LOG_ERROR("IblBakePanel: HDRI 読み込み失敗 [%s]", m_hdriPath.data());
        m_status    = Status::Error;
        m_statusMsg = "HDRI 読み込み失敗 (コンソールを確認)";
        return;
    }

    // ── Step 2: IblBaker 生成 ───────────────────────────────────────────────
    // DX11Renderer::CreateIblBaker() が DX11IblBaker を返す。
    // 上位レイヤーは IIblBaker* として使い DX11 を直接参照しない。
    auto baker = ctx.renderer->CreateIblBaker();
    if (!baker) {
        FBZZ_LOG_ERROR("IblBakePanel: CreateIblBaker() が nullptr を返しました");
        m_status    = Status::Error;
        m_statusMsg = "IblBaker 生成失敗";
        return;
    }

    // ── Step 3: ベイク実行 ─────────────────────────────────────────────────
    // DX11 は DXBC、DX12 は DXIL を別ディレクトリへ生成するため、抽象 API の表示名で選ぶ。
    // WHY: 上位レイヤーから具象 Renderer へダウンキャストせず、IBL Compute も同じ SM 契約に揃える。
    const bool usesDx12 = std::string_view(ctx.renderer->GetBackendName()) == "DirectX 12";
    const std::string compiledDir = ctx.hlslSourceDir
        + (usesDx12 ? "/compiled_dx12/" : "/compiled/");

    renderer::IblBakeInput input;
    input.pixels              = pixels.Pixels();
    input.equirectW           = pixels.width;
    input.equirectH           = pixels.height;
    input.envCubemapSize      = static_cast<uint32_t>(m_envCubemapSize);
    input.irradianceSize      = static_cast<uint32_t>(m_irradianceSize);
    input.prefilteredSize     = static_cast<uint32_t>(m_prefilteredSize);
    input.prefilteredMipCount = static_cast<uint32_t>(m_prefilteredMips);
    input.sampleCount         = static_cast<uint32_t>(m_sampleCount);
    input.brdfLutSize         = static_cast<uint32_t>(m_brdfLutSize);
    input.compiledShadersDir  = compiledDir;

    renderer::IblBakeOutput output;
    if (!baker->Bake(input, m_outputDir.data(), m_baseName.data(), output)) {
        m_status    = Status::Error;
        m_statusMsg = "ベイク失敗 (コンソールを確認)";
        return;
    }

    // 同名 DDS を上書きした場合も、実行中のビューポートへ新しいベイク結果を即時反映する。
    // WHY: LoadTexture のパスキャッシュを放置すると、ディスク更新後も古い IBL が表示され続ける。
    if (ctx.resources) {
        ctx.resources->ReloadTexture(output.envCubemapPath);
        ctx.resources->ReloadTexture(output.irradiancePath);
        ctx.resources->ReloadTexture(output.prefilteredPath);
        ctx.resources->ReloadTexture(output.brdfLutPath);
    }

    m_status    = Status::Done;
    m_statusMsg = std::string("完了: ") + m_baseName.data() + ".ibl";
    FBZZ_LOG_INFO("IblBakePanel: ベイク完了 → [%s/%s.ibl]",
                  m_outputDir.data(), m_baseName.data());
}

} // namespace fbzz::editor
