/// @file    FluidEditorPanel.cpp
/// @brief   Fluid Editor パネルの枠 (文書の開閉・ツールバー・焼き・Properties・3 列のレイアウト)
/// @author  Hasegawa Jin
/// @date    2026-09-12
#include <Editor/Panels/FluidEditorPanel.hpp>

#include "FluidEditor/FluidEditorExtras.hpp"
#include "FluidEditor/FluidEditorInternal.hpp"

#include <Editor/EditorContext.hpp>
#include <Editor/Op/EditorOperator.hpp>
#include <Editor/Util/AssetDirtyRegistry.hpp>
#include <Editor/Util/FluidBakeService.hpp>
#include <Editor/Util/FluidAssetWriters.hpp>
#include <Editor/Util/EditorIcons.hpp>
#include <Editor/Util/FluidRecipeWidgets.hpp>
#include <Editor/Util/ImGuiWidgets.hpp>
#include <Editor/Util/EditorSettings.hpp>
#include <Editor/Util/ModalDialog.hpp>
#include <Editor/Util/Toast.hpp>
#include <Engine/Asset/FluidRecipeCodec.hpp>
#include <Engine/Asset/FluidBakeBudget.hpp>
#include <Engine/Asset/FluidVolumeBake.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Engine/Util/StringUtils.hpp>
#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <string>
#include <utility>

namespace fbzz::editor {
namespace {

using fluideditor::ClampF;
using fluideditor::SetStatus;

constexpr float kSplitterThickness = 5.0f;
constexpr ImU32 kErrorColor = IM_COL32(255, 96, 80, 255);
constexpr ImU32 kOkColor = IM_COL32(150, 210, 150, 255);
constexpr ImU32 kWarningColor = IM_COL32(255, 200, 80, 255);
constexpr const char* kQualityNames[] = { "Draft", "Normal", "Final" };
constexpr const char* kQualityTips[] = { "Draft — 2D: 格子 48 / 96px、3D: 格子 48 / タイル 128px。触っている間の既定",
                                         "Normal — 2D: 格子 96 / 128px、3D: 格子 96 / タイル 256px",
                                         "Final — 焼きと同じ格子・コマ解像度 (重い)" };

void SameLineIfFits(float width)
{
    const float right = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
    if (ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + width <= right)
        ImGui::SameLine();
}

void NextToolbarItem(const char* label)
{
    SameLineIfFits(ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2.0f);
}

std::string DisplayPath(const EditorContext& ctx, const std::string& path)
{
    std::string shown = util::FileSystem::NormalizePathSeparators(path);
    const std::string root = util::FileSystem::NormalizePathSeparators(ctx.projectRoot);
    if (!root.empty() && shown.size() > root.size() + 1 && shown.compare(0, root.size(), root) == 0
        && (shown[root.size()] == '/' || shown[root.size()] == '\\'))
        shown = shown.substr(root.size() + 1);
    return shown;
}

const char* JobStateLabel(FluidJobState state)
{
    switch (state) {
    case FluidJobState::Queued:    return "Queued";
    case FluidJobState::Running:   return "Baking...";
    case FluidJobState::Encoding:  return "Encoding...";
    case FluidJobState::Done:      return "Done";
    case FluidJobState::Failed:    return "Failed";
    case FluidJobState::Cancelled: return "Cancelled";
    }
    return "";
}

/// @brief 列の境目をつかんで幅を変える。sign はマウスを右 (下) へ動かしたときに value が増えるなら +1。
void Splitter(const char* id, float& value, float sign, ImVec2 size, bool vertical)
{
    ImGui::InvisibleButton(id, size);
    const bool hot = ImGui::IsItemHovered() || ImGui::IsItemActive();
    if (hot) ImGui::SetMouseCursor(vertical ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_ResizeNS);
    if (ImGui::IsItemActive()) {
        const ImVec2 delta = ImGui::GetIO().MouseDelta;
        value += sign * (vertical ? delta.x : delta.y);
    }
    if (hot) {
        ImGui::GetWindowDrawList()->AddRectFilled(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                                                  ImGui::GetColorU32(ImGuiCol_SeparatorHovered));
    }
}

void CenteredText(const char* text, bool disabled)
{
    const float width = ImGui::CalcTextSize(text).x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (std::max)(0.0f, (ImGui::GetContentRegionAvail().x - width) * 0.5f));
    if (disabled)
        ImGui::TextDisabled("%s", text);
    else
        ImGui::TextUnformatted(text);
}

std::string TrimFileName(std::string name)
{
    const auto notSpace = [](unsigned char c) { return std::isspace(c) == 0; };
    name.erase(name.begin(), std::find_if(name.begin(), name.end(), notSpace));
    name.erase(std::find_if(name.rbegin(), name.rend(), notSpace).base(), name.end());
    constexpr const char* kExtension = ".fluid";
    constexpr std::size_t kExtensionLength = 6;
    if (name.size() > kExtensionLength) {
        std::string tail = name.substr(name.size() - kExtensionLength);
        std::transform(tail.begin(), tail.end(), tail.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (tail == kExtension) name.resize(name.size() - kExtensionLength);
    }
    return name;
}

/// @brief 新しい .fluid を置くフォルダ。Asset Browser の «今いるフォルダ» は外から見えない。
/// @note 選んでいるアセット (フォルダならそれ、ファイルならその親) を使い、Assets の外なら既定の置き場へ戻す。
std::string DefaultNewFolder(const EditorContext& ctx)
{
    const std::string assetsRoot = ctx.projectRoot.empty()
        ? std::string("Assets")
        : util::FileSystem::NormalizePathSeparators(ctx.projectRoot) + "/Assets";
    if (!ctx.selectedAssetPath.empty()) {
        std::string folder = util::FileSystem::IsDirectory(ctx.selectedAssetPath)
            ? ctx.selectedAssetPath
            : util::FileSystem::GetDirectory(ctx.selectedAssetPath);
        folder = util::FileSystem::NormalizePathSeparators(folder, true);
        if (!folder.empty()
            && (util::FileSystem::SamePathText(folder, assetsRoot)
                || util::FileSystem::IsChildPathText(folder, assetsRoot)))
            return folder;
    }
    return assetsRoot + "/VFX/Fluid";
}

} // namespace

FluidEditorPanel::FluidEditorPanel()
    : m_state(std::make_unique<fluideditor::State>())
{
}

FluidEditorPanel::~FluidEditorPanel() = default;

void FluidEditorPanel::OnInit(EditorContext& ctx)
{
    m_resources = ctx.resources;
    m_context = &ctx;
}

void FluidEditorPanel::OnShutdown()
{
    /// @note 保存関数は this を掴んでいる。パネルが消えた後にレジストリから呼ばれないよう外しておく。
    if (!m_registeredDirtyPath.empty()) {
        AssetDirtyRegistry::MarkClean(m_registeredDirtyPath);
        m_registeredDirtyPath.clear();
    }
    m_state->preview.Shutdown(m_resources);
    if (m_context != nullptr) fluideditor::ShutdownFluidBakedTab(*m_context);
}

void FluidEditorPanel::OnLoadSettings(const EditorSettings& settings)
{
    /// @note 自動保存の入り切りは Extras 側が持っている。設定との往復はここで通す。
    if (m_context != nullptr) fluideditor::SetFluidAutoSaveEnabled(*m_context, settings.fluidEditorAutoSave);
    m_followSelection = settings.fluidEditorFollowSelection;
    m_recentFluids = settings.recentFluids;
}

void FluidEditorPanel::OnSaveSettings(EditorSettings& settings) const
{
    if (m_context != nullptr) settings.fluidEditorAutoSave = fluideditor::FluidAutoSaveEnabled(*m_context);
    settings.fluidEditorFollowSelection = m_followSelection;
    settings.recentFluids = m_recentFluids;
}

/// @name 文書の開閉・保存

void FluidEditorPanel::RequestOpen(EditorContext& ctx, const std::string& absPath)
{
    if (absPath.empty()) return;
    fluideditor::State& state = *m_state;
    FluidDocument& document = state.document;
    if (document.IsOpen() && util::FileSystem::SamePathText(document.Path(), absPath)) return;

    if (document.IsOpen() && document.IsDirty()) {
        /// @note コールバックは ModalDialog::OnRender の中で走る。そこから別のモーダルを開くと
/// @note 実行中の std::function ごと状態が差し替わるので、ここでは開き直すだけにする。
        EditorContext* context = &ctx;
        const std::string target = absPath;
        ModalDialog::OpenUnsavedChanges(
            "Fluid Editor - Unsaved Changes",
            "\"" + util::FileSystem::GetFilename(document.Path()) + "\" に未保存の変更があります。\n"
                + util::FileSystem::GetFilename(target) + " を開く前に保存しますか?",
            [this, context, target]() {
                if (!SaveDocument(*context)) return false;
                OpenNow(*context, target);
                return true;
            },
            [this, context, target]() { OpenNow(*context, target); });
        return;
    }
    OpenNow(ctx, absPath);
}

void FluidEditorPanel::OpenNow(EditorContext& ctx, const std::string& absPath)
{
    fluideditor::State& state = *m_state;
    FluidDocument& document = state.document;
    if (document.InInteractiveEdit()) document.EndInteractiveEdit(ctx, "Move Fluid Part");
    state.viewDrag = {};
    state.timelineDrag = {};
    state.renameTarget = FluidSelection{};

    std::string error;
    if (!document.Open(absPath, error)) {
        SetStatus(state, "開けません: " + DisplayPath(ctx, absPath) + " (" + error + ")", true);
        Toast::Error("Fluid Editor: 開けません " + util::FileSystem::GetFilename(absPath));
        return;
    }
    document.selection = FluidSelection{ FluidSelectionKind::Simulation, -1 };
    state.playhead = 0.0f;
    state.playing = false;
    state.hasSent = false;
    state.selectedKey = -1;
    state.gizmoActive = false;
    /// @note 表示は «そのレシピが焼かれる形» から始める (次の描画で recipe.bake.mode を見る)。
    state.viewModeChosen = false;
    state.volumeRecipeKey = 0;
    RememberRecent(ctx, absPath);
    SetStatus(state, "開きました: " + DisplayPath(ctx, absPath), false);
}

void FluidEditorPanel::RememberRecent(EditorContext& ctx, const std::string& absPath)
{
    if (absPath.empty()) return;
    /// @note 表記の違い (区切り文字) で «同じファイルが 2 本» にならないよう、入れる形を揃えてから持つ。
    const std::string normalized = util::FileSystem::NormalizePathSeparators(absPath);
    if (!m_recentFluids.empty() && util::FileSystem::SamePathText(m_recentFluids.front(), normalized)) return;
    ForgetRecent(normalized);
    m_recentFluids.insert(m_recentFluids.begin(), normalized);
    if (m_recentFluids.size() > static_cast<std::size_t>(EditorSettings::kMaxRecentFluids))
        m_recentFluids.resize(static_cast<std::size_t>(EditorSettings::kMaxRecentFluids));
    /// @note 次の起動へ持ち越す値なので、終了を待たずに書かせる (落ちても «さっき開いていた» が残る)。
    ctx.requestEditorSettingsSave = true;
}

void FluidEditorPanel::ForgetRecent(const std::string& absPath)
{
    const auto same = [&absPath](const std::string& entry) {
        return util::FileSystem::SamePathText(entry, absPath);
    };
    m_recentFluids.erase(std::remove_if(m_recentFluids.begin(), m_recentFluids.end(), same), m_recentFluids.end());
}

void FluidEditorPanel::FollowAssetSelection(EditorContext& ctx)
{
    m_followBlockedPath.clear();
    if (!m_followSelection || !visible) return;
    /// @note 最小化中に対象が入れ替わると、戻ったときに何が起きたか分からない。
    if (!WasContentRendered()) return;

    const std::string& picked = ctx.selectedAssetPath;
    if (picked.empty() || util::StringUtils::ToLower(util::FileSystem::GetExtension(picked)) != ".fluid")
        return;
    FluidDocument& document = m_state->document;
    if (document.IsOpen() && util::FileSystem::SamePathText(document.Path(), picked)) return;

    /// @note dirty なら切り替えない: 素朴に追従すると別ファイルのクリックだけで未保存の変更が
/// @note 確認なしに消える事故になる (EditorContext.hpp の openAnimationGraph の注記と同種)。
/// @note 掴んでいる最中も切らない (ギズモのドラッグが宛先を失う)。
    if (document.IsOpen() && (document.IsDirty() || document.InInteractiveEdit())) {
        m_followBlockedPath = picked;
        return;
    }
    OpenNow(ctx, picked);
}

bool FluidEditorPanel::SaveDocument(EditorContext& ctx)
{
    fluideditor::State& state = *m_state;
    FluidDocument& document = state.document;
    if (!document.IsOpen()) return false;
    /// @note テンプレートを書き換えるのは «たまたま開いていた» ではなく人が選んだ結果であってほしい。
/// @note 選び直す口は名前を付けて保存のモーダル (上書きもそこにある)。
    if (fluideditor::IsFluidTemplatePath(ctx, document.Path())) {
        BeginSaveAs(ctx);
        SetStatus(state, "テンプレートです。名前を付けて保存してください", false);
        /// @note ステータス行はパネルが描かれないと読めない。終了時の一括保存やダイアログ経由でも
/// @note «保存しなかった» ことだけは必ず届くようにする。
        Toast::Warning("テンプレートは上書きしません: " + util::FileSystem::GetFilename(document.Path())
                       + " (名前を付けて保存してください)");
        return false;
    }
    return WriteDocumentToDisk(ctx);
}

bool FluidEditorPanel::WriteDocumentToDisk(EditorContext& ctx)
{
    fluideditor::State& state = *m_state;
    FluidDocument& document = state.document;
    if (!document.IsOpen()) return false;
    std::string error;
    if (!document.Save(ctx, error)) {
        SetStatus(state, "保存できません: " + error, true);
        Toast::Error("Fluid Editor: " + error);
        return false;
    }
    AssetDirtyRegistry::MarkClean(document.Path());
    m_registeredDirtyPath.clear();
    SetStatus(state, "保存しました: " + DisplayPath(ctx, document.Path()), false);
    Toast::Success("Saved " + util::FileSystem::GetFilename(document.Path()));
    return true;
}

void FluidEditorPanel::BeginSaveAs(EditorContext& ctx)
{
    const std::string assetsRoot = ctx.projectRoot.empty()
        ? std::string("Assets")
        : util::FileSystem::NormalizePathSeparators(ctx.projectRoot) + "/Assets";
    std::string folder = DefaultNewFolder(ctx);
    /// @note 既定がテンプレート置き場のままだと、そのまま確定したときに原本がもう 1 本増えるだけになる。
    if (folder.empty() || fluideditor::IsFluidTemplatePath(ctx, folder)) folder = assetsRoot + "/VFX/Fluid";
    fluideditor::OpenFluidSaveAsModal(m_state->document.Path(), folder);
}

void FluidEditorPanel::FinishSaveAs(EditorContext& ctx, const fluideditor::FluidSaveAsResult& result)
{
    fluideditor::State& state = *m_state;
    if (result.action == fluideditor::FluidSaveAsAction::Overwrite) {
        if (WriteDocumentToDisk(ctx))
            SetStatus(state, "テンプレートを上書きしました: " + DisplayPath(ctx, state.document.Path()), false);
        return;
    }
    if (result.path.empty()) return;
    if (!m_registeredDirtyPath.empty()) {
        AssetDirtyRegistry::MarkClean(m_registeredDirtyPath);
        m_registeredDirtyPath.clear();
    }
    ctx.requestAssetBrowserRefresh = true;
    /// @note 書いた先へ持ち替える。ここから先はただの .fluid なので、Save も自動保存もそのまま通る。
    OpenNow(ctx, result.path);
    SetStatus(state, "名前を付けて保存しました: " + DisplayPath(ctx, result.path), false);
    Toast::Success("Saved " + util::FileSystem::GetFilename(result.path));
}

void FluidEditorPanel::SyncDirtyRegistry(EditorContext& ctx)
{
    const FluidDocument& document = m_state->document;
    const bool dirty = document.IsOpen() && document.IsDirty();
    const std::string current = dirty ? document.Path() : std::string{};
    if (!m_registeredDirtyPath.empty() && m_registeredDirtyPath != current) {
        AssetDirtyRegistry::MarkClean(m_registeredDirtyPath);
        m_registeredDirtyPath.clear();
    }
    if (dirty && m_registeredDirtyPath.empty()) {
        /// @note 終了時の一括保存ダイアログはこれを見る。載せないと Fluid Editor の編集だけ黙って捨てられる。
        EditorContext* context = &ctx;
        AssetDirtyRegistry::Register(current, DisplayPath(ctx, current), "FLUID",
                                     [this, context]() { return SaveDocument(*context); });
        m_registeredDirtyPath = current;
    }
}

void FluidEditorPanel::BeginNewFromPreset(EditorContext& ctx)
{
    const std::string folder = DefaultNewFolder(ctx);
    const auto preset = static_cast<asset::FluidPreset>(m_newPresetIndex);
    EditorContext* context = &ctx;
    ModalDialog::OpenInput(
        "New Fluid", "New Fluid",
        [this, context](const std::string& name) { CreateFromPreset(*context, name); },
        "Preset: " + std::string(asset::FluidPresetName(preset)) + "   Folder: " + DisplayPath(ctx, folder));
}

void FluidEditorPanel::CreateFromPreset(EditorContext& ctx, const std::string& rawName)
{
    fluideditor::State& state = *m_state;
    /// @note ModalDialog の中から呼ばれるので、ここで確認のモーダルは出せない (呼び手が未保存でないことを保証する)。
    if (state.document.IsOpen() && state.document.IsDirty()) {
        SetStatus(state, "未保存の変更があります。保存してから新しく作ってください", true);
        return;
    }
    const std::string name = TrimFileName(rawName);
    if (name.empty() || name.find_first_of("\\/:*?\"<>|") != std::string::npos) {
        SetStatus(state, "ファイル名に使えない文字があります: " + rawName, true);
        return;
    }
    const std::string folder = DefaultNewFolder(ctx);
    if (!util::FileSystem::EnsureDirectory(folder)) {
        SetStatus(state, "フォルダを作れません: " + DisplayPath(ctx, folder), true);
        return;
    }
    std::string path = folder + "/" + name + ".fluid";
    int suffix = 1;
    while (util::FileSystem::Exists(path)) path = folder + "/" + name + " " + std::to_string(suffix++) + ".fluid";

    const auto preset = static_cast<asset::FluidPreset>(m_newPresetIndex);
    if (!asset::SaveFluidRecipe(path, asset::MakeFluidPreset(preset))) {
        SetStatus(state, "書き出せません: " + DisplayPath(ctx, path), true);
        return;
    }
    ctx.requestAssetBrowserRefresh = true;
    OpenNow(ctx, path);
}

/// @name 焼き

void FluidEditorPanel::StartBake(EditorContext& ctx, bool makeVfx, bool draft)
{
    fluideditor::State& state = *m_state;
    FluidDocument& document = state.document;
    if (!document.IsOpen() || ctx.fluidBake == nullptr) return;
    /// @note 焼きはディスクの .fluid を読む。手元の変更を焼くには先に書く。
    if (!draft && document.IsDirty() && !SaveDocument(ctx)) return;

    FluidBakeRequest request;
    request.fluidPath = document.Path();
    request.draft = draft;
    if (draft) request.recipeOverride = std::make_shared<fluid::FluidRecipe>(document.Recipe());
    if (makeVfx) {
        const std::filesystem::path fluidFile = util::FileSystem::PathFromUtf8(document.Path());
        std::filesystem::path vfxFile = fluidFile;
        vfxFile.replace_extension(".vfx");
        request.vfxPath = util::FileSystem::PathToUtf8(vfxFile);
        request.vfxRootName = util::FileSystem::PathToUtf8(fluidFile.stem());
    }

    FluidJobError error;
    const std::uint32_t id = ctx.fluidBake->EnqueueBake(ctx, request, error);
    if (id == 0) {
        SetStatus(state, "焼きを始められません: " + error.message + " (" + error.code + ")", true);
        return;
    }
    m_bakeJob = id;
    m_bakeMakesVfx = makeVfx;
    m_bakeVfxPath = request.vfxPath;
    SetStatus(state, draft ? "仮 Bake 中 (Library に保存します)..."
                           : makeVfx ? "焼いています (終わったら .vfx を開きます)..." : "焼いています...", false);
}

void FluidEditorPanel::PollBakeJob(EditorContext& ctx)
{
    if (m_bakeJob == 0 || ctx.fluidBake == nullptr) return;
    const FluidJobStatus* job = ctx.fluidBake->Find(m_bakeJob);
    if (job == nullptr) {
        m_bakeJob = 0;
        return;
    }
    if (!job->Finished()) return;

    const FluidJobState finished = job->state;
    const std::string message = job->message;
    const std::string materialPath = job->materialPath;
    const std::string draftFluidPath = job->draftFluidPath;
    const std::string vfxPath = job->vfxPath.empty() ? m_bakeVfxPath : job->vfxPath;
    m_bakeJob = 0;

    fluideditor::State& state = *m_state;
    switch (finished) {
    case FluidJobState::Done: {
        if (draftFluidPath.empty()) ctx.requestAssetBrowserRefresh = true;
        /// @note 焼いた結果 (Flipbook / MV) を Baked タブへ渡し、そのタブを前へ出す。
        fluideditor::SetFluidBakedResult(ctx, draftFluidPath.empty() ? state.document.Path() : draftFluidPath,
                                         state.document.Path());
        m_selectBakedTab = true;
        if (m_bakeMakesVfx && !vfxPath.empty()) {
            /// @note Volume Flipbook Baker の «Preview as VFX» と同じ道 (asset.open → Prefab 編集モード)。
            OpArgs args;
            args.Set("path", vfxPath);
            const OpResult result = InvokeOperator(ctx, "asset.open", args);
            if (result.ok)
                SetStatus(state, "焼いて .vfx を作りました: " + DisplayPath(ctx, vfxPath), false);
            else
                SetStatus(state, "焼けましたが .vfx を開けません: " + result.message, true);
        } else {
            SetStatus(state, !draftFluidPath.empty() ? std::string("仮 Bake が終わりました (Library 内の結果を表示中)")
                                                   : materialPath.empty() ? std::string("焼きました")
                                                   : "焼きました → " + DisplayPath(ctx, materialPath),
                      false);
        }
        Toast::Success("Fluid baked: " + util::FileSystem::GetFilename(state.document.Path()));
        break;
    }
    case FluidJobState::Failed:
        SetStatus(state, "焼けませんでした: " + message, true);
        Toast::Error("Fluid bake failed: " + message);
        break;
    case FluidJobState::Cancelled:
        SetStatus(state, "焼きを止めました", false);
        break;
    case FluidJobState::Queued:
    case FluidJobState::Running:
    case FluidJobState::Encoding:
        break;
    }
}

/// @name プレビューと再生

void FluidEditorPanel::SyncPreview()
{
    fluideditor::State& state = *m_state;
    const FluidDocument& document = state.document;
    if (!document.IsOpen()) return;
    if (state.hasSent && document.Revision() == state.sentDocRevision
        && state.visibilityGeneration == state.sentVisibilityGeneration)
        return;

    fluid::FluidRecipe next = document.PreviewRecipe();
    const float from = state.hasSent ? FluidInvalidationTime(state.sentRecipe, next) : 0.0f;
    /// @note hide / solo は文書の Revision を進めないので、プレビューへ渡す鍵は自前の通番にする。
    state.preview.SetRecipe(next, ++state.previewSerial, from);
    state.sentRecipe = std::move(next);
    state.sentDocRevision = document.Revision();
    state.sentVisibilityGeneration = state.visibilityGeneration;
    state.hasSent = true;
}

void FluidEditorPanel::AdvancePlayback()
{
    fluideditor::State& state = *m_state;
    if (!state.playing) return;
    const float duration = fluideditor::TimelineDuration(state.document.Recipe());
    float next = state.playhead + ImGui::GetIO().DeltaTime;
    if (next >= duration) {
        if (state.loop) {
            next = std::fmod(next, duration);
        } else {
            next = duration;
            state.playing = false;
        }
    }
    /// @note 解けていない先へ進むと同じコマで止まって見える。解けた所で待ち、追いついたら進む。
    const float solved = state.preview.SolvedUntil();
    if (state.preview.IsSolving() && next > solved) next = (std::min)(next, (std::max)(solved, state.playhead));
    state.playhead = next;
}

void FluidEditorPanel::HandleShortcuts(EditorContext& ctx)
{
    fluideditor::State& state = *m_state;
    if (!state.document.IsOpen()) return;
    if (ImGui::GetIO().WantTextInput || ImGui::IsAnyItemActive()) return;

    /// @note Ctrl+Z / Ctrl+Y は Scope::Global の edit.undo / edit.redo がそのまま効く (文書は同じ
/// @note UndoStack へ積むため、ここで拾うと 2 回戻る。ツールバーの Undo / Redo ボタンも不要。
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_S)) (void)SaveDocument(ctx);
    if (ImGui::Shortcut(ImGuiKey_Space)) state.playing = !state.playing;
    if (ImGui::Shortcut(ImGuiKey_LeftArrow, ImGuiInputFlags_Repeat)) fluideditor::StepFrame(state, -1);
    if (ImGui::Shortcut(ImGuiKey_RightArrow, ImGuiInputFlags_Repeat)) fluideditor::StepFrame(state, 1);
    if (ImGui::Shortcut(ImGuiKey_Home)) {
        state.playhead = 0.0f;
        state.playing = false;
    }
    if (ImGui::Shortcut(ImGuiKey_End)) {
        state.playhead = fluideditor::TimelineDuration(state.document.Recipe());
        state.playing = false;
    }
    if (ImGui::Shortcut(ImGuiKey_Delete)) (void)fluideditor::RemoveSelectedPart(ctx, state);
    if (ImGui::Shortcut(ImGuiKey_K)) (void)fluideditor::InsertMotionKeyAtPlayhead(ctx, state);
    if (ImGui::Shortcut(ImGuiKey_F2) && state.document.selection.IsPart())
        fluideditor::BeginRename(state, state.document.selection.kind, state.document.selection.index);
}

/// @name 描画

void FluidEditorPanel::DrawRecentEntries(EditorContext& ctx, std::string& picked)
{
    fluideditor::State& state = *m_state;
    const FluidDocument& document = state.document;

    bool pruneMissing = false;
    bool clearAll = false;

    if (m_recentFluids.empty()) ImGui::TextDisabled("(まだありません)");
    for (std::size_t i = 0; i < m_recentFluids.size(); ++i) {
        const std::string& path = m_recentFluids[i];
        /// @note 実体を見るのはメニューを開いている間だけ (閉じていれば 1 回も叩かない)。
        const bool exists = util::FileSystem::Exists(path);
        const bool current = document.IsOpen() && util::FileSystem::SamePathText(document.Path(), path);
        std::string label = DisplayPath(ctx, path);
        if (!exists) label += "   (見つかりません)";
        ImGui::PushID(static_cast<int>(i));
        if (!exists) ImGui::PushStyleColor(ImGuiCol_Text, kWarningColor);
        if (ImGui::MenuItem(label.c_str(), nullptr, current)) picked = path;
        if (!exists) ImGui::PopStyleColor();
        const std::string tip = exists ? path : "移動か削除されています: " + path;
        ImGui::SetItemTooltip("%s", tip.c_str());
        ImGui::PopID();
    }
    if (!m_recentFluids.empty()) {
        ImGui::Separator();
        if (ImGui::MenuItem("消えたものを外す##fe_recent_prune")) pruneMissing = true;
        if (ImGui::MenuItem("履歴を空にする##fe_recent_clear")) clearAll = true;
    }

    if (pruneMissing) {
        const std::size_t before = m_recentFluids.size();
        const auto missing = [](const std::string& entry) { return !util::FileSystem::Exists(entry); };
        m_recentFluids.erase(std::remove_if(m_recentFluids.begin(), m_recentFluids.end(), missing),
                             m_recentFluids.end());
        SetStatus(state, "履歴から " + std::to_string(before - m_recentFluids.size()) + " 件外しました", false);
        ctx.requestEditorSettingsSave = true;
    }
    if (clearAll) {
        m_recentFluids.clear();
        SetStatus(state, "履歴を空にしました", false);
        ctx.requestEditorSettingsSave = true;
    }
}

void FluidEditorPanel::OpenRecent(EditorContext& ctx, const std::string& picked)
{
    if (picked.empty()) return;
    if (!util::FileSystem::Exists(picked)) {
        /// @note 押しても何も起きないのが一番困る。理由を出したうえで、次からは並べない。
        ForgetRecent(picked);
        ctx.requestEditorSettingsSave = true;
        SetStatus(*m_state, "開けません: " + DisplayPath(ctx, picked) + " (もうありません。履歴から外しました)",
                  true);
        Toast::Error("Fluid Editor: 見つかりません " + util::FileSystem::GetFilename(picked));
        return;
    }
    RequestOpen(ctx, picked);
}

void FluidEditorPanel::DrawOpenMenu(EditorContext& ctx)
{
    fluideditor::State& state = *m_state;
    FluidDocument& document = state.document;
    const bool open = document.IsOpen();

    if (ImGui::Button("Open \xe2\x96\xbc##fe_open")) ImGui::OpenPopup("##fe_open_popup");
    ImGui::SetItemTooltip("最近開いた .fluid (新しい順に %d 件まで)・テンプレートから新しく作る・"
                          "レシピをプリセットで置き換える",
                          EditorSettings::kMaxRecentFluids);

    /// @note 押した結果はポップアップを閉じてから効かせる。開く経路 (RequestOpen) は確認モーダルを開くことがあり、
/// @note ポップアップを描いている最中にもう 1 枚開くと積み順が崩れる。
    std::string picked;
    bool openGallery = false;
    int appliedPreset = -1;

    if (ImGui::BeginPopup("##fe_open_popup")) {
        ImGui::SeparatorText("Recent");
        DrawRecentEntries(ctx, picked);

        ImGui::SeparatorText("New");
        if (ImGui::MenuItem("New...##fe_new")) openGallery = true;
        ImGui::SetItemTooltip("テンプレートの一覧から新しい .fluid を作って開く");

        /// @note BeginDisabled でなく enabled 引数を使う: 子メニューの窓を挟む間に無効スタックを跨がせない。
        const bool presetMenu = ImGui::BeginMenu("Preset...##fe_preset", open);
        /// @note 畳んでいるときだけ説明を出す: 開いていれば «最後の項目» が子メニューへ移り、一覧そのものが見える。
        if (!presetMenu) ImGui::SetItemTooltip("レシピ全体をプリセットで置き換える (Undo で戻せる)");
        if (presetMenu) {
            for (int i = 0; i < static_cast<int>(asset::FluidPreset::Count); ++i) {
                ImGui::PushID(i);
                if (ImGui::MenuItem(asset::FluidPresetName(static_cast<asset::FluidPreset>(i))))
                    appliedPreset = i;
                ImGui::PopID();
            }
            ImGui::EndMenu();
        }
        ImGui::EndPopup();
    }

    if (openGallery) fluideditor::OpenFluidTemplateGallery();
    if (appliedPreset >= 0) {
        const auto preset = static_cast<asset::FluidPreset>(appliedPreset);
        if (document.Edit(ctx, "Apply Fluid Preset",
                          [preset](fluid::FluidRecipe& recipe) { recipe = asset::MakeFluidPreset(preset); })) {
            document.selection = FluidSelection{ FluidSelectionKind::Simulation, -1 };
            state.playhead = 0.0f;
            ++state.visibilityGeneration;
        }
    }
    OpenRecent(ctx, picked);
}

void FluidEditorPanel::DrawSettingsMenu(EditorContext& ctx)
{
    fluideditor::State& state = *m_state;

    /// @note 記号を押しボタンのラベルにすると ID が «その字» になる。隣のアイコンボタンと衝突しないよう囲う。
    ImGui::PushID("fe_settings");
    if (ImGui::Button(icons::Or(icons::kSettings, "Settings"))) ImGui::OpenPopup("##fe_settings_popup");
    ImGui::SetItemTooltip("自動保存・選択への追従・プレビューの品質");

    if (ImGui::BeginPopup("##fe_settings_popup")) {
        ImGui::SeparatorText("Preview Quality");
        const int quality = static_cast<int>(state.preview.Quality());
        for (int i = 0; i < 3; ++i) {
            ImGui::PushID(i);
            if (ImGui::RadioButton(kQualityNames[i], quality == i))
                state.preview.SetQuality(static_cast<FluidPreviewQuality>(i));
            ImGui::SetItemTooltip("%s", kQualityTips[i]);
            ImGui::PopID();
        }

        ImGui::SeparatorText("Behaviour");
        bool autoSave = fluideditor::FluidAutoSaveEnabled(ctx);
        if (ImGui::Checkbox("Auto Save##fe_autosave", &autoSave)) fluideditor::SetFluidAutoSaveEnabled(ctx, autoSave);
        ImGui::SetItemTooltip("編集の手が止まってしばらく経ったら、開いている .fluid を自動で保存する"
                              " (Assets/Templates の中は除く)");
        ImGui::Checkbox("Follow Selection##fe_follow", &m_followSelection);
        ImGui::SetItemTooltip("Asset Browser で .fluid を選んだら編集対象を移す。"
                              "未保存の変更があるときは移さない (黙って捨てないため)");
        ImGui::EndPopup();
    }
    ImGui::PopID();

    /// @note 品質だけ外に出す: 開かずに切り分けられないと Draft のまま詰めてしまう。
    ImGui::SameLine();
    ImGui::TextDisabled("%s", kQualityNames[static_cast<int>(state.preview.Quality())]);
}

void FluidEditorPanel::DrawEffectTemplates(EditorContext& ctx)
{
    if (ImGui::Button("Effect Templates...")) {
        m_effectDirectory = DefaultNewFolder(ctx) + "/NewEffect";
        m_effectError.clear();
        ImGui::OpenPopup("##fe_effect_templates");
    }
    ImGui::SetNextWindowSize({ 540.0f, 0.0f }, ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("##fe_effect_templates", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        constexpr const char* names[] = { "Landing", "Charge Release", "Magic Eruption" };
        constexpr const char* ids[] = { "landing", "charge_release", "magic_eruption" };
        ImGui::Combo("Effect", &m_effectPreset, names, IM_ARRAYSIZE(names));
        const auto layers = MakeFluidEffectLayers(static_cast<FluidEffectTemplate>(m_effectPreset));
        for (const auto& layer : layers)
            ImGui::Text("%5.2fs  %s  (%s)", layer.startDelay, layer.name.c_str(),
                        layer.recipe.render.shading == fluid::FluidShading::Glow ? "Glow" : "Smoke");
        ImGui::TextWrapped("素材ごとの .fluid と .mat を作成し、2D ベイク後に複数層の .vfx を作ります。");
        ImGui::SetNextItemWidth(390.0f);
        widgets::InputString("New Folder", m_effectDirectory, 512);
        ImGui::TextDisabled("作成済みのフォルダは上書きしません。");
        if (!m_effectError.empty()) ImGui::TextWrapped("%s", m_effectError.c_str());
        const auto* running = ctx.fluidBake != nullptr ? ctx.fluidBake->Find(m_effectJob) : nullptr;
        ImGui::BeginDisabled(ctx.fluidBake == nullptr || (running != nullptr && !running->Finished()));
        if (ImGui::Button("Create & Bake")) {
            OpArgs args;
            args.Set("preset", std::string(ids[m_effectPreset]));
            args.Set("path", m_effectDirectory);
            const auto result = InvokeOperator(ctx, "fluid.effect_template.create", args);
            if (result.ok) {
                if (const auto* id = result.data.Find("jobId")) m_effectJob = static_cast<std::uint32_t>(id->AsInt());
                ImGui::CloseCurrentPopup();
            } else {
                m_effectError = result.message;
            }
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

void FluidEditorPanel::DrawToolbar(EditorContext& ctx)
{
    fluideditor::State& state = *m_state;
    FluidDocument& document = state.document;
    const bool open = document.IsOpen();

    /// @note 1 行目: 何を開いているか + その都度押すもの。
    /// @note ウィンドウのタイトルは保存済みの配置とフォーカス操作の識別子なので、ファイル名はここへ出す。
    if (open) {
        const std::string name =
            util::FileSystem::GetFilename(document.Path()) + (document.IsDirty() ? " *" : "");
        ImGui::TextWrapped("%s", name.c_str());
        ImGui::SetItemTooltip("%s", DisplayPath(ctx, document.Path()).c_str());
    } else {
        ImGui::TextDisabled("(no .fluid open)");
    }
    if (open && fluideditor::IsFluidTemplatePath(ctx, document.Path())) {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, kWarningColor);
        ImGui::TextUnformatted("テンプレートです (自動保存しません)");
        ImGui::PopStyleColor();
        ImGui::SetItemTooltip("Save は «名前を付けて保存» になります。上書きしたいときはそこで選べます");
    }

    ImGui::BeginDisabled(!open);
    if (ImGui::Button("Save##fe_save")) (void)SaveDocument(ctx);
    ImGui::SetItemTooltip("Ctrl+S");
    ImGui::EndDisabled();

    const bool busy = open && ctx.fluidBake != nullptr && ctx.fluidBake->IsBusy(document.Path());
    SameLineIfFits(ImGui::CalcTextSize("Bake Mode").x + ImGui::GetStyle().ItemSpacing.x + 72.0f);
    ImGui::BeginDisabled(!open || busy);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Bake Mode");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(72.0f);
    int bakeMode = open ? static_cast<int>(document.Recipe().bake.mode) : 0;
    if (ImGui::Combo("##fe_bake_mode", &bakeMode, "2D\0" "3D\0")) {
        (void)document.Edit(ctx, "Set Fluid Bake Mode", [bakeMode](fluid::FluidRecipe& recipe) {
            recipe.bake.mode = static_cast<fluid::FluidBakeMode>(bakeMode);
        });
    }
    ImGui::SetItemTooltip("この .fluid の焼き方を選びます。次の Bake 時に .fluid へ保存します");
    ImGui::EndDisabled();

    const bool volumeBake = open && document.Recipe().bake.mode == fluid::FluidBakeMode::Volume3D;
    NextToolbarItem(volumeBake ? "Bake 3D + .mat" : "Bake 2D + .mat");
    ImGui::BeginDisabled(!open || ctx.fluidBake == nullptr || busy);
    if (ImGui::Button(volumeBake ? "Bake 3D + .mat##fe_bake" : "Bake 2D + .mat##fe_bake"))
        StartBake(ctx, false);
    ImGui::SetItemTooltip("フリップブックと隣の .mat を作る / 更新する (未保存なら先に保存)");
    NextToolbarItem(volumeBake ? "Bake 3D & Make VFX" : "Bake 2D & Make VFX");
    if (ImGui::Button(volumeBake ? "Bake 3D & Make VFX##fe_bake_vfx"
                                 : "Bake 2D & Make VFX##fe_bake_vfx")) StartBake(ctx, true);
    ImGui::SetItemTooltip("焼いて .mat を貼った 1 層の .vfx を .fluid の隣に作り、Prefab 編集モードで開く");
    NextToolbarItem(volumeBake ? "Draft Bake 3D" : "Draft Bake 2D");
    if (ImGui::Button(volumeBake ? "Draft Bake 3D##fe_draft" : "Draft Bake 2D##fe_draft"))
        StartBake(ctx, false, true);
    ImGui::SetItemTooltip("低解像度・少ないコマで試す。未保存の編集も使い、結果は Library に隔離する");
    ImGui::EndDisabled();

    if (open) {
        const fluid::FluidRecipe& recipe = document.Recipe();
        const asset::FluidBakeBudget budget = volumeBake
            ? asset::EstimateVolumeBakeBudget(asset::MakeVolumeBakeSettings(recipe, document.Path()))
            : asset::EstimateFluidBakeBudget(recipe);
        constexpr std::uint64_t mib = 1024ull * 1024ull;
        ImGui::TextDisabled("Bake estimate: RAM %llu MiB | GPU %llu MiB | disk %llu MiB | atlas %.1f MP",
            static_cast<unsigned long long>((budget.cpuBytes + mib - 1) / mib),
            static_cast<unsigned long long>((budget.gpuBytes + mib - 1) / mib),
            static_cast<unsigned long long>((budget.diskBytes + mib - 1) / mib),
            static_cast<double>(budget.atlasPixels) / 1000000.0);
        ImGui::SetItemTooltip("Bake 開始時に空き RAM / ディスク容量と Atlas 上限を検査します。GPU は概算です。");
    }

    if (m_bakeJob != 0 && ctx.fluidBake != nullptr) {
        if (const FluidJobStatus* job = ctx.fluidBake->Find(m_bakeJob); job != nullptr && !job->Finished()) {
            SameLineIfFits(140.0f);
            ImGui::ProgressBar(job->progress, { 140.0f, 0.0f }, JobStateLabel(job->state));
            NextToolbarItem("Cancel");
            if (ImGui::SmallButton("Cancel##fe_bake_cancel")) (void)ctx.fluidBake->Cancel(m_bakeJob);
            const char* stage = job->stage == 2 ? "Output" : job->stage == 1 ? "Render" : "Simulation";
            ImGui::TextDisabled("%s | simulation %.1f s | render %.1f s | output %.1f s | elapsed %.1f s",
                                stage, job->simulationSeconds, job->renderSeconds, job->outputSeconds,
                                job->elapsedSeconds);
            if (job->remainingSeconds >= 0.0f)
                ImGui::TextDisabled("Remaining ~%.1f s", job->remainingSeconds);
            else
                ImGui::TextDisabled("Remaining: estimating...");
            if (volumeBake && ImGui::IsItemHovered())
                ImGui::SetTooltip("3D simulation and GPU rendering can overlap; shown times are wall-clock estimates.");
            if (!volumeBake && job->slowestRenderFrame >= 0) {
                const char* phase = job->slowestSimulationFrame < 0 ? "warmup" : "sim";
                ImGui::TextDisabled("Slowest 2D: %s frame %d %.3f s | render frame %d %.3f s",
                    phase, job->slowestSimulationFrame, job->slowestSimulationFrameSeconds,
                    job->slowestRenderFrame, job->slowestRenderFrameSeconds);
            }
            if (volumeBake && job->slowestRenderFrame >= 0)
                ImGui::TextDisabled("Slowest 3D frame %d: %.3f s wall clock",
                    job->slowestRenderFrame, job->slowestRenderFrameSeconds);
        }
    }

    NextToolbarItem("Open...");
    DrawOpenMenu(ctx);
    SameLineIfFits(ImGui::GetFontSize() * 12.0f);
    DrawSettingsMenu(ctx);
    NextToolbarItem("Effect Templates...");
    DrawEffectTemplates(ctx);

    if (ctx.fluidBake != nullptr && m_effectJob != 0) {
        if (const auto* job = ctx.fluidBake->Find(m_effectJob)) {
            if (!job->Finished()) {
                ImGui::ProgressBar(job->progress, { -1.0f, 0.0f }, job->message.c_str());
                if (ImGui::SmallButton("Cancel Template Bake")) (void)ctx.fluidBake->Cancel(m_effectJob);
            } else {
                ImGui::TextWrapped("%s", job->message.c_str());
                if (!job->vfxPath.empty() && ImGui::SmallButton("Open Effect VFX")) {
                    OpArgs args;
                    args.Set("path", job->vfxPath);
                    (void)InvokeOperator(ctx, "asset.open", args);
                }
            }
        }
    }

    DrawStatusRow(ctx);
}

void FluidEditorPanel::DrawStatusRow(EditorContext& ctx)
{
    fluideditor::State& state = *m_state;
    const FluidDocument& document = state.document;
    const bool open = document.IsOpen();

    bool anyDrawn = false;
    const auto divider = [&anyDrawn]() {
        if (anyDrawn) {
            SameLineIfFits(ImGui::GetFontSize() * 20.0f);
        }
        anyDrawn = true;
    };

    /// @note 開いていないと読む値を出さない: コマ番号も解けた所も焼く形も、文書が無いと指す相手が居ない。
    if (open) {
        const fluid::FluidRecipe& recipe = document.Recipe();

        divider();
        const int frames = fluideditor::FluidViewportLiveFrameCount(state, recipe);
        ImGui::Text("%d / %d", fluideditor::FluidViewportLiveFrame(state, recipe, frames) + 1, frames);
        ImGui::SetItemTooltip("いま出しているコマ / 焼くコマ数。← → で 1 コマ送り、Space で再生");
        ImGui::SameLine();
        ImGui::TextDisabled("%.2f s", state.playhead);

        divider();
        const float duration = fluideditor::TimelineDuration(recipe);
        const float solved = ClampF(state.preview.SolvedUntil(), 0.0f, duration);
        ImGui::TextDisabled("solved");
        ImGui::SameLine();
        ImGui::ProgressBar(duration > 0.0f ? solved / duration : 0.0f,
                           { 90.0f, ImGui::GetTextLineHeight() }, "");
        ImGui::SetItemTooltip("頭から途切れずに解けている所 (%.2f s / %.2f s)%s", solved, duration,
                              state.preview.IsSolving() ? "\n裏でまだ解いています" : "");
        ImGui::SameLine();
        ImGui::TextDisabled("%.2f s", solved);

        divider();
        const bool volume = recipe.bake.mode == fluid::FluidBakeMode::Volume3D;
        ImGui::TextDisabled("Bake %s", volume ? "3D" : "2D");
        ImGui::SetItemTooltip("この .fluid が焼く形 ([bake] の Mode。変えるのは Outliner の Bake)。\n"
                              "ビューポートの 2D / 3D は «見せ方» で、焼く形と食い違うときはそちらが知らせます");
    }

    if (m_autoSaveNotice > 0.0f) {
        divider();
        ImGui::PushStyleColor(ImGuiCol_Text, kOkColor);
        ImGui::TextUnformatted("自動保存しました");
        ImGui::PopStyleColor();
    }
    if (!m_followBlockedPath.empty()) {
        divider();
        ImGui::PushStyleColor(ImGuiCol_Text, kWarningColor);
        ImGui::TextUnformatted("未保存のため追従を止めています");
        ImGui::PopStyleColor();
        ImGui::SetItemTooltip("%s", DisplayPath(ctx, m_followBlockedPath).c_str());
        ImGui::SameLine();
        /// @note 追従が止まったことは分かっても «行く手段» が無いと結局 Asset Browser へ戻ることになる。
        if (ImGui::SmallButton("Switch##fe_follow_switch")) RequestOpen(ctx, m_followBlockedPath);
        ImGui::SetItemTooltip("選んでいる .fluid へ移る (保存するか捨てるかを尋ねます)");
    }
    /// @note 状態だけは開いていなくても出す: «開けません» «作れません» は文書が無いときにこそ出る。
    if (!state.status.empty()) {
        divider();
        ImGui::PushStyleColor(ImGuiCol_Text, state.statusIsError ? kErrorColor : kOkColor);
        ImGui::TextWrapped("%s", state.status.c_str());
        ImGui::PopStyleColor();
    }
}

void FluidEditorPanel::DrawConflictBar()
{
    fluideditor::State& state = *m_state;
    FluidDocument& document = state.document;
    ImGui::PushStyleColor(ImGuiCol_Text, kWarningColor);
    ImGui::TextUnformatted("この .fluid は外 (AI / Baker) で書き換えられ、未保存の変更と食い違っています。");
    ImGui::PopStyleColor();
    ImGui::SameLine();
    if (ImGui::SmallButton("Reload##fe_conflict_reload")) {
        document.ReloadFromDisk();
        fluideditor::ClampSelection(state);
        SetStatus(state, "ディスクの内容で読み直しました", false);
    }
    ImGui::SetItemTooltip("手元の変更を捨ててディスクの内容にする");
    ImGui::SameLine();
    if (ImGui::SmallButton("Keep mine##fe_conflict_keep")) {
        document.KeepLocal();
        SetStatus(state, "手元の変更を残しました (次の Save でディスクを上書きします)", false);
    }
    ImGui::SetItemTooltip("手元を残す。次の Save でディスクを上書きする");
}

void FluidEditorPanel::DrawEmptyState(EditorContext& ctx)
{
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float blockHeight = ImGui::GetTextLineHeightWithSpacing() * 2.0f + ImGui::GetFrameHeightWithSpacing() * 2.0f;
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (std::max)(0.0f, (avail.y - blockHeight) * 0.5f));

    CenteredText("Open a .fluid (double-click in Asset Browser)", false);
    CenteredText("or start from a preset", true);
    ImGui::Spacing();

    const float comboWidth = 180.0f;
    const char* buttonLabel = "New from preset...";
    const float buttonWidth = ImGui::CalcTextSize(buttonLabel).x + ImGui::GetStyle().FramePadding.x * 2.0f;
    const float total = comboWidth + ImGui::GetStyle().ItemSpacing.x + buttonWidth;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (std::max)(0.0f, (ImGui::GetContentRegionAvail().x - total) * 0.5f));
    ImGui::SetNextItemWidth(comboWidth);
    const auto current = static_cast<asset::FluidPreset>(m_newPresetIndex);
    if (ImGui::BeginCombo("##fe_new_preset", asset::FluidPresetName(current))) {
        for (int i = 0; i < static_cast<int>(asset::FluidPreset::Count); ++i) {
            ImGui::PushID(i);
            if (ImGui::Selectable(asset::FluidPresetName(static_cast<asset::FluidPreset>(i)), i == m_newPresetIndex))
                m_newPresetIndex = i;
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button(buttonLabel)) BeginNewFromPreset(ctx);
    ImGui::SetItemTooltip("作成先: %s", DisplayPath(ctx, DefaultNewFolder(ctx)).c_str());

    ImGui::Spacing();
    const char* galleryLabel = "Browse templates...";
    const float galleryWidth = ImGui::CalcTextSize(galleryLabel).x + ImGui::GetStyle().FramePadding.x * 2.0f;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX()
                         + (std::max)(0.0f, (ImGui::GetContentRegionAvail().x - galleryWidth) * 0.5f));
    if (ImGui::Button(galleryLabel)) fluideditor::OpenFluidTemplateGallery();
    ImGui::SetItemTooltip("絵の見本つきの一覧から選ぶ");
}

void FluidEditorPanel::DrawProperties(EditorContext& ctx)
{
    fluideditor::State& state = *m_state;
    FluidDocument& document = state.document;
    fluidui::FluidWidgetContext widgetContext;
    widgetContext.projectRoot = ctx.projectRoot;
    ImGui::PushID(document.Path().c_str());
    ImGui::PushID(static_cast<int>(document.selection.kind));
    ImGui::PushID(document.selection.index);

    /// @note 即時モードの編集はコピーへ書き、毎フレーム Commit へ渡す (ドラッグ中の変更は 1 つの Undo にまとまる)。
    fluid::FluidRecipe working = document.Recipe();
    const float previousBakeStart = working.output.warmup;
    bool changed = false;
    const char* label = "Edit Fluid";
    const FluidSelection selection = document.selection;
    switch (selection.kind) {
    case FluidSelectionKind::Simulation:
        ImGui::SeparatorText("Simulation");
        changed = fluidui::EditSimulation(widgetContext, working);
        label = "Edit Fluid Simulation";
        break;
    case FluidSelectionKind::Look:
        ImGui::SeparatorText("Look");
        changed = fluidui::EditLook(widgetContext, working);
        label = "Edit Fluid Look";
        break;
    case FluidSelectionKind::Output:
        ImGui::SeparatorText("Output");
        changed = fluidui::EditOutput(widgetContext, working);
        ImGui::TextDisabled("Playhead: %.2f s (simulation %.2f s)", state.playhead,
                            working.output.warmup + state.playhead);
        {
            const int frameCount = std::clamp(working.output.columns, 1, 32)
                                 * std::clamp(working.output.rows, 1, 32);
            const float frameDt = (std::max)(working.output.duration, 0.05f) / static_cast<float>(frameCount);
            const int playheadFrame = std::clamp(static_cast<int>(std::lround(state.playhead / frameDt)),
                                                 0, frameCount);
            const float playhead = static_cast<float>(playheadFrame) * frameDt;
            ImGui::BeginDisabled(playhead <= 0.0f || working.output.warmup + playhead > 30.0f);
            if (ImGui::Button("Start at Playhead")) {
                working.output.warmup += playhead;
                state.playing = false;
                changed = true;
            }
            ImGui::SetItemTooltip("再生位置をコマに吸着して Bake 開始へ。収録する長さは維持します");
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(playhead < 0.05f || playhead >= working.output.duration);
            if (ImGui::Button("End at Playhead")) {
                working.output.duration = playhead;
                state.playing = false;
                changed = true;
            }
            ImGui::SetItemTooltip("再生位置を Bake 終了にします");
            ImGui::EndDisabled();
        }
        label = "Edit Fluid Output";
        break;
    case FluidSelectionKind::Bake:
        ImGui::SeparatorText("Bake");
        changed = fluidui::EditBake(widgetContext, working);
        label = "Edit Fluid Bake Settings";
        break;
    case FluidSelectionKind::Source:
    case FluidSelectionKind::Force:
    case FluidSelectionKind::Collider: {
        if (selection.index < 0 || selection.index >= fluidui::PartCount(working, selection.kind)) {
            ImGui::TextDisabled("(選んだ部品がありません)");
            break;
        }
        const std::string title = fluidui::PartDisplayName(working, selection.kind, selection.index);
        ImGui::SeparatorText(title.c_str());
        ImGui::PushID("fe_part_header");
        if (bool* enabled = fluidui::PartEnabled(working, selection.kind, selection.index))
            changed |= ImGui::Checkbox("Enabled", enabled);
        ImGui::PopID();
        ImGui::Spacing();
        if (selection.kind == FluidSelectionKind::Source) {
            changed |= fluidui::EditSource(widgetContext, working, selection.index);
            label = "Edit Fluid Source";
        } else if (selection.kind == FluidSelectionKind::Force) {
            changed |= fluidui::EditForce(widgetContext, working, selection.index);
            label = "Edit Fluid Force";
        } else {
            changed |= fluidui::EditCollider(widgetContext, working, selection.index);
            label = "Edit Fluid Collider";
        }
        break;
    }
    case FluidSelectionKind::None:
        ImGui::TextDisabled("Outliner で項目を選ぶと、ここに設定が出ます");
        break;
    }
    document.Commit(ctx, label, working, changed);
    if (changed && selection.kind == FluidSelectionKind::Output) {
        const fluid::FluidOutputSettings& output = document.Recipe().output;
        state.playhead = std::clamp(state.playhead + previousBakeStart - output.warmup,
                                    0.0f, fluideditor::TimelineDuration(document.Recipe()));
    }
    ImGui::PopID();
    ImGui::PopID();
    ImGui::PopID();
}

/// @note フレームの流れ

void FluidEditorPanel::OnBeforeBegin(EditorContext& ctx)
{
    m_resources = ctx.resources;
    m_context = &ctx;
    /// @note NoAutoMerge はメイン HWND 内の座標でも専用 HWND を作る。親を持たせずタスクバーにも表示する。
    ImGuiWindowClass windowClass;
    windowClass.ParentViewportId = 0;
    windowClass.ViewportFlagsOverrideSet = ImGuiViewportFlags_NoAutoMerge;
    windowClass.ViewportFlagsOverrideClear = ImGuiViewportFlags_NoDecoration | ImGuiViewportFlags_NoTaskBarIcon;
    ImGui::SetNextWindowClass(&windowClass);

    const ImGuiViewport* mainViewport = ImGui::GetMainViewport();
    ImVec2 initialSize{ 1280.0f, 760.0f };
    if (mainViewport != nullptr) {
        initialSize.x = (std::min)(initialSize.x, mainViewport->WorkSize.x * 0.9f);
        initialSize.y = (std::min)(initialSize.y, mainViewport->WorkSize.y * 0.9f);
        const ImVec2 center{ mainViewport->WorkPos.x + mainViewport->WorkSize.x * 0.5f,
                             mainViewport->WorkPos.y + mainViewport->WorkSize.y * 0.5f };
        ImGui::SetNextWindowPos(center, ImGuiCond_FirstUseEver, { 0.5f, 0.5f });
    }
    ImGui::SetNextWindowSize(initialSize, ImGuiCond_FirstUseEver);

    if (!ctx.requestOpenFluidEditor.empty()) {
        const std::string path = std::move(ctx.requestOpenFluidEditor);
        ctx.requestOpenFluidEditor.clear();
        RequestOpen(ctx, path);
        /// @note 既に開いている OS ウィンドウも前へ出す (EditorApp は閉じているときだけ開く)。
        ImGui::SetNextWindowFocus();
    }
    FollowAssetSelection(ctx);

    /// @note 最小化中も、焼きの完了と外からの書き換えは拾っておく。
    PollBakeJob(ctx);
    if (m_state->document.IsOpen()) {
        m_state->document.PollExternalChange();
        /// @note 保存できたら dirty が下りる。レジストリからの取り下げは下の SyncDirtyRegistry がやる。
        if (fluideditor::TickFluidAutoSave(ctx, m_state->document)) m_autoSaveNotice = 2.5f;
    }
    m_autoSaveNotice = (std::max)(m_autoSaveNotice - ImGui::GetIO().DeltaTime, 0.0f);
    SyncDirtyRegistry(ctx);

    /// @note 3D のライブプレビューは GPU を記録するので、レンダラーのフレーム内 = Begin より前に積む
    /// @note (Volume Flipbook Baker パネルと同じ場所)。共有 Baker は 1 つしかなく、最小化中も
    /// @note 撃つと Volume Flipbook Baker パネルのプレビューと鍵を奪い合って両方が解き直し続ける。
    if (WasContentRendered()) fluideditor::TickVolumePreview(ctx, *m_state);
}

HotkeyScope FluidEditorPanel::GetHotkeyScope() const
{
    /// @note 何も開いていない Fluid Editor はキーの文脈を持たない。
    /// @note ここで名乗ると、フォーカスがあるだけで Ctrl+S がシーンへ届かなくなる。
    if (m_state == nullptr || !m_state->document.IsOpen()) return HotkeyScope::None;
    return HotkeyScope::FluidEditor;
}

void FluidEditorPanel::OnRenderContent(EditorContext& ctx)
{
    fluideditor::State& state = *m_state;
    FluidDocument& document = state.document;

    /// @note フォーカスの申告は IPanel::OnRender が GetHotkeyScope() を見て行う。
    ImGui::PushID("fluid_editor");
    fluideditor::EndStaleDrags(ctx, state);
    HandleShortcuts(ctx);

    DrawToolbar(ctx);
    /// @note テンプレート一覧は文書を開いていなくても出す (何も無い所から作り始める道)。
    if (const std::string created = fluideditor::DrawFluidTemplateGallery(ctx, DefaultNewFolder(ctx));
        !created.empty()) {
        ctx.requestAssetBrowserRefresh = true;
        RequestOpen(ctx, created);
    }
    if (const fluideditor::FluidSaveAsResult saveAs = fluideditor::DrawFluidSaveAsModal(ctx, document);
        saveAs.action != fluideditor::FluidSaveAsAction::None)
        FinishSaveAs(ctx, saveAs);
    if (document.IsOpen() && document.HasExternalConflict()) DrawConflictBar();
    ImGui::Separator();

    if (!document.IsOpen()) {
        DrawEmptyState(ctx);
        ImGui::PopID();
        return;
    }

    fluideditor::ClampSelection(state);
    fluideditor::ClampKeySelection(state);
    SyncPreview();
    state.preview.Tick(ctx.resources, ctx.imguiRenderer);
    AdvancePlayback();

    const ImGuiStyle& style = ImGui::GetStyle();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float height = (std::max)(avail.y, 1.0f);
    const bool compact = avail.x < ImGui::GetFontSize() * 58.0f;
    const float fixed = kSplitterThickness * 2.0f + style.ItemSpacing.x * 4.0f;
    if (!compact) {
        m_outlinerWidth = ClampF(m_outlinerWidth, 140.0f, (std::max)(140.0f, avail.x * 0.4f));
        m_propertiesWidth = ClampF(m_propertiesWidth, 220.0f, (std::max)(220.0f, avail.x * 0.5f));
    }
    float centerWidth = avail.x - m_outlinerWidth - m_propertiesWidth - fixed;
    if (!compact && centerWidth < 160.0f) {
        m_propertiesWidth = (std::max)(220.0f, m_propertiesWidth - (160.0f - centerWidth));
        centerWidth = (std::max)(avail.x - m_outlinerWidth - m_propertiesWidth - fixed, 80.0f);
    }

    if (!compact) {
        ImGui::BeginChild("##fe_outliner", { m_outlinerWidth, height }, ImGuiChildFlags_Borders);
        fluideditor::DrawOutliner(ctx, state);
        ImGui::EndChild();
        ImGui::SameLine();
        Splitter("##fe_split_left", m_outlinerWidth, 1.0f, { kSplitterThickness, height }, true);
        ImGui::SameLine();
    }

    const auto drawCenter = [&]() {
        ImGui::BeginChild("##fe_center", { compact ? 0.0f : centerWidth, compact ? 0.0f : height }, ImGuiChildFlags_None,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        {
            const float centerHeight = ImGui::GetContentRegionAvail().y;
            const float timelineMax = (std::max)(32.0f, centerHeight * 0.55f);
            float timelineHeight = ClampF(m_timelineHeight, (std::min)(90.0f, timelineMax), timelineMax);
            const float viewportHeight =
                (std::max)(1.0f, centerHeight - timelineHeight - kSplitterThickness - style.ItemSpacing.y * 2.0f);
            /// @note NoMove が要る: ImGuizmo は ImGui の項目を使わないので掴んでも ActiveId が立たず、
            /// @note «何も無い所のドラッグ» としてウィンドウ移動が始まる。移動の取り消しはマウス下の
            /// @note ウィンドウ (= この子) の NoMove を見るので、ここに付ければ止まる。
            ImGui::BeginChild("##fe_viewport", { 0.0f, viewportHeight }, ImGuiChildFlags_Borders,
                              ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse
                                  | ImGuiWindowFlags_NoMove);
            if (ImGui::BeginTabBar("##fe_view_tabs")) {
                if (ImGui::BeginTabItem("Viewport###fe_tab_view")) {
                    fluideditor::DrawViewport(ctx, state);
                    ImGui::EndTabItem();
                }
                if (fluideditor::HasFluidBakedResult()) {
                    const ImGuiTabItemFlags flags = m_selectBakedTab ? ImGuiTabItemFlags_SetSelected : 0;
                    m_selectBakedTab = false;
                    if (ImGui::BeginTabItem("Baked###fe_tab_baked", nullptr, flags)) {
                        fluideditor::DrawFluidBakedTab(ctx, state);
                        ImGui::EndTabItem();
                    }
                }
                ImGui::EndTabBar();
            }
            ImGui::EndChild();
            Splitter("##fe_split_timeline", timelineHeight, -1.0f,
                     { (std::max)(ImGui::GetContentRegionAvail().x, 1.0f), kSplitterThickness }, false);
            if (ImGui::IsItemActive()) m_timelineHeight = timelineHeight;
            ImGui::BeginChild("##fe_timeline", { 0.0f, 0.0f }, ImGuiChildFlags_Borders,
                              ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            fluideditor::DrawTimeline(ctx, state);
            ImGui::EndChild();
        }
        ImGui::EndChild();
    };

    if (compact) {
        if (ImGui::BeginTabBar("##fe_layout_tabs")) {
            if (ImGui::BeginTabItem("Preview")) {
                drawCenter();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Outliner")) {
                ImGui::BeginChild("##fe_outliner", { 0.0f, 0.0f }, ImGuiChildFlags_Borders);
                fluideditor::DrawOutliner(ctx, state);
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Properties")) {
                ImGui::BeginChild("##fe_properties", { 0.0f, 0.0f }, ImGuiChildFlags_Borders);
                DrawProperties(ctx);
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    } else {
        drawCenter();

        ImGui::SameLine();
        Splitter("##fe_split_right", m_propertiesWidth, -1.0f, { kSplitterThickness, height }, true);
        ImGui::SameLine();
        ImGui::BeginChild("##fe_properties", { 0.0f, height }, ImGuiChildFlags_Borders);
        DrawProperties(ctx);
        ImGui::EndChild();
    }

    ImGui::PopID();
}

} // namespace fbzz::editor
