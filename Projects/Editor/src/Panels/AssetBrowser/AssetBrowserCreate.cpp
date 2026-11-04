/// @file    AssetBrowserCreate.cpp
/// @brief   AssetBrowser の FBX 内容表示と Create メニュー。
/// @author  Hasegawa Jin
/// @date    2026-06-07
#include "AssetBrowserCommon.hpp"
#include <Editor/Op/EditorOperator.hpp>
#include <Editor/Util/AssetCreation.hpp>
#include <Editor/Util/ImGuiWidgets.hpp>
#include <Editor/Util/UndoStack.hpp>
#include <Engine/Asset/AnimatorControllerAsset.hpp>
#include <Engine/Asset/AvatarMaskAsset.hpp>
#include <Engine/Asset/DataAssetFactory.hpp>
#include <Engine/Asset/FluidRecipeCodec.hpp>
#include <Engine/Asset/PhysicsMaterialAsset.hpp>
#include <Engine/Asset/PostProcessProfile.hpp>
#include <Engine/Asset/RenderPipelineAsset.hpp>
#include <Engine/Asset/DataAssetRegistry.hpp>
#include <Editor/Util/ManagedDataAssetEditor.hpp>
#include <Engine/Asset/SequenceAsset.hpp>
#include <Engine/Asset/SynthAsset.hpp>
#include <Engine/Asset/VectorFieldFile.hpp>
#include <Engine/Audio/Synth.hpp>
#include <Engine/AI/BehaviorTreeAsset.hpp>
#include <filesystem>

namespace fbzz::editor {

namespace {

/// @brief 生成したアセットをユーザーへ知らせる。Undo 履歴には積まない。
/// @note 旧実装は「Undo = そのパスを RemoveAll する」を積んでいたが、ディスク上のファイルの存在自体を Undo 対象にすると壊れる: 中身を編集後に無関係な Ctrl+Z で編集ぶんごとファイルが消える、フォルダ Undo で後から追加した中身まで巻き添えで消える、パスだけで別物を消しうる。Undo スタックはシーン編集と共有のため、ファイルの生成・削除は Unity と同じく対象外にし、履歴には「メモリ上の値の編集」だけを載せる。
void NotifyAssetCreated(const std::string& path)
{
    if (path.empty()) return;
    FBZZ_LOG_INFO("Asset created: %s", path.c_str());

    /// @note 失敗キャッシュを掃除する。AssetManager はロード失敗も cache へ焼き付けるため、実体が生まれる前に一度でも参照されたパスはファイルを作っても無音で無効なままになる (先にパスを書いてからアセットを作る順序は普通に起きる)。ここは全生成経路が通る唯一の合流点なので、1 か所で掃除を効かせられる。
    /// @note RefreshDirectory() 側では掃除しない。あちらはファイル監視や各操作の後に高頻度で走り、そこで掃除すると壊れているアセットの再インポートを延々と試み続けるため、「実体が増えた瞬間」だけに絞る。
    asset::AssetManager::FlushFailed();

    Toast::Success("Created " + util::FileSystem::GetFilename(path));
}

} /// @note namespace

void AssetBrowserPanel::BeginRenameForPath(const std::string& path, EditorContext* ctx)
{
    if (path.empty()) return;

    m_renamingPath = path;
    m_pendingRenamePath.clear();
    m_selectedPaths.clear();

    if (ctx && !util::FileSystem::IsDirectory(path))
        SelectAsset(*ctx, path);

    /// @note 編集させるのは拡張子より前だけ。拡張子は m_renameExtension に退避して固定表示する。拡張子はアセットの種類そのものなので、名前を直すついでに変えられると困る。ここは新規作成・複製・F2・遅延リネームの全経路が通る唯一の開始地点なので、分割をここでやれば呼び出し側に手を入れずに全リネームへ効く。
    /// @note フォルダと、先頭がドットのファイル (.gitignore 等) は分割しない。後者は「拡張子だけの名前」であり、切り出すと編集できる部分が無くなる。
    const std::string fileName = util::FileSystem::GetFilename(path);
    std::string       stem     = fileName;
    m_renameExtension.clear();

    if (!util::FileSystem::IsDirectory(path)) {
        const std::size_t dot = fileName.rfind('.');
        if (dot != std::string::npos && dot > 0) {
            stem              = fileName.substr(0, dot);
            m_renameExtension = fileName.substr(dot);
        }
    }

    std::strncpy(m_renameBuffer, stem.c_str(), sizeof(m_renameBuffer) - 1);
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

    /// @note アイコン描画ラムダ (DrawEntry と同じスタイル)
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
            const std::string payloadPath = ToAssetDragPayloadPath(payload, ctx);
            ImGui::SetDragDropPayload("ASSET_PATH", payloadPath.c_str(), payloadPath.size() + 1);
            ImGui::Text("%s: %s", label,
                util::FileSystem::GetFilename(payloadPath).c_str());
            ImGui::EndDragDropSource();
        }

        if (hov) ImGui::SetTooltip("%s", tooltip);

        /// @note 名前テキスト (省略、中央揃え)
        /// @note 1 バイトずつ pop_back すると日本語のファイル名が文字の途中で切れる。
        /// @note 文字境界を知っている共通の縮め方へ寄せる。
        const std::string disp = widgets::ElideToWidth(displayName, sz, "..");

        const float ind = (sz - ImGui::CalcTextSize(disp.c_str()).x) * 0.5f;
        if (ind > 0.0f) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ind);
        ImGui::TextUnformatted(disp.c_str());

        ImGui::PopID();
        ImGui::EndGroup();
        ++col;
    };

    /// @name MESH アイコン
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

    /// @name ANIM アイコン (クリップ1件につき1個)
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
        /// @note 重複回避
        int suffix = 1;
        while (util::FileSystem::Exists(newDir))
            newDir = m_currentPath + "/New Folder " + std::to_string(suffix++);
        if (!CreateEditorAsset("folder", newDir)) return;
        NotifyAssetCreated(newDir);
        RefreshDirectory();
        BeginRenameForPath(newDir, &ctx);
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Scene")) {
        std::string newPath = m_currentPath + "/New Scene.scene";
        int suffix = 1;
        while (util::FileSystem::Exists(newPath))
            newPath = m_currentPath + "/New Scene " + std::to_string(suffix++) + ".scene";
        if (!CreateEditorAsset("scene", newPath)) return;
        NotifyAssetCreated(newPath);
        RefreshDirectory();
        BeginRenameForPath(newPath, &ctx);
    }
    if (ImGui::MenuItem("Material")) {
        std::string newPath = m_currentPath + "/New Material.mat";
        int suffix = 1;
        while (util::FileSystem::Exists(newPath))
            newPath = m_currentPath + "/New Material " + std::to_string(suffix++) + ".mat";
        if (!CreateEditorAsset("material", newPath)) return;
        NotifyAssetCreated(newPath);
        RefreshDirectory();
        BeginRenameForPath(newPath, &ctx);
    }
    if (ImGui::MenuItem("Vector Field")) {
        std::string newPath = m_currentPath + "/New Vector Field.png";
        int suffix = 1;
        while (util::FileSystem::Exists(newPath))
            newPath = m_currentPath + "/New Vector Field " + std::to_string(suffix++) + ".png";
        /// @note 既定は 32³ のカールノイズ。空のグリッドを置くと «貼っても何も起きない» という
        /// @note 最も分かりにくい状態から始まるので、開いた時点で流れが見えるものを焼く。
        if (CreateEditorAsset("vector_field", newPath)) {
            NotifyAssetCreated(newPath);
            RefreshDirectory();
            BeginRenameForPath(newPath, &ctx);
        }
    }
    if (ImGui::MenuItem("Fluid Effect")) {
        std::string newPath = m_currentPath + "/New Fluid.fluid";
        int suffix = 1;
        while (util::FileSystem::Exists(newPath))
            newPath = m_currentPath + "/New Fluid " + std::to_string(suffix++) + ".fluid";
        /// @note 既定は煙のプリセット。空のレシピだと焼いても何も写らず、«壊れている» のか
        /// @note «設定が無い» のか区別できない。
        if (CreateEditorAsset("fluid", newPath)) {
            NotifyAssetCreated(newPath);
            RefreshDirectory();
            BeginRenameForPath(newPath, &ctx);
        }
    }
    if (ImGui::MenuItem("Post Process Profile")) {
        std::string newPath = m_currentPath + "/New Post Process Profile.fzdata";
        int suffix = 1;
        while (util::FileSystem::Exists(newPath))
            newPath = m_currentPath + "/New Post Process Profile " +
                std::to_string(suffix++) + ".fzdata";

        /// @note DataAssetRegistry::Create は型名から実体を生成し既定値のまま保存するため、Inspector が期待する全セクションが Reflect() 経由で自動的に揃う。専用のテンプレート文字列を持たなくて済む。
        if (!CreateEditorAsset("post_process_profile", newPath)) {
            FBZZ_LOG_ERROR("Post Process Profile creation failed: %s", newPath.c_str());
            return;
        }
        NotifyAssetCreated(newPath);
        RefreshDirectory();
        BeginRenameForPath(newPath, &ctx);
    }
    if (ImGui::MenuItem("Animator Controller")) {
        std::string newPath = m_currentPath + "/New Animator Controller.animcontroller";
        int suffix = 1;
        while (util::FileSystem::Exists(newPath))
            newPath = m_currentPath + "/New Animator Controller " +
                std::to_string(suffix++) + ".animcontroller";
        if (!CreateEditorAsset("animator_controller", newPath)) {
            FBZZ_LOG_ERROR("Animator Controller creation failed: %s", newPath.c_str());
            return;
        }
        NotifyAssetCreated(newPath);
        RefreshDirectory();
        BeginRenameForPath(newPath, &ctx);
    }
    /// @note 演出タイムライン。空でも「合図を並べる EventTrack 1 本」だけは入れて出す。完全に空の .sequence は SequencePlayerComponent に挿しても何も起きず、設定が足りないのかアセットが壊れているのか見分けが付かない。
    if (ImGui::MenuItem("Sequence")) {
        std::string newPath = m_currentPath + "/New Sequence.sequence";
        int suffix = 1;
        while (util::FileSystem::Exists(newPath))
            newPath = m_currentPath + "/New Sequence " + std::to_string(suffix++) + ".sequence";
        if (!CreateEditorAsset("sequence", newPath)) {
            FBZZ_LOG_ERROR("Sequence creation failed: %s", newPath.c_str());
            return;
        }
        NotifyAssetCreated(newPath);
        RefreshDirectory();
        BeginRenameForPath(newPath, &ctx);
    }
    /// @note Avatar Mask: アニメーションレイヤーを「どのボーンに効かせるか」の再利用アセット。上半身だけ / 下半身だけの制御はこれが無いと毎回ボーンパスを手書きすることになる。
    if (ImGui::MenuItem("Avatar Mask")) {
        std::string newPath = m_currentPath + "/New Avatar Mask.mask";
        int suffix = 1;
        while (util::FileSystem::Exists(newPath))
            newPath = m_currentPath + "/New Avatar Mask " + std::to_string(suffix++) + ".mask";
        if (!CreateEditorAsset("avatar_mask", newPath)) {
            FBZZ_LOG_ERROR("Avatar Mask creation failed: %s", newPath.c_str());
            return;
        }
        NotifyAssetCreated(newPath);
        RefreshDirectory();
        BeginRenameForPath(newPath, &ctx);
    }
    if (ImGui::MenuItem("Behavior Tree")) {
        std::string newPath = m_currentPath + "/New Behavior Tree.behaviortree";
        int suffix = 1;
        while (util::FileSystem::Exists(newPath))
            newPath = m_currentPath + "/New Behavior Tree " + std::to_string(suffix++)
                    + ".behaviortree";

        /// @note ルート 1 個の最小構成で作る。ValidateBehaviorTreeAsset が「ルート 0 個」を拒否するため空のまま保存できず、すぐ編集を始められる形で生成する。
        if (!CreateEditorAsset("behavior_tree", newPath)) {
            FBZZ_LOG_ERROR("Behavior Tree creation failed: %s", newPath.c_str());
            return;
        }
        NotifyAssetCreated(newPath);
        RefreshDirectory();
        BeginRenameForPath(newPath, &ctx);
    }

    /// @name Physics Material (共有物理マテリアル)
    /// @note プリセットを選んで .physmat を生成する。反発 0.3 / 摩擦 0.6 のような数値だけでは「どんな材質か」が分からないため、ゴム・氷・金属という名前から始めて微調整させる。
    if (ImGui::BeginMenu("Physics Material")) {
        for (int i = 0; i < physics::PhysicsMaterial::PRESET_COUNT; ++i) {
            const char* presetName = physics::PhysicsMaterial::PresetName(i);
            if (!ImGui::MenuItem(presetName)) continue;

            std::string newPath = m_currentPath + "/" + presetName + ".physmat";
            int suffix = 1;
            while (util::FileSystem::Exists(newPath))
                newPath = m_currentPath + "/" + presetName + " " +
                    std::to_string(suffix++) + ".physmat";

            if (!CreateEditorAsset("physics_material", newPath, presetName)) {
                FBZZ_LOG_ERROR("Physics Material creation failed: %s", newPath.c_str());
                continue;
            }
            NotifyAssetCreated(newPath);
            RefreshDirectory();
            BeginRenameForPath(newPath, &ctx);
        }
        ImGui::EndMenu();
    }

    /// @name Data Asset (純共有 ScriptableObject)
    /// @note プリセットを選んで .synth を生成する。空の SynthSpec は無音に近く、そこから耳で目的の音へ辿り着くのは現実的でないため、「Laser」から始めて調整させる。
    if (ImGui::BeginMenu("Synth Clip")) {
        for (int i = 0; i < static_cast<int>(audio::SynthPreset::Count); ++i) {
            const auto preset = static_cast<audio::SynthPreset>(i);
            const char* presetName = audio::PresetToString(preset);
            if (!ImGui::MenuItem(presetName)) continue;

            std::string newPath = m_currentPath + "/" + presetName + ".synth";
            int suffix = 1;
            while (util::FileSystem::Exists(newPath))
                newPath = m_currentPath + "/" + presetName + " " +
                    std::to_string(suffix++) + ".synth";

            if (!CreateEditorAsset("synth", newPath, presetName)) {
                FBZZ_LOG_ERROR("Synth Clip creation failed: %s", newPath.c_str());
                continue;
            }
            NotifyAssetCreated(newPath);
            RefreshDirectory();
            BeginRenameForPath(newPath, &ctx);
        }
        ImGui::EndMenu();
    }

    /// @note 登録済み DataAsset 型を列挙し、選んだ型の .fzdata を生成する。ファイル内容は `type = ...` の 1 行だけにしておき、フィールドの既定値は Inspector の初回 Resolve 時に型のフィールド初期化子から補完する。Create 側でデフォルト値を二重管理せずに済む。
    if (ImGui::BeginMenu("Data Asset")) {
        DrawManagedDataAssetCreateMenu(ctx, m_currentPath);
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
                if (typeName == asset::RenderPipelineAsset::TYPE_NAME) {
                    if (!asset::CreateRenderPipelineAsset(newPath, renderer::RenderSettings{})) {
                        FBZZ_LOG_ERROR("Pipeline asset creation failed: %s", newPath.c_str());
                        continue;
                    }
                } else if (!util::FileSystem::WriteText(newPath, "type = \"" + typeName + "\"\n")) {
                    FBZZ_LOG_ERROR("Data asset creation failed: %s", newPath.c_str());
                    continue;
                }
                NotifyAssetCreated(newPath);
                RefreshDirectory();
                BeginRenameForPath(newPath, &ctx);
            }
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("C#...")) {
        const std::string scriptsDir = ctx.projectRoot + "/Assets/Scripts";
        const std::string setupNote = "Destination: " + scriptsDir +
            "\nRequires exactly one <Project>.Scripts.csproj from the SDK ManagedStarter. Names use ASCII C# identifiers.";
        EditorContext* context = &ctx;
        auto makeCSharpCallback = [this, context, scriptsDir](const char* operatorId, const char* label) {
            return [this, context, scriptsDir, operatorId, label](const std::string& name) {
                OpArgs args;
                args.Set("name", name);
                const OpResult result = InvokeOperator(*context, operatorId, args);
                if (!result.ok) {
                    FBZZ_LOG_WARN("%s creation failed: %s", label, result.message.c_str());
                    return;
                }
                const OpData* pathValue = result.data.Find("path");
                if (!pathValue || !pathValue->IsString()) {
                    FBZZ_LOG_ERROR("%s creation returned no source path", label);
                    return;
                }
                NotifyAssetCreated(pathValue->AsString());
                m_pendingNavigate = scriptsDir;
                RefreshDirectory();
            };
        };

        if (ImGui::MenuItem("Script (attach to GameObject)...")) {
            ModalDialog::OpenInput("New C# Script", "Enemy",
                makeCSharpCallback("asset.create_csharp_script", "C# Script"),
                setupNote + "\nComponent is appended only when the name does not already end with it.");
        }
        if (ImGui::MenuItem("Helper Class (not attached)...")) {
            ModalDialog::OpenInput("New C# Helper", "DamageCalculator",
                makeCSharpCallback("asset.create_csharp_helper", "C# Helper"), setupNote);
        }
        if (ImGui::MenuItem("DataAsset Type...")) {
            ModalDialog::OpenInput("New C# DataAsset Type", "WeaponData",
                makeCSharpCallback("asset.create_csharp_data_asset_type", "C# DataAsset Type"),
                setupNote + "\nThis creates the managed type; .fzdata instance authoring is separate.");
        }
        ImGui::EndMenu();
    }

    /// @name HLSL シェーダー
    if (ImGui::BeginMenu("HLSL Shader...")) {
        auto makeHlslCallback = [this, &ctx](ScriptCodeGen::HlslKind kind) {
            return [this, &ctx, kind](const std::string& name) {
                const char* value = kind == ScriptCodeGen::HlslKind::SurfaceVSPS ? "surface"
                    : kind == ScriptCodeGen::HlslKind::ParticlePS ? "particle"
                    : kind == ScriptCodeGen::HlslKind::ComputeCS ? "compute" : "post_process";
                OpArgs args; args.Set("name", name); args.Set("kind", std::string(value));
                const OpResult result = InvokeOperator(ctx, "asset.hlsl.create", args);
                if (!result.ok) { FBZZ_LOG_WARN("HLSL creation failed: %s", result.message.c_str()); return; }
                const auto* path = result.data.Find("path");
                if (!path) return;
                NotifyAssetCreated(path->AsString());
                m_pendingNavigate = util::FileSystem::GetDirectory(path->AsString());
                RefreshDirectory();
            };
        };

        if (ImGui::MenuItem("Surface Shader (VS+PS)")) {
            ModalDialog::OpenInput("New Surface Shader", "MyMaterial",
                makeHlslCallback(ScriptCodeGen::HlslKind::SurfaceVSPS),
                "Generated under Material/Custom/");
        }
        if (ImGui::MenuItem("Particle Shader (PS)")) {
            ModalDialog::OpenInput("New Particle Shader", "MyParticle",
                makeHlslCallback(ScriptCodeGen::HlslKind::ParticlePS),
                "Generated under Material/Custom/ (assign via a .mat with render_path = \"particle\")");
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

/// @name メインレイアウト

/// @name ファイル監視ヘルパー


} /// @note namespace fbzz::editor
