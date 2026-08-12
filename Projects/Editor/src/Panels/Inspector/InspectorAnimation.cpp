// FBZZ Engine
// InspectorAnimation.cpp | fbzz::editor
// Animation / IK 系 Component の Inspector 描画
#include "InspectorAnimation.hpp"
#include <Editor/Util/EditorTheme.hpp>
#include <Engine/Asset/AnimatorControllerAsset.hpp>
#include <Engine/Asset/AvatarMaskAsset.hpp>

namespace fbzz::editor {

namespace {

// ── Root Motion ──────────────────────────────────────────────────────────────
// WHY: 従来は "Apply Root Motion" チェックボックス 1 個しかなく、
//      「抽出だけしてスクリプトへ渡す」「RigidBody 速度として食わせる」
//      「トラックをノード名で指定する」といった受け取り方をどれも選べなかった。
//      Mode / Source / 軸マスクを分けて提示し、実行中の値も併せて見せる。
void DrawRootMotionSettings(scene::AnimatorComponent& anim)
{
    using scene::RootMotionMode;
    using scene::RootMotionSource;

    if (!ImGui::CollapsingHeader("Root Motion")) return;
    ImGui::PushID("RootMotion");

    static const char* kModeNames[] = {
        "None (抽出しない)",
        "Apply To Transform (エンジンが動かす)",
        "Extract Only (Script が動かす)",
        "Apply To RigidBody (速度として渡す)",
    };
    ImGui::Combo("Mode", reinterpret_cast<int*>(&anim.rootMotion.mode),
                 kModeNames, IM_ARRAYSIZE(kModeNames));

    switch (anim.rootMotion.mode) {
    case RootMotionMode::ExtractOnly:
        ImGui::TextDisabled("Script::OnAnimatorMove() で移動量を受け取って適用します。");
        break;
    case RootMotionMode::ApplyToRigidBody:
        ImGui::TextDisabled("水平成分を RigidBody 速度へ渡します。落下と衝突は物理側が解きます。");
        break;
    case RootMotionMode::ApplyToTransform:
        ImGui::TextDisabled("Transform を直接動かします。コライダはテレポート扱いになります。");
        break;
    default:
        break;
    }

    if (anim.rootMotion.mode == RootMotionMode::None) {
        static const char* kPoseNames[] = {
            "Strip (その場再生)",
            "Keep (クリップのまま前進)",
        };
        ImGui::Combo("Pose", reinterpret_cast<int*>(&anim.rootMotion.poseMode),
                     kPoseNames, IM_ARRAYSIZE(kPoseNames));
    }

    static const char* kSourceNames[] = {
        "Clip Defined (.anim の指定)",
        "Auto Detect (候補名から推定)",
        "Node Name (名前で指定)",
    };
    ImGui::Combo("Source", reinterpret_cast<int*>(&anim.rootMotion.source),
                 kSourceNames, IM_ARRAYSIZE(kSourceNames));

    if (anim.rootMotion.source == RootMotionSource::NodeName) {
        char nodeName[128] = {};
        std::snprintf(nodeName, sizeof(nodeName), "%s",
                      anim.rootMotion.nodeName.c_str());
        if (ImGui::InputText("Node", nodeName, sizeof(nodeName))) {
            anim.rootMotion.nodeName = nodeName;
            anim.rootMotionSamples.clear();
        }
        ImGui::TextDisabled("例: mixamorig:Hips / Armature|Root");
    }

    char targetPath[128] = {};
    std::snprintf(targetPath, sizeof(targetPath), "%s",
                  anim.rootMotion.targetPath.c_str());
    if (ImGui::InputText("Target", targetPath, sizeof(targetPath)))
        anim.rootMotion.targetPath = targetPath;
    ImGui::TextDisabled("空 = この GameObject。\"..\" で親 (RigidBody が親にある構成)。");

    static const char* kAxisNames[] = { "Use Clip", "Disabled", "Enabled" };
    ImGui::Combo("Horizontal (XZ)",
                 reinterpret_cast<int*>(&anim.rootMotion.applyXZ),
                 kAxisNames, IM_ARRAYSIZE(kAxisNames));
    ImGui::Combo("Vertical (Y)",
                 reinterpret_cast<int*>(&anim.rootMotion.applyY),
                 kAxisNames, IM_ARRAYSIZE(kAxisNames));
    ImGui::Combo("Rotation",
                 reinterpret_cast<int*>(&anim.rootMotion.applyRotation),
                 kAxisNames, IM_ARRAYSIZE(kAxisNames));

    ImGui::DragFloat("Position Scale", &anim.rootMotion.positionScale,
                     0.01f, 0.0f, 10.0f);
    ImGui::DragFloat("Rotation Scale", &anim.rootMotion.rotationScale,
                     0.01f, 0.0f, 1.0f);

    // 実行中の値。歩幅とゲーム速度が合っているかはここを見ながら詰める。
    if (anim.rootMotion.mode != RootMotionMode::None) {
        ImGui::SeparatorText("Runtime");
        const auto& delta = anim.rootMotionDeltaPosition;
        const auto& velocity = anim.rootMotionWorldVelocity;
        ImGui::Text("Delta  %.4f, %.4f, %.4f", delta.x, delta.y, delta.z);
        ImGui::Text("Speed  %.3f m/s (world %.3f, %.3f, %.3f)",
                    velocity.Length(), velocity.x, velocity.y, velocity.z);
        ImGui::TextDisabled(anim.rootMotionAppliedByEngine
                                ? "エンジンが適用済み"
                                : "エンジンは適用していない (Script 側の責任)");
    }

    ImGui::PopID();
}

// ── Animator Layers ──────────────────────────────────────────────────────────
// WHY: 上半身だけ / 下半身だけの制御はレイヤーが入口になる。しかし従来 Inspector には
//      レイヤーを作る導線が一切なく、ランタイムが対応していても実際には使えなかった。
//      「レイヤーを足す → .mask を割り当てる → 加算にするか決める」までをここで完結させる。
void DrawAnimatorLayers(scene::AnimatorComponent& anim, EditorContext& ctx)
{
    ImGui::SeparatorText("Layers");
    ImGui::TextDisabled(
        "Base Layer が全身のポーズを作り、各レイヤーが Mask のボーンだけを上書き / 加算します。");

    const auto markDirty = [&ctx]() { if (ctx.markSceneDirty) ctx.markSceneDirty(); };

    // Base Layer 自身のマスク。外したボーンはバインドポーズのまま残り、上のレイヤーだけが動かす。
    if (widgets::AssetPathField("Base Layer Mask", anim.baseLayerMask.path,
                                ".mask", ctx.projectRoot)) {
        anim.baseLayerMask.Invalidate();
        markDirty();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::SetTooltip(
            "Base Layer から外したボーンはバインドポーズで固定され、\n"
            "上のレイヤーだけが動かす形になります。空なら全身に効きます。");
    }

    int removeIndex = -1;
    for (int i = 0; i < static_cast<int>(anim.layers.size()); ++i) {
        auto& layer = anim.layers[static_cast<size_t>(i)];
        ImGui::PushID(i);

        // ヘッダーには実行中の状態 (weight / Slot) を出す。
        // WHY: 上半身レイヤーが効いていない原因は大抵 weight か mask なので、
        //      折りたたんだままでも一目で分かるようにしておく。
        char header[192];
        std::snprintf(header, sizeof(header), "%s  [%s %.0f%%]%s",
                      layer.name.empty() ? "(unnamed)" : layer.name.c_str(),
                      layer.mode == scene::AnimationLayerMode::Additive ? "Additive" : "Override",
                      layer.weight * 100.0f,
                      layer.slot.active ? "  (slot)" : "");

        if (ImGui::Checkbox("##layer_enabled", &layer.enabled)) markDirty();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("このレイヤーを評価するか");
        ImGui::SameLine();

        const bool open = ImGui::TreeNodeEx(header, ImGuiTreeNodeFlags_DefaultOpen);
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 60.0f);
        if (ImGui::SmallButton("Remove")) removeIndex = i;

        if (open) {
            char nameBuffer[128];
            std::snprintf(nameBuffer, sizeof(nameBuffer), "%s", layer.name.c_str());
            if (ImGui::InputText("Name", nameBuffer, sizeof(nameBuffer))) {
                layer.name = nameBuffer;
                markDirty();
            }

            if (ImGui::SliderFloat("Weight", &layer.weight, 0.0f, 1.0f)) markDirty();

            static constexpr const char* kModeNames[] = { "Override", "Additive" };
            int modeIndex = static_cast<int>(layer.mode);
            if (ImGui::Combo("Blending", &modeIndex, kModeNames, 2)) {
                layer.mode = static_cast<scene::AnimationLayerMode>(modeIndex);
                markDirty();
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                ImGui::SetTooltip(
                    "Override: Base のポーズを置き換える (上半身の構えなど)\n"
                    "Additive: Base のポーズに差分を足す (呼吸・反動など)");
            }

            // ── Mask ─────────────────────────────────────────────────────
            if (widgets::AssetPathField("Mask", layer.mask.path, ".mask", ctx.projectRoot)) {
                // 次フレームの AnimatorSystem に読み直させる。
                layer.mask.Invalidate();
                markDirty();
            }
            if (layer.mask.path.empty()) {
                ImGui::TextDisabled("  Mask 未設定 = 全身に効きます");
            }

            // ── Additive Reference ───────────────────────────────────────
            if (layer.mode == scene::AnimationLayerMode::Additive) {
                ImGui::SeparatorText("Additive Reference Pose");
                ImGui::TextDisabled("空なら加算クリップ自身の先頭フレームを基準にします。");
                if (widgets::AssetPathField("Ref Source", layer.additiveReference.sourcePath,
                                            ".fbx", ctx.projectRoot))
                    markDirty();
                char clipBuffer[128];
                std::snprintf(clipBuffer, sizeof(clipBuffer), "%s",
                              layer.additiveReference.clipName.c_str());
                if (ImGui::InputText("Ref Clip", clipBuffer, sizeof(clipBuffer))) {
                    layer.additiveReference.clipName = clipBuffer;
                    markDirty();
                }
                if (ImGui::DragFloat("Ref Time", &layer.additiveReference.time,
                                     0.01f, 0.0f, 600.0f, "%.2f s"))
                    markDirty();
            }

            // ── State Machine ────────────────────────────────────────────
            ImGui::SeparatorText("State Machine");
            if (layer.states.empty()) {
                // レイヤー専用グラフを持たない場合は、共有 states の 1 ステートを流す旧挙動。
                char stateBuffer[128];
                std::snprintf(stateBuffer, sizeof(stateBuffer), "%s", layer.stateName.c_str());
                if (ImGui::InputText("State", stateBuffer, sizeof(stateBuffer))) {
                    layer.stateName = stateBuffer;
                    markDirty();
                }
                ImGui::TextDisabled(
                    "  専用グラフ未設定。Base Layer の 1 ステートを再生します。");
            } else {
                ImGui::Text("States: %d   Default: %s",
                            static_cast<int>(layer.states.size()),
                            layer.defaultStateName.empty() ? "(first)"
                                                           : layer.defaultStateName.c_str());
                if (!layer.runtime.currentStateName.empty()) {
                    ImGui::TextDisabled("  Current: %s",
                                        layer.runtime.currentStateName.c_str());
                }
            }

            // ── Slot (ランタイム表示のみ) ─────────────────────────────────
            if (layer.slot.active) {
                ImGui::SeparatorText("Slot");
                ImGui::Text("%s", layer.slot.clipName.empty()
                            ? layer.slot.sourcePath.c_str() : layer.slot.clipName.c_str());
                ImGui::ProgressBar(layer.slot.weight, { -1.0f, 0.0f },
                                   layer.slot.stopping ? "fading out" : "blending in");
                if (ImGui::SmallButton("Stop Slot")) anim.StopSlot(layer.name);
            }

            ImGui::TreePop();
        }
        ImGui::PopID();
    }

    if (removeIndex >= 0) {
        anim.layers.erase(anim.layers.begin() + removeIndex);
        markDirty();
    }

    if (ImGui::Button("Add Layer", ImVec2(-1.0f, 0.0f))) {
        scene::AnimationLayer layer;
        // 同名レイヤーは名前引き API (SetLayerWeight / PlaySlot) が壊れるため採番する。
        layer.name = "Layer " + std::to_string(anim.layers.size() + 1);
        for (int suffix = 1; anim.FindLayer(layer.name) != nullptr && suffix < 1000; ++suffix)
            layer.name = "Layer " + std::to_string(anim.layers.size() + 1 + suffix);
        anim.layers.push_back(std::move(layer));
        markDirty();
    }
}

} // namespace

void DrawAnimationInspectors(scene::GameObject* go, EditorContext& ctx, std::any& m_componentClipboard, const std::type_info*& m_componentClipboardType)
{
    // WHY: Unity の Animator コンポーネントと同じ密度に揃える。ステート/遷移/パラメーターの
    //      編集は Animation Graph に一本化し、Inspector には Controller 参照と再生設定だけを置く。
    //      Controller 未設定はサポート対象外の過渡状態として扱い、割り当てを促す。
    DrawComponentSection<scene::AnimatorComponent>(go, ctx, m_componentClipboard, m_componentClipboardType, "Animator",
        [go](scene::AnimatorComponent& anim, EditorContext& ctx) {
            if (widgets::AssetPathField("Controller", anim.controllerPath,
                                        ".animcontroller", ctx.projectRoot)) {
                anim.loadedControllerPath.clear();
                if (ctx.markSceneDirty) ctx.markSceneDirty();
            }

            const bool hasController = !anim.controllerPath.empty();
            if (!hasController) {
                ImGui::TextColored({ 0.9f, 0.55f, 0.2f, 1.0f },
                                   "Animator Controller が未設定です。");
                if (ImGui::Button("Create Controller From Current", ImVec2(-1.0f, 0.0f))) {
                    const std::string directory = ctx.projectRoot + "/Assets/Animation";
                    util::FileSystem::EnsureDirectory(directory);
                    std::string safeName = go->name.empty() ? "Animator" : go->name;
                    for (char& character : safeName) {
                        const bool valid =
                            std::isalnum(static_cast<unsigned char>(character)) ||
                            character == '_' || character == '-';
                        if (!valid) character = '_';
                    }
                    std::string path = directory + "/" + safeName + ".animcontroller";
                    for (int suffix = 1;
                         util::FileSystem::Exists(path) && suffix < 10000;
                         ++suffix) {
                        path = directory + "/" + safeName + " " +
                            std::to_string(suffix) + ".animcontroller";
                    }
                    const auto controller = asset::MakeAnimatorControllerAsset(anim);
                    if (asset::SaveAnimatorControllerAsset(path, controller)) {
                        anim.controllerPath = NormalizeAssetPath(path);
                        anim.loadedControllerPath = anim.controllerPath;
                        ctx.selectedAssetPath = path;
                        ctx.requestAssetBrowserRefresh = true;
                        if (ctx.markSceneDirty) ctx.markSceneDirty();
                    }
                }
                return;
            }

            if (ImGui::Button("Open Animation Graph", ImVec2(-1.0f, 0.0f)))
                ctx.requestOpenAnimationGraph = true;

            ImGui::DragFloat("Speed", &anim.speed, 0.01f, -10.0f, 10.0f);
            ImGui::Checkbox("Playing", &anim.playing);
            DrawRootMotionSettings(anim);

            if (!anim.currentStateName.empty()) {
                ImGui::SeparatorText("Runtime");
                ImGui::Text("Current: %s", anim.currentStateName.c_str());
                ImGui::ProgressBar(anim.GetNormalizedTime(), { -1.0f, 0.0f });
                if (!anim.blendToState.empty()) {
                    ImGui::TextDisabled(
                        "-> %s  (blend: %.0f%%)",
                        anim.blendToState.c_str(), anim.blendWeight * 100.0f);
                }
            }

            DrawAnimatorLayers(anim, ctx);
        });

    // Bone は FBX インポートで自動付与される内部コンポーネント。編集は想定しないが、
    // ボーン名とインデックスの対応をシーン上で確認できないとリターゲットや
    // IK 設定のデバッグが Hierarchy 頼みになるため、read-only で表示する。
    DrawComponentSection<scene::BoneComponent>(go, ctx, m_componentClipboard, m_componentClipboardType, "Bone",
        [](scene::BoneComponent& bone, EditorContext&) {
            ImGui::BeginDisabled();
            ImGui::Text("Bone Name: %s", bone.boneName.c_str());
            ImGui::Text("Node Index: %d   Bone Index: %d", bone.nodeIndex, bone.boneIndex);
            ImGui::Text("Generated: %s", bone.generated ? "Yes" : "No");
            ImGui::EndDisabled();
        });

    DrawComponentSection<scene::IKSolverComponent>(go, ctx, m_componentClipboard, m_componentClipboardType, "IK Solver",
        [go](scene::IKSolverComponent& ik, EditorContext& ctx) {

            // ── Hip Height Correction ────────────────────────────────────────
            // ── IK Chains ────────────────────────────────────────────────────
            int removeIdx = -1;

            for (int ci = 0; ci < static_cast<int>(ik.chains.size()); ++ci) {
                auto& chain = ik.chains[static_cast<size_t>(ci)];
                ImGui::PushID(ci);

                // ヘッダー行: [▶] [✓] Chain 0  (TipBone)           [Remove]
                // WHY: Unity の Constraint コンポーネントと同様に enabled を
                //      折りたたみ矢印の横に置き、開かずに ON/OFF できるようにする。
                const char* solverNames[] = {
                    "Two Bone", "Foot Place", "Aim At", "FABRIK", "Hand Place",
                    "Full Body Biped"
                };
                const int solverIndex = static_cast<int>(chain.type);
                const char* tipLabel = solverIndex >= 0 && solverIndex < 6
                    ? solverNames[solverIndex] : "Unknown";
                char header[64];
                std::snprintf(header, sizeof(header), "##chain%d", ci);

                const float removeW   = ImGui::CalcTextSize("Remove").x + ImGui::GetStyle().FramePadding.x * 2.0f;
                const float checkboxW = ImGui::GetFrameHeight();

                bool open = ImGui::TreeNodeEx(header,
                    ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap,
                    "Chain %d  (%s)", ci, tipLabel);

                ImGui::SameLine(ImGui::GetContentRegionMax().x - removeW - checkboxW
                                - ImGui::GetStyle().ItemSpacing.x);
                ImGui::Checkbox("##en", &chain.enabled);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Enable / Disable this chain");
                ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button,        EditorTheme::Color(ThemeColor::Danger));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, EditorTheme::Color(ThemeColor::Danger));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  EditorTheme::Color(ThemeColor::AccentActive));
                if (ImGui::SmallButton("Remove")) removeIdx = ci;
                ImGui::PopStyleColor(3);

                if (open) {
                    // 無効チェーンは薄く表示
                    if (!chain.enabled)
                        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.5f);

                    int type = static_cast<int>(chain.type);
                    if (ImGui::Combo("Solver Type", &type, solverNames, 6)) {
                        chain.type = static_cast<scene::IKSolverType>(type);
                        chain.lookAtHasState = false;
                        if (chain.type == scene::IKSolverType::FootPlace) chain.order = 0;
                        if (chain.type == scene::IKSolverType::TwoBone) chain.order = 10;
                        if (chain.type == scene::IKSolverType::FABRIK) chain.order = 10;
                        if (chain.type == scene::IKSolverType::AimAt) chain.order = 20;
                        if (chain.type == scene::IKSolverType::HandPlace) chain.order = 30;
                        if (chain.type == scene::IKSolverType::FullBodyBiped) chain.order = -100;
                        if ((chain.type == scene::IKSolverType::TwoBone ||
                             chain.type == scene::IKSolverType::HandPlace) &&
                            chain.boneNames.size() != 3)
                            chain.boneNames.resize(3);
                        if (chain.type == scene::IKSolverType::AimAt && chain.boneNames.size() != 1)
                            chain.boneNames.resize(1);
                    }
                    ImGui::DragInt("Order", &chain.order, 1.0f);
                    ImGui::DragFloat("Weight", &chain.weight, 0.01f, 0.0f, 1.0f, "%.2f");

                    if (chain.type == scene::IKSolverType::FullBodyBiped) {
                        ImGui::SeparatorText("Full Body Biped");
                        ImGui::DragInt("Iterations", &chain.fullBodyIterations,
                                       1.0f, 1, 16);
                        ImGui::DragFloat("Max Joint Correction",
                                         &chain.fullBodyMaxRotationDegrees,
                                         1.0f, 1.0f, 180.0f, "%.1f deg");
                        ImGui::DragFloat("Tolerance", &chain.fullBodyTolerance,
                                         0.0005f, 0.0001f, 0.1f, "%.4f m");
                        ImGui::TextDisabled(
                            "Runs FootPlace -> FABRIK -> AimAt -> HandPlace repeatedly.");
                        ImGui::Text("Iterations Used: %d", ik.runtimeFullBodyIterations);
                        ImGui::Text("Effector Error: %.4f m", ik.runtimeFullBodyError);
                        ImGui::Text("Converged: %s",
                                    ik.runtimeFullBodyConverged ? "yes" : "no");
                    }

                    if (ci == 0) {
                        ImGui::SeparatorText("Runtime Diagnostics");
                        ImGui::Text("Updates: %llu",
                                    static_cast<unsigned long long>(ik.runtimeUpdateCount));
                        ImGui::Text("Animator IK Weight: %.3f", ik.runtimeAnimatorWeight);
                        ImGui::Text("Solved Chains: %d / %d",
                                    ik.runtimeSolvedChainCount,
                                    static_cast<int>(ik.chains.size()));
                        ImGui::Text("Grounded L/R: %s / %s",
                                    ik.runtimeLeftFootGrounded ? "yes" : "no",
                                    ik.runtimeRightFootGrounded ? "yes" : "no");
                        ImGui::Text("Hip Offset: %.4f", ik.runtimeHipOffset);
                        ImGui::Text("Skinning Upload: %s",
                                    ik.runtimeSkinningUploaded ? "yes" : "no");
                    }

                    // ── Bones ─────────────────────────────────────────────────
                    if (chain.type == scene::IKSolverType::TwoBone ||
                        chain.type == scene::IKSolverType::HandPlace) {
                        if (chain.boneNames.size() != 3) chain.boneNames.resize(3);
                        ImGui::SeparatorText("Bones");
                        ImGui::TextDisabled("Drag from Hierarchy or type name");
                        {
                        char buf[256];
                        std::snprintf(buf, sizeof(buf), "%s", chain.boneNames[0].c_str());
                        if (ImGui::InputText("Root", buf, sizeof(buf)))
                            chain.boneNames[0] = buf;
                        if (auto* dropped = AcceptHierarchyDrop(ctx.activeScene))
                            chain.boneNames[0] = dropped->name;

                        std::snprintf(buf, sizeof(buf), "%s", chain.boneNames[1].c_str());
                        if (ImGui::InputText("Mid",  buf, sizeof(buf)))
                            chain.boneNames[1] = buf;
                        if (auto* dropped = AcceptHierarchyDrop(ctx.activeScene))
                            chain.boneNames[1] = dropped->name;

                        std::snprintf(buf, sizeof(buf), "%s", chain.boneNames[2].c_str());
                        if (ImGui::InputText("Tip",  buf, sizeof(buf)))
                            chain.boneNames[2] = buf;
                        if (auto* dropped = AcceptHierarchyDrop(ctx.activeScene))
                            chain.boneNames[2] = dropped->name;
                    }

                    // ── Targets ───────────────────────────────────────────────
                    // WHY: Target/Pole は名前文字列で保持し、Resolve ボタンで EntityID を
                    //      解決する。解決状態を色付きドットで即座に確認できる。
                    ImGui::SeparatorText("Targets");
                    {
                        char buf[256];
                        const float resolveW = ImGui::CalcTextSize("Resolve").x
                                             + ImGui::GetStyle().FramePadding.x * 2.0f;
                        const float dotW     = ImGui::GetFrameHeight();

                        // WHY: guid を追加することでリネーム後も参照が壊れなくなる。
                        //      手入力時は guid をクリアし名前フォールバックで解決させる。
                        //      ドロップ・Resolve 時は dropped/found の instanceId を記録する。
                        auto DrawObjectField = [&](const char* label,
                                                   const char* idStr,
                                                   std::string& name,
                                                   std::string& guid,
                                                   scene::EntityID& eid)
                        {
                            const bool resolved = eid.IsValid();
                            // 解決状態ドット (緑=OK / 赤=未解決)
                            const ImVec4 dotColor = resolved
                                ? ImVec4(0.2f, 0.8f, 0.2f, 1.0f)
                                : ImVec4(0.8f, 0.2f, 0.2f, 1.0f);
                            ImGui::TextColored(dotColor, resolved ? "●" : "○");
                            if (ImGui::IsItemHovered())
                                ImGui::SetTooltip(resolved ? "Resolved" : "Not resolved — click Resolve");
                            ImGui::SameLine();
                            ImGui::SetNextItemWidth(
                                ImGui::GetContentRegionAvail().x - resolveW
                                - ImGui::GetStyle().ItemSpacing.x);
                            std::snprintf(buf, sizeof(buf), "%s", name.c_str());
                            if (ImGui::InputText(idStr, buf, sizeof(buf))) {
                                name = buf;
                                guid.clear(); // 手入力時は GUID をクリアして名前で再解決させる
                            }
                            if (auto* dropped = AcceptHierarchyDrop(ctx.activeScene)) {
                                name = dropped->name;
                                guid = dropped->instanceId;
                                eid  = dropped->GetID();
                            }
                            ImGui::SameLine();
                            if (ImGui::SmallButton(label)) {
                                if (ctx.activeScene) {
                                    auto* found = ctx.activeScene->Find(name);
                                    eid  = found ? found->GetID()    : scene::EntityID::INVALID;
                                    guid = found ? found->instanceId : std::string{};
                                }
                            }
                        };

                        DrawObjectField("Resolve##t", "##tgt",  chain.targetName, chain.targetGuid, chain.targetEntity);
                        ImGui::SameLine();
                        ImGui::TextUnformatted("Target");

                        // Auto Pole: ON のとき Pole フィールドを非活性化し、IKSystem が自動計算する。
                        ImGui::Checkbox("Auto Pole##ap", &chain.autoPole);
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip(
                                "Automatically compute pole direction from owner rotation or FK bend angle.\n"
                                "No Pole GameObject needed. Pole field is ignored when enabled.");
                        if (chain.autoPole) {
                            widgets::DragVec3("Auto Pole Local Dir",
                                              chain.autoPoleLocalDirection,
                                              0.01f,
                                              -1.0f,
                                              1.0f);
                            if (ImGui::IsItemHovered())
                                ImGui::SetTooltip(
                                    "Owner-local knee direction. Zero uses FK bend direction.\n"
                                    "Example: Player with 180 yaw offset can use +Z or -Z depending on rig forward.");
                        }
                        if (chain.autoPole)
                            ImGui::BeginDisabled();
                        DrawObjectField("Resolve##p", "##pole", chain.poleName,   chain.poleGuid,   chain.poleEntity);
                        ImGui::SameLine();
                        ImGui::TextUnformatted("Pole");
                        if (chain.autoPole)
                            ImGui::EndDisabled();
                    }

                    // ── Settings ──────────────────────────────────────────────
                        ImGui::SeparatorText("Settings");
                        ImGui::DragFloat("Max Extension", &chain.maxExtension,
                                         0.005f, 0.5f, 1.0f, "%.3f");
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("Limits how far the chain can stretch (ratio of total bone length)");
                        ImGui::DragFloat("Softness", &chain.softness,
                                         0.005f, 0.0f, 0.5f, "%.3f");
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("Exponential ease-out before max extension (0 = off)");
                        ImGui::DragFloatRange2("Bend Angle", &chain.minBendAngleDegrees,
                                               &chain.maxBendAngleDegrees,
                                               1.0f, 0.0f, 179.0f,
                                               "Min %.1f deg", "Max %.1f deg");
                        widgets::DragVec3("Target Offset", chain.targetOffset, 0.001f, 0.0f, 0.0f);
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("World-space offset added to the target position");
                        if (chain.type == scene::IKSolverType::HandPlace) {
                            ImGui::SeparatorText("Hand Placement");
                            widgets::DragQuatEuler3(
                                "Rotation Offset", chain.handRotationOffset, 0.5f);
                            ImGui::DragFloat("Rotation Weight", &chain.handRotationWeight,
                                             0.01f, 0.0f, 1.0f, "%.2f");
                        }
                    }

                    if (chain.type == scene::IKSolverType::AimAt ||
                        chain.type == scene::IKSolverType::FABRIK) {
                        auto DrawTargetField = [&]() {
                            char targetName[256];
                            std::snprintf(targetName, sizeof(targetName), "%s", chain.targetName.c_str());
                            if (ImGui::InputText("Target", targetName, sizeof(targetName))) {
                                chain.targetName = targetName;
                                chain.targetGuid.clear();
                                chain.targetEntity = scene::EntityID::INVALID;
                            }
                            if (auto* dropped = AcceptHierarchyDrop(ctx.activeScene)) {
                                chain.targetName = dropped->name;
                                chain.targetGuid = dropped->instanceId;
                                chain.targetEntity = dropped->GetID();
                            }
                            ImGui::SameLine();
                            if (ImGui::SmallButton("Resolve##solverTarget") && ctx.activeScene) {
                                scene::GameObject* found = nullptr;
                                if (!chain.targetGuid.empty())
                                    found = ctx.activeScene->FindByGuid(chain.targetGuid);
                                if (!found && !chain.targetName.empty())
                                    found = ctx.activeScene->Find(chain.targetName);
                                chain.targetEntity = found ? found->GetID() : scene::EntityID::INVALID;
                                if (found) {
                                    chain.targetName = found->name;
                                    chain.targetGuid = found->instanceId;
                                }
                            }
                        };

                        ImGui::SeparatorText(
                            chain.type == scene::IKSolverType::AimAt ? "Aim At" : "FABRIK");
                        if (chain.type == scene::IKSolverType::AimAt) {
                            if (chain.boneNames.size() != 1) chain.boneNames.resize(1);
                            char boneName[256];
                            std::snprintf(boneName, sizeof(boneName), "%s", chain.boneNames[0].c_str());
                            if (ImGui::InputText("Bone", boneName, sizeof(boneName)))
                                chain.boneNames[0] = boneName;
                            if (auto* dropped = AcceptHierarchyDrop(ctx.activeScene))
                                chain.boneNames[0] = dropped->name;
                        } else {
                            int removeBone = -1;
                            for (int boneIndex = 0;
                                 boneIndex < static_cast<int>(chain.boneNames.size());
                                 ++boneIndex) {
                                ImGui::PushID(boneIndex);
                                char boneName[256];
                                std::snprintf(boneName, sizeof(boneName), "%s",
                                              chain.boneNames[static_cast<size_t>(boneIndex)].c_str());
                                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 28.0f);
                                if (ImGui::InputText("##spineBone", boneName, sizeof(boneName)))
                                    chain.boneNames[static_cast<size_t>(boneIndex)] = boneName;
                                if (auto* dropped = AcceptHierarchyDrop(ctx.activeScene))
                                    chain.boneNames[static_cast<size_t>(boneIndex)] = dropped->name;
                                ImGui::SameLine();
                                if (ImGui::SmallButton("x")) removeBone = boneIndex;
                                ImGui::PopID();
                            }
                            if (removeBone >= 0)
                                chain.boneNames.erase(chain.boneNames.begin() + removeBone);
                            if (ImGui::Button("+ Spine Bone", { -1.0f, 0.0f }))
                                chain.boneNames.emplace_back();
                        }

                        DrawTargetField();
                        widgets::DragVec3("Target Offset", chain.targetOffset, 0.001f, 0.0f, 0.0f);
                        if (chain.type == scene::IKSolverType::FABRIK) {
                            ImGui::SeparatorText("Auto Slope Weight");
                            ImGui::Checkbox("Auto Slope Weight", &chain.spineAutoWeight);
                            if (chain.spineAutoWeight) {
                                ImGui::DragFloat("Flat Weight", &chain.spineFlatWeight,
                                                 0.005f, 0.0f, 1.0f, "%.3f");
                                ImGui::DragFloat("Slope Ramp (m)", &chain.spineSlopeRampMeters,
                                                 0.005f, 0.001f, 1.0f, "%.3f m");
                                ImGui::TextDisabled("Flat %.3f -> Slope %.3f at %.3f m",
                                    chain.spineFlatWeight, chain.weight,
                                    chain.spineSlopeRampMeters);
                            }
                        }
                        if (chain.type == scene::IKSolverType::AimAt) {
                            widgets::DragVec3("Look Axis", chain.lookAtAxis, 0.01f, -1.0f, 1.0f);
                            widgets::DragVec3("Up Axis", chain.lookAtUpAxis, 0.01f, -1.0f, 1.0f);
                            ImGui::DragFloat("Clamp Angle", &chain.lookAtClampAngle,
                                             1.0f, 0.0f, 180.0f, "%.1f deg");
                            ImGui::DragFloat("Response Speed", &chain.lookAtSpeed,
                                             0.1f, 0.0f, 100.0f, "%.1f");
                        }
                    }

                    if (chain.type == scene::IKSolverType::FootPlace) {
                        ImGui::SeparatorText("Foot Placement");
                        ImGui::Checkbox("Use Animator IK Weight", &chain.useAnimatorIKWeight);
                        ImGui::DragFloat("Ray Up Ratio", &chain.rayUpRatio, 0.01f, 0.0f, 2.0f, "%.2f");
                        ImGui::DragFloat("Ray Down Ratio", &chain.rayDownRatio, 0.01f, 0.0f, 4.0f, "%.2f");
                        ImGui::DragFloat("Surface Offset", &chain.footSurfaceOffset, 0.001f, 0.0f, 0.5f, "%.3f");
                        ImGui::DragFloat("Dead Zone", &chain.correctionDeadZone, 0.001f, 0.0f, 0.25f, "%.3f");
                        ImGui::DragFloat("Max Correction", &chain.maxCorrection, 0.001f, 0.0f, 1.0f, "%.3f");
                        ImGui::DragFloat("Plant Distance", &chain.footPlantDistance,
                                         0.001f, 0.0f, 1.0f, "%.3f");
                        ImGui::DragFloat("Smooth Time", &chain.smoothTime, 0.005f, 0.001f, 1.0f, "%.3f s");
                        ImGui::DragFloat("Max Extension", &chain.maxExtension, 0.005f, 0.5f, 1.0f, "%.3f");
                        ImGui::DragFloat("Softness", &chain.softness, 0.005f, 0.0f, 0.5f, "%.3f");
                        ImGui::Checkbox("Adjust Hip", &chain.adjustHip);
                        if (chain.adjustHip) {
                            char hipName[128];
                            std::snprintf(hipName, sizeof(hipName), "%s", chain.hipBoneName.c_str());
                            if (ImGui::InputText("Hip Bone", hipName, sizeof(hipName)))
                                chain.hipBoneName = hipName;
                        }
                        widgets::DragVec3("Foot Normal Axis", chain.footNormalAxis, 0.01f, -1.0f, 1.0f);
                    }

                    // ── Ground Snap ───────────────────────────────────────────
                    if (!chain.enabled)
                        ImGui::PopStyleVar();

                    ImGui::TreePop();
                }

                ImGui::PopID();
                ImGui::Spacing();
            }

            // チェーン削除
            if (removeIdx >= 0)
                ik.chains.erase(ik.chains.begin() + removeIdx);

            // チェーン追加
            if (ImGui::Button("+ Add Chain", { -1.0f, 0.0f })) {
                scene::IKChain chain;
                chain.enabled = true;
                ik.chains.push_back(std::move(chain));
            }
        });

}


} // namespace fbzz::editor
