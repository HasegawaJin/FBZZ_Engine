// FBZZ Engine
// VFXTemplateCatalog.cpp | fbzz::editor
// Assets/VFX/Templates の走査と Template 書き出しの実装
#include <Editor/VFXEditor/Services/VFXTemplateCatalog.hpp>

#include <Editor/EditorContext.hpp>
#include <Engine/Asset/VFXGraphAsset.hpp>
#include <Engine/Core/Time.hpp>
#include <Engine/Scene/Components/ParticleEmitter.hpp>
#include <Engine/Scene/Components/VFXGraphComponent.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include <Editor/VFXEditor/Document/VFXGraphOps.hpp>
#include <system_error>

namespace fbzz::editor {

// 共有ヘルパー (ノード色・アイコン・パラメーター編集など) は fbzz::editor::vfx に
// 隔離してある。Editor 全体の名前空間を汚さないための境界。
using namespace vfx;

namespace {

std::string LowerCopy(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

// Templates ルートからの相対サブフォルダをカテゴリ名として取り出す。
// "Combat/Impact.vfx" -> "Combat" / "Impact.vfx" -> ""
std::string CategoryOf(const std::filesystem::path& root, const std::filesystem::path& file)
{
    std::error_code errorCode;
    const std::filesystem::path relative =
        std::filesystem::relative(file.parent_path(), root, errorCode);
    if (errorCode) return {};
    const std::string text = relative.generic_string();
    return (text == "." || text == "..") ? std::string{} : text;
}

} // namespace

const char* TemplateApplyModeName(TemplateApplyMode mode)
{
    switch (mode) {
    case TemplateApplyMode::Replace: return "Replace";
    case TemplateApplyMode::Merge: return "Merge";
    case TemplateApplyMode::SubGraph: return "Sub Graph";
    }
    return "Unknown";
}

const char* TemplateOriginName(TemplateOrigin origin)
{
    switch (origin) {
    case TemplateOrigin::Project: return "Project";
    case TemplateOrigin::Engine: return "Engine";
    case TemplateOrigin::Bundled: return "SDK";
    }
    return "Unknown";
}

std::string TemplateThumbnailPath(const std::string& templateFilePath)
{
    namespace fs = std::filesystem;
    const fs::path file(templateFilePath);
    return (file.parent_path() / ".thumbnails" / (file.stem().generic_string() + ".png"))
        .generic_string();
}

void VFXTemplateCatalog::Scan(EditorContext& ctx, bool force, bool checkAssetReferences)
{
    if (scanned && !force) return;
    scanned = true;
    entries.clear();

    namespace fs = std::filesystem;
    // Project側のTemplateを最優先し、次に開発Engine、最後にSDK/bundle同梱Assetsを探す。
    // WHY: 独立VFX EditorはEditorAppのProject metadata読込を通らずengineRootが空になり得るため、
    //      executable基準のfallbackがないと配布版で組み込みTemplateが常に見つからない。
    struct Root { fs::path path; TemplateOrigin origin; };
    std::vector<Root> roots;
    if (!ctx.projectRoot.empty())
        roots.push_back({ fs::path(ctx.projectRoot) / kTemplateRelativeDir, TemplateOrigin::Project });
    if (!ctx.engineRoot.empty())
        roots.push_back({ fs::path(ctx.engineRoot) / kTemplateRelativeDir, TemplateOrigin::Engine });
    const fs::path executableDirectory = util::FileSystem::GetExecutableDirectory();
    roots.push_back({ executableDirectory / "assets/VFX/Templates", TemplateOrigin::Bundled });
    roots.push_back({ executableDirectory / kTemplateRelativeDir, TemplateOrigin::Bundled });

    std::error_code errorCode;
    for (const Root& root : roots) {
        if (!fs::is_directory(root.path, errorCode)) { errorCode.clear(); continue; }
        // 再帰走査。サブフォルダをカテゴリとして扱えるようにするため。
        // .thumbnails は生成物なので降りない (PNG しか無いが、無駄な走査を避ける)。
        for (fs::recursive_directory_iterator iterator(root.path, errorCode), end;
             iterator != end; iterator.increment(errorCode)) {
            if (errorCode) { errorCode.clear(); break; }
            const fs::directory_entry& file = *iterator;
            if (file.is_directory(errorCode)) {
                if (file.path().filename() == ".thumbnails") iterator.disable_recursion_pending();
                continue;
            }
            if (!file.is_regular_file(errorCode) || file.path().extension() != ".vfx") continue;

            GraphTemplateEntry entry;
            entry.name = file.path().stem().generic_string();
            entry.category = CategoryOf(root.path, file.path());
            entry.origin = root.origin;
            // 先に見つかったroot(=優先度が高い)の同名Templateを勝たせる。
            // カテゴリが違えば別物として扱う (Combat/Impact と Env/Impact は別)。
            if (std::any_of(entries.begin(), entries.end(),
                    [&entry](const GraphTemplateEntry& item) {
                        return item.name == entry.name && item.category == entry.category;
                    }))
                continue;
            entry.path = file.path().generic_string();

            // 適用前に中身が分かるよう、カタログ化の時点で1度だけ解析してサマリを作る。
            asset::VFXGraphAsset preview;
            if (asset::ParseVFXGraphAsset(entry.path, preview, nullptr)) {
                entry.valid = true;
                entry.nodeCount = static_cast<int>(preview.nodes.size());
                entry.linkCount = static_cast<int>(preview.links.size());
                std::vector<float> starts;
                float duration = 0.0f;
                if (asset::BuildVFXGraphSchedule(preview, starts, duration, nullptr))
                    entry.duration = duration;
                entry.summary = SummarizeGraphNodeTypes(preview);
                entry.description = preview.description;
                entry.tags = preview.tags;
                entry.requiredRoles = preview.requiredRoles;
                const asset::VFXGraphBudgetStats budget =
                    asset::CalculateVFXGraphBudget(preview);
                entry.particleBudget = budget.particles;
                entry.lightBudget = budget.lights;
                entry.audioBudget = budget.audioVoices;
                for (const auto& variant : preview.variants)
                    entry.variants.push_back({ variant.name,
                                               static_cast<int>(variant.overrides.size()) });
                // 層 (グループ枠) は部分取り込みの単位。ノード数が 0 の枠は選ばせない。
                for (const auto& group : preview.groups) {
                    const int members =
                        static_cast<int>(CollectGroupMemberNodes(preview, group.id).size());
                    if (members == 0) continue;
                    entry.layers.push_back({ group.id, group.title, group.note, members });
                }
                // 参照切れは「置いても何も出ない」形でしか現れないため、適用前に出す。
                if (checkAssetReferences)
                    entry.missingAssets = asset::CollectMissingVFXReferences(preview);
            } else {
                entry.summary = "Failed to parse";
            }
            const std::string thumbnail = TemplateThumbnailPath(entry.path);
            if (fs::is_regular_file(fs::path(thumbnail), errorCode))
                entry.thumbnailPath = thumbnail;
            errorCode.clear();

            std::string searchable = entry.name + " " + entry.category + " " + entry.summary
                                   + " " + entry.description;
            for (const auto& tag : entry.tags) searchable += " " + tag;
            entry.searchKey = LowerCopy(std::move(searchable));
            entries.push_back(std::move(entry));
        }
        errorCode.clear();
    }
    // カテゴリ優先で並べる。同カテゴリ内は名前順。
    std::sort(entries.begin(), entries.end(),
        [](const GraphTemplateEntry& a, const GraphTemplateEntry& b) {
            if (a.category != b.category) return a.category < b.category;
            return a.name < b.name;
        });
}


std::vector<std::string> VFXTemplateCatalog::Categories() const
{
    std::vector<std::string> result;
    for (const auto& entry : entries)
        if (std::find(result.begin(), result.end(), entry.category) == result.end())
            result.push_back(entry.category);
    std::sort(result.begin(), result.end());
    return result;
}


std::vector<const GraphTemplateEntry*> VFXTemplateCatalog::Filter(const std::string& query) const
{
    const std::string needle = LowerCopy(query);
    std::vector<const GraphTemplateEntry*> result;
    for (const auto& entry : entries)
        if (needle.empty() || entry.searchKey.find(needle) != std::string::npos)
            result.push_back(&entry);
    return result;
}


const GraphTemplateEntry* VFXTemplateCatalog::Find(const std::string& nameOrPath) const
{
    const std::string needle = LowerCopy(nameOrPath);
    // パス指定を優先する。名前は重複しうるがパスは一意なため。
    for (const auto& entry : entries)
        if (LowerCopy(entry.path) == needle) return &entry;
    for (const auto& entry : entries) {
        const std::string qualified = entry.category.empty()
            ? entry.name : entry.category + "/" + entry.name;
        if (LowerCopy(qualified) == needle || LowerCopy(entry.name) == needle) return &entry;
    }
    return nullptr;
}


bool VFXTemplateCatalog::SaveAsTemplate(EditorContext& ctx, const asset::VFXGraphAsset& graph,
                                        const std::string& name, const std::string& category,
                                        const std::string& description,
                                        const std::vector<std::string>& tags)
{
    if (name.empty()) {
        status = "Template名を入力してください";
        return false;
    }
    // パス区切りは category で表現する。名前に混ぜると Find の解決と衝突する。
    if (name.find('/') != std::string::npos || name.find('\\') != std::string::npos) {
        status = "Template名にパス区切りは使えません (カテゴリ欄を使ってください)";
        return false;
    }
    // Templateの書き込み先はProjectのみ。Engine/SDK同梱ディレクトリは読み取り専用として扱う。
    if (ctx.projectRoot.empty()) {
        status = "Project rootが未設定のため保存できません";
        return false;
    }
    namespace fs = std::filesystem;
    fs::path directory = fs::path(ctx.projectRoot) / kTemplateRelativeDir;
    if (!category.empty()) directory /= category;
    std::error_code errorCode;
    fs::create_directories(directory, errorCode);
    if (errorCode) {
        status = "Templatesフォルダを作成できません: " + errorCode.message();
        return false;
    }
    const fs::path destination = directory / (name + ".vfx");
    asset::VFXGraphAsset outputGraph = graph;
    outputGraph.name = name;
    // 説明・タグは「説明」として持たせる。以前は graph.name へ押し込んでいたため、
    // AI は graphName として読めるのに Editor のカタログはファイル名しか出せなかった。
    outputGraph.description = description;
    outputGraph.tags = tags;
    std::string error;
    if (!asset::SaveVFXGraphAsset(destination.generic_string(), outputGraph, &error)) {
        status = error.empty() ? "Templateを保存できませんでした" : error;
        return false;
    }
    Scan(ctx, true);
    status = "保存しました: " + destination.generic_string();
    return true;
}

} // namespace fbzz::editor
