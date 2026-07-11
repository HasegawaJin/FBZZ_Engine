// FBZZ Engine
// InspectorAnimation.cpp | fbzz::editor
// Animation / IK 系 Component の Inspector 描画
#include "InspectorAnimation.hpp"
#include <Engine/Asset/AnimatorControllerAsset.hpp>

namespace fbzz::editor {

void DrawAnimationInspectors(scene::GameObject* go, EditorContext& ctx, std::any& m_componentClipboard, const std::type_info*& m_componentClipboardType)
{
    DrawComponentSection<scene::AnimatorComponent>(go, ctx, m_componentClipboard, m_componentClipboardType, "Animator",
        [go](scene::AnimatorComponent& anim, EditorContext& ctx) {
            ImGui::SeparatorText("Controller");
            if (widgets::AssetPathField("Animator Controller", anim.controllerPath,
                                        ".animcontroller", ctx.projectRoot)) {
                anim.loadedControllerPath.clear();
                if (ctx.markSceneDirty) ctx.markSceneDirty();
            }
            if (ImGui::Button(
                    "Create Controller From Current", ImVec2(-1.0f, 0.0f))) {
                const std::string directory =
                    ctx.projectRoot + "/Assets/Animation";
                util::FileSystem::EnsureDirectory(directory);
                std::string safeName = go->name.empty() ? "Animator" : go->name;
                for (char& character : safeName) {
                    const bool valid =
                        std::isalnum(static_cast<unsigned char>(character)) ||
                        character == '_' || character == '-';
                    if (!valid) character = '_';
                }
                std::string path =
                    directory + "/" + safeName + ".animcontroller";
                for (int suffix = 1;
                     util::FileSystem::Exists(path) && suffix < 10000;
                     ++suffix) {
                    path = directory + "/" + safeName + " " +
                        std::to_string(suffix) + ".animcontroller";
                }
                const auto controller =
                    asset::MakeAnimatorControllerAsset(anim);
                if (asset::SaveAnimatorControllerAsset(path, controller)) {
                    anim.controllerPath = NormalizeAssetPath(path);
                    anim.loadedControllerPath = anim.controllerPath;
                    ctx.selectedAssetPath = path;
                    ctx.requestAssetBrowserRefresh = true;
                    if (ctx.markSceneDirty) ctx.markSceneDirty();
                }
            }
            ImGui::Separator();

            const bool usesController = !anim.controllerPath.empty();
            if (!usesController) {
            // --- Clip Sources list ---
            ImGui::Text("Clip Sources");

            for (int i = 0; i < static_cast<int>(anim.clipSources.size()); ++i) {
                ImGui::PushID(i);
                char buf[256];
                std::snprintf(buf, sizeof(buf), "%s", anim.clipSources[i].c_str());
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 24.0f);
                if (ImGui::InputText("##src", buf, sizeof(buf)))
                    anim.clipSources[i] = NormalizeAssetPath(buf);
                if (ImGui::IsItemDeactivatedAfterEdit())
                    { anim.clips.clear(); anim.clipSourcePaths.clear(); anim.clipsLoaded = false; }
                // DragDrop target must be right after InputText, before SameLine
                if (widgets::AcceptAssetPathDrop(anim.clipSources[i])) {
                    anim.clips.clear(); anim.clipsLoaded = false;
                    anim.clipSourcePaths.clear();
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("x")) {
                    anim.clipSources.erase(anim.clipSources.begin() + i);
                    anim.clips.clear(); anim.clipsLoaded = false;
                    anim.clipSourcePaths.clear();
                    ImGui::PopID(); break;
                }
                ImGui::PopID();
            }
            // "+ Add Source" also acts as drop zone: drag FBX directly onto it
            if (ImGui::Button("+ Add Source  (or drop FBX)", { -1.0f, 0.0f }))
                anim.clipSources.emplace_back();
            std::string droppedClip;
            if (widgets::AcceptAssetPathDrop(droppedClip)) {
                anim.clipSources.push_back(std::move(droppedClip));
                anim.clips.clear(); anim.clipsLoaded = false;
                anim.clipSourcePaths.clear();
            }

            ImGui::Separator();

            // --- Clip selector ---
            if (!anim.clips.empty()) {
                const int clipCount = static_cast<int>(anim.clips.size());
                int sel = std::clamp(anim.clipIndex, 0, clipCount - 1);
                std::vector<const char*> names;
                names.reserve(static_cast<size_t>(clipCount));
                for (const auto& c : anim.clips) names.push_back(c.name.c_str());
                if (ImGui::Combo("Clip", &sel, names.data(), clipCount)) {
                    anim.clipIndex = sel;
                    anim.clipName  = anim.clips[static_cast<size_t>(sel)].name;
                    anim.time = 0.0f;
                }
                const auto& cur = anim.clips[static_cast<size_t>(sel)];
                const float dur  = static_cast<float>(cur.GetDurationSeconds());
                const float t    = (dur > 0.0f) ? std::clamp(anim.time / dur, 0.0f, 1.0f) : 0.0f;
                char overlay[32];
                std::snprintf(overlay, sizeof(overlay), "%.2f / %.2fs", anim.time, dur);
                ImGui::ProgressBar(t, { -1.0f, 0.0f }, overlay);
                ImGui::TextDisabled("%d clip(s) | %d tracks", clipCount,
                                    static_cast<int>(cur.tracks.size()));
            } else if (anim.clipsLoaded) {
                ImGui::TextDisabled("No clips loaded");
            } else {
                ImGui::TextDisabled("(clips not loaded yet)");
            }
            }

            ImGui::DragFloat("Speed", &anim.speed, 0.01f, -10.0f, 10.0f);
            ImGui::Checkbox("Playing", &anim.playing);
            // Time / Loop はステートマシン未使用時のみ表示する（ステートマシン使用時は per-state で管理）
            if (anim.states.empty()) {
                ImGui::DragFloat("Time", &anim.time, 0.01f, 0.0f, 100000.0f);
                ImGui::Checkbox("Loop", &anim.loop);
            }

            // ── ステートマシン UI ────────────────────────────────────────────────
            if (usesController && !anim.currentStateName.empty()) {
                ImGui::SeparatorText("Runtime");
                ImGui::Text("Current: %s", anim.currentStateName.c_str());
                const float normalizedTime = anim.GetNormalizedTime();
                ImGui::ProgressBar(normalizedTime, { -1.0f, 0.0f });
                if (!anim.blendToState.empty()) {
                    ImGui::TextDisabled(
                        "-> %s  (blend: %.0f%%, %.3f / %.3f s)",
                        anim.blendToState.c_str(),
                        anim.blendWeight * 100.0f,
                        anim.blendWeight * anim.blendDuration,
                        anim.blendDuration);
                }
                ImGui::TextDisabled(
                    "Edit nodes, sources, parameters and transitions in Animation Graph.");
            }

            if (!usesController) {
                ImGui::Separator();
                ImGui::TextColored({ 0.9f, 0.7f, 0.2f, 1.0f }, "State Machine");

                // ── ランタイム状態表示 ─────────────────────────────────────────
                if (!anim.currentStateName.empty()) {
                    ImGui::Text("Current: %s", anim.currentStateName.c_str());
                    const float nt = anim.GetNormalizedTime();
                    char overlay[64];
                    std::snprintf(overlay, sizeof(overlay), "%.2f", nt);
                    ImGui::ProgressBar(nt, { -1.0f, 0.0f }, overlay);
                    if (!anim.blendToState.empty()) {
                        ImGui::TextDisabled(
                            " -> %s  (blend: %.0f%%, %.3f / %.3f s)",
                            anim.blendToState.c_str(),
                            anim.blendWeight * 100.0f,
                            anim.blendWeight * anim.blendDuration,
                            anim.blendDuration);
                    }
                    ImGui::Separator();
                }

                // ── Default State コンボ ──────────────────────────────────────
                if (!anim.states.empty()) {
                    int defIdx = 0;
                    std::vector<const char*> stateNames;
                    stateNames.reserve(anim.states.size());
                    for (int si = 0; si < static_cast<int>(anim.states.size()); ++si) {
                        stateNames.push_back(anim.states[static_cast<size_t>(si)].name.c_str());
                        if (anim.states[static_cast<size_t>(si)].name == anim.defaultStateName)
                            defIdx = si;
                    }
                    if (ImGui::Combo("Default State", &defIdx,
                                     stateNames.data(), static_cast<int>(stateNames.size()))) {
                        anim.defaultStateName  = anim.states[static_cast<size_t>(defIdx)].name;
                        anim.currentStateName  = "";  // 再初期化トリガー
                    }
                }

                // ── Parameters ────────────────────────────────────────────────
                ImGui::Separator();
                if (ImGui::CollapsingHeader("Parameters")) {
                    static const char* kParamTypes[] = { "Float", "Int", "Bool", "Trigger" };
                    int removeParamIdx = -1;

                    for (int pi = 0; pi < static_cast<int>(anim.parameters.size()); ++pi) {
                        auto& param = anim.parameters[static_cast<size_t>(pi)];
                        ImGui::PushID(pi);

                        // 型コンボ（幅を絞る）
                        ImGui::SetNextItemWidth(70.0f);
                        int typeIdx = static_cast<int>(param.type);
                        if (ImGui::Combo("##ptype", &typeIdx, kParamTypes, 4))
                            param.type = static_cast<scene::ParamType>(typeIdx);
                        ImGui::SameLine();

                        // 名前入力
                        char buf[64];
                        std::snprintf(buf, sizeof(buf), "%s", param.name.c_str());
                        ImGui::SetNextItemWidth(100.0f);
                        if (ImGui::InputText("##pname", buf, sizeof(buf)))
                            param.name = buf;
                        ImGui::SameLine();

                        // 値ウィジェット
                        switch (param.type) {
                        case scene::ParamType::Float:
                            ImGui::SetNextItemWidth(80.0f);
                            ImGui::DragFloat("##pval", &param.floatValue, 0.01f);
                            break;
                        case scene::ParamType::Int:
                            ImGui::SetNextItemWidth(80.0f);
                            ImGui::DragInt("##pval", &param.intValue);
                            break;
                        case scene::ParamType::Bool:
                            ImGui::Checkbox("##pval", &param.boolValue);
                            break;
                        case scene::ParamType::Trigger:
                            if (ImGui::SmallButton("Fire"))
                                param.boolValue = true;
                            break;
                        }
                        ImGui::SameLine();

                        if (ImGui::SmallButton("x"))
                            removeParamIdx = pi;

                        ImGui::PopID();
                    }
                    if (removeParamIdx >= 0)
                        anim.parameters.erase(anim.parameters.begin() + removeParamIdx);

                    // "+ Add Parameter" ボタン（型コンボ付き）
                    static int s_newParamType = 0;
                    ImGui::SetNextItemWidth(70.0f);
                    ImGui::Combo("##newptype", &s_newParamType, kParamTypes, 4);
                    ImGui::SameLine();
                    if (ImGui::Button("+ Add Parameter")) {
                        scene::AnimatorParameter p;
                        p.name = "NewParam";
                        p.type = static_cast<scene::ParamType>(s_newParamType);
                        anim.parameters.push_back(std::move(p));
                    }
                }

                // ── States ────────────────────────────────────────────────────
                ImGui::Separator();
                if (ImGui::CollapsingHeader("States")) {
                    // 利用可能なクリップ名リスト（Clip コンボ用）
                    std::vector<const char*> clipNames;
                    clipNames.push_back("(none)");
                    for (const auto& c : anim.clips)
                        clipNames.push_back(c.name.c_str());

                    // 利用可能なステート名リスト（遷移先コンボ用）
                    std::vector<const char*> stateNamesForTrans;
                    for (const auto& s : anim.states)
                        stateNamesForTrans.push_back(s.name.c_str());

                    static const char* kOpNames[] = {
                        "Greater", "Less", "Equal", "NotEqual", "True", "False"
                    };

                    int removeStateIdx = -1;
                    for (int si = 0; si < static_cast<int>(anim.states.size()); ++si) {
                        auto& st = anim.states[static_cast<size_t>(si)];
                        ImGui::PushID(si);

                        const bool isCurrent = (st.name == anim.currentStateName);
                        if (isCurrent)
                            ImGui::PushStyleColor(ImGuiCol_Header, { 0.3f, 0.6f, 0.3f, 1.0f });

                        const bool open = ImGui::CollapsingHeader(st.name.c_str());

                        if (isCurrent) ImGui::PopStyleColor();

                        if (open) {
                            ImGui::Indent();

                            // State 名入力
                            char nameBuf[64];
                            std::snprintf(nameBuf, sizeof(nameBuf), "%s", st.name.c_str());
                            if (ImGui::InputText("Name", nameBuf, sizeof(nameBuf))) {
                                // defaultStateName / currentStateName も追随して更新する
                                if (anim.defaultStateName == st.name)
                                    anim.defaultStateName = nameBuf;
                                if (anim.currentStateName == st.name)
                                    anim.currentStateName = nameBuf;
                                st.name = nameBuf;
                            }

                            static const char* kStateModes[] = {
                                "Clip", "Blend Tree 1D", "Blend Tree 2D"
                            };
                            int stateMode = static_cast<int>(st.mode);
                            if (ImGui::Combo("Mode", &stateMode, kStateModes, 3))
                                st.mode = static_cast<scene::AnimationStateMode>(stateMode);

                            if (st.mode == scene::AnimationStateMode::Clip) {
                                int clipSel = 0;
                                for (int ci = 1; ci < static_cast<int>(clipNames.size()); ++ci)
                                    if (st.clipName == clipNames[static_cast<size_t>(ci)])
                                        { clipSel = ci; break; }
                                if (ImGui::Combo("Clip", &clipSel,
                                                 clipNames.data(),
                                                 static_cast<int>(clipNames.size()))) {
                                    st.clipName = (clipSel == 0)
                                        ? ""
                                        : clipNames[static_cast<size_t>(clipSel)];
                                }
                                ImGui::DragFloat("IK Weight##st", &st.ikWeight,
                                                 0.01f, 0.0f, 1.0f);
                            } else {
                                auto drawMotions = [&](std::vector<scene::BlendTreeMotion>& motions,
                                                       bool is2D) {
                                    int removeMotion = -1;
                                    for (int mi = 0; mi < static_cast<int>(motions.size()); ++mi) {
                                        auto& motion = motions[static_cast<size_t>(mi)];
                                        ImGui::PushID(mi);
                                        char motionClip[128]{};
                                        std::snprintf(motionClip, sizeof(motionClip), "%s",
                                                      motion.clipName.c_str());
                                        if (ImGui::InputText("Motion Clip", motionClip,
                                                             sizeof(motionClip)))
                                            motion.clipName = motionClip;
                                        if (is2D) {
                                            ImGui::DragFloat("X", &motion.posX, 0.01f);
                                            ImGui::SameLine();
                                            ImGui::DragFloat("Y", &motion.posY, 0.01f);
                                        } else {
                                            ImGui::DragFloat(
                                                "Threshold", &motion.threshold, 0.01f);
                                        }
                                        ImGui::DragFloat(
                                            "Motion Speed", &motion.speed,
                                            0.01f, -10.0f, 10.0f);
                                        ImGui::DragFloat(
                                            "Motion IK", &motion.ikWeight,
                                            0.01f, 0.0f, 1.0f);
                                        if (ImGui::SmallButton("Remove Motion"))
                                            removeMotion = mi;
                                        ImGui::Separator();
                                        ImGui::PopID();
                                    }
                                    if (removeMotion >= 0)
                                        motions.erase(motions.begin() + removeMotion);
                                    if (ImGui::Button("+ Motion"))
                                        motions.emplace_back();
                                };

                                if (st.mode == scene::AnimationStateMode::BlendTree1D) {
                                    char paramName[96]{};
                                    std::snprintf(paramName, sizeof(paramName), "%s",
                                                  st.blendTree1D.paramName.c_str());
                                    if (ImGui::InputText(
                                            "Blend Parameter", paramName, sizeof(paramName)))
                                        st.blendTree1D.paramName = paramName;
                                    drawMotions(st.blendTree1D.motions, false);
                                } else {
                                    char paramX[96]{};
                                    char paramY[96]{};
                                    std::snprintf(paramX, sizeof(paramX), "%s",
                                                  st.blendTree2D.paramX.c_str());
                                    std::snprintf(paramY, sizeof(paramY), "%s",
                                                  st.blendTree2D.paramY.c_str());
                                    if (ImGui::InputText("Parameter X", paramX, sizeof(paramX)))
                                        st.blendTree2D.paramX = paramX;
                                    if (ImGui::InputText("Parameter Y", paramY, sizeof(paramY)))
                                        st.blendTree2D.paramY = paramY;
                                    static const char* kBlend2DTypes[] = {
                                        "Simple Directional", "Freeform Cartesian"
                                    };
                                    int blendType = static_cast<int>(st.blendTree2D.type);
                                    if (ImGui::Combo(
                                            "2D Type", &blendType, kBlend2DTypes, 2))
                                        st.blendTree2D.type =
                                            static_cast<scene::BlendTree2DType>(blendType);
                                    drawMotions(st.blendTree2D.motions, true);
                                }
                            }
                            ImGui::DragFloat("Speed##st", &st.speed, 0.01f, -10.0f, 10.0f);
                            ImGui::Checkbox("Loop##st", &st.loop);

                            // ── Transitions ──────────────────────────────────
                            ImGui::Separator();
                            ImGui::Text("Transitions");
                            int removeTrIdx = -1;
                            for (int ti = 0; ti < static_cast<int>(st.transitions.size()); ++ti) {
                                auto& tr = st.transitions[static_cast<size_t>(ti)];
                                ImGui::PushID(ti);

                                // 遷移先コンボ
                                int toIdx = 0;
                                for (int xi = 0; xi < static_cast<int>(stateNamesForTrans.size()); ++xi)
                                    if (tr.toStateName == stateNamesForTrans[static_cast<size_t>(xi)])
                                        { toIdx = xi; break; }
                                ImGui::SetNextItemWidth(120.0f);
                                if (ImGui::Combo("->##to", &toIdx,
                                                 stateNamesForTrans.data(),
                                                 static_cast<int>(stateNamesForTrans.size())))
                                    tr.toStateName = stateNamesForTrans[static_cast<size_t>(toIdx)];

                                ImGui::SameLine();
                                ImGui::Checkbox("ExitTime", &tr.hasExitTime);
                                if (tr.hasExitTime) {
                                    ImGui::SameLine();
                                    ImGui::SetNextItemWidth(60.0f);
                                    ImGui::DragFloat("##et", &tr.exitTime, 0.01f, 0.0f, 1.0f);
                                }
                                ImGui::SetNextItemWidth(80.0f);
                                ImGui::Checkbox("Fixed Duration", &tr.fixedDuration);
                                ImGui::SetNextItemWidth(110.0f);
                                ImGui::DragFloat(
                                    tr.fixedDuration ? "Duration (s)" : "Duration (Normalized)",
                                    &tr.transitionDuration, 0.01f, 0.0f,
                                    tr.fixedDuration ? 5.0f : 2.0f);

                                // 条件リスト
                                ImGui::Indent();
                                int removeCondIdx = -1;
                                for (int ci = 0; ci < static_cast<int>(tr.conditions.size()); ++ci) {
                                    auto& cond = tr.conditions[static_cast<size_t>(ci)];
                                    ImGui::PushID(ci);

                                    // パラメーター名コンボ
                                    std::vector<const char*> paramNamesList;
                                    for (const auto& pp : anim.parameters)
                                        paramNamesList.push_back(pp.name.c_str());
                                    int pIdx = 0;
                                    for (int xi = 0; xi < static_cast<int>(paramNamesList.size()); ++xi)
                                        if (cond.paramName == paramNamesList[static_cast<size_t>(xi)])
                                            { pIdx = xi; break; }
                                    ImGui::SetNextItemWidth(90.0f);
                                    if (!paramNamesList.empty() &&
                                        ImGui::Combo("##cp", &pIdx,
                                                     paramNamesList.data(),
                                                     static_cast<int>(paramNamesList.size())))
                                        cond.paramName = paramNamesList[static_cast<size_t>(pIdx)];
                                    ImGui::SameLine();

                                    // 演算子コンボ
                                    int opIdx = static_cast<int>(cond.op);
                                    ImGui::SetNextItemWidth(70.0f);
                                    if (ImGui::Combo("##cop", &opIdx, kOpNames, 6))
                                        cond.op = static_cast<scene::ConditionOp>(opIdx);
                                    ImGui::SameLine();

                                    // 閾値（Greater/Less/Equal/NotEqual のとき表示）
                                    if (opIdx < 4) {
                                        ImGui::SetNextItemWidth(60.0f);
                                        ImGui::DragFloat("##cth", &cond.threshold, 0.01f);
                                        ImGui::SameLine();
                                    }
                                    if (ImGui::SmallButton("x##cond"))
                                        removeCondIdx = ci;

                                    ImGui::PopID();
                                }
                                if (removeCondIdx >= 0)
                                    tr.conditions.erase(tr.conditions.begin() + removeCondIdx);

                                if (ImGui::SmallButton("+ Condition")) {
                                    scene::AnimatorCondition c;
                                    if (!anim.parameters.empty())
                                        c.paramName = anim.parameters[0].name;
                                    tr.conditions.push_back(std::move(c));
                                }
                                ImGui::Unindent();

                                ImGui::SameLine();
                                if (ImGui::SmallButton("x##tr"))
                                    removeTrIdx = ti;

                                ImGui::PopID();
                            }
                            if (removeTrIdx >= 0)
                                st.transitions.erase(st.transitions.begin() + removeTrIdx);

                            if (ImGui::Button("+ Add Transition")) {
                                scene::AnimationTransition tr;
                                if (!anim.states.empty())
                                    tr.toStateName = anim.states[0].name;
                                st.transitions.push_back(std::move(tr));
                            }

                            ImGui::Separator();
                            if (ImGui::SmallButton("Remove State"))
                                removeStateIdx = si;

                            ImGui::Unindent();
                        }
                        ImGui::PopID();
                    }
                    if (removeStateIdx >= 0)
                        anim.states.erase(anim.states.begin() + removeStateIdx);

                    if (ImGui::Button("+ Add State")) {
                        scene::AnimationState newSt;
                        newSt.name = "NewState";
                        if (anim.defaultStateName.empty())
                            anim.defaultStateName = newSt.name;
                        anim.states.push_back(std::move(newSt));
                    }
                }
            }
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
                ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.6f, 0.15f, 0.15f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.8f, 0.25f, 0.25f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.9f, 0.10f, 0.10f, 1.0f));
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
