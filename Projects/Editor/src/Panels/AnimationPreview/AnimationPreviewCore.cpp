/// @file    AnimationPreviewCore.cpp
/// @brief   Animation プレビューの対象解決・ポーズ評価・オフスクリーン描画。
/// @author  Hasegawa Jin
/// @date    2026-09-16
///
/// @note 選択中の State / Transition / .anim / モデルからクリップを解決し、
/// @note スキンメッシュを SkinnedLit でレンダーターゲットへ毎フレーム描画する。
/// @note UI (ツールバー・トランスポート・オーバーレイ) は AnimationPreviewUI.cpp が持つ。
#include "AnimationPreviewInternal.hpp"
#include <Editor/Util/AssetPath.hpp>
#include <Editor/Util/EditorSettings.hpp>
#include <Editor/Util/ModelPlacement.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/MaterialAsset.hpp>
#include <Engine/Asset/MaterialParamBinding.hpp>
#include <Engine/Asset/ModelAsset.hpp>
#include <Engine/Asset/TextureAsset.hpp>
#include <Engine/Renderer/Camera.hpp>
#include <Engine/Renderer/DrawCall.hpp>
#include <Engine/Renderer/IRenderer.hpp>
#include <Engine/Renderer/LightSystem.hpp>
#include <Engine/Renderer/Mesh.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Renderer/ShaderDescriptor.hpp>
#include <Engine/Scene/Components/SkinnedMeshRenderer.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Engine/Util/StringUtils.hpp>
#include <algorithm>
#include <bit>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace fbzz::editor::animpreview {

PreviewState         g_state;
PreviewGpu           g_gpu;
PreviewLineGpu       g_lineGpu;
PreviewMaterialCache g_materialCache;
MaskPreviewState     g_maskPreview;
MaskPreviewRequest   g_maskPreviewRequest;

namespace {

/// @name クリップサンプリング
/// @note AnimatorSystem.cpp と同じ規約 (tick 空間サンプル + FBX チャンネル名の正規化)。
/// @note ランタイムは Scene の AnimatorComponent と密結合するため、プレビューは Skeleton + Clip だけで完結する軽量版をここに持つ。

math::Vector3 SampleVectorKeys(const std::vector<asset::VectorKey>& keys,
                               double ticks,
                               const math::Vector3& fallback)
{
    if (keys.empty()) return fallback;
    if (keys.size() == 1 || ticks <= keys.front().time) return keys.front().value;
    if (ticks >= keys.back().time) return keys.back().value;

    for (size_t i = 0; i + 1 < keys.size(); ++i) {
        const auto& a = keys[i];
        const auto& b = keys[i + 1];
        if (ticks < a.time || ticks > b.time) continue;
        const double span = b.time - a.time;
        const float t = span > 0.0 ? static_cast<float>((ticks - a.time) / span) : 0.0f;
        return math::Vector3::Lerp(a.value, b.value, t);
    }
    return keys.back().value;
}

math::Quaternion SampleQuaternionKeys(const std::vector<asset::QuaternionKey>& keys,
                                      double ticks,
                                      const math::Quaternion& fallback)
{
    if (keys.empty()) return fallback;
    if (keys.size() == 1 || ticks <= keys.front().time) return keys.front().value;
    if (ticks >= keys.back().time) return keys.back().value;

    for (size_t i = 0; i + 1 < keys.size(); ++i) {
        const auto& a = keys[i];
        const auto& b = keys[i + 1];
        if (ticks < a.time || ticks > b.time) continue;
        const double span = b.time - a.time;
        const float t = span > 0.0 ? static_cast<float>((ticks - a.time) / span) : 0.0f;
        return math::Quaternion::Slerp(a.value, b.value, t);
    }
    return keys.back().value;
}

/// @note クリップ A (+任意でクリップ B とのブレンド) を評価し、スキニングパレットを構築する。
/// @note blendWeight: 0 = A のみ / 1 = B のみ。Transition プレビューのクロスフェードに使う。
/// @note nodeGlobals (任意): ボーン可視化などデバッグ表示のため、全ノードのグローバル行列も併記する。
void EvaluatePreviewNode(const asset::Skeleton& skeleton,
                         const asset::AnimationClip* clipA,
                         double ticksA,
                         const asset::AnimationClip* clipB,
                         double ticksB,
                         float blendWeight,
                         int nodeIndex,
                         const math::Matrix4& parentGlobal,
                         std::vector<math::Matrix4>& palette,
                         std::vector<math::Matrix4>* nodeGlobals = nullptr)
{
    const auto& node = skeleton.nodes[static_cast<size_t>(nodeIndex)];
    LocalPose pose = SamplePose(node, clipA, ticksA);
    if (clipB && blendWeight > 0.0001f) {
        const LocalPose poseB = SamplePose(node, clipB, ticksB);
        pose.translation = math::Vector3::Lerp(pose.translation, poseB.translation, blendWeight);
        pose.rotation = math::Quaternion::Slerp(pose.rotation, poseB.rotation, blendWeight);
        pose.scale = math::Vector3::Lerp(pose.scale, poseB.scale, blendWeight);
    }
    const math::Matrix4 global =
        parentGlobal * math::Matrix4::TRS(pose.translation, pose.rotation, pose.scale);

    if (nodeGlobals && nodeIndex < static_cast<int>(nodeGlobals->size()))
        (*nodeGlobals)[static_cast<size_t>(nodeIndex)] = global;

    if (node.boneIndex >= 0 && node.boneIndex < static_cast<int>(palette.size())) {
        const auto& bone = skeleton.bones[static_cast<size_t>(node.boneIndex)];
        palette[static_cast<size_t>(node.boneIndex)] =
            skeleton.rootInverseTransform * global * bone.offsetMatrix;
    }
    for (int child : node.children)
        EvaluatePreviewNode(skeleton, clipA, ticksA, clipB, ticksB, blendWeight,
                            child, global, palette, nodeGlobals);
}

/// @note 根元から幅優先で最初に見つかる «変形ボーン» のノード index。通常 Hips に当たる。
/// @note 軌跡・接地リング・ルートモーション解析が «どの骨をルートとみなすか» の唯一の定義。
int FindRootMotionNode(const asset::Skeleton& skeleton)
{
    if (skeleton.rootNodeIndex < 0) return -1;
    std::vector<int> pending{ skeleton.rootNodeIndex };
    while (!pending.empty()) {
        const int nodeIndex = pending.back();
        pending.pop_back();
        if (nodeIndex < 0 || nodeIndex >= static_cast<int>(skeleton.nodes.size())) continue;
        if (skeleton.nodes[static_cast<size_t>(nodeIndex)].boneIndex >= 0) return nodeIndex;
        for (int child : skeleton.nodes[static_cast<size_t>(nodeIndex)].children)
            pending.push_back(child);
    }
    return -1;
}

/// @note ノードごとのトラックを 1 回だけ引いて表にする。
/// @note FindTrack はノード名の正規化で文字列を組み直すため、全ノード×全サンプルで毎回引くと文字列生成が数十万回走る。
std::vector<const asset::NodeAnimationTrack*> BuildTrackTable(const asset::Skeleton& skeleton,
                                                              const asset::AnimationClip& clip)
{
    std::vector<const asset::NodeAnimationTrack*> tracks(skeleton.nodes.size(), nullptr);
    for (size_t i = 0; i < skeleton.nodes.size(); ++i)
        tracks[i] = FindTrack(clip, skeleton.nodes[i].name);
    return tracks;
}

LocalPose SamplePoseFromTrack(const asset::SkeletonNode& node,
                              const asset::NodeAnimationTrack* track,
                              double ticks)
{
    if (!track) return { node.bindTranslation, node.bindRotation, node.bindScale };
    return {
        SampleVectorKeys(track->positions, ticks, node.bindTranslation),
        SampleQuaternionKeys(track->rotations, ticks, node.bindRotation),
        SampleVectorKeys(track->scales, ticks, node.bindScale)
    };
}

void EvaluateGlobalsFromTable(const asset::Skeleton& skeleton,
                              const std::vector<const asset::NodeAnimationTrack*>& tracks,
                              double ticks,
                              int nodeIndex,
                              const math::Matrix4& parentGlobal,
                              std::vector<math::Matrix4>& outGlobals)
{
    const auto& node = skeleton.nodes[static_cast<size_t>(nodeIndex)];
    const LocalPose pose =
        SamplePoseFromTrack(node, tracks[static_cast<size_t>(nodeIndex)], ticks);
    const math::Matrix4 global =
        parentGlobal * math::Matrix4::TRS(pose.translation, pose.rotation, pose.scale);
    outGlobals[static_cast<size_t>(nodeIndex)] = global;
    for (const int child : node.children)
        EvaluateGlobalsFromTable(skeleton, tracks, ticks, child, global, outGlobals);
}

/// @note node が ancestor の子孫か。足の候補が «Foot と、その子の Toe» に偏るのを避ける。
bool IsDescendantOf(const asset::Skeleton& skeleton, int node, int ancestor)
{
    for (int i = node; i >= 0; i = skeleton.nodes[static_cast<size_t>(i)].parentIndex)
        if (i == ancestor) return true;
    return false;
}

/// @note 列ベクトル規約の行列から、ローカル +Z がワールドで向く方向を取り出す。
math::Vector3 MatrixForward(const math::Matrix4& m)
{
    return { m.m[0][2], m.m[1][2], m.m[2][2] };
}

/// @note State からプレビュー用の (sourcePath, clipName) を取り出す。
/// @note Blend Tree はランタイム Weight 依存のため、先頭モーションを代表として使う。
bool StatePreviewSource(const scene::AnimationState& state,
                        std::string& outSource,
                        std::string& outClip)
{
    if (state.mode == scene::AnimationStateMode::Clip) {
        outSource = state.sourcePath;
        outClip = state.clipName;
        return true;
    }
    const auto& motions = state.mode == scene::AnimationStateMode::BlendTree1D
        ? state.blendTree1D.motions
        : state.blendTree2D.motions;
    if (motions.empty()) return false;
    outSource = motions.front().sourcePath;
    outClip = motions.front().clipName;
    return true;
}

/// @note State / Animator の sourcePath を PreviewTarget の適切なスロットへ振り分ける。
/// @note 「1 クリップ = 1 FBX」規約で sourcePath は .anim を指すことが多い。そのまま modelPath に入れると `Load<Model>` が失敗し真っ黒になるため、.anim ならクリップ側スロットへ入れ器は別途探す。
std::string ResolvePreviewSourcePath(const std::string& source)
{
    if (source.empty()) return {};
    const std::string resolved = asset::AssetManager::ResolveAssetPath(source);
    return NormalizeAssetPath(resolved.empty() ? source : resolved);
}

void AssignClipSource(PreviewTarget& out, const std::string& source,
                      const std::string& clipName)
{
    if (source.empty()) return;
    const std::string resolvedSource = ResolvePreviewSourcePath(source);
    if (util::StringUtils::EndsWith(util::StringUtils::ToLower(resolvedSource), ".anim")) {
        out.animAssetPath = resolvedSource;
        out.clipName.clear();
        std::string geometry = FindGeometryForAnim(resolvedSource);
        if (geometry.empty()) geometry = g_state.attachedModelPath;
        out.modelPath = geometry;
    } else {
        out.modelPath = resolvedSource;
        out.clipName = clipName;
    }
}

/// @note GameObject 階層から最初のスキンメッシュを探し、その modelPath を返す。
/// @note MiniBot は本体 GO に Animator、子 GO 群に SkinnedMeshRenderer を持つ構成のため、自分自身だけでは器が見つからない。
std::string FindGeometryInHierarchy(scene::GameObject* go, int depth = 0)
{
    if (!go || depth > 4) return {};
    if (auto* smr = go->GetComponent<scene::SkinnedMeshRenderer>()) {
        if (!smr->modelPath.empty() && LoadsAsPreviewableGeometry(smr->modelPath))
            return NormalizeAssetPath(smr->modelPath);
    }
    const int count = go->GetChildCount();
    for (int i = 0; i < count; ++i) {
        std::string found = FindGeometryInHierarchy(go->GetChild(i), depth + 1);
        if (!found.empty()) return found;
    }
    return {};
}

/// @note 選択中の GameObject (Animator 付き) からプレビュー対象を解決する。
/// @note Unity と同じく、Animator を持つオブジェクトを選ぶだけでプレビューできるようにする。
bool ResolveGameObjectTarget(EditorContext& ctx, PreviewTarget& out)
{
    scene::GameObject* go = ctx.GetSelectedGO();
    if (!go) return false;
    auto* animator = go->GetComponent<scene::AnimatorComponent>();
    if (!animator) return false;

    /// @note クリップ: 現在ステート → デフォルトステート → 先頭ステート
    std::string source, clipName;
    auto tryState = [&](const std::string& name) {
        if (name.empty() || !source.empty()) return;
        for (const auto& st : animator->states)
            if (st.name == name) { StatePreviewSource(st, source, clipName); return; }
    };
    tryState(animator->currentStateName);
    tryState(animator->defaultStateName);
    if (source.empty() && !animator->states.empty())
        StatePreviewSource(animator->states.front(), source, clipName);

    out.mode = PreviewTarget::Mode::Clip;
    out.label = go->name;
    AssignClipSource(out, source, clipName);

    /// @note 明示アタッチ済みの FBX を最優先し、未アタッチ時だけシーン上の実物を使う。
    if (g_state.attachedModelPath.empty()) {
        if (std::string geometry = FindGeometryInHierarchy(go); !geometry.empty())
            out.modelPath = geometry;
    } else {
        out.modelPath = g_state.attachedModelPath;
    }

    /// @note クリップも器も無ければプレビューする意味がない。
    return !out.modelPath.empty() || !out.animAssetPath.empty();
}

/// @note 現在の Animation Graph 選択からプレビュー対象を解決する。
bool ResolveGraphTarget(EditorContext& ctx, PreviewTarget& out)
{
    const auto& selection = ctx.animationGraphSelection;
    using Type = EditorContext::AnimationGraphSelection::Type;
    if (selection.type == Type::None || selection.type == Type::AnyState) return false;

    /// @note Controller アセット編集中はそのモデル、Scene 上の GameObject 選択ならそのコンポーネント。
    scene::AnimatorComponent* animator = nullptr;
    if (!selection.assetPath.empty() &&
        selection.assetPath == ctx.animationControllerEditorPath &&
        ctx.animationControllerEditor) {
        animator = ctx.animationControllerEditor.get();
    } else if (selection.entityId.IsValid() && ctx.activeScene) {
        if (auto* go = ctx.activeScene->GetGameObject(selection.entityId))
            animator = go->GetComponent<scene::AnimatorComponent>();
    }
    if (!animator) return false;

    /// @note Graph が追加 Layer を表示しているときは、その Layer の states / Any State を
    /// @note 参照する。Base Layer の配列を見続けると、遷移線を選択しても別グラフのクリップを
    /// @note プレビューするか、添字不一致で対象なしになる。
    const std::vector<scene::AnimationState>* graphStates = &animator->states;
    const std::vector<scene::AnimationTransition>* graphAnyStateTransitions =
        &animator->anyStateTransitions;
    if (!selection.layerName.empty()) {
        const scene::AnimationLayer* layer = animator->FindLayer(selection.layerName);
        if (!layer) return false;
        graphStates = &layer->states;
        graphAnyStateTransitions = &layer->anyStateTransitions;
    }

    auto stateByName = [&](const std::string& name) -> const scene::AnimationState* {
        for (const auto& state : *graphStates)
            if (state.name == name) return &state;
        return nullptr;
    };

    if (selection.type == Type::State) {
        if (selection.stateIndex < 0 ||
            selection.stateIndex >= static_cast<int>(graphStates->size()))
            return false;
        const auto& state = (*graphStates)[static_cast<size_t>(selection.stateIndex)];
        std::string source, clip;
        if (!StatePreviewSource(state, source, clip) || source.empty()) return false;
        out.mode = PreviewTarget::Mode::Clip;
        AssignClipSource(out, source, clip);
        /// @note 一度 FBX を明示的にアタッチした後は、Clip を切り替えても同じ器を使う。
        /// @note 未アタッチ時だけ Scene 上の SkinnedMeshRenderer を自動解決する。
        if (g_state.attachedModelPath.empty() && selection.entityId.IsValid() && ctx.activeScene) {
            if (auto* owner = ctx.activeScene->GetGameObject(selection.entityId)) {
                if (std::string geo = FindGeometryInHierarchy(owner); !geo.empty())
                    out.modelPath = geo;
            }
        } else if (!g_state.attachedModelPath.empty()) {
            out.modelPath = g_state.attachedModelPath;
        }
        out.label = state.name;
        if (state.mode != scene::AnimationStateMode::Clip)
            out.label += "  (Blend Tree: first motion)";
        return true;
    }

    /// @note Transition / AnyStateTransition
    const scene::AnimationTransition* transition = nullptr;
    const scene::AnimationState* fromState = nullptr;
    if (selection.type == Type::Transition) {
        if (selection.stateIndex < 0 ||
            selection.stateIndex >= static_cast<int>(graphStates->size()))
            return false;
        fromState = &(*graphStates)[static_cast<size_t>(selection.stateIndex)];
        if (selection.transitionIndex < 0 ||
            selection.transitionIndex >= static_cast<int>(fromState->transitions.size()))
            return false;
        transition = &fromState->transitions[static_cast<size_t>(selection.transitionIndex)];
    } else {
        if (selection.transitionIndex < 0 ||
            selection.transitionIndex >= static_cast<int>(graphAnyStateTransitions->size()))
            return false;
        transition =
            &(*graphAnyStateTransitions)[static_cast<size_t>(selection.transitionIndex)];
    }

    const scene::AnimationState* toState = stateByName(transition->toStateName);
    if (!toState) return false;
    std::string toSource, toClip;
    if (!StatePreviewSource(*toState, toSource, toClip) || toSource.empty()) return false;

    std::string fromSource, fromClip;
    const bool hasFrom =
        fromState && StatePreviewSource(*fromState, fromSource, fromClip) && !fromSource.empty();

    if (!hasFrom) {
        /// @note Any State 遷移は遷移元が不定のため、遷移先クリップの単独プレビューにする。
        out.mode = PreviewTarget::Mode::Clip;
        AssignClipSource(out, toSource, toClip);
        out.label = std::string("Any State -> ") + toState->name;
        return true;
    }

    /// @note 遷移元クリップの長さから、Exit Time とブレンド秒数を Transition Preview と同じ式で求める。
    const std::string resolvedFromSource = ResolvePreviewSourcePath(fromSource);
    const bool fromIsAnim = util::StringUtils::EndsWith(
        util::StringUtils::ToLower(resolvedFromSource), ".anim");
    const asset::Model* fromModel = fromIsAnim
        ? nullptr : asset::AssetManager::LoadAndGet<asset::Model>(resolvedFromSource);
    const asset::AnimationClip* fromClipPtr = fromIsAnim
        ? ResolveClip(nullptr, std::string{}, resolvedFromSource)
        : FindModelClip(fromModel, fromClip);
    const float fromLength = ClipDurationSeconds(fromClipPtr, 1.0f);
    const float blendSeconds = transition->fixedDuration
        ? transition->transitionDuration
        : transition->transitionDuration * fromLength;
    const float transitionStart = transition->hasExitTime
        ? transition->exitTime * fromLength
        : (std::max)(fromLength - blendSeconds, 0.0f);

    out.mode = PreviewTarget::Mode::Transition;
    AssignClipSource(out, fromSource, fromClip);
    out.toClipName = toClip;
    const std::string resolvedToSource = ResolvePreviewSourcePath(toSource);
    if (util::StringUtils::EndsWith(util::StringUtils::ToLower(resolvedToSource), ".anim")) {
        out.toAnimAssetPath = resolvedToSource;
        out.toSourcePath = FindGeometryForAnim(resolvedToSource);
        if (out.toSourcePath.empty()) out.toSourcePath = out.modelPath;
    } else {
        out.toSourcePath = resolvedToSource;
    }
    out.blendSeconds = (std::max)(blendSeconds, 0.0f);
    out.transitionStartSeconds = (std::max)(transitionStart, 0.0f);
    out.label = fromState->name + " -> " + toState->name;
    return true;
}

bool ResolveAssetTarget(EditorContext& ctx, PreviewTarget& out)
{
    return ResolvePathTarget(ctx.selectedAssetPath, out);
}

/// @note GPU

bool EnsurePreviewGpu(renderer::ResourceManager& resources)
{
    if (!g_gpu.skinnedShader.IsValid())
        g_gpu.skinnedShader = resources.LoadShader("Assets/Shaders/Material/Skinned/SkinnedLit.hlsl");
    if (!g_gpu.surfaceShader.IsValid())
        g_gpu.surfaceShader = resources.LoadShader("Assets/Shaders/Material/Surface/Lit.hlsl");
    if (!g_gpu.pso.IsValid()) {
        g_gpu.pso = resources.CreatePipelineState({
            renderer::RasterizerMode::SOLID_NOCULL,
            renderer::BlendMode::OPAQUE_BLEND,
            renderer::DepthMode::DEPTH_ON
        });
    }
    if (!g_gpu.wireframePso.IsValid()) {
        g_gpu.wireframePso = resources.CreatePipelineState({
            renderer::RasterizerMode::WIREFRAME,
            renderer::BlendMode::OPAQUE_BLEND,
            renderer::DepthMode::DEPTH_ON
        });
    }
    if (!g_gpu.frameCB.IsValid())
        g_gpu.frameCB = resources.CreateConstantBuffer(sizeof(scene::PerFrameCB));
    if (!g_gpu.objectCB.IsValid())
        g_gpu.objectCB = resources.CreateConstantBuffer(sizeof(scene::PerObjectCB));
    if (!g_gpu.materialCB.IsValid())
        g_gpu.materialCB = resources.CreateConstantBuffer(sizeof(PreviewMaterialCB));
    if (!g_gpu.lightCB.IsValid())
        g_gpu.lightCB = resources.CreateConstantBuffer(sizeof(renderer::LightConstantsCB));
    if (!g_gpu.shadowCB.IsValid())
        g_gpu.shadowCB = resources.CreateConstantBuffer(sizeof(scene::ShadowConstantsCB));
    if (!g_gpu.punctualShadowCB.IsValid()) {
        g_gpu.punctualShadowCB =
            resources.CreateConstantBuffer(sizeof(scene::PunctualShadowConstantsCB));
        const scene::PunctualShadowConstantsCB emptyPunctual{};
        resources.Update(g_gpu.punctualShadowCB, &emptyPunctual, sizeof(emptyPunctual));
    }

    if (!g_gpu.clusterCB.IsValid()) {
        g_gpu.clusterCB = resources.CreateConstantBuffer(sizeof(scene::ClusterConstantsCB));
        /// @note clusterLightMode = 0 = LEGACY
        const scene::ClusterConstantsCB legacyCluster{};
        resources.Update(g_gpu.clusterCB, &legacyCluster, sizeof(legacyCluster));
    }

    if (!g_gpu.skinningCB.IsValid())
        g_gpu.skinningCB = resources.CreateConstantBuffer(sizeof(PreviewSkinningCB));
    if (!g_gpu.renderTarget.IsValid())
        g_gpu.renderTarget = resources.CreateRenderTarget(PREVIEW_RT_SIZE, PREVIEW_RT_SIZE,
                                                          renderer::CameraDepthTargetDesc(1));

    return g_gpu.skinnedShader.IsValid() && g_gpu.surfaceShader.IsValid() &&
           g_gpu.pso.IsValid() && g_gpu.frameCB.IsValid() && g_gpu.objectCB.IsValid() &&
           g_gpu.materialCB.IsValid() && g_gpu.lightCB.IsValid() &&
           g_gpu.shadowCB.IsValid() && g_gpu.skinningCB.IsValid() &&
           g_gpu.renderTarget.IsValid();
}

/// @note 床グリッド用の頂点カラー Unlit 経路。DebugDraw と同じシェーダーを使う。
/// @note DebugDraw.hlsl の b0 は viewProjection だけの 64 バイトで、専用 CB が要る。PerFrameCB (先頭は view) を差すと別の行列を読む。
struct PreviewLineCameraCB {
    math::Matrix4 viewProjection;
};

bool EnsurePreviewLineGpu(renderer::ResourceManager& resources)
{
    if (!g_lineGpu.shader.IsValid())
        g_lineGpu.shader = resources.LoadShader("Assets/Shaders/Debug/DebugDraw.hlsl");
    if (!g_lineGpu.cameraCB.IsValid())
        g_lineGpu.cameraCB = resources.CreateConstantBuffer(sizeof(PreviewLineCameraCB));
    if (!g_lineGpu.pso.IsValid()) {
        /// @note DEPTH_READ: モデルに遮蔽させつつ、深度は書かない (後続の線が互いを消さない)。
        g_lineGpu.pso = resources.CreatePipelineState({
            renderer::RasterizerMode::SOLID,
            renderer::BlendMode::ALPHA_BLEND,
            renderer::DepthMode::DEPTH_READ
        });
    }
    return g_lineGpu.shader.IsValid() && g_lineGpu.cameraCB.IsValid() && g_lineGpu.pso.IsValid();
}

/// @note meshIndex の submesh に割り当たる .mat から描画用 Material を返す。無ければ nullptr。
/// @note 束縛は MaterialParamBinding (シーン描画と同じ規則) に任せ、ここでは写さない。
renderer::Material* ResolveImportedPreviewMaterial(EditorContext& ctx,
                                                   renderer::ResourceManager& resources,
                                                   size_t meshCount,
                                                   size_t meshIndex)
{
    if (g_materialCache.modelPath != g_state.target.modelPath ||
        g_materialCache.slots.size() != meshCount) {
        g_materialCache.modelPath = g_state.target.modelPath;
        g_materialCache.slots.clear();
        g_materialCache.slots.resize(meshCount);
        const asset::ModelAsset* modelAsset = asset::AssetManager::Get(
            asset::AssetManager::Load<asset::ModelAsset>(g_materialCache.modelPath));
        for (size_t i = 0; i < meshCount; ++i)
            g_materialCache.slots[i].materialPath = FindImportedMaterialPath(
                g_materialCache.modelPath, modelAsset, static_cast<int>(i));
    }
    if (meshIndex >= g_materialCache.slots.size()) return nullptr;
    PreviewMaterialSlot& slot = g_materialCache.slots[meshIndex];
    if (slot.materialPath.empty()) return nullptr;

    /// @note 再インポートで .mat が読み直されるとハンドルが変わり、Inspector の未保存編集は
    /// @note リビジョンだけが進む。どちらでも組み直す。
    const auto assetHandle = asset::AssetManager::Load<asset::MaterialAsset>(slot.materialPath);
    const uint64_t revision =
        ctx.MaterialPreviewRevision(NormalizeAssetPath(slot.materialPath));
    if (slot.built && slot.asset == assetHandle && slot.revision == revision)
        return slot.material.get();

    slot.asset = assetHandle;
    slot.revision = revision;
    slot.built = true;
    slot.material.reset();
    const asset::MaterialAsset* materialAsset = asset::AssetManager::Get<asset::MaterialAsset>(assetHandle);
    if (!materialAsset || materialAsset->shaderPath.empty()) return nullptr;

    auto material = std::make_unique<renderer::Material>();
    material->shaderPath = materialAsset->shaderPath;
    material->shader = resources.LoadShader(material->shaderPath);
    auto* shader = resources.Get(material->shader);
    if (!shader) return nullptr;
    const auto& desc = shader->GetDescriptor();
    material->paramData.assign(desc.cbufferSize, 0u);
    asset::InitDefaultMaterialParams(desc, material->paramData);
    asset::ApplyMaterialAssetParams(*materialAsset, desc, material->paramData);
    const auto texturePaths = asset::ResolveMaterialTexturePaths(*materialAsset);
    material->textures.resize(texturePaths.size());
    for (size_t t = 0; t < texturePaths.size(); ++t) {
        if (texturePaths[t].empty()) continue;
        asset::TextureImportSettings settings;
        const bool flipGreen = asset::GetCachedTextureImportSettings(texturePaths[t], settings)
            && settings.flipGreen;
        material->textures[t] = resources.LoadTexture(texturePaths[t], flipGreen);
    }
    material->Upload(resources, desc);
    slot.material = std::move(material);
    return slot.material.get();
}

std::string BuildModelNodePath(const asset::Model& model, int nodeIndex)
{
    if (nodeIndex < 0 || nodeIndex >= static_cast<int>(model.nodes.size())) return {};
    std::vector<std::string_view> reverseNames;
    std::vector<bool> visited(model.nodes.size(), false);
    int current = nodeIndex;
    while (current >= 0 && current < static_cast<int>(model.nodes.size())) {
        if (visited[static_cast<size_t>(current)]) return {};
        visited[static_cast<size_t>(current)] = true;
        reverseNames.push_back(model.nodes[static_cast<size_t>(current)].name);
        current = model.nodes[static_cast<size_t>(current)].parentIndex;
    }
    std::string path;
    for (auto it = reverseNames.rbegin(); it != reverseNames.rend(); ++it) {
        if (!path.empty()) path += '/';
        path += *it;
    }
    return path;
}

void BuildBindPoseGlobals(const asset::Skeleton& skeleton,
                          int nodeIndex,
                          const math::Matrix4& parentGlobal,
                          std::vector<math::Matrix4>& outGlobals)
{
    if (nodeIndex < 0 || nodeIndex >= static_cast<int>(skeleton.nodes.size())) return;
    const auto& node = skeleton.nodes[static_cast<size_t>(nodeIndex)];
    const math::Matrix4 global = parentGlobal * math::Matrix4::TRS(
        node.bindTranslation, node.bindRotation, node.bindScale);
    outGlobals[static_cast<size_t>(nodeIndex)] = global;
    for (const int child : node.children)
        BuildBindPoseGlobals(skeleton, child, global, outGlobals);
}

int FindSkeletonNodeForModelNode(const asset::Model& model, int modelNodeIndex)
{
    if (!model.skeleton || modelNodeIndex < 0 ||
        modelNodeIndex >= static_cast<int>(model.nodes.size())) return -1;
    const std::string modelPath = BuildModelNodePath(model, modelNodeIndex);
    for (int i = 0; i < static_cast<int>(model.skeleton->nodes.size()); ++i) {
        if (asset::BuildSkeletonNodePath(*model.skeleton, i) == modelPath)
            return i;
    }
    /// @note 旧形式で階層パスが欠けている場合は、同名ノードへ縮退する。
    const std::string& modelName = model.nodes[static_cast<size_t>(modelNodeIndex)].name;
    for (int i = 0; i < static_cast<int>(model.skeleton->nodes.size()); ++i)
        if (model.skeleton->nodes[static_cast<size_t>(i)].name == modelName)
            return i;
    return -1;
}

std::string MaskPathForMesh(const asset::Model& model, size_t meshIndex)
{
    return MaskPathForModelNode(
        model, model.FindNodeForMesh(static_cast<uint32_t>(meshIndex)));
}

/// @note スキニングパレット (skeleton.bones) の 1 本ごとのマスクウェイト。
/// @note メッシュ平均は全頂点×4 影響を舐めるため、ボーンパスの組み立てをその内側でやると文字列生成が頂点数ぶん走る。事前に表を作る。
std::vector<float> BuildBoneMaskWeights(const asset::Skeleton& skeleton,
                                        const asset::AvatarMaskAsset& mask)
{
    std::vector<float> weights(skeleton.bones.size(), 0.0f);
    for (size_t b = 0; b < skeleton.bones.size(); ++b) {
        const asset::Bone& bone = skeleton.bones[b];
        const std::string path = bone.nodeIndex >= 0
            ? asset::BuildSkeletonNodePath(skeleton, bone.nodeIndex) : bone.name;
        weights[b] = asset::EvaluateAvatarMaskWeight(mask, path, bone.name);
    }
    return weights;
}

/// @note スキンメッシュ 1 枚の代表マスクウェイト。頂点ごとの影響ボーンで加重平均する。
/// @note メッシュノードでは評価しない。スキンメッシュのノード (`P_ArmorGrey` 等) はアーマチュア外側にあり、そこでマスクを引くと default_include=false は常に 0 になる (「全部赤」の原因だった)。
/// @note 動かされる度合いは、頂点を動かすボーンのマスクウェイトでしか決まらない。
float SkinnedMaskWeight(const renderer::Mesh& mesh, const std::vector<float>& boneWeights)
{
    double total = 0.0;
    double influenceSum = 0.0;
    for (const auto& vertex : mesh.cpuSkinnedVertices) {
        for (int i = 0; i < 4; ++i) {
            const float influence = vertex.boneWeights[i];
            if (influence <= 0.0f) continue;
            const uint32_t bone = vertex.boneIndices[i];
            if (bone >= boneWeights.size()) continue;
            total += static_cast<double>(influence) * boneWeights[bone];
            influenceSum += influence;
        }
    }
    return influenceSum > 0.0 ? static_cast<float>(total / influenceSum) : 0.0f;
}

float MaskWeightForMesh(const asset::Model& model,
                        size_t meshIndex,
                        const asset::AvatarMaskAsset& mask,
                        const std::vector<float>& boneMaskWeights)
{
    const renderer::Mesh* mesh =
        meshIndex < model.meshes.size() ? model.meshes[meshIndex].get() : nullptr;
    if (mesh && mesh->isSkinned && !mesh->cpuSkinnedVertices.empty() && !boneMaskWeights.empty())
        return SkinnedMaskWeight(*mesh, boneMaskWeights);

    /// @note 非スキンメッシュ (小物・アタッチメント) は、そのノード自身の位置で決まる。
    const int nodeIndex = model.FindNodeForMesh(static_cast<uint32_t>(meshIndex));
    if (nodeIndex < 0 || nodeIndex >= static_cast<int>(model.nodes.size()))
        return mask.defaultInclude ? 1.0f : 0.0f;
    const auto& node = model.nodes[static_cast<size_t>(nodeIndex)];
    return asset::EvaluateAvatarMaskWeight(mask, MaskPathForMesh(model, meshIndex), node.name);
}

/// @note 指定時刻におけるブレンド係数と両クリップのサンプル時刻 (秒) を求める純関数。
/// @note 現在時刻の描画とオニオンスキン (前後フレーム) の評価で同じ規則を共有するために関数化している。
void ComputePlaybackSampleAt(float time,
                             const asset::AnimationClip* clipA,
                             const asset::AnimationClip* clipB,
                             float& outSecondsA,
                             float& outSecondsB,
                             float& outBlend,
                             float& outTimelineLength)
{
    const auto& target = g_state.target;
    const float lenA = ClipDurationSeconds(clipA, 1.0f);

    if (target.mode != PreviewTarget::Mode::Transition || !clipB) {
        outTimelineLength = lenA;
        outSecondsA = WrapTime(time, lenA);
        outSecondsB = 0.0f;
        outBlend = 0.0f;
        return;
    }

    const float lenB = ClipDurationSeconds(clipB, lenA);
    const float start = target.transitionStartSeconds;
    const float blend = target.blendSeconds;
    /// @note タイムライン: 遷移元 → ブレンド区間 → 遷移先 1 ループ分。全体をループ再生する。
    outTimelineLength = (std::max)(start + blend + lenB, 0.1f);

    outSecondsA = WrapTime(time, lenA);
    const float intoBlend = time - start;
    outBlend = intoBlend <= 0.0f
        ? 0.0f
        : (blend > 0.0001f ? std::clamp(intoBlend / blend, 0.0f, 1.0f) : 1.0f);
    outSecondsB = intoBlend <= 0.0f ? 0.0f : WrapTime(intoBlend, lenB);
}

/// @note 現在時刻版。共有ステート (タイムライン長・ブレンド率表示) も更新する。
void ComputePlaybackSample(const asset::AnimationClip* clipA,
                           const asset::AnimationClip* clipB,
                           float& outSecondsA,
                           float& outSecondsB,
                           float& outBlend)
{
    ComputePlaybackSampleAt(g_state.time, clipA, clipB,
                            outSecondsA, outSecondsB, outBlend, g_state.timelineLength);
    g_state.currentBlendWeight = outBlend;
}

/// @note 床グリッド / 接地リング

/// @note 1 / 2 / 5 × 10^n に丸めたグリッド間隔。ズームやモデルの大きさが変わっても
/// @note 目盛りが «中途半端な実数» にならないようにする。
float NiceGridStep(float raw)
{
    if (!(raw > 0.0f)) return 1.0f;
    const float decade = std::pow(10.0f, std::floor(std::log10(raw)));
    const float normalized = raw / decade;
    const float snapped = normalized < 1.5f ? 1.0f
                        : normalized < 3.5f ? 2.0f
                        : normalized < 7.5f ? 5.0f
                                            : 10.0f;
    return snapped * decade;
}

void BuildPreviewGridLines(std::vector<PreviewLineVertex>& out)
{
    /// @note 中心から片側の本数
    constexpr int HALF_LINES = 12;
    const float step = NiceGridStep((std::max)(g_state.boundsRadius, 0.01f) * 0.45f);
    const float extent = step * HALF_LINES;
    /// @note 交点を格子に固定する。焦点が動くたびに線がにじり寄るのを防ぐ。
    const float centerX = std::round(g_state.focus.x / step) * step;
    const float centerZ = std::round(g_state.focus.z / step) * step;
    const float y = g_state.groundY;

    /// @note 中心から遠いほど薄くする。線ごと 1 マスに割って端点で減衰させないと、
    /// @note «両端が薄い = 真ん中も薄い» という線形補間の結果になって中央が沈む。
    const auto fade = [&](float x, float z) {
        const float dx = x - centerX;
        const float dz = z - centerZ;
        const float d = std::sqrt(dx * dx + dz * dz) / extent;
        return std::clamp(1.0f - d, 0.0f, 1.0f);
    };
    const auto emit = [&](const math::Vector3& a, const math::Vector3& b,
                          const math::Vector3& rgb, float alpha) {
        out.push_back({ a, { rgb.x, rgb.y, rgb.z, alpha * fade(a.x, a.z) } });
        out.push_back({ b, { rgb.x, rgb.y, rgb.z, alpha * fade(b.x, b.z) } });
    };

    const math::Vector3 minor{ 0.55f, 0.58f, 0.66f };
    const math::Vector3 major{ 0.72f, 0.76f, 0.84f };
    const math::Vector3 axisX{ 0.86f, 0.36f, 0.38f };
    const math::Vector3 axisZ{ 0.38f, 0.58f, 0.92f };

    for (int i = -HALF_LINES; i <= HALF_LINES; ++i) {
        const float offset = static_cast<float>(i) * step;
        const bool isAxis = i == 0;
        const bool isMajor = (i % 5) == 0;
        const math::Vector3 colorAlongX = isAxis ? axisX : (isMajor ? major : minor);
        const math::Vector3 colorAlongZ = isAxis ? axisZ : (isMajor ? major : minor);
        const float alpha = isAxis ? 0.75f : (isMajor ? 0.34f : 0.16f);

        for (int s = -HALF_LINES; s < HALF_LINES; ++s) {
            const float a = static_cast<float>(s) * step;
            const float b = a + step;
            /// @note Z 一定の線 (X 方向へ伸びる) は X 軸色を、X 一定の線は Z 軸色を持つ。
            emit({ centerX + a, y, centerZ + offset },
                 { centerX + b, y, centerZ + offset }, colorAlongX, alpha);
            emit({ centerX + offset, y, centerZ + a },
                 { centerX + offset, y, centerZ + b }, colorAlongZ, alpha);
        }
    }
}

void BuildPreviewGroundRing(std::vector<PreviewLineVertex>& out, const math::Vector3& rootWorld)
{
    constexpr int SEGMENTS = 48;
    constexpr float TWO_PI = 6.28318530718f;
    const float radius = (std::max)(g_state.footprintRadius, 0.02f);
    const float y = g_state.groundY;
    const math::Vector4 ringColor{ 1.0f, 0.78f, 0.42f, 0.55f };

    for (int i = 0; i < SEGMENTS; ++i) {
        const float a0 = TWO_PI * static_cast<float>(i) / SEGMENTS;
        const float a1 = TWO_PI * static_cast<float>(i + 1) / SEGMENTS;
        out.push_back({ { rootWorld.x + std::cos(a0) * radius, y,
                          rootWorld.z + std::sin(a0) * radius }, ringColor });
        out.push_back({ { rootWorld.x + std::cos(a1) * radius, y,
                          rootWorld.z + std::sin(a1) * radius }, ringColor });
    }
    /// @note ルートの高さを読むための垂線。上端を薄くして «落ちている» ように見せる。
    out.push_back({ rootWorld, { 1.0f, 0.78f, 0.42f, 0.0f } });
    out.push_back({ { rootWorld.x, y, rootWorld.z }, { 1.0f, 0.78f, 0.42f, 0.7f } });
}

void SubmitPreviewLines(renderer::IRenderer& renderer,
                        renderer::ResourceManager& resources,
                        const std::vector<PreviewLineVertex>& vertices)
{
    if (vertices.empty()) return;
    const auto vb = g_lineGpu.pool.Acquire(
        resources, vertices.size(), static_cast<std::uint32_t>(sizeof(PreviewLineVertex)));
    if (!vb.IsValid()) return;
    resources.Update(vb, vertices.data(), vertices.size() * sizeof(PreviewLineVertex));

    renderer::DrawCall call;
    call.vertexBuffer       = vb;
    call.shader             = g_lineGpu.shader;
    call.pipelineState      = g_lineGpu.pso;
    call.constantBuffers[0] = g_lineGpu.cameraCB;
    call.vertexCount        = static_cast<uint32_t>(vertices.size());
    call.topology           = renderer::PrimitiveTopology::LINE_LIST;
    renderer.Submit(call, resources);
}

} /// @note namespace

/// @note クリップ / ポーズ

const asset::NodeAnimationTrack* FindTrack(const asset::AnimationClip& clip,
                                           const std::string& nodeName)
{
    for (const auto& track : clip.tracks)
        if (track.nodeName == nodeName)
            return &track;

    /// @note FBX の補助ノード suffix / namespace 差を吸収する (AnimatorSystem::FindTrack と同一)。
    auto canonical = [](std::string name) {
        std::replace(name.begin(), name.end(), '\\', '/');
        const std::string helper = "_$AssimpFbx$_";
        if (const size_t helperPos = name.find(helper); helperPos != std::string::npos)
            name = name.substr(0, helperPos);
        if (const size_t pathPos = name.find_last_of("/|"); pathPos != std::string::npos)
            name = name.substr(pathPos + 1);
        if (const size_t nsPos = name.find_last_of(':'); nsPos != std::string::npos)
            name = name.substr(nsPos + 1);
        return name;
    };

    const std::string canonicalNodeName = canonical(nodeName);
    for (const auto& track : clip.tracks)
        if (canonical(track.nodeName) == canonicalNodeName)
            return &track;
    return nullptr;
}

LocalPose SamplePose(const asset::SkeletonNode& node,
                     const asset::AnimationClip* clip,
                     double ticks)
{
    if (!clip) return { node.bindTranslation, node.bindRotation, node.bindScale };
    const auto* track = FindTrack(*clip, node.name);
    if (!track) return { node.bindTranslation, node.bindRotation, node.bindScale };
    return {
        SampleVectorKeys(track->positions, ticks, node.bindTranslation),
        SampleQuaternionKeys(track->rotations, ticks, node.bindRotation),
        SampleVectorKeys(track->scales, ticks, node.bindScale)
    };
}

double ClipTicksPerSecond(const asset::AnimationClip& clip)
{
    return clip.ticksPerSecond > 0.0 ? clip.ticksPerSecond : 30.0;
}

float ClipDurationSeconds(const asset::AnimationClip* clip, float fallback)
{
    if (!clip) return fallback;
    const float duration = static_cast<float>(clip->GetDurationSeconds());
    return duration > 0.0001f ? duration : fallback;
}

float WrapTime(float time, float duration)
{
    if (duration <= 0.0001f) return 0.0f;
    float wrapped = std::fmod(time, duration);
    return wrapped < 0.0f ? wrapped + duration : wrapped;
}

float ClipFrameRate(const asset::AnimationClip* clip)
{
    return clip && clip->frameRate > 0.0f ? clip->frameRate : 30.0f;
}

/// @note 列ベクトル規約 (M * v) の行優先行列から平行移動成分を取り出す。
math::Vector3 MatrixTranslation(const math::Matrix4& m)
{
    return { m.m[0][3], m.m[1][3], m.m[2][3] };
}

/// @note 表示専用のオイラー角分解 (XYZ, degrees)。Inspector の数値表示にだけ使い、計算には使わない。
math::Vector3 QuaternionToEulerDegrees(const math::Quaternion& q)
{
    const float sinrCosp = 2.0f * (q.w * q.x + q.y * q.z);
    const float cosrCosp = 1.0f - 2.0f * (q.x * q.x + q.y * q.y);
    const float roll = std::atan2(sinrCosp, cosrCosp);
    const float sinp = std::clamp(2.0f * (q.w * q.y - q.z * q.x), -1.0f, 1.0f);
    const float pitch = std::asin(sinp);
    const float sinyCosp = 2.0f * (q.w * q.z + q.x * q.y);
    const float cosyCosp = 1.0f - 2.0f * (q.y * q.y + q.z * q.z);
    const float yaw = std::atan2(sinyCosp, cosyCosp);
    constexpr float RAD_TO_DEG = 57.29577951f;
    return { roll * RAD_TO_DEG, pitch * RAD_TO_DEG, yaw * RAD_TO_DEG };
}

/// @note FBX の namespace prefix (mixamorig: 等) を落とした短いボーン名。オーバーレイ表示用。
std::string ShortBoneName(const std::string& name)
{
    const size_t namespacePos = name.find_last_of(':');
    return namespacePos != std::string::npos ? name.substr(namespacePos + 1) : name;
}

/// @note ルートモーション確認用の軌跡: 最初の変形ボーン (通常 Hips) の位置をクリップ全長に渡って
/// @note サンプルする。親チェーンだけ評価するので全階層評価より大幅に軽い。
void BuildTrailPoints(const asset::Skeleton& skeleton,
                      const asset::AnimationClip& clip,
                      std::vector<math::Vector3>& outPoints,
                      int& outTrailNode)
{
    outPoints.clear();
    outTrailNode = -1;
    if (skeleton.rootNodeIndex < 0) return;

    const int trailNode = FindRootMotionNode(skeleton);
    if (trailNode < 0) return;
    outTrailNode = trailNode;

    std::vector<int> chain;
    for (int nodeIndex = trailNode; nodeIndex >= 0;
         nodeIndex = skeleton.nodes[static_cast<size_t>(nodeIndex)].parentIndex)
        chain.push_back(nodeIndex);
    std::reverse(chain.begin(), chain.end());

    const double ticksPerSecond = clip.ticksPerSecond > 0.0 ? clip.ticksPerSecond : 30.0;
    const float duration = (std::max)(static_cast<float>(clip.GetDurationSeconds()), 0.001f);
    constexpr int TRAIL_SAMPLES = 48;
    outPoints.reserve(TRAIL_SAMPLES + 1);
    for (int sample = 0; sample <= TRAIL_SAMPLES; ++sample) {
        const double ticks = static_cast<double>(duration) * sample / TRAIL_SAMPLES *
                             ticksPerSecond;
        math::Matrix4 global = math::Matrix4::Identity();
        for (int nodeIndex : chain) {
            const LocalPose pose =
                SamplePose(skeleton.nodes[static_cast<size_t>(nodeIndex)], &clip, ticks);
            global = global * math::Matrix4::TRS(pose.translation, pose.rotation, pose.scale);
        }
        outPoints.push_back(MatrixTranslation(skeleton.rootInverseTransform * global));
    }
}

void AnalyzeRootMotion(const asset::Skeleton& skeleton,
                       const asset::AnimationClip& clip,
                       RootMotionAnalysis& out)
{
    out = RootMotionAnalysis{};
    if (skeleton.rootNodeIndex < 0 || skeleton.nodes.empty()) return;
    const int rootBone = FindRootMotionNode(skeleton);
    if (rootBone < 0) return;

    constexpr int SAMPLES = 64;
    const double ticksPerSecond = ClipTicksPerSecond(clip);
    const float duration = (std::max)(static_cast<float>(clip.GetDurationSeconds()), 0.001f);
    const float dt = duration / SAMPLES;
    const auto tracks = BuildTrackTable(skeleton, clip);

    /// @note 全ノードの «ワールド» 位置をクリップ全長ぶん持つ。足の接地判定が、ルートの移動を
    /// @note 含んだ位置でないと成立しないため、チェーンだけでなく階層全体を評価する。
    const size_t nodeCount = skeleton.nodes.size();
    std::vector<std::vector<math::Vector3>> positions(SAMPLES + 1);
    std::vector<math::Matrix4> globals(nodeCount, math::Matrix4::Identity());
    math::Vector3 firstForward{ 0.0f, 0.0f, 1.0f };
    math::Vector3 lastForward{ 0.0f, 0.0f, 1.0f };
    for (int sample = 0; sample <= SAMPLES; ++sample) {
        const double ticks =
            static_cast<double>(duration) * sample / SAMPLES * ticksPerSecond;
        std::fill(globals.begin(), globals.end(), math::Matrix4::Identity());
        EvaluateGlobalsFromTable(skeleton, tracks, ticks, skeleton.rootNodeIndex,
                                 math::Matrix4::Identity(), globals);
        auto& frame = positions[static_cast<size_t>(sample)];
        frame.resize(nodeCount);
        for (size_t i = 0; i < nodeCount; ++i)
            frame[i] = MatrixTranslation(skeleton.rootInverseTransform * globals[i]);
        const math::Vector3 forward =
            MatrixForward(skeleton.rootInverseTransform * globals[static_cast<size_t>(rootBone)]);
        if (sample == 0) firstForward = forward;
        if (sample == SAMPLES) lastForward = forward;
    }

    out.valid = true;
    out.rootNode = rootBone;
    out.duration = duration;

    /// @name 移動量と速度
    out.speedSamples.reserve(SAMPLES);
    out.verticalSamples.reserve(SAMPLES);
    float rootMinY = FLT_MAX, rootMaxY = -FLT_MAX;
    for (int sample = 0; sample <= SAMPLES; ++sample) {
        const float y = positions[static_cast<size_t>(sample)][static_cast<size_t>(rootBone)].y;
        rootMinY = (std::min)(rootMinY, y);
        rootMaxY = (std::max)(rootMaxY, y);
    }
    out.verticalRange = rootMaxY - rootMinY;
    for (int sample = 1; sample <= SAMPLES; ++sample) {
        const math::Vector3& a =
            positions[static_cast<size_t>(sample - 1)][static_cast<size_t>(rootBone)];
        const math::Vector3& b =
            positions[static_cast<size_t>(sample)][static_cast<size_t>(rootBone)];
        const float dx = b.x - a.x;
        const float dz = b.z - a.z;
        const float horizontal = std::sqrt(dx * dx + dz * dz);
        out.pathLength += horizontal;
        out.speedSamples.push_back(horizontal / dt);
        out.verticalSamples.push_back((b.y - a.y) / dt);
        out.maxSpeed = (std::max)(out.maxSpeed, horizontal / dt);
    }
    const math::Vector3& start = positions[0][static_cast<size_t>(rootBone)];
    const math::Vector3& end = positions[SAMPLES][static_cast<size_t>(rootBone)];
    out.netDelta = { end.x - start.x, end.y - start.y, end.z - start.z };
    out.netDistance = std::sqrt(out.netDelta.x * out.netDelta.x +
                                out.netDelta.z * out.netDelta.z);
    out.averageSpeed = out.pathLength / duration;

    /// @note 向きの変化は «水平面へ落とした前方ベクトル» 同士の符号付き角度で見る。
    constexpr float RAD_TO_DEG = 57.29577951f;
    const float firstYaw = std::atan2(firstForward.x, firstForward.z);
    const float lastYaw = std::atan2(lastForward.x, lastForward.z);
    float turn = lastYaw - firstYaw;
    constexpr float PI = 3.14159265358979f;
    while (turn > PI) turn -= 2.0f * PI;
    while (turn < -PI) turn += 2.0f * PI;
    out.netTurnDegrees = turn * RAD_TO_DEG;

    /// @name 身長の目安 (しきい値を絶対値で決めると、スケールの違うリグで破綻する)
    float bodyMinY = FLT_MAX, bodyMaxY = -FLT_MAX;
    for (const auto& frame : positions) {
        for (size_t i = 0; i < nodeCount; ++i) {
            if (skeleton.nodes[i].boneIndex < 0) continue;
            bodyMinY = (std::min)(bodyMinY, frame[i].y);
            bodyMaxY = (std::max)(bodyMaxY, frame[i].y);
        }
    }
    out.bodyHeight = (std::max)(bodyMaxY - bodyMinY, 0.01f);
    out.inPlace = out.netDistance < out.bodyHeight * 0.02f;

    /// @name ループ整合
    /// @note ルートボーンは «進む・回る» のが正しい姿なので、この比較からは外す。
    /// @note 残りのボーンが先頭と末尾で食い違っていれば、それがループ時の «跳ね» になる。
    const double endTicks = static_cast<double>(duration) * ticksPerSecond;
    double positionSumSq = 0.0;
    int comparedBones = 0;
    for (size_t i = 0; i < nodeCount; ++i) {
        if (skeleton.nodes[i].boneIndex < 0 || static_cast<int>(i) == rootBone) continue;
        const LocalPose a = SamplePoseFromTrack(skeleton.nodes[i], tracks[i], 0.0);
        const LocalPose b = SamplePoseFromTrack(skeleton.nodes[i], tracks[i], endTicks);
        const float dx = b.translation.x - a.translation.x;
        const float dy = b.translation.y - a.translation.y;
        const float dz = b.translation.z - a.translation.z;
        positionSumSq += static_cast<double>(dx * dx + dy * dy + dz * dz);
        ++comparedBones;

        const float dot = std::abs(a.rotation.x * b.rotation.x + a.rotation.y * b.rotation.y +
                                   a.rotation.z * b.rotation.z + a.rotation.w * b.rotation.w);
        const float angle = 2.0f * std::acos(std::clamp(dot, 0.0f, 1.0f)) * RAD_TO_DEG;
        if (angle > out.loopRotationGap) {
            out.loopRotationGap = angle;
            out.loopWorstBone = ShortBoneName(skeleton.nodes[i].name);
        }
    }
    if (comparedBones > 0)
        out.loopPositionGap =
            static_cast<float>(std::sqrt(positionSumSq / comparedBones));
    if (!out.speedSamples.empty())
        out.loopSpeedGap = std::abs(out.speedSamples.back() - out.speedSamples.front());

    /// @name 接地と足滑り
    /// @note 足ボーンは «名前» と «クリップ中いちばん低いところまで下りるか» の二段で選ぶ。ヒューマノイド定義に頼らないのは、FBZZ に Avatar の骨マップが無く Mixamo / Blender / 自作リグで名前もノード構成も揃わないため。
    std::vector<int> candidates;
    for (size_t i = 0; i < nodeCount; ++i) {
        if (skeleton.nodes[i].boneIndex < 0) continue;
        const std::string lower = util::StringUtils::ToLower(skeleton.nodes[i].name);
        if (lower.find("toe") != std::string::npos ||
            lower.find("foot") != std::string::npos ||
            lower.find("ankle") != std::string::npos)
            candidates.push_back(static_cast<int>(i));
    }
    if (candidates.empty()) {
        for (size_t i = 0; i < nodeCount; ++i)
            if (skeleton.nodes[i].boneIndex >= 0) candidates.push_back(static_cast<int>(i));
    }

    std::vector<float> nodeMinY(nodeCount, FLT_MAX);
    for (const auto& frame : positions)
        for (int node : candidates)
            nodeMinY[static_cast<size_t>(node)] =
                (std::min)(nodeMinY[static_cast<size_t>(node)], frame[static_cast<size_t>(node)].y);
    std::sort(candidates.begin(), candidates.end(), [&](int a, int b) {
        return nodeMinY[static_cast<size_t>(a)] < nodeMinY[static_cast<size_t>(b)];
    });

    int footCount = 0;
    for (int node : candidates) {
        if (footCount >= 2) break;
        /// @note 1 本目の親子 (Foot と その子の Toe) を 2 本と数えると、片足だけを見てしまう。
        if (footCount == 1 &&
            (IsDescendantOf(skeleton, node, out.footNodes[0]) ||
             IsDescendantOf(skeleton, out.footNodes[0], node)))
            continue;
        out.footNodes[footCount] = node;
        out.footNames[footCount] = ShortBoneName(skeleton.nodes[static_cast<size_t>(node)].name);
        ++footCount;
    }

    const float plantThreshold = out.bodyHeight * 0.06f;
    int plantedSamples = 0;
    for (int f = 0; f < footCount; ++f) {
        const size_t foot = static_cast<size_t>(out.footNodes[f]);
        const float footFloor = nodeMinY[foot];
        for (int sample = 1; sample <= SAMPLES; ++sample) {
            const math::Vector3& a = positions[static_cast<size_t>(sample - 1)][foot];
            const math::Vector3& b = positions[static_cast<size_t>(sample)][foot];
            /// @note 前後どちらのサンプルも床付近にある区間だけを «接地» とみなす。
            /// @note 片側だけで判定すると、踏み込み・蹴り出しの 1 コマを滑りに数えてしまう。
            if (a.y > footFloor + plantThreshold || b.y > footFloor + plantThreshold) continue;
            const float dx = b.x - a.x;
            const float dz = b.z - a.z;
            out.plantedSlide += std::sqrt(dx * dx + dz * dz);
            ++plantedSamples;
        }
    }
    out.plantedSeconds = static_cast<float>(plantedSamples) * dt;
    out.requiredRootSpeed =
        out.plantedSeconds > 0.0001f ? out.plantedSlide / out.plantedSeconds : 0.0f;
}

/// @note 単一ボーンのローカル位置 XYZ・回転オイラー XYZ をクリップ全長でサンプルし、
/// @note カーブミニグラフ用の配列と min/max を構築する。位置と回転は別スケールで正規化する。
void BuildBoneCurves(const asset::Skeleton& skeleton,
                     const asset::AnimationClip& clip,
                     int nodeIndex,
                     std::vector<float>& posX, std::vector<float>& posY, std::vector<float>& posZ,
                     std::vector<float>& rotX, std::vector<float>& rotY, std::vector<float>& rotZ,
                     float& posMin, float& posMax,
                     float& rotMin, float& rotMax)
{
    posX.clear(); posY.clear(); posZ.clear();
    rotX.clear(); rotY.clear(); rotZ.clear();
    posMin = posMax = rotMin = rotMax = 0.0f;
    if (nodeIndex < 0 || nodeIndex >= static_cast<int>(skeleton.nodes.size())) return;

    const auto& node = skeleton.nodes[static_cast<size_t>(nodeIndex)];
    const double ticksPerSecond = ClipTicksPerSecond(clip);
    const float duration = (std::max)(static_cast<float>(clip.GetDurationSeconds()), 0.001f);
    constexpr int CURVE_SAMPLES = 64;

    posX.reserve(CURVE_SAMPLES + 1);
    bool first = true;
    for (int sample = 0; sample <= CURVE_SAMPLES; ++sample) {
        const double ticks = static_cast<double>(duration) * sample / CURVE_SAMPLES *
                             ticksPerSecond;
        const LocalPose pose = SamplePose(node, &clip, ticks);
        const math::Vector3 euler = QuaternionToEulerDegrees(pose.rotation);
        posX.push_back(pose.translation.x);
        posY.push_back(pose.translation.y);
        posZ.push_back(pose.translation.z);
        rotX.push_back(euler.x);
        rotY.push_back(euler.y);
        rotZ.push_back(euler.z);

        auto expandPos = [&](float v) {
            if (first) { posMin = posMax = v; }
            else { posMin = (std::min)(posMin, v); posMax = (std::max)(posMax, v); }
        };
        auto expandRot = [&](float v) {
            if (first) { rotMin = rotMax = v; }
            else { rotMin = (std::min)(rotMin, v); rotMax = (std::max)(rotMax, v); }
        };
        expandPos(pose.translation.x); expandPos(pose.translation.y); expandPos(pose.translation.z);
        expandRot(euler.x); expandRot(euler.y); expandRot(euler.z);
        first = false;
    }
}

/// @note モデル内クリップを名前で検索。空名は先頭クリップ (State の <Auto / First Clip> と同じ規約)。
const asset::AnimationClip* FindModelClip(const asset::Model* model, const std::string& clipName)
{
    if (!model || model->clips.empty()) return nullptr;
    if (!clipName.empty()) {
        for (const auto& clip : model->clips)
            if (clip.name == clipName) return &clip;
    }
    return &model->clips.front();
}

/// @note .anim 直接指定があればそちら、なければモデル内クリップを解決する。
const asset::AnimationClip* ResolveClip(const asset::Model* model,
                                        const std::string& clipName,
                                        const std::string& animAssetPath)
{
    if (!animAssetPath.empty()) {
        const auto handle = asset::AssetManager::Load<asset::AnimationClip>(animAssetPath);
        if (const auto* clip = asset::AssetManager::Get(handle)) return clip;
    }
    return FindModelClip(model, clipName);
}

/// @note 対象解決

/// @note スキンメッシュとスケルトンを両方持ち、プレビューの器として使えるモデルか。
/// @note FBZZ の「1 クリップ = 1 FBX」で書き出されたクリップ FBX はアーマチュアと Empty しか含まないため、器としては使えない。
bool IsPreviewableGeometry(const asset::Model* model)
{
    return model && model->skeleton && !model->skeleton->bones.empty() &&
           !model->meshes.empty();
}

bool LoadsAsPreviewableGeometry(const std::string& path)
{
    if (!util::FileSystem::Exists(path)) return false;
    return IsPreviewableGeometry(asset::AssetManager::LoadAndGet<asset::Model>(path));
}

/// @note .anim からスキンメッシュを持つモデルを探す。
/// @note Assets/Models/MiniBot/Walk/anims/Walk@Walk.anim
/// @note → Assets/Models/MiniBot/Walk.fbx  (クリップ FBX: メッシュ無しなので不採用)
/// @note → Assets/Models/MiniBot.fbx       (パッケージ本体: 採用)
/// @note AssetManager の「パッケージフォルダ Foo/ の隣に原本 Foo.fbx」規約を利用し、
/// @note 親ディレクトリを遡って最初に見つかったスキンメッシュ付きモデルを返す。
std::string FindGeometryForAnim(const std::string& animPath)
{
    namespace fs = std::filesystem;
    static constexpr const char* kExts[] = { ".fbx", ".FBX", ".fzasset", ".asset",
                                             ".gltf", ".glb", ".obj" };
    fs::path dir = util::FileSystem::PathFromUtf8(animPath).parent_path();

    /// @note anims/ → クリップパッケージ → モデルパッケージ … と最大 5 階層遡る
    for (int depth = 0; depth < 5 && !dir.empty(); ++depth) {
        const std::string dirName = util::FileSystem::PathToUtf8(dir.filename());
        if (!dirName.empty()) {
            for (const char* ext : kExts) {
                const std::string candidate =
                    util::FileSystem::PathToUtf8(dir.parent_path() / (dirName + ext));
                if (LoadsAsPreviewableGeometry(candidate))
                    return NormalizeAssetPath(candidate);
            }
        }
        const fs::path parent = dir.parent_path();
        if (parent == dir) break;
        dir = parent;
    }
    return {};
}

/// @note モデルのパッケージ配下にある .anim を全部集める。
/// @note Assets/Models/MiniBot.fbx
/// @note → Assets/Models/MiniBot/*/anims/*.anim   (Idle, Walk, Run …)
/// @note → Assets/Models/MiniBot/anims/*.anim     (モデル自身に同梱された場合)
/// @note 結果はモデルパスをキーにキャッシュする (毎フレーム走査すると重いため)。
const std::vector<std::string>& CollectPackageAnims(const std::string& modelPath)
{
    static std::string cachedKey;
    static std::vector<std::string> cached;
    static std::vector<std::string> empty;
    if (modelPath.empty()) return empty;
    if (modelPath == cachedKey) return cached;

    namespace fs = std::filesystem;
    cachedKey = modelPath;
    cached.clear();

    const fs::path model = util::FileSystem::PathFromUtf8(modelPath);
    const fs::path packageDir = model.parent_path() /
                                util::FileSystem::PathToUtf8(model.stem());
    std::error_code ec;
    if (!fs::exists(packageDir, ec)) return cached;

    /// @note パッケージ直下の anims/ と、その 1 階層下 (クリップパッケージ) の anims/ を見る。
    auto scanAnimsDir = [&](const fs::path& dir) {
        if (!fs::exists(dir, ec)) return;
        for (const auto& e : fs::directory_iterator(dir, ec)) {
            if (ec) break;
            if (!e.is_regular_file(ec)) continue;
            if (util::StringUtils::ToLower(
                    util::FileSystem::PathToUtf8(e.path().extension())) != ".anim") continue;
            cached.push_back(NormalizeAssetPath(util::FileSystem::PathToUtf8(e.path())));
        }
    };
    scanAnimsDir(packageDir / "anims");
    for (const auto& e : fs::directory_iterator(packageDir, ec)) {
        if (ec) break;
        if (e.is_directory(ec)) scanAnimsDir(e.path() / "anims");
    }
    std::sort(cached.begin(), cached.end());
    return cached;
}

bool ResolvePathTarget(const std::string& path, PreviewTarget& out)
{
    if (path.empty()) return false;
    const std::string lower = util::StringUtils::ToLower(path);

    if (util::StringUtils::EndsWith(lower, ".anim")) {
        out.mode = PreviewTarget::Mode::Clip;
        out.animAssetPath = path;
        out.label = util::FileSystem::GetFilename(path);

        /// @note ジオメトリは (1) パッケージ規約から自動発見 (2) ユーザー指定の使い回し の順。どちらも無ければ modelPath は空のままにし、UI がドロップ待ち表示を出す (ここで false を返すとプレビューごと消える)。
        out.modelPath = g_state.attachedModelPath.empty()
            ? FindGeometryForAnim(path)
            : g_state.attachedModelPath;
        return true;
    }

    if (util::StringUtils::EndsWith(lower, ".fbx") ||
        util::StringUtils::EndsWith(lower, ".fzasset") ||
        util::StringUtils::EndsWith(lower, ".asset")) {
        const asset::Model* model = asset::AssetManager::LoadAndGet<asset::Model>(path);
        /// @note クリップを持たないモデルでも器として成立させる (バインドポーズを表示し、クリップは後からコンボ/ドロップで指定できる)。FBZZ のスキンメッシュ本体 FBX は clips が常に空なので、clips.empty() で弾くと本体モデルごと弾かれる。
        if (!IsPreviewableGeometry(model)) return false;
        out.mode = PreviewTarget::Mode::Clip;
        out.modelPath = NormalizeAssetPath(path);
        /// @note 直前と同じモデルならクリップ選択 (コンボ) を維持する。
        if (g_state.target.modelPath == out.modelPath) {
            out.clipName = g_state.target.clipName;
            out.animAssetPath = g_state.target.animAssetPath;
        }
        out.label = util::FileSystem::GetFilename(path);
        return true;
    }
    return false;
}

/// @note ドロップ等で明示指定された対象を採用する。以降は選択操作があるまでこの対象を維持する。
void AdoptManualTarget(const PreviewTarget& target, bool attachModel)
{
    const bool identityChanged = !target.SameIdentity(g_state.target);
    g_state.target = target;
    g_state.origin = TargetOrigin::Manual;
    if (attachModel && !target.modelPath.empty())
        g_state.attachedModelPath = target.modelPath;
    if (identityChanged) {
        g_state.time = 0.0f;
        g_state.playing = true;
        g_state.needsFraming = true;
    }
}

/// @note 現在のクリップを保ったままジオメトリだけ差し替える。
/// @note Unity の .anim プレビューと同じく「動きは今のまま、器だけ別モデルで見たい」操作を成立させる。モデルをドロップしてもクリップは消えない。
void SwapPreviewGeometry(const std::string& modelPath)
{
    if (modelPath.empty()) return;
    g_state.attachedModelPath = modelPath;
    g_state.target.modelPath = modelPath;
    if (g_state.target.mode == PreviewTarget::Mode::None)
        g_state.target.mode = PreviewTarget::Mode::Clip;
    if (g_state.target.label.empty())
        g_state.target.label = util::FileSystem::GetFilename(modelPath);
    g_state.origin = TargetOrigin::Manual;
    g_state.needsFraming = true;
}

/// @note ImGui の直前アイテムを ASSET_PATH ドロップターゲットとして扱い、
/// @note アニメーション関連アセットが落とされたらプレビュー対象を差し替える。
bool AcceptPreviewAssetDrop()
{
    bool accepted = false;
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_PATH")) {
            const std::string dropped =
                NormalizeAssetPath(static_cast<const char*>(payload->Data));
            const std::string lower = util::StringUtils::ToLower(dropped);

            /// @note モデルのドロップで、既にクリップが載っている場合は器だけ差し替える。
            const bool isModelDrop =
                util::StringUtils::EndsWith(lower, ".fbx") ||
                util::StringUtils::EndsWith(lower, ".fzasset") ||
                util::StringUtils::EndsWith(lower, ".asset");
            const bool hasClip = !g_state.target.animAssetPath.empty() ||
                                 !g_state.target.clipName.empty();
            if (isModelDrop && hasClip && LoadsAsPreviewableGeometry(dropped)) {
                SwapPreviewGeometry(dropped);
                accepted = true;
            } else {
                PreviewTarget target;
                if (ResolvePathTarget(dropped, target)) {
                    AdoptManualTarget(target, isModelDrop);
                    accepted = true;
                }
            }
        }
        ImGui::EndDragDropTarget();
    }
    return accepted;
}

/// @note 選択の変化を検知し、「後から操作された方」をプレビュー対象として採用する。
/// @note グラフの遷移矢印とアセットの .anim はどちらも選択状態を持ち続けるため、優先順位を固定すると片方が永遠にプレビューできなくなる。
void UpdatePreviewTarget(EditorContext& ctx)
{
    const auto& selection = ctx.animationGraphSelection;
    const bool graphChanged =
        selection.type != g_state.lastGraphType ||
        selection.stateIndex != g_state.lastGraphState ||
        selection.transitionIndex != g_state.lastGraphTransition ||
        selection.assetPath != g_state.lastGraphAssetPath ||
        selection.layerName != g_state.lastGraphLayerName ||
        selection.entityId != g_state.lastGraphEntity;
    const bool assetChanged = ctx.selectedAssetPath != g_state.lastSelectedAssetPath;

    g_state.lastGraphType = selection.type;
    g_state.lastGraphState = selection.stateIndex;
    g_state.lastGraphTransition = selection.transitionIndex;
    g_state.lastGraphAssetPath = selection.assetPath;
    g_state.lastGraphLayerName = selection.layerName;
    g_state.lastGraphEntity = selection.entityId;
    g_state.lastSelectedAssetPath = ctx.selectedAssetPath;

    PreviewTarget candidate;
    bool resolved = false;
    TargetOrigin origin = g_state.origin;
    const auto isModelAssetPath = [](const std::string& path) {
        const std::string lower = util::StringUtils::ToLower(path);
        return util::StringUtils::EndsWith(lower, ".fbx") ||
               util::StringUtils::EndsWith(lower, ".fzasset") ||
               util::StringUtils::EndsWith(lower, ".asset");
    };

    /// @note Animator 付き GameObject の選択変化も対象切り替えのトリガーにする。
    scene::GameObject* selectedGo = ctx.GetSelectedGO();
    const scene::EntityID selectedEntity =
        selectedGo ? selectedGo->GetID() : scene::EntityID::INVALID;
    const bool goChanged = !(selectedEntity == g_state.lastSelectedEntity);
    g_state.lastSelectedEntity = selectedEntity;

    if (graphChanged && ResolveGraphTarget(ctx, candidate)) {
        origin = TargetOrigin::Graph;
        resolved = true;
    } else if (assetChanged && ResolveAssetTarget(ctx, candidate)) {
        origin = TargetOrigin::Asset;
        resolved = true;
        /// @note Asset Browser から別のモデルを選んだ場合も「再アタッチ」とみなし、
        /// @note 以降の Clip 選択ではこのモデルを保持する。`.anim` 選択では変更しない。
        if (isModelAssetPath(ctx.selectedAssetPath) && !candidate.modelPath.empty())
            g_state.attachedModelPath = candidate.modelPath;
    } else if (goChanged && ResolveGameObjectTarget(ctx, candidate)) {
        origin = TargetOrigin::GameObject;
        resolved = true;
    } else if (g_state.origin == TargetOrigin::GameObject) {
        resolved = ResolveGameObjectTarget(ctx, candidate);
    } else if (g_state.origin == TargetOrigin::Graph) {
        /// @note 継続中の対象は毎フレーム再解決し、Duration 等のパラメーター編集を即反映する。
        resolved = ResolveGraphTarget(ctx, candidate);
    } else if (g_state.origin == TargetOrigin::Asset) {
        resolved = ResolveAssetTarget(ctx, candidate);
    }
    /// @note origin == Manual (ドロップ指定) は選択に追従しないため、
    /// @note graphChanged / assetChanged で新しい対象が解決されるまで現状維持する。

    if (!resolved && g_state.origin == TargetOrigin::None) {
        if (ResolveGraphTarget(ctx, candidate)) { origin = TargetOrigin::Graph; resolved = true; }
        else if (ResolveAssetTarget(ctx, candidate)) { origin = TargetOrigin::Asset; resolved = true; }
    }
    /// @note 解決できない間は直前の対象を維持する (Unity と同じ sticky 挙動)
    if (!resolved) return;

    const bool identityChanged = !candidate.SameIdentity(g_state.target);
    g_state.target = candidate;
    g_state.origin = origin;
    if (identityChanged) {
        g_state.time = 0.0f;
        g_state.playing = true;
        g_state.needsFraming = true;
    }
}

/// @note Avatar Mask 共有ヘルパー

std::string MaskPathForModelNode(const asset::Model& model, int nodeIndex)
{
    if (nodeIndex < 0 || nodeIndex >= static_cast<int>(model.nodes.size()))
        return {};
    if (const int skeletonNodeIndex = FindSkeletonNodeForModelNode(model, nodeIndex);
        skeletonNodeIndex >= 0) {
        return asset::BuildSkeletonNodePath(*model.skeleton, skeletonNodeIndex);
    }
    return BuildModelNodePath(model, nodeIndex);
}

ImVec4 MaskWeightColor(float weight)
{
    const float clamped = std::isfinite(weight)
        ? std::clamp(weight, 0.0f, 1.0f)
        : 0.0f;
    if (clamped < 0.5f) {
        const float t = clamped * 2.0f;
        return { 0.90f, 0.20f + 0.65f * t, 0.20f, 1.0f };
    }
    const float t = (clamped - 0.5f) * 2.0f;
    return { 0.90f - 0.70f * t, 0.85f + 0.05f * t, 0.20f + 0.15f * t, 1.0f };
}

ImU32 MaskWeightColorU32(float weight, int alpha)
{
    const ImVec4 color = MaskWeightColor(weight);
    return IM_COL32(static_cast<int>(color.x * 255.0f),
                    static_cast<int>(color.y * 255.0f),
                    static_cast<int>(color.z * 255.0f), alpha);
}

std::string MaskPreviewRevision(const asset::AvatarMaskAsset& mask)
{
    /// @note 同一フレームに通常 Preview と Mask Preview が描画される場合でも、Weight の中身が変わったら RenderPreviewFrame のキャッシュを無効化する。パスだけをキーにすると、Apply 前の Inspector 編集が古い RT のまま残る。
    std::string revision;
    revision.reserve(32 + mask.entries.size() * 32);
    revision += mask.defaultInclude ? '1' : '0';
    for (const auto& entry : mask.entries) {
        revision += '|';
        revision += entry.bonePath;
        revision += ':';
        revision += std::to_string(std::bit_cast<uint32_t>(entry.weight));
        revision += entry.includeChildren ? ":1:" : ":0:";
        revision += std::to_string(entry.blendDepth);
    }
    return revision;
}

/// @note カメラ / 描画

void ComputeModelBounds(const asset::Model& model, PreviewBounds& out)
{
    math::Vector3 minP{ FLT_MAX, FLT_MAX, FLT_MAX };
    math::Vector3 maxP{ -FLT_MAX, -FLT_MAX, -FLT_MAX };
    bool any = false;
    for (const auto& mesh : model.meshes) {
        if (!mesh) continue;
        const math::Vector3 c = mesh->boundsCenter;
        const float r = (std::max)(mesh->boundsRadius, 0.001f);
        minP.x = (std::min)(minP.x, c.x - r); maxP.x = (std::max)(maxP.x, c.x + r);
        minP.y = (std::min)(minP.y, c.y - r); maxP.y = (std::max)(maxP.y, c.y + r);
        minP.z = (std::min)(minP.z, c.z - r); maxP.z = (std::max)(maxP.z, c.z + r);
        any = true;
    }
    if (!any) {
        out = PreviewBounds{};
        return;
    }
    out.center = (minP + maxP) * 0.5f;
    out.radius = (std::max)((maxP - minP).Length() * 0.5f, 0.05f);
    out.minY = minP.y;
    const float dx = (maxP.x - minP.x) * 0.5f;
    const float dz = (maxP.z - minP.z) * 0.5f;
    out.radiusXZ = (std::max)(std::sqrt(dx * dx + dz * dz), 0.02f);
}

void PreviewCameraBasis(math::Vector3& outRight, math::Vector3& outUp, math::Vector3& outForward)
{
    const float cosPitch = std::cos(g_state.pitch);
    const math::Vector3 orbitDir{
        cosPitch * std::sin(g_state.yaw),
        std::sin(g_state.pitch),
        cosPitch * std::cos(g_state.yaw)
    };
    outForward = { -orbitDir.x, -orbitDir.y, -orbitDir.z };
    outRight = { std::cos(g_state.yaw), 0.0f, -std::sin(g_state.yaw) };
    /// @note up = (-forward) × right。yaw=pitch=0 で (0,1,0) になる並び。
    outUp = {
        orbitDir.y * outRight.z - orbitDir.z * outRight.y,
        orbitDir.z * outRight.x - orbitDir.x * outRight.z,
        orbitDir.x * outRight.y - orbitDir.y * outRight.x
    };
}

bool RenderPreviewFrame(EditorContext& ctx, float displayAspect)
{
    if (!ctx.renderer || !ctx.resources) return false;
    const std::string renderIdentity = g_maskPreview.active
        ? "mask|" + g_maskPreview.modelPath + "|" + g_maskPreview.maskPath + "|" +
            MaskPreviewRevision(g_maskPreview.mask)
        : "animation";
    if (g_state.lastRenderFrame == ImGui::GetFrameCount()
        && g_state.lastRenderIdentity == renderIdentity)
        return g_state.lastRenderOk;
    g_state.lastRenderFrame = ImGui::GetFrameCount();
    g_state.lastRenderIdentity = renderIdentity;
    g_state.lastRenderOk = false;

    auto& resources = *ctx.resources;
    if (!EnsurePreviewGpu(resources)) return false;

    const asset::Model* model = asset::AssetManager::LoadAndGet<asset::Model>(g_state.target.modelPath);
    if (!model || model->meshes.empty()) return false;

    /// @name クリップ解決 + 時刻計算
    const asset::AnimationClip* clipA =
        ResolveClip(model, g_state.target.clipName, g_state.target.animAssetPath);
    const asset::AnimationClip* clipB = nullptr;
    if (g_state.target.mode == PreviewTarget::Mode::Transition) {
        const asset::Model* toModel = g_state.target.toSourcePath == g_state.target.modelPath
            ? model
            : asset::AssetManager::LoadAndGet<asset::Model>(g_state.target.toSourcePath);
        clipB = ResolveClip(toModel, g_state.target.toClipName,
                            g_state.target.toAnimAssetPath);
    }
    float secondsA = 0.0f, secondsB = 0.0f, blendWeight = 0.0f;
    ComputePlaybackSample(clipA, clipB, secondsA, secondsB, blendWeight);

    /// @name スキニングパレット + ノードアニメーション
    const bool hasRigidMeshes = std::any_of(
        model->meshes.begin(), model->meshes.end(),
        [](const auto& mesh) { return mesh && !mesh->isSkinned; });
    /// @note 剛体メッシュは GPU スキニングを通らないため、ノード姿勢も常に計算する。
    const bool needsNodePose = hasRigidMeshes && model->skeleton && clipA;
    /// @note 接地リングはルートボーンの «今» の位置を要るので、有効ならポーズを組む。
    const bool wantsDebugPose =
        g_state.showBones || g_state.showBoneNames || g_state.showTrail ||
        g_state.showGhost || g_state.showInfo || g_state.showRootMotion ||
        g_state.selectedBoneNode >= 0 || g_state.view.showGroundRing || needsNodePose;
    g_state.debugPoseValid = false;
    g_state.selectedBoneValid = false;

    PreviewSkinningCB skinning{};
    for (auto& bone : skinning.boneMatrices) bone = math::Matrix4::Identity();
    std::vector<math::Matrix4> previewNodeGlobals;
    std::vector<math::Matrix4> previewBindGlobals;
    if (model->skeleton && model->skeleton->rootNodeIndex >= 0 && hasRigidMeshes) {
        const asset::Skeleton& skeleton = *model->skeleton;
        previewBindGlobals.assign(skeleton.nodes.size(), math::Matrix4::Identity());
        BuildBindPoseGlobals(skeleton, skeleton.rootNodeIndex,
                             math::Matrix4::Identity(), previewBindGlobals);
        /// @note クリップが無い場合も剛体メッシュを正しいバインド姿勢で描けるようにする。
        previewNodeGlobals = previewBindGlobals;
    }
    if (model->skeleton && model->skeleton->rootNodeIndex >= 0 && clipA) {
        const asset::Skeleton& skeleton = *model->skeleton;
        std::vector<math::Matrix4> palette(skeleton.bones.size(),
                                           math::Matrix4::Identity());
        std::vector<math::Matrix4>* nodeGlobalsPtr = nullptr;
        if (wantsDebugPose) {
            if (previewNodeGlobals.empty())
                previewNodeGlobals.assign(skeleton.nodes.size(), math::Matrix4::Identity());
            nodeGlobalsPtr = &previewNodeGlobals;
        }
        const double ticksA = static_cast<double>(secondsA) * ClipTicksPerSecond(*clipA);
        const double ticksB = clipB
            ? static_cast<double>(secondsB) * ClipTicksPerSecond(*clipB)
            : 0.0;
        EvaluatePreviewNode(skeleton, clipA, ticksA, clipB, ticksB, blendWeight,
                            skeleton.rootNodeIndex, math::Matrix4::Identity(), palette,
                            nodeGlobalsPtr);
        const size_t count =
            (std::min)(palette.size(), static_cast<size_t>(asset::MAX_SKINNING_BONES));
        for (size_t i = 0; i < count; ++i) skinning.boneMatrices[i] = palette[i];

        if (wantsDebugPose) {
            /// @note クリップが変わったときだけトラック有無・軌跡を再計算する (FindTrack が高コストのため)。
            const std::string cacheIdentity = g_state.target.modelPath + "|" +
                g_state.target.clipName + "|" + g_state.target.animAssetPath + "|" +
                g_state.target.toClipName;
            if (cacheIdentity != g_state.debugCacheIdentity) {
                g_state.debugCacheIdentity = cacheIdentity;
                g_state.selectedBoneNode = -1;
                g_state.nodeHasTrack.assign(skeleton.nodes.size(), 0);
                g_state.animatedNodeCount = 0;
                g_state.boneNodeCount = 0;
                for (size_t i = 0; i < skeleton.nodes.size(); ++i) {
                    if (skeleton.nodes[i].boneIndex < 0) continue;
                    ++g_state.boneNodeCount;
                    if (FindTrack(*clipA, skeleton.nodes[i].name)) {
                        g_state.nodeHasTrack[i] = 1;
                        ++g_state.animatedNodeCount;
                    }
                }
                BuildTrailPoints(skeleton, *clipA, g_state.trailPoints, g_state.trailNodeIndex);
            }

            /// @note ルートモーション解析はクリップ単位で 1 回。トグルを後から点けた場合も
            /// @note «そのクリップでまだ解析していない» としてここで組む。
            if (g_state.showRootMotion &&
                g_state.rootMotionCacheKey != g_state.debugCacheIdentity) {
                g_state.rootMotionCacheKey = g_state.debugCacheIdentity;
                AnalyzeRootMotion(skeleton, *clipA, g_state.rootMotion);
            }

            /// @note ノードグローバル → ワールド位置 (メッシュと同じく rootInverse 空間へ揃える)
            auto globalsToPositions = [&](const std::vector<math::Matrix4>& globals,
                                          std::vector<math::Vector3>& out) {
                out.resize(globals.size());
                for (size_t i = 0; i < globals.size(); ++i)
                    out[i] = MatrixTranslation(skeleton.rootInverseTransform * globals[i]);
            };
            globalsToPositions(previewNodeGlobals, g_state.jointPositions);

            /// @note オニオンスキン: 前後フレームのスケルトンを併記して動きの変化量を読めるようにする。
            if (g_state.showGhost) {
                /// @note 手動オフセット指定があればそれを、無ければタイムライン長依存の自動値を使う。
                const float ghostOffset = g_state.ghostOffsetSeconds > 0.0001f
                    ? g_state.ghostOffsetSeconds
                    : (std::max)(g_state.timelineLength / 14.0f, 1.0f / 30.0f);
                auto evaluateGhost = [&](float atTime, std::vector<math::Vector3>& out) {
                    float ghostA = 0.0f, ghostB = 0.0f, ghostBlend = 0.0f, unusedLen = 0.0f;
                    ComputePlaybackSampleAt(WrapTime(atTime, g_state.timelineLength),
                                            clipA, clipB,
                                            ghostA, ghostB, ghostBlend, unusedLen);
                    std::vector<math::Matrix4> ghostPalette(skeleton.bones.size(),
                                                            math::Matrix4::Identity());
                    std::vector<math::Matrix4> ghostGlobals(skeleton.nodes.size(),
                                                            math::Matrix4::Identity());
                    EvaluatePreviewNode(
                        skeleton, clipA,
                        static_cast<double>(ghostA) * ClipTicksPerSecond(*clipA),
                        clipB,
                        clipB ? static_cast<double>(ghostB) * ClipTicksPerSecond(*clipB) : 0.0,
                        ghostBlend, skeleton.rootNodeIndex, math::Matrix4::Identity(),
                        ghostPalette, &ghostGlobals);
                    globalsToPositions(ghostGlobals, out);
                };
                evaluateGhost(g_state.time - ghostOffset, g_state.ghostPrevPositions);
                evaluateGhost(g_state.time + ghostOffset, g_state.ghostNextPositions);
            }

            /// @note 選択中ボーンのローカル TRS とキー数をライブ更新する。
            if (g_state.selectedBoneNode >= 0 &&
                g_state.selectedBoneNode < static_cast<int>(skeleton.nodes.size())) {
                const auto& node =
                    skeleton.nodes[static_cast<size_t>(g_state.selectedBoneNode)];
                g_state.selectedBonePose = SamplePose(node, clipA, ticksA);
                if (const auto* track = FindTrack(*clipA, node.name)) {
                    g_state.selectedBoneKeyCounts[0] = static_cast<int>(track->positions.size());
                    g_state.selectedBoneKeyCounts[1] = static_cast<int>(track->rotations.size());
                    g_state.selectedBoneKeyCounts[2] = static_cast<int>(track->scales.size());
                } else {
                    g_state.selectedBoneKeyCounts[0] = 0;
                    g_state.selectedBoneKeyCounts[1] = 0;
                    g_state.selectedBoneKeyCounts[2] = 0;
                }
                g_state.selectedBoneValid = true;

                /// @note カーブグラフ用の全長サンプルは、ボーンかクリップが変わったときだけ再計算する。
                if (g_state.showCurves) {
                    const std::string curveKey = g_state.debugCacheIdentity + "|" +
                        std::to_string(g_state.selectedBoneNode);
                    if (curveKey != g_state.curveCacheKey) {
                        g_state.curveCacheKey = curveKey;
                        BuildBoneCurves(
                            skeleton, *clipA, g_state.selectedBoneNode,
                            g_state.curvePosX, g_state.curvePosY, g_state.curvePosZ,
                            g_state.curveRotX, g_state.curveRotY, g_state.curveRotZ,
                            g_state.curvePosMin, g_state.curvePosMax,
                            g_state.curveRotMin, g_state.curveRotMax);
                    }
                }
            }

            g_state.debugPoseValid = true;
        }
    }
    resources.Update(g_gpu.skinningCB, &skinning, sizeof(skinning));

    /// @name カメラ (オービット)
    PreviewBounds bounds;
    ComputeModelBounds(*model, bounds);
    g_state.groundY = bounds.minY;
    g_state.boundsRadius = bounds.radius;
    g_state.footprintRadius = bounds.radiusXZ;
    if (g_state.hasPendingFocus) {
        /// @note ボーンフォーカス: 距離は «その部位が画面に収まる» 程度まで寄せる。
        g_state.focus = g_state.pendingFocus;
        if (g_state.pendingDistance > 0.0f) g_state.distance = g_state.pendingDistance;
        g_state.hasPendingFocus = false;
        g_state.needsFraming = false;
    } else if (g_state.needsFraming || g_state.distance <= 0.0f) {
        g_state.focus = bounds.center;
        g_state.distance = bounds.radius * 2.6f;
        g_state.needsFraming = false;
    }
    const float cosPitch = std::cos(g_state.pitch);
    const math::Vector3 orbitOffset{
        cosPitch * std::sin(g_state.yaw) * g_state.distance,
        std::sin(g_state.pitch) * g_state.distance,
        cosPitch * std::cos(g_state.yaw) * g_state.distance
    };
    renderer::Camera camera;
    camera.m_position = g_state.focus + orbitOffset;
    camera.m_aspect = (std::max)(displayAspect, 0.1f);
    camera.m_fovY = std::clamp(g_state.view.fovY, 12.0f, 90.0f);
    camera.m_near = (std::max)(0.01f, g_state.distance * 0.01f);
    camera.m_far = (std::max)(50.0f, g_state.distance + bounds.radius * 8.0f);
    camera.LookAt(g_state.focus);

    scene::PerFrameCB frameData{};
    frameData.view = camera.GetViewMatrix();
    /// @note 描画先 RT は Reversed-Z (CameraDepthTargetDesc)。xy と w は CPU 用の射影と同じ。
    frameData.projection = camera.GetGpuProjectionMatrix();
    frameData.viewProjection = camera.GetGpuViewProjection();
    frameData.invViewProjection = math::Matrix4::Inverse(frameData.viewProjection);
    frameData.cameraPos = camera.m_position;
    frameData.nearZ = camera.m_near;
    frameData.farZ = camera.m_far;
    resources.Update(g_gpu.frameCB, &frameData, sizeof(frameData));
    /// @note オーバーレイ (ボーン線など) が CPU 側で同じ射影を使えるように保持する。
    g_state.debugViewProjection = frameData.viewProjection;

    scene::PerObjectCB objectData{};
    objectData.world = math::Matrix4::Identity();
    objectData.worldInvTranspose = math::Matrix4::Identity();

    /// @name ライティング (サムネイルと同じ 3 点照明リグの簡易版)
    renderer::LightConstantsCB lightData{};
    math::Vector3 keyOffset =
        camera.m_position +
        math::Vector3{ bounds.radius * 1.4f, bounds.radius * 1.8f, bounds.radius * 0.8f } -
        g_state.focus;
    /// @note キーライトだけを水平回転させる。陰の出方を変えてシルエットを読むための調整口。
    if (std::abs(g_state.view.lightYaw) > 0.0001f) {
        const float cs = std::cos(g_state.view.lightYaw);
        const float sn = std::sin(g_state.view.lightYaw);
        keyOffset = { keyOffset.x * cs + keyOffset.z * sn,
                      keyOffset.y,
                      -keyOffset.x * sn + keyOffset.z * cs };
    }
    const math::Vector3 keyDirection = keyOffset.Normalized();
    lightData.lightDir = { -keyDirection.x, -keyDirection.y, -keyDirection.z };
    lightData.lightColor = { 1.0f, 0.97f, 0.92f };
    /// @note 手調整リグなので、Lighting.hlsli の LIGHT_UNIT_SCALE (= PI) を相殺して
    /// @note 記述値がそのまま「絵として狙った明るさ」を表すようにする (サムネイルと同じ方針)。
    constexpr float kPreviewUnitScale = 3.14159265358979323846f;
    lightData.lightIntensity = 1.6f / kPreviewUnitScale;
    lightData.ambientColor = { 0.16f, 0.17f, 0.20f };
    /// @note LightAttenuation の逆二乗を打ち消し、intensity を「最終的な明るさ」として扱う。
    /// @note range = boundsRadius * 20 に対し dist は boundsRadius * 3 前後なので range 窓はほぼ 1.0。
    auto placeLight = [&](renderer::PointLight& light,
                          const math::Vector3& offset,
                          const math::Vector3& color,
                          float intensity) {
        light.position = g_state.focus + offset;
        light.color = color;
        light.range = bounds.radius * 20.0f;
        const float dist = offset.Length();
        /// @note シェーダー側の特異点ガード max(d*d, 0.01) と同じ下限を掛ける。
        light.intensity = intensity * std::max(dist * dist, 0.01f) / kPreviewUnitScale;
    };
    placeLight(lightData.pointLights[0],
               { bounds.radius * 2.6f, -bounds.radius * 1.2f, -bounds.radius * 2.2f },
               { 0.55f, 0.65f, 1.0f }, 0.35f);
    placeLight(lightData.pointLights[1],
               { -bounds.radius * 1.6f, bounds.radius * 2.4f, bounds.radius * 2.6f },
               { 1.0f, 1.0f, 1.0f }, 0.9f);
    lightData.pointLightCount = 2;
    resources.Update(g_gpu.lightCB, &lightData, sizeof(lightData));

    /// @note シャドウは bias=1.0 で常時ライティング扱いにして無効化する (サムネイルと同じ手法)。
    scene::ShadowConstantsCB shadowData{};
    shadowData.lightViewProjection = math::Matrix4::Translate({ 16.0f, 16.0f, 0.0f });
    shadowData.shadowMapTexelSize[0] = 0.0f;
    shadowData.shadowMapTexelSize[1] = 0.0f;
    shadowData.shadowBias = 1.0f;
    resources.Update(g_gpu.shadowCB, &shadowData, sizeof(shadowData));

    /// @name 描画
    auto& renderer = *ctx.renderer;
    renderer.SetRenderTarget(g_gpu.renderTarget, resources);
    const int backgroundIndex =
        std::clamp(g_state.view.background, 0, kPreviewBackgroundCount - 1);
    const auto& bg = kPreviewBackgrounds[backgroundIndex];
    renderer.Clear({ bg[0], bg[1], bg[2], bg[3] });
    renderer.ClearDepth();
    const bool maskColorMode = g_maskPreview.active && g_maskPreview.loaded;
    /// @note スキンメッシュの着色に使うボーン別ウェイト。描画は RenderPreviewFrame が
    /// @note キャッシュミスのときだけ呼ぶので、ここで組んでもフレームごとには走らない。
    const std::vector<float> boneMaskWeights =
        (maskColorMode && model->skeleton)
            ? BuildBoneMaskWeights(*model->skeleton, g_maskPreview.mask)
            : std::vector<float>{};

    const auto previewWorldForMesh = [&](size_t meshIndex,
                                         const renderer::Mesh& mesh) {
        math::Matrix4 meshWorld = math::Matrix4::Identity();
        if (!mesh.isSkinned && model->skeleton && !previewNodeGlobals.empty()) {
            const int modelNodeIndex = model->FindNodeForMesh(
                static_cast<uint32_t>(meshIndex));
            const int skeletonNodeIndex =
                FindSkeletonNodeForModelNode(*model, modelNodeIndex);
            if (skeletonNodeIndex >= 0 &&
                skeletonNodeIndex < static_cast<int>(previewNodeGlobals.size())) {
                const math::Matrix4 animatedGlobal =
                    previewNodeGlobals[static_cast<size_t>(skeletonNodeIndex)];
                /// @note 現行フォーマットでは、スケルトンを持つモデル内の剛体メッシュは
                /// @note ノードローカル頂点として保存される。旧アセットも同じ挙動へ縮退させ、
                /// @note アニメーション付き FBX の混在メッシュをバインド姿勢で二重補正しない。
                const bool meshTransformsBaked = model->nodeTransformsBaked &&
                    model->skeleton == nullptr;
                if (meshTransformsBaked &&
                    skeletonNodeIndex < static_cast<int>(previewBindGlobals.size())) {
                    /// @note 静的メッシュはバインド姿勢が頂点へ焼き込まれているため、
                    /// @note 「現在姿勢 × バインド姿勢の逆行列」だけを追加して二重変換を避ける。
                    meshWorld = animatedGlobal * math::Matrix4::Inverse(
                        previewBindGlobals[static_cast<size_t>(skeletonNodeIndex)]);
                } else {
                    /// @note 未ベイク形式は頂点がノードローカルなので、スキニングと同じ
                    /// @note rootInverse 空間へ変換した現在のノード姿勢を使う。
                    meshWorld = model->skeleton->rootInverseTransform * animatedGlobal;
                }
            }
        }
        return meshWorld;
    };
    g_state.meshPreviewWorlds.assign(model->meshes.size(), math::Matrix4::Identity());
    const auto updateMeshObjectTransform = [&](size_t meshIndex,
                                               const renderer::Mesh& mesh) {
        const math::Matrix4 meshWorld = previewWorldForMesh(meshIndex, mesh);
        g_state.meshPreviewWorlds[meshIndex] = meshWorld;
        objectData.world = meshWorld;
        objectData.worldInvTranspose = math::Matrix4::InverseTransposeAffine(meshWorld);
        resources.Update(g_gpu.objectCB, &objectData, sizeof(objectData));
    };

    const auto meshPso = (g_state.view.wireframe && g_gpu.wireframePso.IsValid())
        ? g_gpu.wireframePso : g_gpu.pso;

    /// @note Mesh トグル Off のときはスケルトンオーバーレイだけを見せる (デバッグ時の視認性優先)。
    for (size_t i = 0; g_state.showMesh && i < model->meshes.size(); ++i) {
        const auto& mesh = model->meshes[i];
        if (!mesh || !mesh->vertexBuffer.IsValid() || !mesh->indexBuffer.IsValid()) continue;
        updateMeshObjectTransform(i, *mesh);

        /// @note インポート済み .mat を優先し、無いモデル (旧 Assimp 経路など) だけ
        /// @note Model 自身の materials へ落とす。
        renderer::Material* material =
            ResolveImportedPreviewMaterial(ctx, resources, model->meshes.size(), i);
        if (!material && i < model->materials.size())
            material = model->materials[i].get();

        /// @note シェーダーの頂点入力がメッシュと合わなければ使えない。描画パスと同じくフォールバックする。スキンモデルの .mat はインポーターが一律 Skinned で書くため、同じ FBX 内の剛体メッシュ (武器など) にも Skinned シェーダーが付いてくる。
        const bool materialSupportsSkinning =
            material && resources.GetShaderCapabilities(material->shader).SupportsSkinning();
        const bool useMaterial =
            material && material->shader.IsValid() && material->paramsBuffer.IsValid() &&
            mesh->isSkinned == materialSupportsSkinning;

        renderer::DrawCall dc;
        dc.vertexBuffer = mesh->vertexBuffer;
        dc.indexBuffer = mesh->indexBuffer;
        dc.indexCount = mesh->indexCount;
        dc.vertexCount = mesh->vertexCount;
        dc.pipelineState = meshPso;
        dc.constantBuffers[0] = g_gpu.frameCB;
        dc.constantBuffers[1] = g_gpu.objectCB;
        dc.constantBuffers[3] = g_gpu.lightCB;
        dc.constantBuffers[4] = g_gpu.shadowCB;
        dc.constantBuffers[12] = g_gpu.punctualShadowCB;
        dc.constantBuffers[9] = g_gpu.clusterCB;
        if (mesh->isSkinned)
            dc.constantBuffers[7] = g_gpu.skinningCB;

        if (maskColorMode) {
            /// @note マスクプレビューでは元材質よりも「どの部位が効くか」の判読性を優先し、
            /// @note メッシュを担当ノードの実効ウェイト色で描く。
            PreviewMaterialCB materialData{};
            const float weight =
                MaskWeightForMesh(*model, i, g_maskPreview.mask, boneMaskWeights);
            ImVec4 color = MaskWeightColor(weight);
            const int meshNodeIndex = model->FindNodeForMesh(static_cast<uint32_t>(i));
            const bool selectedMesh = meshNodeIndex >= 0
                && g_maskPreview.selectedNodePath ==
                    MaskPathForModelNode(*model, meshNodeIndex);
            if (selectedMesh) {
                /// @note 選択部位は白を少し混ぜて明るくし、色相 (Weight) を残したままフォーカスを示す。
                color.x += (1.0f - color.x) * 0.45f;
                color.y += (1.0f - color.y) * 0.45f;
                color.z += (1.0f - color.z) * 0.45f;
            }
            materialData.albedo = { color.x, color.y, color.z, 1.0f };
            resources.Update(g_gpu.materialCB, &materialData, sizeof(materialData));
            dc.shader = mesh->isSkinned ? g_gpu.skinnedShader : g_gpu.surfaceShader;
            dc.constantBuffers[2] = g_gpu.materialCB;
        } else if (useMaterial) {
            dc.shader             = material->shader;
            dc.constantBuffers[2] = material->paramsBuffer;
            for (size_t ti = 0; ti < material->textures.size() && ti < 8; ++ti)
                if (material->textures[ti].IsValid()) dc.textures[ti] = material->textures[ti];
        } else {
            /// @note フォールバック: プレビュー既定のフラットマテリアル。
            /// @note アルベドテクスチャがあればそれだけは反映する。
            PreviewMaterialCB materialData{};
            renderer::ResourceHandle<renderer::TextureTag> albedoTexture;
            if (material && !material->textures.empty() && material->textures[0].IsValid()) {
                albedoTexture = material->textures[0];
                materialData.textureMask = 1u;
            }
            resources.Update(g_gpu.materialCB, &materialData, sizeof(materialData));
            dc.shader             = mesh->isSkinned ? g_gpu.skinnedShader : g_gpu.surfaceShader;
            dc.constantBuffers[2] = g_gpu.materialCB;
            dc.textures[0]        = albedoTexture;
        }
        renderer.Submit(dc, resources);
    }

    /// @name 床グリッド / 接地リング
    /// @note メッシュより «後» に出す。DEPTH_READ なので、既に書かれた深度がそのまま
    /// @note 「モデルの手前か奥か」の判定になり、キャラクターの向こう側だけが隠れる。
    if ((g_state.view.showGrid || g_state.view.showGroundRing) &&
        EnsurePreviewLineGpu(resources)) {
        const PreviewLineCameraCB lineCamera{ frameData.viewProjection };
        resources.Update(g_lineGpu.cameraCB, &lineCamera, sizeof(lineCamera));

        std::vector<PreviewLineVertex> lines;
        lines.reserve(2048);
        if (g_state.view.showGrid) BuildPreviewGridLines(lines);
        if (g_state.view.showGroundRing) {
            math::Vector3 rootWorld{ g_state.focus.x, g_state.groundY, g_state.focus.z };
            if (g_state.trailNodeIndex >= 0 &&
                g_state.trailNodeIndex < static_cast<int>(g_state.jointPositions.size()))
                rootWorld = g_state.jointPositions[
                    static_cast<size_t>(g_state.trailNodeIndex)];
            BuildPreviewGroundRing(lines, rootWorld);
        }
        SubmitPreviewLines(renderer, resources, lines);
    }

    /// @note ImGui 描画中の RT 切り替えなので、必ずバックバッファへ戻す。
    renderer.SetRenderTarget(renderer::ResourceHandle<renderer::RenderTargetTag>{}, resources);
    g_state.lastRenderOk = true;
    return true;
}

/// @note 再生時刻を進める。複数箇所 (Inspector + パネル) から呼ばれても 1 フレーム 1 回だけ。
void AdvancePlayback()
{
    if (g_state.lastAdvanceFrame == ImGui::GetFrameCount()) return;
    g_state.lastAdvanceFrame = ImGui::GetFrameCount();
    if (!g_state.playing) return;

    g_state.time += ImGui::GetIO().DeltaTime * g_state.speed;
    if (g_state.time > g_state.timelineLength) {
        if (g_state.loop) {
            g_state.time = WrapTime(g_state.time, g_state.timelineLength);
        } else {
            g_state.time = g_state.timelineLength;
            g_state.playing = false;
        }
    }
}

} /// @note namespace fbzz::editor::animpreview

namespace fbzz::editor {

bool HasAnimationPreviewTarget()
{
    return animpreview::g_state.target.mode != animpreview::PreviewTarget::Mode::None;
}

void TickAnimationPreview(EditorContext& ctx)
{
    /// @note Mask Preview の一時対象を通常 Animation の選択追従で上書きしない。
    if (!animpreview::g_maskPreview.active)
        animpreview::UpdatePreviewTarget(ctx);
    animpreview::AdvancePlayback();
}

void ShutdownAnimationPreview()
{
    /// @note ResourceManager は EditorApp の所有物なのでここでは解放せず、次回起動時に無効なハンドルと前回のアタッチ FBX が残らないよう状態だけ初期化する。
    /// @note 表示設定だけ持ち越すのは、EditorApp がこの関数を «パネルから設定を回収する前» に呼ぶため。トグルやカメラまで既定へ戻すと保存される値が常に既定値になる。捨てるのはハンドルと対象だけ。
    const animpreview::PreviewViewSettings view = animpreview::g_state.view;
    const bool showMesh      = animpreview::g_state.showMesh;
    const bool showBones     = animpreview::g_state.showBones;
    const bool showBoneNames = animpreview::g_state.showBoneNames;
    const bool showTrail     = animpreview::g_state.showTrail;
    const bool showGhost     = animpreview::g_state.showGhost;
    const bool showInfo      = animpreview::g_state.showInfo;
    const bool showCurves    = animpreview::g_state.showCurves;
    const bool showRootMotion = animpreview::g_state.showRootMotion;
    const int  labelMode     = animpreview::g_state.labelMode;
    const float ghostOffset  = animpreview::g_state.ghostOffsetSeconds;
    const bool loop          = animpreview::g_state.loop;
    const float speed        = animpreview::g_state.speed;
    const float yaw          = animpreview::g_state.yaw;
    const float pitch        = animpreview::g_state.pitch;

    animpreview::g_state = animpreview::PreviewState{};
    animpreview::g_gpu = animpreview::PreviewGpu{};
    animpreview::g_lineGpu.shader = {};
    animpreview::g_lineGpu.cameraCB = {};
    animpreview::g_lineGpu.pso = {};
    animpreview::g_materialCache = animpreview::PreviewMaterialCache{};
    animpreview::g_maskPreview = animpreview::MaskPreviewState{};

    animpreview::g_state.view = view;
    animpreview::g_state.showMesh = showMesh;
    animpreview::g_state.showBones = showBones;
    animpreview::g_state.showBoneNames = showBoneNames;
    animpreview::g_state.showTrail = showTrail;
    animpreview::g_state.showGhost = showGhost;
    animpreview::g_state.showInfo = showInfo;
    animpreview::g_state.showCurves = showCurves;
    animpreview::g_state.showRootMotion = showRootMotion;
    animpreview::g_state.labelMode = labelMode;
    animpreview::g_state.ghostOffsetSeconds = ghostOffset;
    animpreview::g_state.loop = loop;
    animpreview::g_state.speed = speed;
    animpreview::g_state.yaw = yaw;
    animpreview::g_state.pitch = pitch;
}

void LoadAnimationPreviewSettings(const EditorSettings& settings)
{
    auto& state = animpreview::g_state;
    state.showMesh           = settings.animPreviewShowMesh;
    state.showBones          = settings.animPreviewShowBones;
    state.showBoneNames      = settings.animPreviewShowBoneNames;
    state.showTrail          = settings.animPreviewShowTrail;
    state.showGhost          = settings.animPreviewShowGhost;
    state.showInfo           = settings.animPreviewShowInfo;
    state.showCurves         = settings.animPreviewShowCurves;
    state.showRootMotion     = settings.animPreviewShowRootMotion;
    state.labelMode          = std::clamp(settings.animPreviewLabelMode, 0, 2);
    state.ghostOffsetSeconds = std::clamp(settings.animPreviewGhostOffset, 0.0f, 0.5f);
    state.loop               = settings.animPreviewLoop;
    state.speed              = std::clamp(settings.animPreviewSpeed, 0.05f, 4.0f);
    state.yaw                = settings.animPreviewCameraYaw;
    state.pitch              = std::clamp(settings.animPreviewCameraPitch, -1.35f, 1.35f);

    state.view.showGrid      = settings.animPreviewShowGrid;
    state.view.showGroundRing = settings.animPreviewShowGroundRing;
    state.view.wireframe     = settings.animPreviewWireframe;
    state.view.showAxisGizmo = settings.animPreviewShowAxisGizmo;
    state.view.background    =
        std::clamp(settings.animPreviewBackground, 0, animpreview::kPreviewBackgroundCount - 1);
    state.view.fovY          = std::clamp(settings.animPreviewFov, 12.0f, 90.0f);
    state.view.lightYaw      = settings.animPreviewLightYaw;
}

void SaveAnimationPreviewSettings(EditorSettings& settings)
{
    const auto& state = animpreview::g_state;
    settings.animPreviewShowMesh       = state.showMesh;
    settings.animPreviewShowBones      = state.showBones;
    settings.animPreviewShowBoneNames  = state.showBoneNames;
    settings.animPreviewShowTrail      = state.showTrail;
    settings.animPreviewShowGhost      = state.showGhost;
    settings.animPreviewShowInfo       = state.showInfo;
    settings.animPreviewShowCurves     = state.showCurves;
    settings.animPreviewShowRootMotion = state.showRootMotion;
    settings.animPreviewLabelMode      = state.labelMode;
    settings.animPreviewGhostOffset    = state.ghostOffsetSeconds;
    settings.animPreviewLoop           = state.loop;
    settings.animPreviewSpeed          = state.speed;
    settings.animPreviewCameraYaw      = state.yaw;
    settings.animPreviewCameraPitch    = state.pitch;

    settings.animPreviewShowGrid       = state.view.showGrid;
    settings.animPreviewShowGroundRing = state.view.showGroundRing;
    settings.animPreviewWireframe      = state.view.wireframe;
    settings.animPreviewShowAxisGizmo  = state.view.showAxisGizmo;
    settings.animPreviewBackground     = state.view.background;
    settings.animPreviewFov            = state.view.fovY;
    settings.animPreviewLightYaw       = state.view.lightYaw;
}

} /// @note namespace fbzz::editor
