// FBZZ Engine
// AssetBrowserItems.cpp | fbzz::editor
// AssetBrowser のフォルダツリーとファイルアイコン描画
#include "AssetBrowserCommon.hpp"

namespace fbzz::editor {

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
        e.path = util::FileSystem::NormalizePathSeparators(p);
        e.name = util::FileSystem::GetFilename(p);
        e.isDir = true;
        dirs.push_back(std::move(e));
    }
    if (util::FileSystem::SamePathText(dirPath, m_rootPath)) {
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
        if (util::FileSystem::SamePathText(dir.path, m_currentPath)) flags |= ImGuiTreeNodeFlags_Selected;

        // WHY: 表示名は Assets 側の仮想名、ID は実パスにすることで同名マウントでも ImGui ID が衝突しない。
        bool open = ImGui::TreeNodeEx(dir.path.c_str(), flags, "%s", dir.name.c_str());
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen()) {
            m_currentPath = dir.path;
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
        // WHY: ダブルクリック後に Scene open / Prefab instantiate / directory navigation が走ると、
        //      AssetBrowser の m_entries が更新される可能性がある。
        //      Entry 参照 e を使い続けると、std::string 比較や string_view 内で無効参照を踏むため、
        //      分岐に入る前に必要な値をコピーしておく。
        const bool isDir = e.isDir;
        const std::string path = e.path;
        const std::string ext = e.ext;

        if (isDir) {
            m_pendingNavigate = path;
        } else {
            // WHY: 単クリックで Inspector を切り替えると、Asset 編集中にブラウズしただけで対象が失われる。
            // WHAT: ファイルはダブルクリック時だけ Inspector の Asset 対象へ反映する。
            ctx.selectedAssetPath = path;
        }

        if (!isDir && ext == ".fbzz" && ctx.activeScene) {
            if (ctx.requestOpenScene) {
                ctx.requestOpenScene(path);
            } else if (SceneIO::Load(*ctx.activeScene, path)) {
                ctx.selectedEntities.clear();
                if (ctx.markSceneDirty) ctx.markSceneDirty();
                FBZZ_LOG_INFO("Opened scene: %s", path.c_str());
            } else {
                FBZZ_LOG_ERROR("Failed to open scene: %s", path.c_str());
            }
        } else if (!isDir && ext == ".fbzzprefab" && ctx.activeScene) {
            std::vector<scene::EntityID> roots;
            if (PrefabSerializer::Instantiate(*ctx.activeScene, path, roots)) {
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

} // namespace fbzz::editor
