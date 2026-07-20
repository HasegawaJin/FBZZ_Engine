// FBZZ Engine
// AssetBrowserCreate.cpp | fbzz::editor
// AssetBrowser の FBX 内容表示と Create メニュー
#include "AssetBrowserCommon.hpp"
#include <Editor/Util/UndoStack.hpp>
#include <Engine/Asset/AnimatorControllerAsset.hpp>
#include <Engine/Asset/VFXGraphAsset.hpp>
#include <Engine/Asset/DataAssetFactory.hpp>
#include <Engine/Asset/PostProcessAsset.hpp>

namespace fbzz::editor {

namespace {

void RegisterCreatedPath(EditorContext& ctx, const std::string& path)
{
    if (!ctx.undoStack || path.empty() || !util::FileSystem::Exists(path)) return;

    const bool isDirectory = util::FileSystem::IsDirectory(path);
    std::string content;
    if (!isDirectory) util::FileSystem::ReadText(path, content);
    EditorContext* context = &ctx;
    auto refresh = [context]() { context->requestAssetBrowserRefresh = true; };
    ctx.undoStack->Push(std::make_unique<LambdaCommand>(
        "Create Asset",
        [path, isDirectory, content, refresh]() {
            if (isDirectory)
                util::FileSystem::EnsureDirectory(path);
            else
                util::FileSystem::WriteText(path, content);
            refresh();
        },
        [path, refresh]() {
            util::FileSystem::RemoveAll(util::FileSystem::PathFromUtf8(path));
            refresh();
        }));
}

} // namespace

void AssetBrowserPanel::BeginRenameForPath(const std::string& path, EditorContext* ctx)
{
    if (path.empty()) return;

    m_renamingPath = path;
    m_pendingRenamePath.clear();
    m_selectedPaths.clear();

    if (ctx && !util::FileSystem::IsDirectory(path))
        ctx->selectedAssetPath = path;

    std::strncpy(m_renameBuffer, util::FileSystem::GetFilename(path).c_str(),
                 sizeof(m_renameBuffer) - 1);
    m_renameBuffer[sizeof(m_renameBuffer) - 1] = '\0';
    m_renameNeedFocus = true;
}

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

        ImDrawList* dl = ImGui::GetWindowDrawList();
        DrawFileIconPolygon(dl, origin, sz, fill, cDark);

        const ImVec2 tsz = ImGui::CalcTextSize(label);
        dl->AddText({ origin.x + (sz - tsz.x) * 0.5f,
                      origin.y + sz * 0.85f * 0.52f - tsz.y * 0.5f },
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
        const float dur = static_cast<float>(clip.GetDurationSeconds());
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
        RegisterCreatedPath(ctx, newDir);
        RefreshDirectory();
        BeginRenameForPath(newDir, &ctx);
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Scene")) {
        std::string newPath = m_currentPath + "/New Scene.scene";
        int suffix = 1;
        while (util::FileSystem::Exists(newPath))
            newPath = m_currentPath + "/New Scene " + std::to_string(suffix++) + ".scene";
        util::FileSystem::WriteText(newPath, "# FBZZ Scene\n");
        RegisterCreatedPath(ctx, newPath);
        RefreshDirectory();
        BeginRenameForPath(newPath, &ctx);
    }
    if (ImGui::MenuItem("Material")) {
        std::string newPath = m_currentPath + "/New Material.mat";
        int suffix = 1;
        while (util::FileSystem::Exists(newPath))
            newPath = m_currentPath + "/New Material " + std::to_string(suffix++) + ".mat";
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
        RegisterCreatedPath(ctx, newPath);
        RefreshDirectory();
        BeginRenameForPath(newPath, &ctx);
    }
    if (ImGui::MenuItem("Post Process Profile")) {
        std::string newPath = m_currentPath + "/New Post Process Profile.fzpp";
        int suffix = 1;
        while (util::FileSystem::Exists(newPath))
            newPath = m_currentPath + "/New Post Process Profile " +
                std::to_string(suffix++) + ".fzpp";

        // WHY: Serializer を経由し、Inspector が期待する全セクションを持つ互換プロファイルを生成する。
        const renderer::PostProcessSettings defaults;
        if (!asset::SavePostProcessAssetToFile(newPath, defaults)) {
            FBZZ_LOG_ERROR("Post Process Profile creation failed: %s", newPath.c_str());
            return;
        }
        RegisterCreatedPath(ctx, newPath);
        RefreshDirectory();
        BeginRenameForPath(newPath, &ctx);
    }
    if (ImGui::MenuItem("Animator Controller")) {
        std::string newPath = m_currentPath + "/New Animator Controller.animcontroller";
        int suffix = 1;
        while (util::FileSystem::Exists(newPath))
            newPath = m_currentPath + "/New Animator Controller " +
                std::to_string(suffix++) + ".animcontroller";
        asset::AnimatorControllerAsset controller;
        if (!asset::SaveAnimatorControllerAsset(newPath, controller)) {
            FBZZ_LOG_ERROR("Animator Controller creation failed: %s", newPath.c_str());
            return;
        }
        RegisterCreatedPath(ctx, newPath);
        RefreshDirectory();
        BeginRenameForPath(newPath, &ctx);
    }
    if (ImGui::MenuItem("VFX Graph")) {
        std::string newPath = m_currentPath + "/New VFX Graph.vfx";
        int suffix = 1;
        while (util::FileSystem::Exists(newPath))
            newPath = m_currentPath + "/New VFX Graph " + std::to_string(suffix++) + ".vfx";
        asset::VFXGraphAsset graph;
        graph.nodes.push_back({ .id = 1, .type = asset::VFXNodeType::Entry,
                                .name = "Entry", .editorX = 40.0f, .editorY = 120.0f,
                                .duration = 0.0f });
        std::string error;
        if (!asset::SaveVFXGraphAsset(newPath, graph, &error)) {
            FBZZ_LOG_ERROR("VFX Graph creation failed: %s (%s)", newPath.c_str(), error.c_str());
            return;
        }
        RegisterCreatedPath(ctx, newPath);
        RefreshDirectory();
        BeginRenameForPath(newPath, &ctx);
    }

    // ── Data Asset (純共有 ScriptableObject) ──────────────────────
    // 登録済み DataAsset 型を列挙し、選んだ型の .fzdata を生成する。
    // WHY: ファイル内容は "type = ..." の 1 行だけにしておき、フィールドの既定値は
    //      Inspector の初回 Resolve 時に型のフィールド初期化子から補完する。これにより
    //      Create 側でデフォルト値を二重管理せずに済む。
    if (ImGui::BeginMenu("Data Asset")) {
        const auto types = asset::DataAssetFactory::RegisteredTypeNames();
        if (types.empty())
            ImGui::TextDisabled("(no DataAsset types registered)");
        for (const auto& typeName : types) {
            if (ImGui::MenuItem(typeName.c_str())) {
                std::string newPath = m_currentPath + "/New " + typeName + ".fzdata";
                int suffix = 1;
                while (util::FileSystem::Exists(newPath))
                    newPath = m_currentPath + "/New " + typeName + " " +
                        std::to_string(suffix++) + ".fzdata";
                util::FileSystem::WriteText(newPath, "type = \"" + typeName + "\"\n");
                RegisterCreatedPath(ctx, newPath);
                RefreshDirectory();
                BeginRenameForPath(newPath, &ctx);
            }
        }
        ImGui::EndMenu();
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
            [this, resolvedScriptsDir, projScriptsDir, dllPath, staticPath, context = &ctx]
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
                RegisterCreatedPath(*context, path);
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

        auto makeHlslCallback = [this, resolvedHlslDir, projHlslDir, context = &ctx]
            (ScriptCodeGen::HlslKind kind) {
            return [this, resolvedHlslDir, projHlslDir, kind, context]
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
                RegisterCreatedPath(*context, path);
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


} // namespace fbzz::editor
