// FBZZ Engine
// AssetBrowserPanel.cpp | fbzz::editor
// Unity スタイルの2ペインアセットブラウザ
#include <Editor/Panels/AssetBrowserPanel.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/EditorTaskOverlay.hpp>
#include <Editor/Import/FbxImportTool.hpp>
#include <Editor/Util/ModalDialog.hpp>
#include <Editor/Util/AssetPath.hpp>
#include <Editor/Util/PrefabSerializer.hpp>
#include <Editor/Util/SceneIO.hpp>
#include <Editor/Util/ScriptCodeGen.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Engine/Util/StringUtils.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>

namespace fbzz::editor {

namespace {

std::string SanitizeEntityName(const std::string& name)
{
    std::string result = name.empty() ? "Prefab" : name;
    for (char& c : result) {
        const bool ok = std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == ' ';
        if (!ok) c = '_';
    }
    return result;
}

std::string UniquePrefabPathInDir(const std::string& dir, const std::string& objectName)
{
    util::FileSystem::EnsureDirectory(dir);
    const std::string base = dir + "/" + SanitizeEntityName(objectName);
    std::string path = base + ".fbzzprefab";
    for (int i = 1; util::FileSystem::Exists(path) && i < 10000; ++i)
        path = base + " " + std::to_string(i) + ".fbzzprefab";
    return path;
}

bool SaveHierarchyPayloadAsPrefab(const ImGuiPayload* payload,
                                  EditorContext& ctx,
                                  const std::string& targetDir)
{
    if (!payload || payload->DataSize != sizeof(scene::EntityID) || !ctx.activeScene)
        return false;

    scene::EntityID droppedId;
    std::memcpy(&droppedId, payload->Data, sizeof(droppedId));

    auto* go = ctx.activeScene->GetGameObject(droppedId);
    if (!go) return false;

    const std::string path = UniquePrefabPathInDir(targetDir, go->name);
    return PrefabSerializer::SaveSelection(*ctx.activeScene, { droppedId }, path);
}

ImVec4 Lighten(ImVec4 c) {
    return { std::min(c.x + 0.15f, 1.0f), std::min(c.y + 0.15f, 1.0f),
             std::min(c.z + 0.15f, 1.0f), c.w };
}

std::string NormalizePathSeparators(std::string path)
{
    for (char& c : path) {
        if (c == '\\') c = '/';
    }
    while (path.size() > 1 && path.back() == '/')
        path.pop_back();
    return path;
}

bool SamePathText(const std::string& a, const std::string& b)
{
    return util::StringUtils::ToLower(NormalizePathSeparators(a))
        == util::StringUtils::ToLower(NormalizePathSeparators(b));
}

bool IsChildPathText(const std::string& path, const std::string& root)
{
    const std::string normalizedPath = util::StringUtils::ToLower(NormalizePathSeparators(path));
    std::string normalizedRoot = util::StringUtils::ToLower(NormalizePathSeparators(root));
    if (normalizedRoot.empty()) return false;
    if (normalizedPath == normalizedRoot) return true;
    normalizedRoot += "/";
    return normalizedPath.rfind(normalizedRoot, 0) == 0;
}

std::string ToProjectAssetPath(const std::string& path, const EditorContext& ctx)
{
    // WHY: Asset Browser の内部パスは実ファイル操作のため絶対パスを保持するが、
    //      Scene / Prefab に保存する payload は配布後も壊れない Assets 起点の相対パスにする。
    (void)ctx;
    return NormalizeAssetPath(path);
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────

AssetBrowserPanel::AssetBrowserPanel(const std::string& rootPath)
    : m_rootPath(NormalizePathSeparators(rootPath)),
      m_currentPath(NormalizePathSeparators(rootPath)) {}

void AssetBrowserPanel::OnInit(EditorContext&)
{
    RefreshDirectory();
    if (!m_rootPath.empty()) {
        m_watcher.Start(m_rootPath);
        ScanAndQueueUnimported(m_rootPath);
    }
}

void AssetBrowserPanel::SetRootPath(const std::string& rootPath)
{
    if (rootPath.empty() || rootPath == m_rootPath) return;
    m_rootPath = NormalizePathSeparators(rootPath);
    m_currentPath = m_rootPath;
    m_pendingNavigate.clear();
    m_mounts.clear();
    m_pendingImports.clear();
    m_watcher.Start(m_rootPath);
    RefreshDirectory();
    ScanAndQueueUnimported(m_rootPath);
}

void AssetBrowserPanel::UpdateMounts(const EditorContext& ctx)
{
    std::vector<AssetMount> next;

    auto addMount = [&](const std::string& name, const std::string& path) {
        if (name.empty() || path.empty() || !util::FileSystem::IsDirectory(path)) return;

        const std::string normalizedPath = NormalizePathSeparators(path);
        const std::string rootChild = NormalizePathSeparators(m_rootPath + "/" + name);

        // WHY: プロジェクト Assets 側に実フォルダがある場合はそれを正とする。
        //      ただし空フォルダだけがあるケースでは、実体側 Scripts/HLSL が見えなくなるためマウントを許可する。
        if (util::FileSystem::IsDirectory(rootChild) &&
            !util::FileSystem::ListAll(rootChild).empty()) {
            return;
        }
        if (SamePathText(normalizedPath, m_rootPath) || IsChildPathText(m_rootPath, normalizedPath)) return;
        for (const AssetMount& mount : next) {
            if (SamePathText(mount.path, normalizedPath) || mount.name == name) return;
        }
        next.push_back({ name, normalizedPath });
    };

    // WHY: Scripts / shaders はプロジェクトテンプレート、エンジン内蔵 Assets、
    //      外部プロジェクト Assets のどこに置かれても編集対象として見える必要がある。
    const std::string scriptsDir = ctx.scriptsSourceDir.empty()
        ? ResolveFallbackAssetDir("Scripts")
        : ctx.scriptsSourceDir;
    const std::string hlslDir = ctx.hlslSourceDir.empty()
        ? ResolveFallbackAssetDir("Shaders")
        : ctx.hlslSourceDir;
    addMount("Scripts", scriptsDir);
    addMount("Shaders", hlslDir);

    const bool changed = next.size() != m_mounts.size()
        || !std::equal(next.begin(), next.end(), m_mounts.begin(),
            [](const AssetMount& a, const AssetMount& b) {
                return a.name == b.name && SamePathText(a.path, b.path);
            });
    if (!changed) return;

    m_mounts = std::move(next);

    if (!IsRootOrMountedPath(m_currentPath))
        m_currentPath = m_rootPath;
    RefreshDirectory();
}

void AssetBrowserPanel::RefreshDirectory()
{
    m_resetScroll = true;
    m_entries.clear();
    const std::string currentPath = NormalizePathSeparators(m_currentPath);
    for (const auto& p : util::FileSystem::ListAll(currentPath)) {
        Entry e;
        e.path  = NormalizePathSeparators(p);
        e.name  = util::FileSystem::GetFilename(p);
        e.ext   = util::StringUtils::ToLower(util::FileSystem::GetExtension(p));
        e.isDir = util::FileSystem::IsDirectory(p);
        m_entries.push_back(std::move(e));
    }

    if (SamePathText(currentPath, m_rootPath)) {
        for (const AssetMount& mount : m_mounts) {
            Entry e;
            e.path = mount.path;
            e.name = mount.name;
            e.isDir = true;
            e.isMount = true;
            m_entries.push_back(std::move(e));
        }
    }

    std::stable_sort(m_entries.begin(), m_entries.end(), [](const Entry& a, const Entry& b) {
        if (a.isDir != b.isDir) return a.isDir > b.isDir; // dirs first
        return a.name < b.name;
    });
}

std::string AssetBrowserPanel::DisplayPath() const
{
    const std::string currentPath = NormalizePathSeparators(m_currentPath);
    if (SamePathText(currentPath, m_rootPath)) return "Assets";

    const std::string rootPrefix = NormalizePathSeparators(m_rootPath) + "/";
    if (currentPath.rfind(rootPrefix, 0) == 0)
        return "Assets/" + currentPath.substr(rootPrefix.size());

    for (const AssetMount& mount : m_mounts) {
        if (!IsChildPathText(currentPath, mount.path)) continue;
        if (SamePathText(currentPath, mount.path)) return "Assets/" + mount.name;
        return "Assets/" + mount.name + "/" + currentPath.substr(mount.path.size() + 1);
    }

    return currentPath;
}

std::string AssetBrowserPanel::ParentPath() const
{
    const std::string currentPath = NormalizePathSeparators(m_currentPath);
    if (SamePathText(currentPath, m_rootPath)) return m_rootPath;
    if (IsMountedRoot(currentPath)) return m_rootPath;

    const size_t pos = currentPath.find_last_of('/');
    if (pos == std::string::npos) return m_rootPath;
    const std::string parent = currentPath.substr(0, pos);
    for (const AssetMount& mount : m_mounts) {
        if (IsChildPathText(currentPath, mount.path) && !IsChildPathText(parent, mount.path))
            return m_rootPath;
    }
    return parent;
}

std::string AssetBrowserPanel::ResolveFallbackAssetDir(const std::string& childDirName) const
{
    if (childDirName.empty()) return {};

    std::filesystem::path current(NormalizePathSeparators(m_rootPath));
    std::error_code ec;

    // WHY: ctx.scriptsSourceDir / ctx.hlslSourceDir は hot reload の ToolchainLocator 成功後にだけ入る。
    //      AssetBrowser は hot reload なしでも使うため、現在の Assets ルートから親をたどって
    //      リポジトリ側 Assets/Scripts や Assets/Shaders を見つける。
    for (int depth = 0; depth < 8 && !current.empty(); ++depth) {
        const std::filesystem::path candidate = current / "Assets" / childDirName;
        if (std::filesystem::is_directory(candidate, ec))
            return NormalizePathSeparators(candidate.string());

        if (current.filename() == "Assets") {
            const std::filesystem::path sibling = current.parent_path() / "Assets" / childDirName;
            if (std::filesystem::is_directory(sibling, ec))
                return NormalizePathSeparators(sibling.string());
        }

        current = current.parent_path();
    }

    return {};
}

bool AssetBrowserPanel::IsMountedRoot(const std::string& path) const
{
    for (const AssetMount& mount : m_mounts) {
        if (SamePathText(path, mount.path)) return true;
    }
    return false;
}

bool AssetBrowserPanel::IsRootOrMountedPath(const std::string& path) const
{
    if (IsChildPathText(path, m_rootPath)) return true;
    for (const AssetMount& mount : m_mounts) {
        if (IsChildPathText(path, mount.path)) return true;
    }
    return false;
}

// 既知の拡張子グループに対応する固定色テーブル。
// WHY: 同じカテゴリのファイルは同色にすることで、ディレクトリ内を一瞥したとき
//      テクスチャ・シェーダー・メッシュ等の割合を直感的に把握できる。
//      未知の拡張子は拡張子文字列のハッシュから色を生成し、
//      追加のコード変更なしにどんなファイルでも識別色が付く。
struct ExtGroup {
    const char*  exts[6];   // 最大 6 拡張子。nullptr 終番。
    ImVec4       color;
    const char*  label;
};

static constexpr ExtGroup kExtGroups[] = {
    { { ".hlsl", ".hlsli", nullptr },                          { 0.15f, 0.65f, 0.25f, 1.0f }, "HLSL"    },
    { { ".hpp", ".cpp", ".h", ".c", ".cc", ".cxx" },            { 0.20f, 0.58f, 0.70f, 1.0f }, "CPP"     },
    { { ".png", ".jpg", ".jpeg", ".dds", ".bmp", ".tga" },     { 0.15f, 0.40f, 0.80f, 1.0f }, "TEX"     },
    { { ".fbx", ".obj", ".gltf", ".glb", nullptr },            { 0.80f, 0.45f, 0.10f, 1.0f }, "MESH"    },
    { { ".fbzzprefab", nullptr },                              { 0.25f, 0.65f, 0.75f, 1.0f }, "PREFAB"  },
    { { ".fbzzterrain", nullptr },                             { 0.35f, 0.70f, 0.30f, 1.0f }, "TERRAIN" },
    { { ".fbzz", nullptr },                                    { 0.60f, 0.15f, 0.70f, 1.0f }, "SCENE"   },
    { { ".fzasset", nullptr },                                 { 0.90f, 0.60f, 0.10f, 1.0f }, "ASSET"   },
    { { ".fzmesh", nullptr },                                  { 0.80f, 0.50f, 0.20f, 1.0f }, "MESH"    },
    { { ".fzmat", nullptr },                                   { 0.20f, 0.70f, 0.80f, 1.0f }, "MAT"     },
    { { ".fzskel", nullptr },                                  { 0.70f, 0.30f, 0.60f, 1.0f }, "SKEL"    },
    { { ".fzanim", nullptr },                                  { 0.20f, 0.75f, 0.35f, 1.0f }, "ANIM"    },
    { { ".toml", ".json", ".yaml", ".yml", nullptr },           { 0.65f, 0.65f, 0.10f, 1.0f }, "DATA"    },
    { { ".wav", ".mp3", ".ogg", ".flac", nullptr },             { 0.70f, 0.20f, 0.50f, 1.0f }, "SFX"     },
    { { ".ttf", ".otf", nullptr },                             { 0.60f, 0.30f, 0.85f, 1.0f }, "FONT"    },
    { { ".fnt", nullptr },                                     { 0.50f, 0.20f, 0.75f, 1.0f }, "FNT"     },
    { { ".txt", ".md", ".rst", nullptr },                      { 0.55f, 0.55f, 0.55f, 1.0f }, "TEXT"    },
    { { ".py", ".lua", ".cs", nullptr },                       { 0.20f, 0.70f, 0.55f, 1.0f }, "SCRIPT"  },
    { { ".lib", ".dll", ".a", nullptr },                       { 0.45f, 0.45f, 0.45f, 1.0f }, "LIB"     },
};

// 未知拡張子をハッシュで色付けする。
// WHAT: FNV-1a の下位ビットを色相に変換し、彩度・明度は固定で
//       読みやすい明るさに調整する。同じ拡張子なら常に同じ色になる。
static ImVec4 ColorFromExt(const std::string& ext)
{
    uint32_t h = 2166136261u;
    for (unsigned char c : ext)
        h = (h ^ c) * 16777619u;
    const float hue = static_cast<float>(h & 0xFFFF) / 65536.0f; // 0..1
    // HSV → RGB (S=0.55, V=0.72)
    const float s = 0.55f, v = 0.72f;
    const float hi = std::fmodf(hue * 6.0f, 6.0f);
    const int   i  = static_cast<int>(hi);
    const float f  = hi - static_cast<float>(i);
    const float p  = v * (1.0f - s);
    const float q  = v * (1.0f - s * f);
    const float t  = v * (1.0f - s * (1.0f - f));
    switch (i % 6) {
    case 0: return { v, t, p, 1.0f };
    case 1: return { q, v, p, 1.0f };
    case 2: return { p, v, t, 1.0f };
    case 3: return { p, q, v, 1.0f };
    case 4: return { t, p, v, 1.0f };
    default:return { v, p, q, 1.0f };
    }
}

// 拡張子がグループに含まれるか確認する。
static const ExtGroup* FindGroup(const std::string& ext)
{
    for (const auto& g : kExtGroups) {
        for (int i = 0; i < 6 && g.exts[i]; ++i)
            if (ext == g.exts[i]) return &g;
    }
    return nullptr;
}

ImVec4 AssetBrowserPanel::EntryColor(const Entry& e)
{
    if (e.isDir) return { 0.80f, 0.60f, 0.10f, 1.0f };
    if (const ExtGroup* g = FindGroup(e.ext)) return g->color;
    if (e.ext.empty()) return { 0.38f, 0.38f, 0.38f, 1.0f };
    // 未知拡張子: ハッシュで自動着色
    return ColorFromExt(e.ext);
}

const char* AssetBrowserPanel::EntryLabel(const Entry& e)
{
    if (e.isDir) return "DIR";
    if (const ExtGroup* g = FindGroup(e.ext)) return g->label;
    // 未知拡張子: 拡張子文字列をそのままラベルに使う (最大 6 文字、先頭の . を除く)
    // WHY: 静的バッファに詰めることでどんな拡張子でもラベル表示できる。
    //      ImGui はフレーム内で文字列を参照するため static thread_local を使う。
    static thread_local char buf[8];
    const char* src = e.ext.size() > 1 ? e.ext.c_str() + 1 : e.ext.c_str(); // skip '.'
    std::size_t len = 0;
    while (src[len] && len < 6) {
        buf[len] = static_cast<char>(std::toupper(static_cast<unsigned char>(src[len])));
        ++len;
    }
    buf[len] = '\0';
    return len > 0 ? buf : "FILE";
}

// ─── フォルダツリー (左ペイン) ───────────────────────────────────────────────

void AssetBrowserPanel::DrawFolderTree(const std::string& dirPath, EditorContext& ctx)
{
    std::vector<Entry> dirs;
    for (const auto& p : util::FileSystem::ListAll(dirPath)) {
        if (!util::FileSystem::IsDirectory(p)) continue;
        Entry e;
        e.path = NormalizePathSeparators(p);
        e.name = util::FileSystem::GetFilename(p);
        e.isDir = true;
        dirs.push_back(std::move(e));
    }
    if (SamePathText(dirPath, m_rootPath)) {
        for (const AssetMount& mount : m_mounts) {
            Entry e;
            e.path = mount.path;
            e.name = mount.name;
            e.isDir = true;
            e.isMount = true;
            dirs.push_back(std::move(e));
        }
    }
    std::stable_sort(dirs.begin(), dirs.end(), [](const Entry& a, const Entry& b) {
        return a.name < b.name;
    });

    for (const Entry& dir : dirs) {
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow
                                 | ImGuiTreeNodeFlags_SpanAvailWidth;
        if (SamePathText(dir.path, m_currentPath)) flags |= ImGuiTreeNodeFlags_Selected;

        // WHY: 表示名は Assets 側の仮想名、ID は実パスにすることで同名マウントでも ImGui ID が衝突しない。
        bool open = ImGui::TreeNodeEx(dir.path.c_str(), flags, "%s", dir.name.c_str());
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen()) {
            m_currentPath = dir.path;
            ctx.selectedAssetPath.clear();
            RefreshDirectory();
        }
        // ヒエラルキーエンティティをフォルダノードにドロップ → そのフォルダへ Prefab 保存
        if (ImGui::BeginDragDropTarget()) {
            if (SaveHierarchyPayloadAsPrefab(
                    ImGui::AcceptDragDropPayload("FBZZ_HIERARCHY_ENTITY"), ctx, dir.path)) {
                RefreshDirectory();
            }
            ImGui::EndDragDropTarget();
        }
        if (open) {
            DrawFolderTree(dir.path, ctx);
            ImGui::TreePop();
        }
    }
}

// ─── アイコン1個 (右ペイン) ──────────────────────────────────────────────────

void AssetBrowserPanel::DrawEntry(const Entry& e, EditorContext& ctx)
{
    ImGui::PushID(e.path.c_str());

    const ImVec4 base    = EntryColor(e);
    const ImU32  cFill   = ImGui::ColorConvertFloat4ToU32(base);
    const ImU32  cHov    = ImGui::ColorConvertFloat4ToU32(Lighten(base));
    const ImU32  cDark   = ImGui::ColorConvertFloat4ToU32(
        { base.x * 0.50f, base.y * 0.50f, base.z * 0.50f, 1.0f });

    const ImVec2 origin  = ImGui::GetCursorScreenPos();
    const float  sz      = m_iconSize;

    ImGui::InvisibleButton("##icon", { sz, sz });
    const bool hov = ImGui::IsItemHovered();
    const ImU32 fill = hov ? cHov : cFill;

    ImDrawList* dl = ImGui::GetWindowDrawList();

    if (e.isDir) {
        // フォルダ形状: タブ (左上) + 本体
        const float tabW  = sz * 0.48f;
        const float tabH  = sz * 0.14f;
        const float bodyY = origin.y + tabH;
        const float bodyH = sz * 0.82f;
        const float r     = sz * 0.07f;

        dl->AddRectFilled({ origin.x, origin.y }, { origin.x + tabW, bodyY + r }, fill, r);
        dl->AddRectFilled({ origin.x, bodyY },    { origin.x + sz,   origin.y + bodyH }, fill, r);
        dl->AddRect(      { origin.x, bodyY },    { origin.x + sz,   origin.y + bodyH }, cDark, r, 0, 1.0f);
    } else {
        // ファイル形状: 右上に折り角
        const float bodyH = sz * 0.85f;
        const float dog   = sz * 0.22f;

        ImVec2 pts[5] = {
            { origin.x,        origin.y       },
            { origin.x+sz-dog, origin.y       },
            { origin.x+sz,     origin.y+dog   },
            { origin.x+sz,     origin.y+bodyH },
            { origin.x,        origin.y+bodyH },
        };
        dl->AddConvexPolyFilled(pts, 5, fill);
        dl->AddPolyline(pts, 5, cDark, ImDrawFlags_Closed, 1.0f);

        // 折り角の三角形 (影)
        ImVec2 tri[3] = {
            { origin.x+sz-dog, origin.y     },
            { origin.x+sz,     origin.y+dog },
            { origin.x+sz-dog, origin.y+dog },
        };
        dl->AddConvexPolyFilled(tri, 3, cDark);

        // 拡張子ラベルをアイコン中央に表示
        const char* lbl  = EntryLabel(e);
        const ImVec2 tsz = ImGui::CalcTextSize(lbl);
        dl->AddText({ origin.x + (sz - tsz.x) * 0.5f,
                      origin.y + bodyH * 0.52f - tsz.y * 0.5f },
                    IM_COL32(255, 255, 255, 220), lbl);
    }

    // 選択ハイライト
    const bool isSelected = !e.isDir && (e.path == ctx.selectedAssetPath);
    if (isSelected) {
        ImGui::GetWindowDrawList()->AddRect(
            origin, { origin.x + sz, origin.y + sz * 0.85f },
            IM_COL32(255, 200, 80, 220), 3.0f, 0, 2.0f);
    }

    // ! バッジ: 未変換ファイル (FBX / PNG 等) に赤丸で警告表示
    if (!e.isDir && IsImportableRaw(e.ext))
    {
        const float r  = sz * 0.15f;
        const float cx = origin.x + sz - r;
        const float cy = origin.y + r;
        dl->AddCircleFilled({ cx, cy }, r, IM_COL32(220, 50, 50, 230));
        const ImVec2 bsz = ImGui::CalcTextSize("!");
        dl->AddText({ cx - bsz.x * 0.5f, cy - bsz.y * 0.5f },
                    IM_COL32(255, 255, 255, 255), "!");
    }

    // ドラッグソース (ファイルのみ)
    if (!e.isDir && ImGui::BeginDragDropSource()) {
        const std::string payloadPath = ToProjectAssetPath(e.path, ctx);
        ImGui::SetDragDropPayload("ASSET_PATH", payloadPath.c_str(), payloadPath.size() + 1);
        ImGui::TextUnformatted(e.name.c_str());
        ImGui::EndDragDropSource();
    }
    // ドロップターゲット (ディレクトリのみ)
    // WHY: コンテンツエリア全体ではなく特定サブフォルダへ直接ドロップして保存先を選べるようにする。
    if (e.isDir && ImGui::BeginDragDropTarget()) {
        if (SaveHierarchyPayloadAsPrefab(
                ImGui::AcceptDragDropPayload("FBZZ_HIERARCHY_ENTITY"), ctx, e.path)) {
            RefreshDirectory();
        }
        ImGui::EndDragDropTarget();
    }

    if (hov && m_renamingPath != e.path) {
        if (e.isMount)
            ImGui::SetTooltip("%s\n\nExternal source folder mounted under Assets", e.path.c_str());
        else if (e.ext == ".fnt")
            ImGui::SetTooltip("%s\n\nFont atlas metadata\nDrag and drop onto UI Text Font Path to assign it", e.path.c_str());
        else if (e.ext == ".ttf" || e.ext == ".otf")
            ImGui::SetTooltip("%s\n\nTTF font\nConvert it to a PNG + FNT atlas with gen_font_atlas.py before use", e.path.c_str());
        else
            ImGui::SetTooltip("%s", e.path.c_str());
    }

    // 右クリックコンテキストメニュー (リネーム / 削除)
    if (ImGui::BeginPopupContextItem("##entry_ctx")) {
        // 未変換ファイルは "Import" を最上位に表示する
        if (!e.isDir && IsImportableRaw(e.ext)) {
            if (ImGui::MenuItem("Import")) {
                bool found = false;
                for (const auto& p : m_pendingImports)
                    if (p.path == e.path) { found = true; break; }
                if (!found)
                    m_pendingImports.push_back({ e.path, PendingImport::Kind::Fbx });
                m_importAllRequested = true;
            }
            ImGui::Separator();
        }
        ImGui::BeginDisabled(e.isMount);
        if (ImGui::MenuItem("Rename")) {
            m_renamingPath    = e.path;
            const std::string stem = util::FileSystem::GetFilename(e.path);
            std::strncpy(m_renameBuffer, stem.c_str(), sizeof(m_renameBuffer) - 1);
            m_renameBuffer[sizeof(m_renameBuffer) - 1] = '\0';
            m_renameNeedFocus = true;
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Delete")) {
            const std::string path = e.path;
            ModalDialog::OpenConfirm("Delete",
                "Delete \"" + util::FileSystem::GetFilename(path) + "\"?",
                [this, path]() {
                    std::error_code ec;
                    std::filesystem::remove_all(
                        std::filesystem::path(path.begin(), path.end()), ec);
                    if (ec) {
                        FBZZ_LOG_ERROR("Delete failed: %s", path.c_str());
                    } else {
                        if (m_selectedFbxPath == path) {
                            m_selectedFbxPath.clear();
                            m_selectedModel.reset();
                        }
                        RefreshDirectory();
                    }
                });
        }
        ImGui::EndDisabled();
        ImGui::Separator();
        if (ImGui::BeginMenu("Create")) {
            DrawCreateMenu(ctx);
            ImGui::EndMenu();
        }
        ImGui::EndPopup();
    }

    if (hov && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (!e.isDir) {
            ctx.selectedAssetPath = e.path;
        }
        const bool isMesh = (e.ext == ".fbx" || e.ext == ".obj" ||
                             e.ext == ".gltf" || e.ext == ".glb");
        if (!e.isDir && isMesh) {
            // FBX をシングルクリック → 内容を非同期ロード
            if (m_selectedFbxPath != e.path) {
                m_selectedFbxPath = e.path;
                m_selectedModel.reset();
                if (auto* res = renderer::ResourceManager::Active())
                    m_selectedModel = asset::AssetManager::Load<asset::Model>(e.path);
            }
        }
    }

    if (hov && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        if (e.isDir) {
            m_pendingNavigate = e.path;
            ctx.selectedAssetPath.clear();
        } else if (e.ext == ".fbzz" && ctx.activeScene) {
            if (ctx.requestOpenScene) {
                ctx.requestOpenScene(e.path);
            } else if (SceneIO::Load(*ctx.activeScene, e.path)) {
                ctx.selectedEntities.clear();
                if (ctx.markSceneDirty) ctx.markSceneDirty();
                FBZZ_LOG_INFO("Opened scene: %s", e.path.c_str());
            } else {
                FBZZ_LOG_ERROR("Failed to open scene: %s", e.path.c_str());
            }
        } else if (e.ext == ".fbzzprefab" && ctx.activeScene) {
            std::vector<scene::EntityID> roots;
            if (PrefabSerializer::Instantiate(*ctx.activeScene, e.path, roots)) {
                ctx.selectedEntities = roots;
                if (ctx.markSceneDirty) ctx.markSceneDirty();
            }
        }
    }

    // ファイル名 (リネーム中は InputText、通常は省略ラベル)
    if (m_renamingPath == e.path) {
        ImGui::SetNextItemWidth(m_iconSize);
        if (m_renameNeedFocus) {
            ImGui::SetKeyboardFocusHere();
            m_renameNeedFocus = false;
        }
        constexpr ImGuiInputTextFlags renameFlags =
            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll;
        const bool enterPressed = ImGui::InputText("##rename", m_renameBuffer,
                                                   sizeof(m_renameBuffer), renameFlags);
        if (enterPressed) {
            if (m_renameBuffer[0] != '\0') {
                const std::string dir     = util::FileSystem::GetDirectory(e.path);
                const std::string newPath = dir + m_renameBuffer;
                if (newPath != e.path) {
                    std::error_code ec;
                    std::filesystem::rename(
                        std::filesystem::path(e.path.begin(), e.path.end()),
                        std::filesystem::path(newPath.begin(), newPath.end()), ec);
                    if (ec) {
                        FBZZ_LOG_ERROR("Rename failed: %s -> %s", e.path.c_str(), newPath.c_str());
                    } else {
                        if (m_selectedFbxPath == e.path) m_selectedFbxPath = newPath;
                        RefreshDirectory();
                    }
                }
            }
            m_renamingPath.clear();
        } else if (ImGui::IsItemDeactivated()) {
            // Escape またはフォーカス外れ → キャンセル
            m_renamingPath.clear();
        }
    } else {
        std::string display = e.name;
        while (display.size() > 2 &&
               ImGui::CalcTextSize(display.c_str()).x + ImGui::CalcTextSize("..").x > m_iconSize)
            display.pop_back();
        if (display.size() < e.name.size()) display += "..";

        float indent = (m_iconSize - ImGui::CalcTextSize(display.c_str()).x) * 0.5f;
        if (indent > 0.0f) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + indent);
        ImGui::TextUnformatted(display.c_str());

        // F2 でリネーム開始 (ホバー中)
        if (!e.isMount && hov && ImGui::IsKeyPressed(ImGuiKey_F2)) {
            m_renamingPath    = e.path;
            const std::string stem = e.name;
            std::strncpy(m_renameBuffer, stem.c_str(), sizeof(m_renameBuffer) - 1);
            m_renameBuffer[sizeof(m_renameBuffer) - 1] = '\0';
            m_renameNeedFocus = true;
        }
    }

    ImGui::PopID();
}

// ─── FBX 内容プレビュー (サブアセットアイコン) ────────────────────────────────

void AssetBrowserPanel::DrawFbxContents(EditorContext& ctx)
{
    if (m_selectedFbxPath.empty()) return;

    ImGui::Separator();
    ImGui::TextDisabled("  %s", util::FileSystem::GetFilename(m_selectedFbxPath).c_str());
    ImGui::Spacing();

    if (!m_selectedModel) {
        ImGui::TextColored({ 1.0f, 0.4f, 0.4f, 1.0f }, "Failed to load");
        return;
    }

    const auto& model = *m_selectedModel;
    const bool hasMesh = !model.meshes.empty();
    const bool hasClip = !model.clips.empty();
    if (!hasMesh && !hasClip) {
        ImGui::TextDisabled("  (no meshes or clips)");
        return;
    }

    // アイコン描画ラムダ (DrawEntry と同じスタイル)
    const float padding = 8.0f;
    const float avail   = ImGui::GetContentRegionAvail().x;
    const int   cols    = std::max(1, (int)(avail / (m_iconSize + padding)));
    int col     = 0;
    int iconIdx = 0;

    auto drawSubIcon = [&](const char* label, const char* displayName,
                           const char* tooltip, ImVec4 color,
                           const std::string& payload,
                           EditorContext& ctx)
    {
        if (col > 0 && (col % cols) != 0) ImGui::SameLine(0.0f, padding);

        char uid[64];
        std::snprintf(uid, sizeof(uid), "##sub%d", iconIdx++);

        ImGui::BeginGroup();
        ImGui::PushID(uid);

        const ImU32 cFill = ImGui::ColorConvertFloat4ToU32(color);
        const ImU32 cHov  = ImGui::ColorConvertFloat4ToU32(Lighten(color));
        const ImU32 cDark = ImGui::ColorConvertFloat4ToU32(
            { color.x * 0.5f, color.y * 0.5f, color.z * 0.5f, 1.0f });

        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const float  sz     = m_iconSize;

        ImGui::InvisibleButton("##icon", { sz, sz });
        const bool   hov  = ImGui::IsItemHovered();
        const ImU32  fill = hov ? cHov : cFill;

        ImDrawList* dl    = ImGui::GetWindowDrawList();
        const float bodyH = sz * 0.85f;
        const float dog   = sz * 0.22f;

        ImVec2 pts[5] = {
            { origin.x,        origin.y       },
            { origin.x+sz-dog, origin.y       },
            { origin.x+sz,     origin.y+dog   },
            { origin.x+sz,     origin.y+bodyH },
            { origin.x,        origin.y+bodyH },
        };
        dl->AddConvexPolyFilled(pts, 5, fill);
        dl->AddPolyline(pts, 5, cDark, ImDrawFlags_Closed, 1.0f);

        ImVec2 tri[3] = {
            { origin.x+sz-dog, origin.y     },
            { origin.x+sz,     origin.y+dog },
            { origin.x+sz-dog, origin.y+dog },
        };
        dl->AddConvexPolyFilled(tri, 3, cDark);

        const ImVec2 tsz = ImGui::CalcTextSize(label);
        dl->AddText({ origin.x + (sz - tsz.x) * 0.5f,
                      origin.y + bodyH * 0.52f - tsz.y * 0.5f },
                    IM_COL32(255, 255, 255, 220), label);

        if (ImGui::BeginDragDropSource()) {
            const std::string payloadPath = ToProjectAssetPath(payload, ctx);
            ImGui::SetDragDropPayload("ASSET_PATH", payloadPath.c_str(), payloadPath.size() + 1);
            ImGui::Text("%s: %s", label,
                util::FileSystem::GetFilename(payloadPath).c_str());
            ImGui::EndDragDropSource();
        }

        if (hov) ImGui::SetTooltip("%s", tooltip);

        // 名前テキスト (省略、中央揃え)
        std::string disp(displayName);
        const std::string full(displayName);
        while (disp.size() > 2 &&
               ImGui::CalcTextSize(disp.c_str()).x + ImGui::CalcTextSize("..").x > sz)
            disp.pop_back();
        if (disp.size() < full.size()) disp += "..";

        const float ind = (sz - ImGui::CalcTextSize(disp.c_str()).x) * 0.5f;
        if (ind > 0.0f) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ind);
        ImGui::TextUnformatted(disp.c_str());

        ImGui::PopID();
        ImGui::EndGroup();
        ++col;
    };

    // ── MESH アイコン ──
    if (hasMesh) {
        char tip[256];
        std::snprintf(tip, sizeof(tip), "%zu mesh(es)%s\nDrag → Skinned Mesh Renderer",
            model.meshes.size(),
            model.skeleton
                ? (std::string(" + skeleton (") +
                   std::to_string(model.skeleton->bones.size()) + " bones)").c_str()
                : "");
        drawSubIcon("MESH", "Mesh", tip, { 0.80f, 0.45f, 0.10f, 1.0f }, m_selectedFbxPath, ctx);
    }

    // ── ANIM アイコン (クリップ1件につき1個) ──
    for (const auto& clip : model.clips) {
        const float dur = static_cast<float>(clip.durationTicks /
            (clip.ticksPerSecond > 0.0 ? clip.ticksPerSecond : 30.0));
        char tip[256];
        std::snprintf(tip, sizeof(tip), "%s  (%.2fs)\nDrag → Animator",
            clip.name.c_str(), dur);
        drawSubIcon("ANIM", clip.name.c_str(), tip,
                    { 0.20f, 0.70f, 0.30f, 1.0f }, m_selectedFbxPath, ctx);
    }
}

void AssetBrowserPanel::DrawCreateMenu(EditorContext& ctx)
{
    if (ImGui::MenuItem("Folder")) {
        std::string newDir = m_currentPath + "/New Folder";
        // 重複回避
        int suffix = 1;
        while (util::FileSystem::Exists(newDir))
            newDir = m_currentPath + "/New Folder " + std::to_string(suffix++);
        util::FileSystem::EnsureDirectory(newDir);
        RefreshDirectory();
        // 新規フォルダをリネームモードで開く
        m_renamingPath = newDir;
        std::strncpy(m_renameBuffer, util::FileSystem::GetFilename(newDir).c_str(),
                     sizeof(m_renameBuffer) - 1);
        m_renameBuffer[sizeof(m_renameBuffer) - 1] = '\0';
        m_renameNeedFocus = true;
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Scene")) {
        std::string newPath = m_currentPath + "/New Scene.fbzz";
        int suffix = 1;
        while (util::FileSystem::Exists(newPath))
            newPath = m_currentPath + "/New Scene " + std::to_string(suffix++) + ".fbzz";
        util::FileSystem::WriteText(newPath, "# FBZZ Scene\n");
        RefreshDirectory();
    }
    if (ImGui::MenuItem("Material")) {
        std::string newPath = m_currentPath + "/New Material.fzmat";
        int suffix = 1;
        while (util::FileSystem::Exists(newPath))
            newPath = m_currentPath + "/New Material " + std::to_string(suffix++) + ".fzmat";
        const char* materialTemplate =
            "version = 1\n"
            "shader = \"\"\n"
            "blend_mode = \"Opaque\"\n"
            "double_sided = false\n"
            "render_queue = 2000\n"
            "\n"
            "[textures]\n"
            "albedo = \"\"\n"
            "normal = \"\"\n"
            "metallic = \"\"\n"
            "roughness = \"\"\n"
            "ao = \"\"\n"
            "emissive = \"\"\n"
            "\n"
            "[params]\n"
            "base_color = [1.0, 1.0, 1.0, 1.0]\n"
            "metallic_factor = 0.0\n"
            "roughness_factor = 0.65\n"
            "normal_strength = 1.0\n"
            "emissive_color = [1.0, 1.0, 1.0]\n"
            "emissive_scale = 0.0\n";
        util::FileSystem::WriteText(newPath, materialTemplate);
        RefreshDirectory();
    }

    // ── C++ スクリプト ────────────────────────────────────────────
    ImGui::Separator();
    if (ImGui::MenuItem("C++ Script...")) {
        const std::string engineScriptsDir = ctx.scriptsSourceDir;
        const std::string projScriptsDir   = ctx.projectRoot + "/Assets/Scripts";
        const std::string dllPath          = ctx.scriptsDllCppPath;
        const std::string staticPath       = ctx.scriptsStaticCppPath;
        // パス未解決のときはフォールバック: プロジェクト側の Scripts/ に直接作成する
        // WHY: ToolchainLocator が失敗した環境 (build.config なし) でも
        //      DLL 登録なしでヘッダだけを生成できるようにする。
        const std::string resolvedScriptsDir =
            engineScriptsDir.empty() ? projScriptsDir : engineScriptsDir;
        ModalDialog::OpenInput(
            "New C++ Script",
            "NewScript",
            [this, resolvedScriptsDir, projScriptsDir, dllPath, staticPath]
            (const std::string& name) {
                const std::string path =
                    ScriptCodeGen::CreateScript(name, resolvedScriptsDir, dllPath, staticPath);
                if (path.empty()) {
                    FBZZ_LOG_WARN("C++ Script creation failed: %s", name.c_str());
                    return;
                }
                // プロジェクト Assets/Scripts/ にも即コピー (AssetBrowser に即反映)
                if (!projScriptsDir.empty() && projScriptsDir != resolvedScriptsDir)
                    ScriptCodeGen::CreateScript(name, projScriptsDir, "", "");
                m_pendingNavigate = projScriptsDir;
                RefreshDirectory();
                FBZZ_LOG_INFO("C++ Script generated: %s", path.c_str());
            },
            "Destination: " + (resolvedScriptsDir.empty() ? projScriptsDir : resolvedScriptsDir));
    }

    // ── HLSL シェーダー ───────────────────────────────────────────
    if (ImGui::BeginMenu("HLSL Shader...")) {
        const std::string engineHlslDir = ctx.hlslSourceDir;
        const std::string projHlslDir   = ctx.projectRoot + "/Assets/shaders";
        // パス未解決のときはプロジェクト側の shaders/ を直接使う
        const std::string resolvedHlslDir =
            engineHlslDir.empty() ? projHlslDir : engineHlslDir;

        auto makeHlslCallback = [this, resolvedHlslDir, projHlslDir]
            (ScriptCodeGen::HlslKind kind) {
            return [this, resolvedHlslDir, projHlslDir, kind]
                (const std::string& name) {
                const std::string path =
                    ScriptCodeGen::CreateHlsl(name, resolvedHlslDir, kind);
                if (path.empty()) {
                    FBZZ_LOG_WARN("HLSL creation failed: %s", name.c_str());
                    return;
                }
                // プロジェクト側にも即コピー
                if (!projHlslDir.empty() && projHlslDir != resolvedHlslDir)
                    ScriptCodeGen::CreateHlsl(name, projHlslDir, kind);
                const std::string destDir =
                    (kind == ScriptCodeGen::HlslKind::SurfaceVSPS)
                        ? (projHlslDir + "/Material/Custom")
                        : (projHlslDir + "/PostProcess/Custom");
                m_pendingNavigate = destDir;
                RefreshDirectory();
                FBZZ_LOG_INFO("HLSL generated: %s", path.c_str());
            };
        };

        if (ImGui::MenuItem("Surface Shader (VS+PS)")) {
            ModalDialog::OpenInput("New Surface Shader", "MyMaterial",
                makeHlslCallback(ScriptCodeGen::HlslKind::SurfaceVSPS),
                "Generated under Material/Custom/");
        }
        if (ImGui::MenuItem("PostProcess Shader (VS+PS)")) {
            ModalDialog::OpenInput("New PostProcess Shader", "MyPostEffect",
                makeHlslCallback(ScriptCodeGen::HlslKind::PostProcessVSPS),
                "Generated under PostProcess/Custom/");
        }
        if (ImGui::MenuItem("Compute Shader (CS)")) {
            ModalDialog::OpenInput("New Compute Shader", "MyCompute",
                makeHlslCallback(ScriptCodeGen::HlslKind::ComputeCS),
                "Generated under PostProcess/Custom/");
        }
        ImGui::EndMenu();
    }
}

// ─── メインレイアウト ─────────────────────────────────────────────────────────

// ─── ファイル監視ヘルパー ─────────────────────────────────────────────────────

bool AssetBrowserPanel::IsImportableRaw(const std::string& ext)
{
    return ext == ".fbx" || ext == ".obj" || ext == ".gltf" || ext == ".glb";
}

bool AssetBrowserPanel::IsAlreadyImported(const std::string& absPath)
{
    namespace fs = std::filesystem;
    const fs::path p(absPath.begin(), absPath.end());
    const std::string stem = p.stem().string();
    const fs::path check = p.parent_path() / stem / (stem + ".fzasset");
    std::error_code ec;
    return fs::exists(check, ec);
}

void AssetBrowserPanel::TryQueuePendingImport(const std::string& relPath)
{
    const std::string ext = util::StringUtils::ToLower(
        util::FileSystem::GetExtension(relPath));
    if (!IsImportableRaw(ext)) return;

    // パス結合 (m_rootPath が trailing slash を持つかどうかに依らず正しく結合)
    namespace fs = std::filesystem;
    const std::string absPath = (fs::path(m_rootPath.begin(), m_rootPath.end())
                                 / fs::path(relPath.begin(), relPath.end())).string();

    // 重複チェック
    for (const auto& p : m_pendingImports)
        if (p.path == absPath) return;

    if (IsAlreadyImported(absPath)) return;

    m_pendingImports.push_back({ absPath, PendingImport::Kind::Fbx });
    m_importAllRequested = true;
}

void AssetBrowserPanel::ScanAndQueueUnimported(const std::string& dirAbsPath)
{
    FBZZ_LOG_INFO("AssetBrowserPanel: scanning for unimported assets in [%s]",
                  dirAbsPath.c_str());
    namespace fs = std::filesystem;
    std::error_code ec;
    for (const auto& entry : fs::recursive_directory_iterator(
             fs::path(dirAbsPath.begin(), dirAbsPath.end()),
             fs::directory_options::skip_permission_denied, ec))
    {
        if (!entry.is_regular_file(ec)) continue;

        const std::string absPath = entry.path().string();
        const std::string ext = util::StringUtils::ToLower(
            util::FileSystem::GetExtension(absPath));
        if (!IsImportableRaw(ext)) continue;

        // 重複チェック
        bool found = false;
        for (const auto& p : m_pendingImports)
            if (p.path == absPath) { found = true; break; }
        if (found) continue;

        if (IsAlreadyImported(absPath)) continue;

        m_pendingImports.push_back({ absPath, PendingImport::Kind::Fbx });
    }

    FBZZ_LOG_INFO("AssetBrowserPanel: scan complete — %zu file(s) queued for import",
                  m_pendingImports.size());
    if (!m_pendingImports.empty())
        m_importAllRequested = true;
}

// ─── インポートバッジバー ─────────────────────────────────────────────────────

void AssetBrowserPanel::DrawPendingImportBar(EditorContext&)
{
    if (m_pendingImports.empty()) return;

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.30f, 0.18f, 0.05f, 1.0f));
    ImGui::BeginChild("##pending_bar", { 0.0f, 36.0f }, false);

    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 6.0f);

    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.7f, 0.2f, 1.0f));
    ImGui::Text("  ! 未変換ファイル %zu 件", m_pendingImports.size());
    ImGui::PopStyleColor();

    ImGui::SameLine();

    if (ImGui::SmallButton("Import All"))
        m_importAllRequested = true;
    ImGui::SameLine();
    if (ImGui::SmallButton("Dismiss"))
        m_pendingImports.clear();

    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::Separator();
}

// ─── インポートキュー処理 ─────────────────────────────────────────────────────
// WHY: OnRenderContent はウィンドウが collapsed のとき呼ばれないため
//      OnBeforeBegin (毎フレーム確実に呼ばれる) でインポートを処理する。

void AssetBrowserPanel::OnBeforeBegin(EditorContext&)
{
    // ── スレッド完了チェック ──────────────────────────────────────────────
    if (m_importThreadDone.load()) {
        m_importThreadDone.store(false);
        m_isImporting.store(false);
        if (m_importThread.joinable()) m_importThread.join();
        EditorTaskOverlay::End();
        FBZZ_LOG_INFO("AssetBrowserPanel: all imports done, flushing asset cache");
        asset::AssetManager::FlushFailed();
        m_showImportResults = true;
        RefreshDirectory();
        return;
    }

    // ── インポート中: 毎フレーム進捗をオーバーレイに反映 ──────────────────
    if (m_isImporting.load()) {
        const size_t total = m_importTotal.load();
        if (total > 0) {
            EditorTaskOverlay::SetProgress(
                static_cast<float>(m_importDone.load()) / static_cast<float>(total));
        }
        {
            std::lock_guard<std::mutex> lock(m_importStatusMtx);
            EditorTaskOverlay::SetStatus(m_importStatusStr.c_str());
        }
        return;
    }

    // ── インポート開始 ────────────────────────────────────────────────────
    if (!m_importAllRequested || m_pendingImports.empty()) {
        m_importAllRequested = false;
        return;
    }
    m_importAllRequested = false;

    const size_t total = m_pendingImports.size();
    m_importTotal.store(total);
    m_importDone.store(0);
    m_isImporting.store(true);
    m_importThreadDone.store(false);
    m_showImportResults = false;
    m_importResults.clear();
    FBZZ_LOG_INFO("AssetBrowserPanel: starting import of %zu file(s)", total);
    EditorTaskOverlay::Begin("Importing Assets");
    EditorTaskOverlay::SetProgress(0.0f);

    auto imports = std::move(m_pendingImports);

    m_importThread = std::thread([this, imports = std::move(imports)]() mutable {
        try {
            for (const auto& imp : imports) {
                FBZZ_LOG_INFO("AssetBrowserPanel: importing [%s]", imp.path.c_str());
                {
                    std::lock_guard<std::mutex> lock(m_importStatusMtx);
                    m_importStatusStr = util::FileSystem::GetFilename(imp.path);
                }

                namespace fs = std::filesystem;
                const fs::path srcPath(imp.path.begin(), imp.path.end());
                const std::string outDir =
                    (srcPath.parent_path() / srcPath.stem()).string();
                FBZZ_LOG_INFO("AssetBrowserPanel: FBX outDir = [%s]", outDir.c_str());
                const bool ok = FbxImportTool::Import(imp.path, outDir, imp.path);

                if (ok)
                    FBZZ_LOG_INFO("AssetBrowserPanel: import OK [%s]", imp.path.c_str());
                else
                    FBZZ_LOG_ERROR("AssetBrowserPanel: import FAILED [%s]", imp.path.c_str());

                {
                    std::lock_guard<std::mutex> lk(m_importStatusMtx);
                    m_importResults.push_back({
                        util::FileSystem::GetFilename(imp.path), ok });
                }
                m_importDone.fetch_add(1);
            }
        } catch (const std::exception& e) {
            FBZZ_LOG_ERROR("AssetBrowserPanel: import thread exception: %s", e.what());
        } catch (...) {
            FBZZ_LOG_ERROR("AssetBrowserPanel: import thread unknown exception");
        }
        m_importThreadDone.store(true);
    });
}

// ─── メインレイアウト ─────────────────────────────────────────────────────────

void AssetBrowserPanel::OnRenderContent(EditorContext& ctx)
{
    DrawImportResultBar(ctx);

    // ファイルシステムの変化をポーリング
    // WHY: スレッドなしでウォッチャーを動かすため毎フレーム Poll() を呼ぶ。
    //      変化があった場合のみ RefreshDirectory() を実行して描画コストを抑える。
    for (const auto& ev : m_watcher.Poll())
    {
        const bool inCurrentDir = IsChildPathText(
            NormalizePathSeparators(m_rootPath + ev.path),
            m_currentPath);

        if (ev.type == AssetFileWatcher::EventType::Added   ||
            ev.type == AssetFileWatcher::EventType::Removed ||
            ev.type == AssetFileWatcher::EventType::Renamed)
        {
            if (inCurrentDir) RefreshDirectory();

            // 新規追加ファイルが未変換形式なら PendingImport に積む
            if (ev.type == AssetFileWatcher::EventType::Added)
                TryQueuePendingImport(ev.path);
        }
    }

    UpdateMounts(ctx);

    if (ctx.requestAssetBrowserRefresh) {
        RefreshDirectory();
        ctx.requestAssetBrowserRefresh = false;
    }

    // ── 左ペイン: フォルダツリー ─────────────────────────────────────────
    ImGui::BeginChild("##tree", { 150.0f, 0.0f }, true);

    ImGuiTreeNodeFlags rootFlags = ImGuiTreeNodeFlags_OpenOnArrow
                                 | ImGuiTreeNodeFlags_SpanAvailWidth
                                 | ImGuiTreeNodeFlags_DefaultOpen;
    if (SamePathText(m_currentPath, m_rootPath)) rootFlags |= ImGuiTreeNodeFlags_Selected;

    bool rootOpen = ImGui::TreeNodeEx("##root", rootFlags, "Assets");
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen()) {
        m_currentPath = m_rootPath;
        ctx.selectedAssetPath.clear();
        RefreshDirectory();
    }
    // Assets ルートへのドロップ
    if (ImGui::BeginDragDropTarget()) {
        if (SaveHierarchyPayloadAsPrefab(
                ImGui::AcceptDragDropPayload("FBZZ_HIERARCHY_ENTITY"), ctx, m_rootPath)) {
            RefreshDirectory();
        }
        ImGui::EndDragDropTarget();
    }
    if (rootOpen) {
        DrawFolderTree(m_rootPath, ctx);
        ImGui::TreePop();
    }

    ImGui::EndChild();
    ImGui::SameLine();

    // ── 右ペイン: コンテンツエリア ───────────────────────────────────────
    ImGui::BeginChild("##content", { 0.0f, 0.0f }, false);
    // ディレクトリ移動後にスクロールをトップへ戻す。
    // WHY: 深いディレクトリで下にスクロールした後に親へ戻ると、
    //      前のスクロール位置が残りトップにある Fonts 等が見えなくなる。
    if (m_resetScroll) {
        ImGui::SetScrollY(0.0f);
        m_resetScroll = false;
    }
    const ImVec2 contentMin = ImGui::GetWindowPos();
    const ImVec2 contentMax = {
        contentMin.x + ImGui::GetWindowSize().x,
        contentMin.y + ImGui::GetWindowSize().y
    };
    const ImGuiID contentDropId = ImGui::GetID("##content_drop_target");

    // ナビゲーションバー
    if (!SamePathText(m_currentPath, m_rootPath)) {
        if (ImGui::SmallButton(" ^ ")) {
            m_currentPath = ParentPath();
            RefreshDirectory();
        }
        ImGui::SameLine();
    }
    const std::string rel = DisplayPath();
    ImGui::TextDisabled("%s", rel.c_str());

    // 未変換ファイルがあれば警告バーを表示
    DrawPendingImportBar(ctx);

    // 検索 + アイコンサイズスライダー + 作成 + 更新
    ImGui::SetNextItemWidth(-260.0f);
    ImGui::InputText("##search", m_searchBuf.data(), m_searchBuf.size());
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90.0f);
    ImGui::SliderFloat("##sz", &m_iconSize, 40.0f, 120.0f, "%.0f");
    ImGui::SameLine();
    if (ImGui::SmallButton("Create")) {
        ImGui::OpenPopup("##content_ctx");
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Refresh")) RefreshDirectory();

    ImGui::Separator();

    // グリッド表示
    std::string filter(m_searchBuf.data());
    const float padding  = 8.0f;
    const float avail    = ImGui::GetContentRegionAvail().x;
    const int   cols     = std::max(1, (int)(avail / (m_iconSize + padding)));

    int col = 0;
    for (const auto& e : m_entries) {
        if (!filter.empty() && !util::StringUtils::ContainsCI(e.name, filter)) continue;

        if (col > 0 && (col % cols) != 0) ImGui::SameLine(0.0f, padding);
        ImGui::BeginGroup();
        DrawEntry(e, ctx);
        ImGui::EndGroup();
        ++col;
    }

    // ループ外でナビゲートを処理
    if (!m_pendingNavigate.empty()) {
        m_currentPath = NormalizePathSeparators(std::move(m_pendingNavigate));
        m_pendingNavigate.clear();
        RefreshDirectory();
    }

    // 右クリック: Create メニュー
    // WHY: BeginPopupContextWindow は OpenPopup と混ぜるとボタン起動が安定しない。
    //      右クリック検出と popup 描画を分け、空白右クリックとツールバー Create を同じ経路にする。
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Right) &&
        !ImGui::IsAnyItemHovered()) {
        ImGui::OpenPopup("##content_ctx");
    }
    if (ImGui::BeginPopup("##content_ctx")) {
        if (ImGui::BeginMenu("Create")) {
            DrawCreateMenu(ctx);
            ImGui::EndMenu();
        }
        ImGui::EndPopup();
    }

    // ヒエラルキーからエンティティをドロップ → Prefab 化
    if (ImGui::BeginDragDropTargetCustom(ImRect(contentMin, contentMax), contentDropId)) {
        if (SaveHierarchyPayloadAsPrefab(
                ImGui::AcceptDragDropPayload("FBZZ_HIERARCHY_ENTITY"), ctx, m_currentPath)) {
            RefreshDirectory();
        }
        ImGui::EndDragDropTarget();
    }

    // 選択 FBX の内容プレビュー
    DrawFbxContents(ctx);

    ImGui::EndChild();
}

// ─── インポート結果バー ───────────────────────────────────────────────────────

void AssetBrowserPanel::DrawImportResultBar(EditorContext&)
{
    if (!m_showImportResults || m_importResults.empty()) return;

    size_t failed = 0;
    for (const auto& r : m_importResults) if (!r.ok) ++failed;
    const size_t succeeded = m_importResults.size() - failed;

    // 背景色付きの矩形を描いてからテキストを重ねる
    const ImVec4 bgColor = (failed > 0)
        ? ImVec4(0.45f, 0.10f, 0.10f, 0.85f)
        : ImVec4(0.10f, 0.35f, 0.10f, 0.85f);
    const float lineH  = ImGui::GetTextLineHeightWithSpacing();
    const float rows   = static_cast<float>(1 + failed); // サマリー1行 + 失敗行
    const float height = rows * lineH + ImGui::GetStyle().FramePadding.y * 2.0f;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p1 = { p0.x + ImGui::GetContentRegionAvail().x, p0.y + height };
    ImGui::GetWindowDrawList()->AddRectFilled(p0, p1, ImGui::ColorConvertFloat4ToU32(bgColor), 3.0f);

    ImGui::SetCursorScreenPos({ p0.x + 6.0f, p0.y + ImGui::GetStyle().FramePadding.y });

    if (failed > 0)
        ImGui::TextColored({ 1.0f, 0.5f, 0.5f, 1.0f },
            "[!] Import: %zu OK  %zu FAILED", succeeded, failed);
    else
        ImGui::TextColored({ 0.5f, 1.0f, 0.5f, 1.0f },
            "[v] Import: %zu file(s) OK", succeeded);

    ImGui::SameLine(ImGui::GetContentRegionAvail().x - 60.0f);
    if (ImGui::SmallButton("Dismiss"))
        m_showImportResults = false;

    for (const auto& r : m_importResults) {
        if (!r.ok) {
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 6.0f);
            ImGui::TextColored({ 1.0f, 0.4f, 0.4f, 1.0f }, "  x  %s", r.filename.c_str());
        }
    }

    ImGui::Dummy({ 0.0f, 2.0f });
    ImGui::Separator();
}

} // namespace fbzz::editor
