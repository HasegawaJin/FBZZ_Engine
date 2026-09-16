/// @file    AssetMaintenancePanel.cpp
/// @brief   GUID 重複を検出して振り直すアセット保守パネル。
/// @author  Hasegawa Jin
/// @date    2026-09-10
#include <Editor/Panels/AssetMaintenancePanel.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/Import/ImportCacheStore.hpp>
#include <Editor/Util/EditorTheme.hpp>
#include <Editor/Util/Toast.hpp>
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>

namespace fbzz::editor {

namespace {

// ソート用の生の時刻。読めない場合は 0 (= いちばん古い扱い) を返す。
long long MetaWriteTicks(const std::string& assetPath)
{
    std::error_code ec;
    const auto ft = std::filesystem::last_write_time(
        util::FileSystem::PathFromUtf8(assetPath + ".meta"), ec);
    return ec ? 0 : static_cast<long long>(ft.time_since_epoch().count());
}

std::string MetaWriteText(const std::string& assetPath)
{
    std::error_code ec;
    const auto ft = std::filesystem::last_write_time(
        util::FileSystem::PathFromUtf8(assetPath + ".meta"), ec);
    if (ec) return "-";
    const auto sysTp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        ft - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
    const std::time_t t = std::chrono::system_clock::to_time_t(sysTp);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", std::localtime(&t));
    return buf;
}

// 表示用にプロジェクトルートを落とす。落とせなければ絶対パスのまま出す。
std::string ToRelativeForDisplay(const std::string& absPath)
{
    const std::string root = asset::AssetDatabase::ProjectRoot();
    const std::string norm = util::FileSystem::NormalizePathSeparators(absPath);
    if (!root.empty() && norm.size() > root.size() && norm.compare(0, root.size(), root) == 0)
        return norm.substr(root.size());
    return norm;
}

} // namespace

void AssetMaintenancePanel::Rescan()
{
    m_groups.clear();
    m_scanned = true;

    // AssetDatabase は «弾かれた 1 件» 単位で記録する。同じ guid の記録をまとめ直し、
    // 先勝ちした側も候補に含める。
    // WHY 先勝ち側も含めるか: 索引の採用はスキャン順で決まっており、古さとは無関係。
    //     3 つ以上が同じ guid を名乗っている場合、残すべき 1 つが «弾かれた側» に
    //     居ることがある。
    std::unordered_map<std::string, std::unordered_set<std::string>> byGuid;
    for (const auto& c : asset::AssetDatabase::GuidConflicts()) {
        auto& paths = byGuid[c.guid];
        paths.insert(util::FileSystem::NormalizePathSeparators(c.keptPath));
        paths.insert(util::FileSystem::NormalizePathSeparators(c.duplicatePath));
    }

    for (auto& [guid, paths] : byGuid) {
        ConflictGroup group;
        group.guid = guid;
        for (const std::string& path : paths) {
            group.entries.push_back({ path, ToRelativeForDisplay(path), MetaWriteText(path),
                                      util::FileSystem::Exists(path) });
        }
        // .meta が古い順。同時刻ならパスで決める (毎回同じ並びにして、押すたびに
        // 残る側が入れ替わらないようにする)。
        std::sort(group.entries.begin(), group.entries.end(),
                  [](const ConflictGroup::Entry& a, const ConflictGroup::Entry& b) {
                      const long long ta = MetaWriteTicks(a.path);
                      const long long tb = MetaWriteTicks(b.path);
                      if (ta != tb) return ta < tb;
                      return a.path < b.path;
                  });
        m_groups.push_back(std::move(group));
    }

    std::sort(m_groups.begin(), m_groups.end(),
              [](const ConflictGroup& a, const ConflictGroup& b) { return a.guid < b.guid; });
}

int AssetMaintenancePanel::FixGroup(const ConflictGroup& group)
{
    int fixed = 0;
    // 先頭 (いちばん古い .meta) は触らない。既存の参照はそこへ解決されている。
    for (std::size_t i = 1; i < group.entries.size(); ++i) {
        const ConflictGroup::Entry& entry = group.entries[i];
        if (!entry.exists) continue;
        std::string newGuid;
        if (!asset::AssetDatabase::ReassignGuid(entry.path, newGuid)) continue;
        // ReassignGuid は Library/Baked/<guid>/ を連れて行く。fingerprint の索引も
        // 同じ guid をキーにしているので、揃えて移さないと焼き直しが走る。
        ImportCacheStore::Rekey(group.guid, newGuid);
        FBZZ_LOG_INFO("Asset Maintenance: %s  %s -> %s",
                      entry.relative.c_str(), group.guid.c_str(), newGuid.c_str());
        ++fixed;
    }
    return fixed;
}

void AssetMaintenancePanel::OnRenderContent(EditorContext& /*ctx*/)
{
    if (!m_scanned) Rescan();

    ImGui::TextWrapped(
        "Assets that share a guid resolve to one side only: every \"guid:\" reference "
        "silently points at the first one the index accepted. Fixing keeps the asset with "
        "the OLDEST .meta and issues a fresh guid to the ones added later, so no existing "
        "reference changes where it resolves. Import output (Library/Baked) and the "
        "reimport fingerprint move along with the new guid.");
    ImGui::Spacing();

    if (ImGui::Button("Rescan")) {
        Rescan();
        m_lastResult.clear();
    }
    ImGui::SameLine();

    ImGui::BeginDisabled(m_groups.empty());
    if (ImGui::Button("Fix All"))
        ImGui::OpenPopup("Reassign guids?");
    ImGui::EndDisabled();

    // WHY 確認を挟むか: guid の振り直しは «参照キーを書き換える» 操作で、取り消せない。
    //     押す前に «何件が新しい guid になるのか» が分かる形にしておく。
    if (ImGui::BeginPopupModal("Reassign guids?", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        int affected = 0;
        for (const ConflictGroup& group : m_groups)
            affected += static_cast<int>(group.entries.size()) - 1;

        ImGui::Text("%d asset(s) in %d group(s) will get a new guid.",
                    affected, static_cast<int>(m_groups.size()));
        ImGui::TextDisabled("The oldest .meta in each group keeps its guid.");
        ImGui::TextDisabled("Import output and fingerprints move with it. Not undoable.");
        ImGui::Spacing();

        if (ImGui::Button("Reassign")) {
            int fixed = 0;
            const int groups = static_cast<int>(m_groups.size());
            for (const ConflictGroup& group : m_groups) fixed += FixGroup(group);
            m_lastResult = "Reassigned " + std::to_string(fixed) + " asset(s) in "
                         + std::to_string(groups) + " group(s).";
            Toast::Info(m_lastResult);
            Rescan();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (!m_lastResult.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", m_lastResult.c_str());
    }

    ImGui::Separator();

    if (m_groups.empty()) {
        ImGui::TextDisabled("No duplicate guids.");
        return;
    }

    for (std::size_t g = 0; g < m_groups.size(); ++g) {
        const ConflictGroup& group = m_groups[g];
        ImGui::PushID(static_cast<int>(g));

        ImGui::SeparatorText(group.guid.c_str());
        if (ImGui::SmallButton("Fix This")) {
            const int fixed = FixGroup(group);
            m_lastResult = "Reassigned " + std::to_string(fixed) + " asset(s).";
            Toast::Info(m_lastResult);
            Rescan();
            ImGui::PopID();
            break;  // Rescan で m_groups が入れ替わったので、この描画は打ち切る
        }

        constexpr ImGuiTableFlags kFlags = ImGuiTableFlags_Borders
                                         | ImGuiTableFlags_RowBg
                                         | ImGuiTableFlags_SizingStretchProp;
        if (ImGui::BeginTable("##conflict", 3, kFlags)) {
            ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, 90.0f);
            ImGui::TableSetupColumn("Asset");
            ImGui::TableSetupColumn(".meta modified", ImGuiTableColumnFlags_WidthFixed, 130.0f);
            ImGui::TableHeadersRow();

            for (std::size_t i = 0; i < group.entries.size(); ++i) {
                const ConflictGroup::Entry& entry = group.entries[i];
                ImGui::TableNextRow();

                ImGui::TableSetColumnIndex(0);
                if (i == 0)
                    ImGui::TextColored(EditorTheme::Color(ThemeColor::Success), "keep");
                else
                    ImGui::TextColored(EditorTheme::Color(ThemeColor::Warning), "new guid");

                ImGui::TableSetColumnIndex(1);
                if (entry.exists) ImGui::TextUnformatted(entry.relative.c_str());
                else              ImGui::TextDisabled("%s (missing)", entry.relative.c_str());
                // WHY フルパスを出すか: 表示はプロジェクトルートを落とした相対形なので、
                //     «同じ実体を別表記で二重登録している» 類の衝突だと 2 行が同じ文字に
                //     見えてしまい、何と何がぶつかっているのか読み取れない。
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", entry.path.c_str());

                ImGui::TableSetColumnIndex(2);
                ImGui::TextDisabled("%s", entry.metaTime.c_str());
            }
            ImGui::EndTable();
        }
        ImGui::PopID();
    }
}

} // namespace fbzz::editor
