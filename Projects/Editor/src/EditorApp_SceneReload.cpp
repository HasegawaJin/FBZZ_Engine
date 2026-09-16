/// @file    EditorApp_SceneReload.cpp
/// @brief   開いているシーンがディスク上で書き換わったときの追従と、上書き事故の防止。
/// @author  Hasegawa Jin
/// @date    2026-08-21
#include <Editor/EditorApp.hpp>
#include <Editor/Util/EditorTheme.hpp>
#include <Editor/Util/ModalDialog.hpp>
#include <Editor/Util/Toast.hpp>
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <imgui.h>
#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace fbzz::editor {

void EditorApp::CaptureSceneDiskStamp()
{
    if (m_ctx.currentScenePath.empty()) {
        m_sceneDiskStampValid = false;
        m_sceneDiskChanged    = false;
        return;
    }
    m_sceneDiskStamp = util::FileSystem::LastWriteTime(
        util::FileSystem::PathFromUtf8(m_ctx.currentScenePath));
    m_sceneDiskStampValid = m_sceneDiskStamp != std::filesystem::file_time_type{};
    m_sceneDiskChanged    = false;
}

bool EditorApp::IsSceneStaleOnDisk() const
{
    if (!m_sceneDiskStampValid || m_ctx.currentScenePath.empty()) return false;

    const auto now = util::FileSystem::LastWriteTime(
        util::FileSystem::PathFromUtf8(m_ctx.currentScenePath));
    // 読めない (削除・排他中) ときは「変わった」と言わない。保存を止める理由にならず、
    // 止めると復旧手段まで塞いでしまう。
    if (now == std::filesystem::file_time_type{}) return false;
    return now != m_sceneDiskStamp;
}

bool EditorApp::ReloadSceneFromDisk()
{
    const std::string path = m_ctx.currentScenePath;
    if (path.empty()) return false;

    // OpenScenePath が lastScenePath / Recent / クリーン状態まで面倒を見る。
    // 「開き直す」と「外部変更に追従する」で経路を分けると、片方だけ手当てが漏れる。
    if (!OpenScenePath(path)) {
        Toast::Error("Reload failed: " + util::FileSystem::GetFilename(path));
        return false;
    }
    CaptureSceneDiskStamp();
    ReportUnresolvedSceneRefs(path);
    return true;
}

// シーンファイルに書かれた guid 参照のうち、どのアセットにも解決できないものを報告する。
//
// WHY テキストを直接見るか: SceneSerializer は解決できない参照を空として復元するため、
//     読み込み後のシーンからは「元々空だったのか、参照が切れたのか」を区別できない。
//     人がまだ触っていない = 直せる段階で気付かせるには、書かれた内容そのものを見るしかない。
void EditorApp::ReportUnresolvedSceneRefs(const std::string& path) const
{
    std::string text;
    if (!util::FileSystem::ReadText(path, text)) return;

    std::vector<std::string> missing;
    constexpr std::string_view kPrefix = "guid:";
    for (size_t pos = text.find(kPrefix); pos != std::string::npos;
         pos = text.find(kPrefix, pos + kPrefix.size())) {
        const size_t start = pos + kPrefix.size();
        size_t end = start;
        while (end < text.size() && std::isxdigit(static_cast<unsigned char>(text[end]))) ++end;
        if (end - start != 32) continue;

        const std::string guid = text.substr(start, 32);
        if (!asset::AssetDatabase::PathFromGuid(guid).empty()) continue;

        // 併記されたパスヒントの実体が残っていれば、GuidRefCodec が拾い直せるので
        // 参照は生きている。guid が引けないこと自体は事実だが、報告はしない。
        // 値の終わりは TOML の引用符 (または行末) で、その手前に '|' があればヒント。
        const size_t valueEnd = text.find_first_of("'\"\n", end);
        const size_t hintSep  = text.find(asset::AssetDatabase::kRefHintSeparator, end);
        if (hintSep != std::string::npos && hintSep < valueEnd) {
            const std::string hint = text.substr(hintSep + 1, valueEnd - hintSep - 1);
            if (!hint.empty() &&
                util::FileSystem::Exists(asset::AssetDatabase::ProjectRoot() + hint))
                continue;
        }

        if (std::find(missing.begin(), missing.end(), guid) == missing.end())
            missing.push_back(guid);
    }

    if (missing.empty()) return;

    // 直前に FBX を入れ直した直後だけは、まだ索引に載っていない生成物を拾って
    // 誤検出することがある。断定せず、確認先まで書いておく。
    FBZZ_LOG_WARN("Scene has %zu guid reference(s) that resolve to nothing: %s "
                  "(the asset was deleted, or its guid was written by hand)",
                  missing.size(), util::FileSystem::GetFilename(path).c_str());
    for (const std::string& guid : missing)
        FBZZ_LOG_WARN("  guid:%s -> no asset", guid.c_str());
    Toast::Warning(std::to_string(missing.size()) + " unresolved reference(s) - see Console");
}

void EditorApp::ProcessSceneDiskReload()
{
    // SaveScene が立てた上書き確認を、モーダル描画の外であるここで開く。
    if (m_staleSaveConfirmPending) {
        m_staleSaveConfirmPending = false;
        const std::string name = util::FileSystem::GetFilename(m_ctx.currentScenePath);
        ModalDialog::OpenUnsavedChanges(
            "Scene changed on disk",
            name + " was modified outside the editor after you opened it.\n\n"
            "Save    - overwrite the file with the version in the editor\n"
            "Discard - throw away the changes here and load the file from disk",
            [this] {
                // 上書きを選んだ = ディスクの版は捨ててよい。基準時刻を今の値へ
                // 進めてから保存し直し、同じ確認を繰り返さないようにする。
                CaptureSceneDiskStamp();
                return SaveScene();
            },
            [this] { (void)ReloadSceneFromDisk(); });
    }

    // 開いているシーン以外への通知と、1 回の保存で複数回届く通知はここで畳む。
    bool touched = false;
    for (const std::string& path : m_ctx.pendingSceneReloads)
        touched = touched || util::FileSystem::SamePathText(path, m_ctx.currentScenePath);
    m_ctx.pendingSceneReloads.clear();

    const auto now = std::chrono::steady_clock::now();
    if (touched && IsSceneStaleOnDisk()) {
        m_sceneDiskChanged   = true;
        m_sceneDiskChangedAt = now;   // 通知が続く間は待ち時間を延ばす
    }
    if (!m_sceneDiskChanged) return;

    // Play 中とプレファブ編集中は保留する。シーンを作り直すと実行中の状態と
    // 編集面が壊れるため、抜けたフレームで改めて同じ判定を通す。
    if (!m_playMode.IsInEditor() || m_ctx.InPrefabEditMode()) return;

    // 未保存の変更があるなら勝手に捨てない。通知バーを出して人に選ばせる。
    if (m_ctx.sceneDirty) return;

    // 書き込みが終わるのを待つ。途中の内容を開き直すと、解析に失敗した空のシーンが
    // 「読み込めたシーン」として画面に出てしまう。
    constexpr std::chrono::milliseconds kSceneReloadQuietTime{ 300 };
    if (now - m_sceneDiskChangedAt < kSceneReloadQuietTime) return;

    m_sceneDiskChanged = false;
    FBZZ_LOG_INFO("Scene changed on disk, reloading: %s", m_ctx.currentScenePath.c_str());
    (void)ReloadSceneFromDisk();
}

void EditorApp::DrawSceneReloadBar(EditorContext& ctx)
{
    if (!m_sceneDiskChanged) return;
    // Play 中とプレファブ編集中の保留はここでは出さない。押せば壊れるボタンを
    // 並べることになるし、抜けた時点で自動的に片付く。
    if (!m_playMode.IsInEditor() || ctx.InPrefabEditMode()) return;

    const std::string name = util::FileSystem::GetFilename(ctx.currentScenePath);

    const float barH = ImGui::GetFrameHeight() + 4.0f;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, EditorTheme::Color(ThemeColor::SurfaceRaised));
    ImGui::BeginChild("##SceneDiskChangedBar", ImVec2(0.0f, barH), false,
                      ImGuiWindowFlags_NoScrollbar);

    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(EditorTheme::Color(ThemeColor::Warning),
                       "  %s changed on disk  \xE2\x80\x94  you have unsaved changes here",
                       name.c_str());

    const float btnW = 190.0f;
    ImGui::SameLine(ImGui::GetWindowWidth() - btnW);
    if (ImGui::SmallButton("Reload")) {
        m_sceneDiskChanged = false;
        (void)ReloadSceneFromDisk();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Discard the unsaved changes here and load the file from disk");

    ImGui::SameLine();
    if (ImGui::SmallButton("Keep Mine")) {
        // 見送るだけ。実際の上書きは保存時にもう一度確認する。
        m_sceneDiskChanged = false;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Keep the version in the editor (saving will ask before overwriting)");

    ImGui::EndChild();
    ImGui::PopStyleColor();
}

} // namespace fbzz::editor
