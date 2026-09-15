/// @file    BuildSettingsPanel.cpp
/// @brief   Build Settings パネルの ImGui UI 実装。
/// @author  Hasegawa Jin
/// @date    2026-05-31
#include <Editor/Panels/BuildSettingsPanel.hpp>

#include <Editor/EditorContext.hpp>
#include <Editor/PlayModeController.hpp>
#include <Editor/ToolchainLocator.hpp>
#include <Editor/Util/AppIconWriter.hpp>
#include <Editor/Util/EditorSettings.hpp>
#include <Editor/Util/ImGuiWidgets.hpp>
#include <Editor/Util/StandaloneLauncher.hpp>
#include <Editor/Util/UndoStack.hpp>
#include <Engine/Renderer/IImGuiRenderer.hpp>
#include <Engine/Renderer/RendererBackend.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Engine/Util/StringUtils.hpp>

#include <imgui.h>
#include <imgui_internal.h>
#include <Windows.h>
#include <shlobj.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <string>

namespace fbzz::editor {

namespace {

const ImVec4 kOkColor    { 0.45f, 0.85f, 0.50f, 1.0f };
const ImVec4 kWarnColor  { 1.00f, 0.72f, 0.25f, 1.0f };
const ImVec4 kErrorColor { 1.00f, 0.42f, 0.42f, 1.0f };

bool ScenesEqual(const std::vector<SceneEntry>& lhs, const std::vector<SceneEntry>& rhs)
{
    if (lhs.size() != rhs.size()) return false;
    for (std::size_t i = 0; i < lhs.size(); ++i) {
        if (lhs[i].path != rhs[i].path || lhs[i].enabled != rhs[i].enabled)
            return false;
    }
    return true;
}

bool BuildSettingsEqual(const BuildSettings& lhs, const BuildSettings& rhs)
{
    return lhs.outputDirectory == rhs.outputDirectory
        && lhs.productName == rhs.productName
        && lhs.version == rhs.version
        && lhs.iconPath == rhs.iconPath
        && lhs.developmentBuild == rhs.developmentBuild
        && lhs.stripEditorAssets == rhs.stripEditorAssets
        && ScenesEqual(lhs.scenes, rhs.scenes);
}

// フォルダ選択ダイアログ (Win32 SHBrowseForFolder)
bool BrowseForFolder(HWND hwnd, std::string& outPath)
{
    BROWSEINFOW bi{};
    bi.hwndOwner = hwnd;
    bi.ulFlags   = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;

    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return false;

    wchar_t path[MAX_PATH]{};
    SHGetPathFromIDListW(pidl, path);
    CoTaskMemFree(pidl);

    outPath = util::FileSystem::PathToUtf8(std::filesystem::path(path));
    return !outPath.empty();
}

void RevealInExplorer(const std::string& path)
{
    if (path.empty()) return;
    const std::wstring wpath = util::FileSystem::PathFromUtf8(path).wstring();
    ShellExecuteW(nullptr, L"open", L"explorer.exe", wpath.c_str(), nullptr, SW_SHOWNORMAL);
}

// プロジェクト相対パスの実体があるか。空パスは「未設定」なので false。
bool SceneExists(const std::string& projectRoot, const std::string& relativePath)
{
    if (projectRoot.empty() || relativePath.empty()) return false;
    const std::filesystem::path p = util::FileSystem::PathFromUtf8(relativePath);
    if (p.is_absolute()) return util::FileSystem::Exists(p);
    return util::FileSystem::Exists(util::FileSystem::PathFromUtf8(projectRoot) / p);
}

// アイコン 1 枚ぶんのプレビュー枠。texId が無ければ emptyLabel を枠の中央に出す。
// WHY 市松を敷くか: アイコンは透明部分を持つ。単色の上に描くと、その色まで絵の一部に見える。
void IconPreviewBox(void* texId, float side, const char* emptyLabel)
{
    const ImVec2 boxMin = ImGui::GetCursorScreenPos();
    const ImVec2 boxMax{ boxMin.x + side, boxMin.y + side };
    ImGui::Dummy({ side, side });

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const float cell = std::max(4.0f, side / 8.0f);
    drawList->PushClipRect(boxMin, boxMax, true);
    drawList->AddRectFilled(boxMin, boxMax, IM_COL32(70, 70, 70, 255));
    for (int y = 0; boxMin.y + static_cast<float>(y) * cell < boxMax.y; ++y) {
        for (int x = 0; boxMin.x + static_cast<float>(x) * cell < boxMax.x; ++x) {
            if (((x + y) & 1) == 0) continue;
            const ImVec2 cellMin{ boxMin.x + static_cast<float>(x) * cell,
                                  boxMin.y + static_cast<float>(y) * cell };
            drawList->AddRectFilled(cellMin, { cellMin.x + cell, cellMin.y + cell },
                                    IM_COL32(96, 96, 96, 255));
        }
    }
    drawList->PopClipRect();

    if (texId) {
        drawList->AddImage(widgets::ToImTextureID(texId), boxMin, boxMax);
    } else if (emptyLabel) {
        const ImVec2 textSize = ImGui::CalcTextSize(emptyLabel);
        drawList->AddText({ boxMin.x + (side - textSize.x) * 0.5f,
                            boxMin.y + (side - textSize.y) * 0.5f },
                          ImGui::GetColorU32(ImGuiCol_TextDisabled), emptyLabel);
    }
    drawList->AddRect(boxMin, boxMax, ImGui::GetColorU32(ImGuiCol_Border), 3.0f);
}

} // namespace

// =============================================================================
// 永続化 (EditorSettings 経由)
// =============================================================================

void BuildSettingsPanel::OnLoadSettings(const EditorSettings& settings)
{
    m_settings = settings.build;
    m_selectedSceneIdx = -1;
    m_checksValid = false;
}

void BuildSettingsPanel::OnSaveSettings(EditorSettings& settings) const
{
    settings.build = m_settings;
}

// =============================================================================
// 描画
// =============================================================================

void BuildSettingsPanel::OnRenderContent(EditorContext& ctx)
{
    // ディスクを読む事前チェックは、開いた瞬間とビルド前後だけ取り直す。
    if (ImGui::IsWindowAppearing() || !m_checksValid)
        RefreshChecks(ctx);

    // 1 回の操作 (テキスト入力の確定・チェックの切り替え) を 1 Undo にまとめる。
    struct UndoTracker {
        ImGuiID       activeId = 0;
        BuildSettings before;
        bool          active = false;
    };
    static UndoTracker undo;

    const bool recording = ctx.undoStack && ctx.undoStack->IsRecordingEnabled();
    const BuildSettings beforeDraw = m_settings;
    const ImGuiID activeBefore = recording ? ImGui::GetActiveID() : 0;

    DrawScenesInBuild(ctx);
    ImGui::Spacing();
    DrawOutputSettings(ctx);
    ImGui::Spacing();
    DrawPackageChecks(ctx);
    ImGui::Spacing();
    DrawProgressAndActions(ctx);

    if (!recording) {
        undo.active = false;
        return;
    }

    const ImGuiID activeAfter = ImGui::GetActiveID();

    auto pushCommand = [&](const BuildSettings& before, const BuildSettings& after) {
        if (!ctx.undoStack || BuildSettingsEqual(before, after)) return;
        BuildSettingsPanel* panel  = this;
        EditorContext*      target = &ctx;
        auto apply = [panel, target](const BuildSettings& value) {
            panel->m_settings = value;
            panel->m_checksValid = false;
            // WHY: Undo / Redo も編集と同じ重みで残す。エディターを落として次に
            //      開いたときに、戻したはずの設定が復活していると追跡できない。
            target->requestEditorSettingsSave = true;
        };
        ctx.undoStack->Push(std::make_unique<LambdaCommand>(
            "Edit Build Settings",
            [apply, after]() { apply(after); },
            [apply, before]() { apply(before); }));
    };

    if (!undo.active && activeAfter != 0 && activeAfter != activeBefore) {
        undo.activeId = activeAfter;
        undo.before   = beforeDraw;
        undo.active   = true;
    } else if (undo.active && activeAfter != undo.activeId) {
        pushCommand(undo.before, m_settings);
        undo.active = false;
    } else if (!undo.active && activeAfter == 0 && activeBefore == 0 &&
               !BuildSettingsEqual(m_settings, beforeDraw)) {
        pushCommand(beforeDraw, m_settings);
    }

    // WHY 全変更で作り直さないか: チェックはディスクを読む。製品名を 1 文字打つたびに
    //     build.config を読み直す理由はなく、結果が変わるのは構成とシーン一覧だけ。
    if (m_settings.developmentBuild != beforeDraw.developmentBuild ||
        !ScenesEqual(m_settings.scenes, beforeDraw.scenes))
        m_checksValid = false;
}

// =============================================================================
// Scenes in Build
// =============================================================================

void BuildSettingsPanel::DrawScenesInBuild(EditorContext& ctx)
{
    widgets::SectionHeader("Scenes in Build");

    const int   count      = static_cast<int>(m_settings.scenes.size());
    const float toolbarW   = widgets::ListRowToolbarWidth(true);
    const float listHeight = ImGui::GetTextLineHeightWithSpacing() * 7.0f;

    int  moveFrom = -1, moveTo = -1;
    int  removeIdx = -1;

    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 4.0f);
    ImGui::BeginChild("##SceneList", { 0.0f, listHeight }, true);

    if (count == 0) {
        ImGui::TextDisabled("No scenes listed.");
        ImGui::TextDisabled("Scenes under Assets/ ship anyway; list only what lives elsewhere.");
    }

    const std::string startScene = ResolveStartScene(ctx);

    for (int i = 0; i < count; ++i) {
        SceneEntry& entry = m_settings.scenes[i];
        ImGui::PushID(i);

        ImGui::Checkbox("##enabled", &entry.enabled);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
            ImGui::SetTooltip("Include this scene in the build");

        ImGui::SameLine();
        const bool selected = (m_selectedSceneIdx == i);
        const float labelWidth = ImGui::GetContentRegionAvail().x - toolbarW
                               - ImGui::GetStyle().ItemSpacing.x * 2.0f;
        if (ImGui::Selectable("##row", selected, ImGuiSelectableFlags_AllowOverlap,
                              { labelWidth, 0.0f }))
            m_selectedSceneIdx = i;

        ImGui::SameLine(0.0f, 0.0f);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() - labelWidth);

        const bool missing = !SceneExists(ctx.projectRoot, entry.path);
        // WHY 起動シーンに印を付けるか: このリストの並びは起動順ではない。
        //     どれが実際に起動するのかは ProjectSettings 側にしか書いておらず、
        //     印が無いと Unity の慣習で「先頭が開始シーン」と読み違える。
        const bool isStart = !entry.path.empty() && entry.path == startScene;
        const ImVec4 color = missing        ? kErrorColor
                           : isStart        ? kOkColor
                           : !entry.enabled ? ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]
                                            : ImGui::GetStyle().Colors[ImGuiCol_Text];
        ImGui::PushStyleColor(ImGuiCol_Text, color);
        ImGui::Text("%d  %s%s", i, entry.path.c_str(), isStart ? "   [START]" : "");
        ImGui::PopStyleColor();
        if (missing && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
            ImGui::SetTooltip("File not found under the project root");

        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - toolbarW);
        const widgets::ListRowButtons buttons = widgets::ListRowToolbar(i, count, true, false);
        if (buttons.moveUp)   { moveFrom = i; moveTo = i - 1; }
        if (buttons.moveDown) { moveFrom = i; moveTo = i + 1; }
        if (buttons.remove)   { removeIdx = i; }

        ImGui::PopID();
    }

    ImGui::EndChild();
    ImGui::PopStyleVar();

    if (moveFrom >= 0 && moveTo >= 0 && moveTo < count) {
        std::swap(m_settings.scenes[moveFrom], m_settings.scenes[moveTo]);
        m_selectedSceneIdx = moveTo;
    }
    if (removeIdx >= 0) {
        m_settings.scenes.erase(m_settings.scenes.begin() + removeIdx);
        m_selectedSceneIdx = -1;
    }

    const bool hasCurrentScene = !ctx.currentScenePath.empty() && !ctx.projectRoot.empty();
    ImGui::BeginDisabled(!hasCurrentScene);
    if (ImGui::Button("Add Open Scene"))
        AddScene(ctx, ctx.currentScenePath);
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::BeginDisabled(ctx.projectRoot.empty());
    if (ImGui::Button("Add All Scenes")) {
        const std::filesystem::path scenesDir =
            util::FileSystem::PathFromUtf8(ctx.projectRoot) / L"Assets";
        for (const std::filesystem::path& file : util::FileSystem::ListFilesRecursive(scenesDir)) {
            if (file.extension() == L".scene")
                AddScene(ctx, util::FileSystem::PathToUtf8(file));
        }
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::BeginDisabled(m_settings.scenes.empty());
    if (ImGui::Button("Clear")) {
        m_settings.scenes.clear();
        m_selectedSceneIdx = -1;
    }
    ImGui::EndDisabled();

    ImGui::TextDisabled("Start scene: %s (set in Project Settings)",
                        startScene.empty() ? "not set" : startScene.c_str());
    ImGui::TextDisabled("This list only decides which scenes outside Assets/ get copied.");
}

bool BuildSettingsPanel::AddScene(const EditorContext& ctx, const std::string& absolutePath)
{
    if (absolutePath.empty() || ctx.projectRoot.empty()) return false;

    // WHY 相対で持つか: プロジェクトを別 PC へ持っていっても、そのまま同じ構成でビルドできる。
    const std::filesystem::path scene = util::FileSystem::PathFromUtf8(absolutePath);
    const std::filesystem::path root  = util::FileSystem::PathFromUtf8(ctx.projectRoot);
    const std::filesystem::path rel   = util::FileSystem::RelativePath(scene, root);
    const std::string relPath = rel.empty() ? absolutePath : rel.generic_string();

    const bool exists = std::any_of(
        m_settings.scenes.begin(), m_settings.scenes.end(),
        [&relPath](const SceneEntry& e) { return e.path == relPath; });
    if (exists) return false;

    m_settings.scenes.push_back({ relPath, true });
    return true;
}

// =============================================================================
// Output
// =============================================================================

void BuildSettingsPanel::DrawOutputSettings(EditorContext& ctx)
{
    widgets::SectionHeader("Output");

    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 90.0f);
    widgets::InputString("Output Directory", m_settings.outputDirectory);
    // WHY 確定時だけ取り直すか: 出力先のチェックはディスクを読む。1 文字打つたびに
    //     走らせる理由はないが、入れ替えたまま気付かずビルドを押せてもいけない。
    if (ImGui::IsItemDeactivatedAfterEdit()) m_checksValid = false;
    ImGui::SameLine();
    if (ImGui::Button("Browse...")) {
        std::string chosen;
        if (BrowseForFolder(GetForegroundWindow(), chosen)) {
            m_settings.outputDirectory = chosen;
            m_checksValid = false;
        }
    }

    // WHY 解決後のパスを出すか: 出力先は相対でも絶対でも書ける。
    //     「Builds/MyGame」がどこに出るのかを押す前に確定させる。
    const std::string resolved =
        util::FileSystem::PathToUtf8(m_settings.ResolveOutputPath(ctx.projectRoot));
    ImGui::TextDisabled("→ %s", resolved.c_str());

    ImGui::SetNextItemWidth(200.0f);
    widgets::InputString("Product Name", m_settings.productName);
    ImGui::SameLine();
    ImGui::TextDisabled("→ %s.exe", m_settings.productName.c_str());

    ImGui::SetNextItemWidth(120.0f);
    widgets::InputString("Version", m_settings.version);

    DrawIconSetting(ctx);

    ImGui::Spacing();
    ImGui::Checkbox("Development Build", &m_settings.developmentBuild);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        ImGui::SetTooltip("Builds the Development configuration and writes development = true "
                          "to game.manifest.toml.\nUncheck to build Release.");

    ImGui::SameLine();
    ImGui::Checkbox("Strip editor & source files", &m_settings.stripEditorAssets);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        ImGui::SetTooltip("Excludes script sources (.hpp/.cpp), EditorConfig, docs, "
                          "DCC originals (.blend/.psd) and anything under a folder "
                          "whose name starts with '_'.\n"
                          "Shaders (.hlsl) and .meta always ship: the runtime needs them "
                          "to accept the precompiled .cso and to resolve guid references.");

    ImGui::TextDisabled("Platform: Windows x64   |   Configuration: %s",
                        m_settings.developmentBuild ? "Development" : "Release");
}

void BuildSettingsPanel::DrawIconSetting(EditorContext& ctx)
{
    ImGui::Spacing();

    if (widgets::AssetPathField("Icon", m_settings.iconPath,
                                ".png,.jpg,.jpeg,.tga,.bmp,.ico", ctx.projectRoot))
        m_checksValid = false;
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        ImGui::SetTooltip("Burned into %s.exe as its Windows icon (Explorer, taskbar, "
                          "title bar and Alt+Tab).\n"
                          "Give it a square 256x256 or larger image; the 16 / 32 / 48 / 64 / "
                          "128 / 256 sizes are generated on build.\n"
                          "Leave it empty to ship the default Windows icon.",
                          m_settings.productName.c_str());

    const bool isIco =
        util::StringUtils::ToLower(util::FileSystem::GetExtension(m_settings.iconPath)) == ".ico";

    // WHY 絵を出すか: パス文字列だけでは «どの絵が exe に付くのか» を確かめられない。
    //     .ico はレンダラーが読めないため、そこだけは文字で代える。
    const std::string previewPath = (m_settings.iconPath.empty() || isIco)
        ? std::string{}
        : util::FileSystem::PathToUtf8(m_settings.ResolveIconPath(ctx.projectRoot));
    const std::uint64_t resetVersion = ctx.resources ? ctx.resources->GetResetVersion() : 0;

    // 読み込みはパスが変わったときだけ。デバイスを作り直した後は取り直す。
    if (previewPath != m_iconPreviewPath || resetVersion != m_iconPreviewResetVersion) {
        m_iconPreviewPath         = previewPath;
        m_iconPreviewResetVersion = resetVersion;
        m_iconPreviewTexture      = renderer::ResourceHandle<renderer::TextureTag>::Null();
        if (!previewPath.empty() && ctx.resources)
            m_iconPreviewTexture = ctx.resources->LoadTexture(previewPath);
    }

    // ImTextureID はフレームごとに引き直す。ホットリロードで実体が入れ替わっても
    // 古いディスクリプタを掴んだままにしない。
    void* texId = (m_iconPreviewTexture.IsValid() && ctx.resources && ctx.imguiRenderer)
        ? ctx.imguiRenderer->GetImTextureID(m_iconPreviewTexture, *ctx.resources)
        : nullptr;

    const char* emptyLabel = m_settings.iconPath.empty() ? "No icon"
                           : isIco                       ? ".ico"
                                                         : "Cannot read";
    IconPreviewBox(texId, 96.0f, emptyLabel);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort) && !m_settings.iconPath.empty())
        ImGui::SetTooltip("%s", m_settings.iconPath.c_str());

    // WHY 小さい方も並べるか: アイコンが潰れて読めなくなるのは 16px のときで、
    //     大きいプレビューだけ見ても気付けない。実際に出る大きさで隣に並べる。
    ImGui::SameLine();
    ImGui::BeginGroup();
    constexpr std::array kPreviewSizes{ 48, 32, 16 };
    for (std::size_t i = 0; i < kPreviewSizes.size(); ++i) {
        if (i > 0) ImGui::SameLine();
        ImGui::PushID(kPreviewSizes[i]);
        ImGui::BeginGroup();
        IconPreviewBox(texId, static_cast<float>(kPreviewSizes[i]), nullptr);
        ImGui::TextDisabled("%d", kPreviewSizes[i]);
        ImGui::EndGroup();
        ImGui::PopID();
    }

    ImGui::TextDisabled("%s", m_settings.iconPath.empty()
        ? "The exe ships with the default Windows icon."
        : isIco ? "Every size inside the .ico is embedded as-is."
                : "Sizes 16-256 are generated from this image on build.");
    ImGui::EndGroup();
}

// =============================================================================
// 事前チェック
// =============================================================================

void BuildSettingsPanel::RefreshChecks(EditorContext& ctx)
{
    m_checks.clear();
    m_standaloneExeDir.clear();
    m_checksValid = true;

    if (ctx.projectRoot.empty()) {
        m_checks.push_back({ Check::Level::Error, "Project", "No project is open." });
        return;
    }

    const std::filesystem::path root = util::FileSystem::PathFromUtf8(ctx.projectRoot);

    // --- 出力先 ---
    // WHY Error にするか: コミットは出力先を remove_all する。空欄やデスクトップを
    //     指したままビルドを押せると、そのフォルダが中身ごと消える。
    {
        std::string reason;
        if (m_settings.ValidateOutputPath(ctx.projectRoot, reason)) {
            m_checks.push_back({ Check::Level::Ok, "Output",
                util::FileSystem::PathToUtf8(m_settings.ResolveOutputPath(ctx.projectRoot)) });
        } else {
            m_checks.push_back({ Check::Level::Error, "Output", reason });
        }
    }

    // --- アイコン ---
    // WHY 事前に読むか: 差し替えはコンパイルの後にしか走らない。読めない画像を
    //     指したままだと、数分かけたビルドがアイコンのためだけに失敗する。
    if (!m_settings.iconPath.empty()) {
        const std::filesystem::path icon = m_settings.ResolveIconPath(ctx.projectRoot);
        AppIconWriter::SourceInfo info;
        std::string reason;
        if (!AppIconWriter::Inspect(icon, info, reason)) {
            m_checks.push_back({ Check::Level::Error, "Icon", m_settings.iconPath + " — " + reason });
        } else if (info.isIco) {
            m_checks.push_back({ Check::Level::Ok, "Icon",
                                 m_settings.iconPath + " (embedded as-is)" });
        } else {
            const std::string size = std::to_string(info.width) + "x" + std::to_string(info.height);
            if (info.width != info.height) {
                m_checks.push_back({ Check::Level::Warn, "Icon",
                                     size + " is not square — it will be centered on a "
                                     "transparent square" });
            } else if (info.width < 256) {
                m_checks.push_back({ Check::Level::Warn, "Icon",
                                     size + " is smaller than 256x256 — large icon views "
                                     "will look soft" });
            } else {
                m_checks.push_back({ Check::Level::Ok, "Icon", m_settings.iconPath + " (" + size + ")" });
            }
        }
    }

    // --- ツールチェーン (cmake / build.config) ---
    const ToolchainLocator::Result toolchain =
        ToolchainLocator::Locate(util::FileSystem::PathFromUtf8(ctx.projectBuildRoot));
    if (!toolchain.found) {
        m_checks.push_back({ Check::Level::Error, "Toolchain", toolchain.error });
    } else {
        m_checks.push_back({ Check::Level::Ok, "Toolchain",
                             util::FileSystem::PathToUtf8(toolchain.buildDir) });

        const std::filesystem::path exe = m_settings.developmentBuild
            ? (!toolchain.exeDevelopment.empty() ? toolchain.exeDevelopment : toolchain.exeDebug)
            : toolchain.exeRelease;
        m_standaloneExeDir = util::FileSystem::PathToUtf8(exe.parent_path());
    }

    // --- ランタイム DLL / EngineAssets ---
    // WHY 未ビルドを警告止まりにするか: DLL も EngineAssets も standalone ターゲットの
    //     POST_BUILD が置く。まだ 1 度もビルドしていない状態でエラーにすると、
    //     「DLL が無いからビルドできない / ビルドしないと DLL が来ない」で詰む。
    if (!m_standaloneExeDir.empty()) {
        const std::filesystem::path exeDir = util::FileSystem::PathFromUtf8(m_standaloneExeDir);
        const bool builtOnce = util::FileSystem::Exists(exeDir);

        if (!builtOnce) {
            m_checks.push_back({ Check::Level::Warn, "Runtime DLLs",
                                 "not staged yet — the build produces them next to the exe" });
        } else {
            // WHY DXC を名指しで見るか: DX12 は焼いた .cso を読むだけの経路でもリフレクションに
            //     dxcompiler.dll が要る。無いまま配ると「起動はするが何も描かれない」になる。
            std::vector<std::wstring> required = {
                L"imgui.dll", L"FBZZMath.dll", L"FBZZPhysics.dll", L"FBZZEngine.dll",
                L"assimp-vc145-mt.dll",
            };
            const bool isDx12 =
                ctx.projectSettings.app.rendererBackend == renderer::RendererBackend::DX12;
            if (isDx12) {
                required.push_back(L"dxcompiler.dll");
                required.push_back(L"dxil.dll");
            }

            std::string missing;
            for (const std::wstring& dll : required) {
                if (util::FileSystem::Exists(exeDir / dll)) continue;
                if (!missing.empty()) missing += ", ";
                missing += util::FileSystem::PathToUtf8(std::filesystem::path(dll));
            }

            if (missing.empty()) {
                m_checks.push_back({ Check::Level::Ok, "Runtime DLLs",
                                     isDx12 ? "including DXC (dx12)" : "dx11" });
            } else {
                m_checks.push_back({ Check::Level::Warn, "Runtime DLLs",
                                     "missing next to the exe: " + missing +
                                     " (the build re-stages them; still missing afterwards means "
                                     "the SDK was not published for this configuration)" });
            }

            if (util::FileSystem::Exists(exeDir / L"EngineAssets")) {
                m_checks.push_back({ Check::Level::Ok, "EngineAssets", "will be packaged" });
            } else {
                m_checks.push_back({ Check::Level::Warn, "EngineAssets",
                                     "not staged next to the exe — assets that only exist in the SDK "
                                     "(default font, etc.) will be missing" });
            }
        }
    }

    // --- SDK が選んだ構成で公開されているか ---
    // WHY: SDK は FBZZSDK ターゲットをビルドした構成ぶんしか bin/lib を持たない。
    //      Release を公開していない状態で Release ビルドを選ぶと、リンクではなく
    //      ステージングの copy_directory が落ち、CMake のログだけを見ても理由が読めない。
    if (!ctx.engineRoot.empty()) {
        const std::string configuration = m_settings.developmentBuild ? "Development" : "Release";
        const std::filesystem::path sdkRoot = util::FileSystem::PathFromUtf8(ctx.engineRoot);
        const bool published = util::FileSystem::Exists(sdkRoot / "bin" / configuration)
                            && util::FileSystem::Exists(sdkRoot / "lib" / configuration);
        if (published) {
            m_checks.push_back({ Check::Level::Ok, "SDK", configuration + " is published" });
        } else {
            m_checks.push_back({ Check::Level::Error, "SDK",
                                 "not published for " + configuration +
                                 " — build the FBZZSDK target in that configuration first" });
        }
    }

    // --- Library/Baked (FBX 由来の実体) ---
    if (util::FileSystem::Exists(root / L"Library" / L"Baked")) {
        m_checks.push_back({ Check::Level::Ok, "Library/Baked", "will be packaged" });
    } else {
        m_checks.push_back({ Check::Level::Warn, "Library/Baked",
                             "not found — imported clips and model materials will be "
                             "re-imported from the source FBX at runtime" });
    }

    // --- 開始シーン ---
    const std::string startScene = ResolveStartScene(ctx);
    if (startScene.empty()) {
        m_checks.push_back({ Check::Level::Error, "Start scene",
                             "Project Settings names neither runtime.start_scene "
                             "nor project.default_scene" });
    } else if (!SceneExists(ctx.projectRoot, startScene)) {
        m_checks.push_back({ Check::Level::Error, "Start scene", startScene + " does not exist" });
    } else {
        m_checks.push_back({ Check::Level::Ok, "Start scene", startScene });
    }
}

std::string BuildSettingsPanel::ResolveStartScene(const EditorContext& ctx) const
{
    // WHY リスト先頭ではないか: このエンジンのランタイムは Build Settings の並びを見ない。
    //     起動シーンは runtime.start_scene、空なら project.default_scene で決まる。
    //     Unity の「index 0 が開始シーン」を真似た表示にすると、リストを並べ替えても
    //     起動シーンが変わらない理由が UI からは読めなくなる。
    const GameProjectConfig& game = ctx.projectSettings.game;
    return !game.runtime.startScene.empty() ? game.runtime.startScene
                                            : game.project.defaultScene;
}

void BuildSettingsPanel::DrawPackageChecks(EditorContext& ctx)
{
    widgets::SectionHeader("Package");

    if (ImGui::SmallButton("Refresh")) RefreshChecks(ctx);
    ImGui::SameLine();
    ImGui::TextDisabled("Assets / Library/Baked / EngineAssets / runtime DLLs");

    for (const Check& check : m_checks) {
        const ImVec4 color = check.level == Check::Level::Error ? kErrorColor
                           : check.level == Check::Level::Warn  ? kWarnColor
                                                                : kOkColor;
        const char* glyph = check.level == Check::Level::Error ? "x"
                          : check.level == Check::Level::Warn  ? "!"
                                                               : "o";
        ImGui::TextColored(color, "[%s] %s", glyph, check.label.c_str());
        if (check.detail.empty()) continue;
        ImGui::SameLine();
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("%s", check.detail.c_str());
        ImGui::PopTextWrapPos();
    }
}

// =============================================================================
// 進捗バー + ビルドボタン
// =============================================================================

void BuildSettingsPanel::StartBuild(EditorContext& ctx, bool runAfterBuild)
{
    // WHY ここで永続化するか: ビルドは分単位で走る。途中でエディターが落ちても
    //     「何をビルドしようとしたか」が残っていないと、設定からやり直しになる。
    ctx.requestEditorSettingsSave = true;

    m_lastOutputDir = util::FileSystem::PathToUtf8(m_settings.ResolveOutputPath(ctx.projectRoot));
    m_pipeline.Start(m_settings, ctx.projectRoot, ctx.projectBuildRoot,
                     ctx.standaloneTargetName, ctx.scriptsDllPath, ctx.engineRoot,
                     runAfterBuild);
}

void BuildSettingsPanel::DrawProgressAndActions(EditorContext& ctx)
{
    widgets::SectionHeader("Build");

    const bool isPlaying  = ctx.playMode && ctx.playMode->IsPlaying();
    const bool isBuilding = m_pipeline.GetState() == BuildPipeline::State::Running;
    const bool hasError   = std::any_of(m_checks.begin(), m_checks.end(),
        [](const Check& c) { return c.level == Check::Level::Error; });

    if (isPlaying)
        ImGui::TextColored(kWarnColor, "Cannot build while playing");

#ifndef NDEBUG
    // WHY: Debug / Development エディターでは配布向けでないランタイムが混ざる可能性があるため、
    //      公開用パッケージは Release プリセットで作るべきである。
    ImGui::TextColored(kWarnColor,
        "[DEV/DEBUG EDITOR] For distribution, switch to the Release preset.");
#endif

    ImGui::BeginDisabled(isPlaying || isBuilding || hasError);
    if (ImGui::Button("Build", { 120.0f, 0.0f }))
        StartBuild(ctx, false);
    ImGui::SameLine();
    if (ImGui::Button("Build and Run", { 140.0f, 0.0f }))
        StartBuild(ctx, true);
    ImGui::EndDisabled();

    if (hasError && !isBuilding) {
        ImGui::SameLine();
        ImGui::TextColored(kErrorColor, "Fix the errors above first");
    }

    if (!m_lastOutputDir.empty()) {
        ImGui::SameLine();
        ImGui::BeginDisabled(!util::FileSystem::Exists(m_lastOutputDir));
        if (ImGui::Button("Open Output Folder"))
            RevealInExplorer(m_lastOutputDir);
        ImGui::EndDisabled();
    }

    if (m_pipeline.GetState() == BuildPipeline::State::Running)
        m_pipeline.Tick();

    const bool isStillBuilding = m_pipeline.GetState() == BuildPipeline::State::Running;

    if (isStillBuilding) {
        ImGui::ProgressBar(m_pipeline.GetProgress(), { -1.0f, 0.0f }, m_pipeline.GetStatus());
        if (ImGui::Button("Cancel"))
            m_pipeline.Cancel();
    }

    if (isStillBuilding && !m_buildLogWasRunning) ++m_buildLogGeneration;
    m_buildLogWasRunning = isStillBuilding;

    const std::string& buildLog = m_pipeline.GetBuildLog();
    if (!buildLog.empty() &&
        (isStillBuilding || m_pipeline.GetState() == BuildPipeline::State::Failed)) {
        // WHY: CMake / MSBuild の失敗理由は標準出力に出るため、失敗後もログを残して原因を読めるようにする。
        m_buildLogFeed.Sync(buildLog, m_buildLogGeneration, 0, m_buildLogView);
        m_buildLogView.DrawToolbar();
        m_buildLogView.DrawList({ 0.0f, ImGui::GetTextLineHeightWithSpacing() * 14.0f });
    }

    // Build and Run: ビルド完了後に exe を起動する
    if (m_pipeline.GetState() == BuildPipeline::State::Done && m_pipeline.WantsRunAfter()) {
        StandaloneLauncher::LaunchExe(m_pipeline.GetOutputExePath(), "");
        // 重複起動を防ぐためリセットする
        m_pipeline.Reset();
        m_checksValid = false;
    }

    if (m_pipeline.GetState() == BuildPipeline::State::Failed) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(kErrorColor, "Error: %s", m_pipeline.GetError());
        ImGui::PopTextWrapPos();
        if (ImGui::Button("Reset")) {
            m_pipeline.Reset();
            m_checksValid = false;
        }
    }

    if (m_pipeline.GetState() == BuildPipeline::State::Done && !m_pipeline.WantsRunAfter()) {
        ImGui::TextColored(kOkColor, "Build complete (%s)",
                           m_pipeline.GetRuntimeConfiguration().c_str());
        ImGui::SameLine();
        if (ImGui::Button("Run"))
            StandaloneLauncher::LaunchExe(m_pipeline.GetOutputExePath(), "");
        ImGui::SameLine();
        if (ImGui::Button("Reset##done")) {
            m_pipeline.Reset();
            m_checksValid = false;
        }
    }
}

} // namespace fbzz::editor
