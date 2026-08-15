// FBZZ Engine
// AnimationPreviewPanel.cpp | fbzz::editor
// Unity の Animation Preview 相当のオフスクリーン再生ビュー
// WHAT: 選択中の State / Transition / .anim / モデルからクリップを解決し、
//       スキンメッシュを SkinnedLit でレンダーターゲットへ毎フレーム描画する。
//       Inspector 下部と独立パネルが同じ再生状態 (時刻・カメラ・速度) を共有する。
#include <Editor/Panels/AnimationPreviewPanel.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/Util/AssetPath.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/Model.hpp>
#include <Engine/Renderer/Camera.hpp>
#include <Engine/Renderer/DrawCall.hpp>
#include <Engine/Renderer/IImGuiRenderer.hpp>
#include <Engine/Renderer/IRenderer.hpp>
#include <Engine/Renderer/LightSystem.hpp>
#include <Engine/Renderer/Mesh.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Scene/Components/AnimatorComponent.hpp>
#include <Engine/Scene/Components/SkinnedMeshRenderer.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Engine/Util/StringUtils.hpp>
#include <imgui.h>
#include <algorithm>
#include <bit>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace fbzz::editor {

namespace {

// ─────────────────────────────────────────────────────────────────────────────
// クリップサンプリング
// AnimatorSystem.cpp と同じ規約 (tick 空間サンプル + FBX チャンネル名の正規化)。
// WHY: ランタイム側は Scene の AnimatorComponent と密結合しているため、
//      プレビューは Skeleton + Clip だけで完結する軽量版をここに持つ。
// ─────────────────────────────────────────────────────────────────────────────

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

const asset::NodeAnimationTrack* FindTrack(const asset::AnimationClip& clip,
                                           const std::string& nodeName)
{
    for (const auto& track : clip.tracks)
        if (track.nodeName == nodeName)
            return &track;

    // FBX の補助ノード suffix / namespace 差を吸収する (AnimatorSystem::FindTrack と同一)。
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

struct LocalPose {
    math::Vector3 translation = math::Vector3::ZERO;
    math::Quaternion rotation = math::Quaternion::Identity();
    math::Vector3 scale = math::Vector3::ONE;
};

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

// クリップ A (+任意でクリップ B とのブレンド) を評価し、スキニングパレットを構築する。
// blendWeight: 0 = A のみ / 1 = B のみ。Transition プレビューのクロスフェードに使う。
// nodeGlobals (任意): ボーン可視化などデバッグ表示のため、全ノードのグローバル行列も併記する。
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

// 列ベクトル規約 (M * v) の行優先行列から平行移動成分を取り出す。
math::Vector3 MatrixTranslation(const math::Matrix4& m)
{
    return { m.m[0][3], m.m[1][3], m.m[2][3] };
}

// 表示専用のオイラー角分解 (XYZ, degrees)。Inspector の数値表示にだけ使い、計算には使わない。
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

// FBX の namespace prefix (mixamorig: 等) を落とした短いボーン名。オーバーレイ表示用。
std::string ShortBoneName(const std::string& name)
{
    const size_t namespacePos = name.find_last_of(':');
    return namespacePos != std::string::npos ? name.substr(namespacePos + 1) : name;
}

// ルートモーション確認用の軌跡: 最初の変形ボーン (通常 Hips) の位置をクリップ全長に渡って
// サンプルする。親チェーンだけ評価するので全階層評価より大幅に軽い。
void BuildTrailPoints(const asset::Skeleton& skeleton,
                      const asset::AnimationClip& clip,
                      std::vector<math::Vector3>& outPoints)
{
    outPoints.clear();
    if (skeleton.rootNodeIndex < 0) return;

    int trailNode = -1;
    std::vector<int> pending{ skeleton.rootNodeIndex };
    while (!pending.empty()) {
        const int nodeIndex = pending.back();
        pending.pop_back();
        if (nodeIndex < 0 || nodeIndex >= static_cast<int>(skeleton.nodes.size())) continue;
        if (skeleton.nodes[static_cast<size_t>(nodeIndex)].boneIndex >= 0) {
            trailNode = nodeIndex;
            break;
        }
        for (int child : skeleton.nodes[static_cast<size_t>(nodeIndex)].children)
            pending.push_back(child);
    }
    if (trailNode < 0) return;

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

double ClipTicksPerSecond(const asset::AnimationClip& clip)
{
    return clip.ticksPerSecond > 0.0 ? clip.ticksPerSecond : 30.0;
}

// 単一ボーンのローカル位置 XYZ・回転オイラー XYZ をクリップ全長でサンプルし、
// カーブミニグラフ用の配列と min/max を構築する。位置と回転は別スケールで正規化する。
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

// ─────────────────────────────────────────────────────────────────────────────
// プレビュー状態 (Inspector 埋め込みと独立パネルで共有)
// ─────────────────────────────────────────────────────────────────────────────

struct PreviewTarget {
    enum class Mode { None, Clip, Transition };
    Mode mode = Mode::None;
    std::string modelPath;      // メッシュとスケルトンの供給元 (.fbx / .asset / .fzasset)
    std::string clipName;       // 空 = モデル先頭クリップ
    std::string animAssetPath;  // .anim 直接プレビュー時のみ。clipName より優先される
    std::string toClipName;     // Transition の遷移先クリップ
    std::string toSourcePath;   // 遷移先クリップの供給元 (from と異なる source を許容)
    float blendSeconds = 0.25f;
    float transitionStartSeconds = 0.0f;
    std::string label;

    // 再生時刻をリセットすべき対象変化かどうかの同一性 (パラメーター編集では維持する)
    bool SameIdentity(const PreviewTarget& other) const
    {
        return mode == other.mode && modelPath == other.modelPath &&
               clipName == other.clipName && animAssetPath == other.animAssetPath &&
               toClipName == other.toClipName && toSourcePath == other.toSourcePath;
    }
};

// Manual = プレビュー画面へ直接ドロップされた対象。選択に追従せず、次の選択操作まで維持する。
// GameObject = Animator を持つシーン上のオブジェクト選択由来 (Unity の既定動線)。
enum class TargetOrigin { None, Graph, Asset, GameObject, Manual };

struct PreviewGpu {
    renderer::ResourceHandle<renderer::ShaderTag>         skinnedShader;
    renderer::ResourceHandle<renderer::ShaderTag>         surfaceShader;
    renderer::ResourceHandle<renderer::PipelineStateTag>  pso;
    renderer::ResourceHandle<renderer::ConstantBufferTag> frameCB;
    renderer::ResourceHandle<renderer::ConstantBufferTag> objectCB;
    renderer::ResourceHandle<renderer::ConstantBufferTag> materialCB;
    renderer::ResourceHandle<renderer::ConstantBufferTag> lightCB;
    renderer::ResourceHandle<renderer::ConstantBufferTag> shadowCB;
    renderer::ResourceHandle<renderer::ConstantBufferTag> skinningCB;
    renderer::ResourceHandle<renderer::RenderTargetTag>   renderTarget;
};

struct PreviewSkinningCB {
    math::Matrix4 boneMatrices[asset::MAX_SKINNING_BONES];
};

struct PreviewMaterialCB {
    math::Vector4 albedo{ 0.72f, 0.72f, 0.75f, 1.0f };
    uint32_t textureMask = 0;
    float _pad[3] = {};
};

struct PreviewState {
    PreviewTarget target;
    TargetOrigin origin = TargetOrigin::None;

    // ユーザーが明示的に指定したジオメトリ (プレビューへ D&D したモデル)。
    // WHY: FBZZ は「1 クリップ = 1 FBX」規約のため、.anim の隣に
    //   スキンメッシュを持つモデルが存在しない。Unity が .anim プレビューで
    //   モデルを差し替えられるのと同じく、一度指定したジオメトリを
    //   セッション中は覚えておき、以降すべてのクリップに使い回す。
    std::string userModelPath;

    // Animator 付き GameObject の選択変化検出用
    scene::EntityID lastSelectedEntity = scene::EntityID::INVALID;

    // 直前フレームの選択スナップショット。「後から変わった方」をプレビュー対象にする。
    EditorContext::AnimationGraphSelection::Type lastGraphType =
        EditorContext::AnimationGraphSelection::Type::None;
    int lastGraphState = -1;
    int lastGraphTransition = -1;
    std::string lastGraphAssetPath;
    scene::EntityID lastGraphEntity = scene::EntityID::INVALID;
    std::string lastSelectedAssetPath;

    // 再生
    float time = 0.0f;
    bool  playing = true;
    bool  loop = true;
    float speed = 1.0f;
    float timelineLength = 1.0f;
    float currentBlendWeight = 0.0f; // Transition の現在ブレンド率 (UI 表示用)

    // カメラ (対象が変わっても維持し、Unity と同じく視点を保つ)
    float yaw = 2.55f;
    float pitch = 0.30f;
    float distance = -1.0f; // <0 = 未フレーミング (バウンディングから自動決定)
    math::Vector3 focus = math::Vector3::ZERO;
    bool needsFraming = true;

    // 1 フレーム 1 回だけ時間更新 + 描画するためのガード
    int lastAdvanceFrame = -1;
    int lastRenderFrame = -1;
    bool lastRenderOk = false;

    // ── デバッグ可視化 (Unity の Preview にない差別化要素) ──────────────────
    bool showMesh = true;       // Off でスケルトンのみ表示
    bool showBones = false;     // ボーンオーバーレイ
    bool showBoneNames = false; // ボーン名ラベル
    bool showTrail = false;     // 代表ボーンの移動軌跡 (ルートモーション確認)
    bool showGhost = false;     // オニオンスキン (前後フレームの残像スケルトン)
    bool showInfo = false;      // 時刻 / フレーム / トラック情報オーバーレイ
    bool showCurves = false;    // 選択ボーンの位置/回転カーブミニグラフ
    int  selectedBoneNode = -1; // クリックで選択中のスケルトンノード index

    // ラベル間引き設定。100+ ボーンモデルで全名を出すと潰れるため、
    // label mode で「選択+ホバー / アニメ有 / 全部」を切り替える。
    // 0=Selected/Hovered only, 1=Animated bones, 2=All
    int labelMode = 1;
    // オニオンスキンの前後フレームオフセット (秒)。0 = タイムライン長依存の自動。
    float ghostOffsetSeconds = 0.0f;

    // RenderPreviewFrame が毎フレーム更新するオーバーレイ用データ
    std::vector<math::Vector3> jointPositions;      // 現在ポーズの各ノード位置 (ワールド)
    std::vector<math::Vector3> ghostPrevPositions;  // オニオンスキン (過去側)
    std::vector<math::Vector3> ghostNextPositions;  // オニオンスキン (未来側)
    math::Matrix4 debugViewProjection = math::Matrix4::Identity();
    bool debugPoseValid = false;

    // クリップが変わったときだけ再計算するキャッシュ
    std::string debugCacheIdentity;
    std::vector<uint8_t> nodeHasTrack; // クリップが実際に動かすノードか (トラック有無)
    std::vector<math::Vector3> trailPoints;
    int animatedNodeCount = 0;
    int boneNodeCount = 0;

    // 選択中ボーンのライブ情報 (RenderPreviewFrame がサンプル)
    LocalPose selectedBonePose;
    bool selectedBoneValid = false;
    int selectedBoneKeyCounts[3] = { 0, 0, 0 }; // P / R / S キー数

    // 選択ボーンのカーブグラフ用キャッシュ。ボーンかクリップが変わったときだけ再サンプルする。
    // WHY: 毎フレーム全キーを再サンプルするとカーブ描画のためだけに CPU を無駄遣いするため。
    std::string curveCacheKey;          // "modelPath|clipName|nodeIndex" で識別
    std::vector<float> curvePosX, curvePosY, curvePosZ;   // 正規化前の生値
    std::vector<float> curveRotX, curveRotY, curveRotZ;   // オイラー角 (degrees)
    float curvePosMin = 0.0f, curvePosMax = 0.0f;
    float curveRotMin = 0.0f, curveRotMax = 0.0f;
};

PreviewState s_state;
PreviewGpu s_gpu;
constexpr int PREVIEW_RT_SIZE = 512;

ImTextureID ToImTextureID(void* ptr)
{
    return static_cast<ImTextureID>(std::bit_cast<std::uintptr_t>(ptr));
}

// モデル内クリップを名前で検索。空名は先頭クリップ (State の <Auto / First Clip> と同じ規約)。
const asset::AnimationClip* FindModelClip(const asset::Model* model, const std::string& clipName)
{
    if (!model || model->clips.empty()) return nullptr;
    if (!clipName.empty()) {
        for (const auto& clip : model->clips)
            if (clip.name == clipName) return &clip;
    }
    return &model->clips.front();
}

// .anim 直接指定があればそちら、なければモデル内クリップを解決する。
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

// State からプレビュー用の (sourcePath, clipName) を取り出す。
// Blend Tree はランタイム Weight 依存のため、先頭モーションを代表として使う。
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

// ジオメトリ探索ヘルパの前方宣言。
// 実体は ResolvePathTarget の直前 (パス解決まわりをまとめた位置) に置いている。
bool        LoadsAsPreviewableGeometry(const std::string& path);
std::string FindGeometryForAnim(const std::string& animPath);

// State / Animator の sourcePath を PreviewTarget の適切なスロットへ振り分ける。
//
// WHY: FBZZ の sourcePath は「1 クリップ = 1 FBX」規約により .anim を指すことが多い。
//   これをそのまま modelPath に入れると LoadModel が失敗してプレビューが真っ黒になる。
//   .anim ならクリップ側スロットへ入れ、器は別途探す。
void AssignClipSource(PreviewTarget& out, const std::string& source,
                      const std::string& clipName)
{
    if (source.empty()) return;
    if (util::StringUtils::EndsWith(util::StringUtils::ToLower(source), ".anim")) {
        out.animAssetPath = source;
        out.clipName.clear();
        std::string geometry = FindGeometryForAnim(source);
        if (geometry.empty()) geometry = s_state.userModelPath;
        out.modelPath = geometry;
    } else {
        out.modelPath = source;
        out.clipName = clipName;
    }
}

// GameObject 階層から最初のスキンメッシュを探し、その modelPath を返す。
// WHY: MiniBot は本体 GO に Animator、子 GO 群に SkinnedMeshRenderer という構成のため、
//   自分自身だけを見ると器が見つからない。
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

// 選択中の GameObject (Animator 付き) からプレビュー対象を解決する。
// Unity と同じく、Animator を持つオブジェクトを選ぶだけでプレビューできるようにする。
bool ResolveGameObjectTarget(EditorContext& ctx, PreviewTarget& out)
{
    scene::GameObject* go = ctx.GetSelectedGO();
    if (!go) return false;
    auto* animator = go->GetComponent<scene::AnimatorComponent>();
    if (!animator) return false;

    // クリップ: 現在ステート → デフォルトステート → 先頭ステート
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

    // 器はシーン上の実物 (SkinnedMeshRenderer) を最優先で使う。
    if (std::string geometry = FindGeometryInHierarchy(go); !geometry.empty())
        out.modelPath = geometry;
    if (out.modelPath.empty()) out.modelPath = s_state.userModelPath;

    // クリップも器も無ければプレビューする意味がない。
    return !out.modelPath.empty() || !out.animAssetPath.empty();
}

// 現在の Animation Graph 選択からプレビュー対象を解決する。
bool ResolveGraphTarget(EditorContext& ctx, PreviewTarget& out)
{
    const auto& selection = ctx.animationGraphSelection;
    using Type = EditorContext::AnimationGraphSelection::Type;
    if (selection.type == Type::None || selection.type == Type::AnyState) return false;

    // Controller アセット編集中はそのモデル、Scene 上の GameObject 選択ならそのコンポーネント。
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

    auto stateByName = [&](const std::string& name) -> const scene::AnimationState* {
        for (const auto& state : animator->states)
            if (state.name == name) return &state;
        return nullptr;
    };

    if (selection.type == Type::State) {
        if (selection.stateIndex < 0 ||
            selection.stateIndex >= static_cast<int>(animator->states.size()))
            return false;
        const auto& state = animator->states[static_cast<size_t>(selection.stateIndex)];
        std::string source, clip;
        if (!StatePreviewSource(state, source, clip) || source.empty()) return false;
        out.mode = PreviewTarget::Mode::Clip;
        AssignClipSource(out, source, clip);
        // Graph 側でも器はシーン上の SkinnedMeshRenderer を優先する。
        if (selection.entityId.IsValid() && ctx.activeScene) {
            if (auto* owner = ctx.activeScene->GetGameObject(selection.entityId)) {
                if (std::string geo = FindGeometryInHierarchy(owner); !geo.empty())
                    out.modelPath = geo;
            }
        }
        out.label = state.name;
        if (state.mode != scene::AnimationStateMode::Clip)
            out.label += "  (Blend Tree: first motion)";
        return true;
    }

    // Transition / AnyStateTransition
    const scene::AnimationTransition* transition = nullptr;
    const scene::AnimationState* fromState = nullptr;
    if (selection.type == Type::Transition) {
        if (selection.stateIndex < 0 ||
            selection.stateIndex >= static_cast<int>(animator->states.size()))
            return false;
        fromState = &animator->states[static_cast<size_t>(selection.stateIndex)];
        if (selection.transitionIndex < 0 ||
            selection.transitionIndex >= static_cast<int>(fromState->transitions.size()))
            return false;
        transition = &fromState->transitions[static_cast<size_t>(selection.transitionIndex)];
    } else {
        if (selection.transitionIndex < 0 ||
            selection.transitionIndex >= static_cast<int>(animator->anyStateTransitions.size()))
            return false;
        transition =
            &animator->anyStateTransitions[static_cast<size_t>(selection.transitionIndex)];
    }

    const scene::AnimationState* toState = stateByName(transition->toStateName);
    if (!toState) return false;
    std::string toSource, toClip;
    if (!StatePreviewSource(*toState, toSource, toClip) || toSource.empty()) return false;

    std::string fromSource, fromClip;
    const bool hasFrom =
        fromState && StatePreviewSource(*fromState, fromSource, fromClip) && !fromSource.empty();

    if (!hasFrom) {
        // Any State 遷移は遷移元が不定のため、遷移先クリップの単独プレビューにする。
        out.mode = PreviewTarget::Mode::Clip;
        out.modelPath = toSource;
        out.clipName = toClip;
        out.label = std::string("Any State -> ") + toState->name;
        return true;
    }

    // 遷移元クリップの長さから、Exit Time とブレンド秒数を Transition Preview と同じ式で求める。
    const asset::Model* fromModel = asset::AssetManager::LoadModel(fromSource);
    const asset::AnimationClip* fromClipPtr = FindModelClip(fromModel, fromClip);
    const float fromLength = ClipDurationSeconds(fromClipPtr, 1.0f);
    const float blendSeconds = transition->fixedDuration
        ? transition->transitionDuration
        : transition->transitionDuration * fromLength;
    const float transitionStart = transition->hasExitTime
        ? transition->exitTime * fromLength
        : (std::max)(fromLength - blendSeconds, 0.0f);

    out.mode = PreviewTarget::Mode::Transition;
    out.modelPath = fromSource;
    out.clipName = fromClip;
    out.toClipName = toClip;
    out.toSourcePath = toSource;
    out.blendSeconds = (std::max)(blendSeconds, 0.0f);
    out.transitionStartSeconds = (std::max)(transitionStart, 0.0f);
    out.label = fromState->name + " -> " + toState->name;
    return true;
}

// アセットパス (.anim / .fbx / .fzasset / .asset) からプレビュー対象を解決する。
// Asset Browser の選択と、プレビュー画面へのドラッグ&ドロップの両方から使う。
// スキンメッシュとスケルトンを両方持ち、プレビューの器として使えるモデルか。
// WHY: FBZZ の「1 クリップ = 1 FBX」で書き出されたクリップ FBX は
//   アーマチュアと Empty しか含まないため、器としては使えない。
bool IsPreviewableGeometry(const asset::Model* model)
{
    return model && model->skeleton && !model->skeleton->bones.empty() &&
           !model->meshes.empty();
}

bool LoadsAsPreviewableGeometry(const std::string& path)
{
    if (!util::FileSystem::Exists(path)) return false;
    return IsPreviewableGeometry(asset::AssetManager::LoadModel(path));
}

// .anim からスキンメッシュを持つモデルを探す。
//   Assets/Models/MiniBot/Walk/anims/Walk@Walk.anim
//     → Assets/Models/MiniBot/Walk.fbx  (クリップ FBX: メッシュ無しなので不採用)
//     → Assets/Models/MiniBot.fbx       (パッケージ本体: 採用)
// AssetManager の「パッケージフォルダ Foo/ の隣に原本 Foo.fbx」規約を利用し、
// 親ディレクトリを遡って最初に見つかったスキンメッシュ付きモデルを返す。
std::string FindGeometryForAnim(const std::string& animPath)
{
    namespace fs = std::filesystem;
    static constexpr const char* kExts[] = { ".fbx", ".FBX", ".fzasset", ".asset",
                                             ".gltf", ".glb", ".obj" };
    fs::path dir = util::FileSystem::PathFromUtf8(animPath).parent_path();

    // anims/ → クリップパッケージ → モデルパッケージ … と最大 5 階層遡る
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

// モデルのパッケージ配下にある .anim を全部集める。
//   Assets/Models/MiniBot.fbx
//     → Assets/Models/MiniBot/*/anims/*.anim   (Idle, Walk, Run …)
//     → Assets/Models/MiniBot/anims/*.anim     (モデル自身に同梱された場合)
// 結果はモデルパスをキーにキャッシュする (毎フレーム走査すると重いため)。
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

    // パッケージ直下の anims/ と、その 1 階層下 (クリップパッケージ) の anims/ を見る。
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

        // ジオメトリは (1) パッケージ規約から自動発見 (2) ユーザー指定の使い回し の順。
        // どちらも無ければ modelPath は空のままにし、UI がドロップ待ち表示を出す。
        // WHY: 以前はここで false を返していたため、クリップ単体を選ぶと
        //   プレビュー自体が消えて「真っ黒」に見えていた。
        out.modelPath = FindGeometryForAnim(path);
        if (out.modelPath.empty()) out.modelPath = s_state.userModelPath;
        return true;
    }

    if (util::StringUtils::EndsWith(lower, ".fbx") ||
        util::StringUtils::EndsWith(lower, ".fzasset") ||
        util::StringUtils::EndsWith(lower, ".asset")) {
        const asset::Model* model = asset::AssetManager::LoadModel(path);
        // クリップを持たないモデルでも器として成立させる (バインドポーズを表示し、
        // クリップは後からコンボ / ドロップで指定できる)。
        // WHY: FBZZ のスキンメッシュ本体 FBX は clips が常に空。
        //   以前の clips.empty() 判定では MiniBot.fbx が必ず弾かれていた。
        if (!IsPreviewableGeometry(model)) return false;
        out.mode = PreviewTarget::Mode::Clip;
        out.modelPath = NormalizeAssetPath(path);
        // 直前と同じモデルならクリップ選択 (コンボ) を維持する。
        if (s_state.target.modelPath == out.modelPath) {
            out.clipName = s_state.target.clipName;
            out.animAssetPath = s_state.target.animAssetPath;
        }
        out.label = util::FileSystem::GetFilename(path);
        return true;
    }
    return false;
}

bool ResolveAssetTarget(EditorContext& ctx, PreviewTarget& out)
{
    return ResolvePathTarget(ctx.selectedAssetPath, out);
}

// ドロップ等で明示指定された対象を採用する。以降は選択操作があるまでこの対象を維持する。
void AdoptManualTarget(const PreviewTarget& target)
{
    const bool identityChanged = !target.SameIdentity(s_state.target);
    s_state.target = target;
    s_state.origin = TargetOrigin::Manual;
    if (!target.modelPath.empty()) s_state.userModelPath = target.modelPath;
    if (identityChanged) {
        s_state.time = 0.0f;
        s_state.playing = true;
        s_state.needsFraming = true;
    }
}

// 現在のクリップを保ったままジオメトリだけ差し替える。
// WHY: Unity の .anim プレビューと同じく「動きは今のまま、器だけ別モデルで見たい」
//   という操作を成立させる。モデルをドロップしてもクリップが消えないようにする。
void SwapPreviewGeometry(const std::string& modelPath)
{
    if (modelPath.empty()) return;
    s_state.userModelPath = modelPath;
    s_state.target.modelPath = modelPath;
    if (s_state.target.mode == PreviewTarget::Mode::None)
        s_state.target.mode = PreviewTarget::Mode::Clip;
    if (s_state.target.label.empty())
        s_state.target.label = util::FileSystem::GetFilename(modelPath);
    s_state.origin = TargetOrigin::Manual;
    s_state.needsFraming = true;
}

// ImGui の直前アイテムを ASSET_PATH ドロップターゲットとして扱い、
// アニメーション関連アセットが落とされたらプレビュー対象を差し替える。
bool AcceptPreviewAssetDrop()
{
    bool accepted = false;
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_PATH")) {
            const std::string dropped =
                NormalizeAssetPath(static_cast<const char*>(payload->Data));
            const std::string lower = util::StringUtils::ToLower(dropped);

            // モデルのドロップで、既にクリップが載っている場合は器だけ差し替える。
            const bool isModelDrop =
                util::StringUtils::EndsWith(lower, ".fbx") ||
                util::StringUtils::EndsWith(lower, ".fzasset") ||
                util::StringUtils::EndsWith(lower, ".asset");
            const bool hasClip = !s_state.target.animAssetPath.empty() ||
                                 !s_state.target.clipName.empty();
            if (isModelDrop && hasClip && LoadsAsPreviewableGeometry(dropped)) {
                SwapPreviewGeometry(dropped);
                accepted = true;
            } else {
                PreviewTarget target;
                if (ResolvePathTarget(dropped, target)) {
                    AdoptManualTarget(target);
                    accepted = true;
                }
            }
        }
        ImGui::EndDragDropTarget();
    }
    return accepted;
}

// 選択の変化を検知し、「後から操作された方」をプレビュー対象として採用する。
// WHY: グラフの遷移矢印とアセットの .anim はどちらも選択状態を持ち続けるため、
//      優先順位を固定すると片方が永遠にプレビューできなくなる。
void UpdatePreviewTarget(EditorContext& ctx)
{
    const auto& selection = ctx.animationGraphSelection;
    const bool graphChanged =
        selection.type != s_state.lastGraphType ||
        selection.stateIndex != s_state.lastGraphState ||
        selection.transitionIndex != s_state.lastGraphTransition ||
        selection.assetPath != s_state.lastGraphAssetPath ||
        selection.entityId != s_state.lastGraphEntity;
    const bool assetChanged = ctx.selectedAssetPath != s_state.lastSelectedAssetPath;

    s_state.lastGraphType = selection.type;
    s_state.lastGraphState = selection.stateIndex;
    s_state.lastGraphTransition = selection.transitionIndex;
    s_state.lastGraphAssetPath = selection.assetPath;
    s_state.lastGraphEntity = selection.entityId;
    s_state.lastSelectedAssetPath = ctx.selectedAssetPath;

    PreviewTarget candidate;
    bool resolved = false;
    TargetOrigin origin = s_state.origin;

    // Animator 付き GameObject の選択変化も対象切り替えのトリガーにする。
    scene::GameObject* selectedGo = ctx.GetSelectedGO();
    const scene::EntityID selectedEntity =
        selectedGo ? selectedGo->GetID() : scene::EntityID::INVALID;
    const bool goChanged = !(selectedEntity == s_state.lastSelectedEntity);
    s_state.lastSelectedEntity = selectedEntity;

    if (graphChanged && ResolveGraphTarget(ctx, candidate)) {
        origin = TargetOrigin::Graph;
        resolved = true;
    } else if (assetChanged && ResolveAssetTarget(ctx, candidate)) {
        origin = TargetOrigin::Asset;
        resolved = true;
    } else if (goChanged && ResolveGameObjectTarget(ctx, candidate)) {
        origin = TargetOrigin::GameObject;
        resolved = true;
    } else if (s_state.origin == TargetOrigin::GameObject) {
        resolved = ResolveGameObjectTarget(ctx, candidate);
    } else if (s_state.origin == TargetOrigin::Graph) {
        // 継続中の対象は毎フレーム再解決し、Duration 等のパラメーター編集を即反映する。
        resolved = ResolveGraphTarget(ctx, candidate);
    } else if (s_state.origin == TargetOrigin::Asset) {
        resolved = ResolveAssetTarget(ctx, candidate);
    }
    // origin == Manual (ドロップ指定) は選択に追従しないため、
    // graphChanged / assetChanged で新しい対象が解決されるまで現状維持する。

    if (!resolved && s_state.origin == TargetOrigin::None) {
        if (ResolveGraphTarget(ctx, candidate)) { origin = TargetOrigin::Graph; resolved = true; }
        else if (ResolveAssetTarget(ctx, candidate)) { origin = TargetOrigin::Asset; resolved = true; }
    }
    if (!resolved) return; // 解決できない間は直前の対象を維持する (Unity と同じ sticky 挙動)

    const bool identityChanged = !candidate.SameIdentity(s_state.target);
    s_state.target = candidate;
    s_state.origin = origin;
    if (identityChanged) {
        s_state.time = 0.0f;
        s_state.playing = true;
        s_state.needsFraming = true;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// レンダリング
// ─────────────────────────────────────────────────────────────────────────────

bool EnsurePreviewGpu(renderer::ResourceManager& resources)
{
    if (!s_gpu.skinnedShader.IsValid())
        s_gpu.skinnedShader = resources.LoadShader("Assets/Shaders/Material/Skinned/SkinnedLit.hlsl");
    if (!s_gpu.surfaceShader.IsValid())
        s_gpu.surfaceShader = resources.LoadShader("Assets/Shaders/Material/Surface/Lit.hlsl");
    if (!s_gpu.pso.IsValid()) {
        s_gpu.pso = resources.CreatePipelineState({
            renderer::RasterizerMode::SOLID_NOCULL,
            renderer::BlendMode::OPAQUE_BLEND,
            renderer::DepthMode::DEPTH_ON
        });
    }
    if (!s_gpu.frameCB.IsValid())
        s_gpu.frameCB = resources.CreateConstantBuffer(sizeof(scene::PerFrameCB));
    if (!s_gpu.objectCB.IsValid())
        s_gpu.objectCB = resources.CreateConstantBuffer(sizeof(scene::PerObjectCB));
    if (!s_gpu.materialCB.IsValid())
        s_gpu.materialCB = resources.CreateConstantBuffer(sizeof(PreviewMaterialCB));
    if (!s_gpu.lightCB.IsValid())
        s_gpu.lightCB = resources.CreateConstantBuffer(sizeof(renderer::LightConstantsCB));
    if (!s_gpu.shadowCB.IsValid())
        s_gpu.shadowCB = resources.CreateConstantBuffer(sizeof(scene::ShadowConstantsCB));
    if (!s_gpu.skinningCB.IsValid())
        s_gpu.skinningCB = resources.CreateConstantBuffer(sizeof(PreviewSkinningCB));
    if (!s_gpu.renderTarget.IsValid())
        s_gpu.renderTarget = resources.CreateRenderTarget(PREVIEW_RT_SIZE, PREVIEW_RT_SIZE);

    return s_gpu.skinnedShader.IsValid() && s_gpu.surfaceShader.IsValid() &&
           s_gpu.pso.IsValid() && s_gpu.frameCB.IsValid() && s_gpu.objectCB.IsValid() &&
           s_gpu.materialCB.IsValid() && s_gpu.lightCB.IsValid() &&
           s_gpu.shadowCB.IsValid() && s_gpu.skinningCB.IsValid() &&
           s_gpu.renderTarget.IsValid();
}

void ComputeModelBounds(const asset::Model& model, math::Vector3& outCenter, float& outRadius)
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
    if (!any) { outCenter = math::Vector3::ZERO; outRadius = 1.0f; return; }
    outCenter = (minP + maxP) * 0.5f;
    outRadius = (std::max)((maxP - minP).Length() * 0.5f, 0.05f);
}

// 指定時刻におけるブレンド係数と両クリップのサンプル時刻 (秒) を求める純関数。
// WHY: 現在時刻の描画とオニオンスキン (前後フレーム) の評価で同じ規則を共有するため。
void ComputePlaybackSampleAt(float time,
                             const asset::AnimationClip* clipA,
                             const asset::AnimationClip* clipB,
                             float& outSecondsA,
                             float& outSecondsB,
                             float& outBlend,
                             float& outTimelineLength)
{
    const auto& target = s_state.target;
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
    // タイムライン: 遷移元 → ブレンド区間 → 遷移先 1 ループ分。全体をループ再生する。
    outTimelineLength = (std::max)(start + blend + lenB, 0.1f);

    outSecondsA = WrapTime(time, lenA);
    const float intoBlend = time - start;
    outBlend = intoBlend <= 0.0f
        ? 0.0f
        : (blend > 0.0001f ? std::clamp(intoBlend / blend, 0.0f, 1.0f) : 1.0f);
    outSecondsB = intoBlend <= 0.0f ? 0.0f : WrapTime(intoBlend, lenB);
}

// 現在時刻版。共有ステート (タイムライン長・ブレンド率表示) も更新する。
void ComputePlaybackSample(const asset::AnimationClip* clipA,
                           const asset::AnimationClip* clipB,
                           float& outSecondsA,
                           float& outSecondsB,
                           float& outBlend)
{
    ComputePlaybackSampleAt(s_state.time, clipA, clipB,
                            outSecondsA, outSecondsB, outBlend, s_state.timelineLength);
    s_state.currentBlendWeight = outBlend;
}

bool RenderPreviewFrame(EditorContext& ctx, float displayAspect)
{
    if (!ctx.renderer || !ctx.resources) return false;
    if (s_state.lastRenderFrame == ImGui::GetFrameCount()) return s_state.lastRenderOk;
    s_state.lastRenderFrame = ImGui::GetFrameCount();
    s_state.lastRenderOk = false;

    auto& resources = *ctx.resources;
    if (!EnsurePreviewGpu(resources)) return false;

    const asset::Model* model = asset::AssetManager::LoadModel(s_state.target.modelPath);
    if (!model || model->meshes.empty()) return false;

    // ── クリップ解決 + 時刻計算 ──
    const asset::AnimationClip* clipA =
        ResolveClip(model, s_state.target.clipName, s_state.target.animAssetPath);
    const asset::AnimationClip* clipB = nullptr;
    if (s_state.target.mode == PreviewTarget::Mode::Transition) {
        const asset::Model* toModel = s_state.target.toSourcePath == s_state.target.modelPath
            ? model
            : asset::AssetManager::LoadModel(s_state.target.toSourcePath);
        clipB = FindModelClip(toModel, s_state.target.toClipName);
    }
    float secondsA = 0.0f, secondsB = 0.0f, blendWeight = 0.0f;
    ComputePlaybackSample(clipA, clipB, secondsA, secondsB, blendWeight);

    // ── スキニングパレット + デバッグ用ポーズデータ ──
    const bool wantsDebugPose =
        s_state.showBones || s_state.showBoneNames || s_state.showTrail ||
        s_state.showGhost || s_state.showInfo || s_state.selectedBoneNode >= 0;
    s_state.debugPoseValid = false;
    s_state.selectedBoneValid = false;

    PreviewSkinningCB skinning{};
    for (auto& bone : skinning.boneMatrices) bone = math::Matrix4::Identity();
    if (model->skeleton && model->skeleton->rootNodeIndex >= 0 && clipA) {
        const asset::Skeleton& skeleton = *model->skeleton;
        std::vector<math::Matrix4> palette(skeleton.bones.size(),
                                           math::Matrix4::Identity());
        std::vector<math::Matrix4> nodeGlobals;
        std::vector<math::Matrix4>* nodeGlobalsPtr = nullptr;
        if (wantsDebugPose) {
            nodeGlobals.assign(skeleton.nodes.size(), math::Matrix4::Identity());
            nodeGlobalsPtr = &nodeGlobals;
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
            // クリップが変わったときだけトラック有無・軌跡を再計算する (FindTrack が高コストのため)。
            const std::string cacheIdentity = s_state.target.modelPath + "|" +
                s_state.target.clipName + "|" + s_state.target.animAssetPath + "|" +
                s_state.target.toClipName;
            if (cacheIdentity != s_state.debugCacheIdentity) {
                s_state.debugCacheIdentity = cacheIdentity;
                s_state.selectedBoneNode = -1;
                s_state.nodeHasTrack.assign(skeleton.nodes.size(), 0);
                s_state.animatedNodeCount = 0;
                s_state.boneNodeCount = 0;
                for (size_t i = 0; i < skeleton.nodes.size(); ++i) {
                    if (skeleton.nodes[i].boneIndex < 0) continue;
                    ++s_state.boneNodeCount;
                    if (FindTrack(*clipA, skeleton.nodes[i].name)) {
                        s_state.nodeHasTrack[i] = 1;
                        ++s_state.animatedNodeCount;
                    }
                }
                BuildTrailPoints(skeleton, *clipA, s_state.trailPoints);
            }

            // ノードグローバル → ワールド位置 (メッシュと同じく rootInverse 空間へ揃える)
            auto globalsToPositions = [&](const std::vector<math::Matrix4>& globals,
                                          std::vector<math::Vector3>& out) {
                out.resize(globals.size());
                for (size_t i = 0; i < globals.size(); ++i)
                    out[i] = MatrixTranslation(skeleton.rootInverseTransform * globals[i]);
            };
            globalsToPositions(nodeGlobals, s_state.jointPositions);

            // オニオンスキン: 前後フレームのスケルトンを併記して動きの変化量を読めるようにする。
            if (s_state.showGhost) {
                // 手動オフセット指定があればそれを、無ければタイムライン長依存の自動値を使う。
                const float ghostOffset = s_state.ghostOffsetSeconds > 0.0001f
                    ? s_state.ghostOffsetSeconds
                    : (std::max)(s_state.timelineLength / 14.0f, 1.0f / 30.0f);
                auto evaluateGhost = [&](float atTime, std::vector<math::Vector3>& out) {
                    float ghostA = 0.0f, ghostB = 0.0f, ghostBlend = 0.0f, unusedLen = 0.0f;
                    ComputePlaybackSampleAt(WrapTime(atTime, s_state.timelineLength),
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
                evaluateGhost(s_state.time - ghostOffset, s_state.ghostPrevPositions);
                evaluateGhost(s_state.time + ghostOffset, s_state.ghostNextPositions);
            }

            // 選択中ボーンのローカル TRS とキー数をライブ更新する。
            if (s_state.selectedBoneNode >= 0 &&
                s_state.selectedBoneNode < static_cast<int>(skeleton.nodes.size())) {
                const auto& node =
                    skeleton.nodes[static_cast<size_t>(s_state.selectedBoneNode)];
                s_state.selectedBonePose = SamplePose(node, clipA, ticksA);
                if (const auto* track = FindTrack(*clipA, node.name)) {
                    s_state.selectedBoneKeyCounts[0] = static_cast<int>(track->positions.size());
                    s_state.selectedBoneKeyCounts[1] = static_cast<int>(track->rotations.size());
                    s_state.selectedBoneKeyCounts[2] = static_cast<int>(track->scales.size());
                } else {
                    s_state.selectedBoneKeyCounts[0] = 0;
                    s_state.selectedBoneKeyCounts[1] = 0;
                    s_state.selectedBoneKeyCounts[2] = 0;
                }
                s_state.selectedBoneValid = true;

                // カーブグラフ用の全長サンプルは、ボーンかクリップが変わったときだけ再計算する。
                if (s_state.showCurves) {
                    const std::string curveKey = s_state.debugCacheIdentity + "|" +
                        std::to_string(s_state.selectedBoneNode);
                    if (curveKey != s_state.curveCacheKey) {
                        s_state.curveCacheKey = curveKey;
                        BuildBoneCurves(
                            skeleton, *clipA, s_state.selectedBoneNode,
                            s_state.curvePosX, s_state.curvePosY, s_state.curvePosZ,
                            s_state.curveRotX, s_state.curveRotY, s_state.curveRotZ,
                            s_state.curvePosMin, s_state.curvePosMax,
                            s_state.curveRotMin, s_state.curveRotMax);
                    }
                }
            }

            s_state.debugPoseValid = true;
        }
    }
    resources.Update(s_gpu.skinningCB, &skinning, sizeof(skinning));

    // ── カメラ (オービット) ──
    math::Vector3 boundsCenter;
    float boundsRadius = 1.0f;
    ComputeModelBounds(*model, boundsCenter, boundsRadius);
    if (s_state.needsFraming || s_state.distance <= 0.0f) {
        s_state.focus = boundsCenter;
        s_state.distance = boundsRadius * 2.6f;
        s_state.needsFraming = false;
    }
    const float cosPitch = std::cos(s_state.pitch);
    const math::Vector3 orbitOffset{
        cosPitch * std::sin(s_state.yaw) * s_state.distance,
        std::sin(s_state.pitch) * s_state.distance,
        cosPitch * std::cos(s_state.yaw) * s_state.distance
    };
    renderer::Camera camera;
    camera.m_position = s_state.focus + orbitOffset;
    camera.m_aspect = (std::max)(displayAspect, 0.1f);
    camera.m_fovY = 40.0f;
    camera.m_near = (std::max)(0.01f, s_state.distance * 0.01f);
    camera.m_far = (std::max)(50.0f, s_state.distance + boundsRadius * 8.0f);
    camera.LookAt(s_state.focus);

    scene::PerFrameCB frameData{};
    frameData.view = camera.GetViewMatrix();
    frameData.projection = camera.GetProjectionMatrix();
    frameData.viewProjection = camera.GetViewProjection();
    frameData.invViewProjection = math::Matrix4::Inverse(frameData.viewProjection);
    frameData.cameraPos = camera.m_position;
    frameData.nearZ = camera.m_near;
    frameData.farZ = camera.m_far;
    resources.Update(s_gpu.frameCB, &frameData, sizeof(frameData));
    // オーバーレイ (ボーン線など) が CPU 側で同じ射影を使えるように保持する。
    s_state.debugViewProjection = frameData.viewProjection;

    scene::PerObjectCB objectData{};
    objectData.world = math::Matrix4::Identity();
    objectData.worldInvTranspose = math::Matrix4::Identity();
    resources.Update(s_gpu.objectCB, &objectData, sizeof(objectData));

    // ── ライティング (サムネイルと同じ 3 点照明リグの簡易版) ──
    renderer::LightConstantsCB lightData{};
    const math::Vector3 keyDirection =
        (camera.m_position +
         math::Vector3{ boundsRadius * 1.4f, boundsRadius * 1.8f, boundsRadius * 0.8f } -
         s_state.focus).Normalized();
    lightData.lightDir = { -keyDirection.x, -keyDirection.y, -keyDirection.z };
    lightData.lightColor = { 1.0f, 0.97f, 0.92f };
    // 手調整リグなので、Lighting.hlsli の LIGHT_UNIT_SCALE (= PI) を相殺して
    // 記述値がそのまま「絵として狙った明るさ」を表すようにする (サムネイルと同じ方針)。
    constexpr float kPreviewUnitScale = 3.14159265358979323846f;
    lightData.lightIntensity = 1.6f / kPreviewUnitScale;
    lightData.ambientColor = { 0.16f, 0.17f, 0.20f };
    // LightAttenuation の逆二乗を打ち消し、intensity を「最終的な明るさ」として扱う。
    // range = boundsRadius * 20 に対し dist は boundsRadius * 3 前後なので range 窓はほぼ 1.0。
    auto placeLight = [&](renderer::PointLight& light,
                          const math::Vector3& offset,
                          const math::Vector3& color,
                          float intensity) {
        light.position = s_state.focus + offset;
        light.color = color;
        light.range = boundsRadius * 20.0f;
        const float dist = offset.Length();
        // シェーダー側の特異点ガード max(d*d, 0.01) と同じ下限を掛ける。
        light.intensity = intensity * std::max(dist * dist, 0.01f) / kPreviewUnitScale;
    };
    placeLight(lightData.pointLights[0],
               { boundsRadius * 2.6f, -boundsRadius * 1.2f, -boundsRadius * 2.2f },
               { 0.55f, 0.65f, 1.0f }, 0.35f);
    placeLight(lightData.pointLights[1],
               { -boundsRadius * 1.6f, boundsRadius * 2.4f, boundsRadius * 2.6f },
               { 1.0f, 1.0f, 1.0f }, 0.9f);
    lightData.pointLightCount = 2;
    resources.Update(s_gpu.lightCB, &lightData, sizeof(lightData));

    // シャドウは bias=1.0 で常時ライティング扱いにして無効化する (サムネイルと同じ手法)。
    scene::ShadowConstantsCB shadowData{};
    shadowData.lightViewProjection = math::Matrix4::Translate({ 16.0f, 16.0f, 0.0f });
    shadowData.shadowMapTexelSize[0] = 0.0f;
    shadowData.shadowMapTexelSize[1] = 0.0f;
    shadowData.shadowBias = 1.0f;
    resources.Update(s_gpu.shadowCB, &shadowData, sizeof(shadowData));

    // ── 描画 ──
    auto& renderer = *ctx.renderer;
    renderer.SetRenderTarget(s_gpu.renderTarget, resources);
    renderer.Clear({ 0.09f, 0.10f, 0.12f, 1.0f });
    renderer.ClearDepth();
    renderer.SetSampler(0, renderer::SamplerMode::WRAP_ANISOTROPIC);
    renderer.SetSampler(1, renderer::SamplerMode::CLAMP_LINEAR);

    // Mesh トグル Off のときはスケルトンオーバーレイだけを見せる (デバッグ時の視認性優先)。
    for (size_t i = 0; s_state.showMesh && i < model->meshes.size(); ++i) {
        const auto& mesh = model->meshes[i];
        if (!mesh || !mesh->vertexBuffer.IsValid() || !mesh->indexBuffer.IsValid()) continue;

        // ── マテリアル解決 ──────────────────────────────────────────────
        // モデルが持つマテリアルの shader / paramsBuffer / textures をそのまま使い、
        // シーンビューと同じ見た目にする。
        // WHY: 以前はアルベドテクスチャだけ拝借してグレー固定のフラット CB を
        //   流し込んでいたため、色・エミッシブ・法線マップなどが一切反映されず、
        //   プレビューだけ別物の見た目になっていた。
        renderer::Material* material =
            (i < model->materials.size()) ? model->materials[i].get() : nullptr;

        // スキンメッシュに非スキニングシェーダーが割り当たっている場合は使えない
        // (頂点入力レイアウトが合わない)。描画パスと同じくフォールバックする。
        const bool materialSupportsSkinning =
            material && material->shaderPath.find("/Skinned/") != std::string::npos;
        const bool useMaterial =
            material && material->shader.IsValid() && material->paramsBuffer.IsValid() &&
            (!mesh->isSkinned || materialSupportsSkinning);

        renderer::DrawCall dc;
        dc.vertexBuffer = mesh->vertexBuffer;
        dc.indexBuffer = mesh->indexBuffer;
        dc.indexCount = mesh->indexCount;
        dc.vertexCount = mesh->vertexCount;
        dc.pipelineState = s_gpu.pso;
        dc.constantBuffers[0] = s_gpu.frameCB;
        dc.constantBuffers[1] = s_gpu.objectCB;
        dc.constantBuffers[3] = s_gpu.lightCB;
        dc.constantBuffers[4] = s_gpu.shadowCB;
        if (mesh->isSkinned)
            dc.constantBuffers[7] = s_gpu.skinningCB;

        if (useMaterial) {
            dc.shader             = material->shader;
            dc.constantBuffers[2] = material->paramsBuffer;
            for (size_t ti = 0; ti < material->textures.size() && ti < 8; ++ti)
                if (material->textures[ti].IsValid()) dc.textures[ti] = material->textures[ti];
        } else {
            // フォールバック: プレビュー既定のフラットマテリアル。
            // アルベドテクスチャがあればそれだけは反映する。
            PreviewMaterialCB materialData{};
            renderer::ResourceHandle<renderer::TextureTag> albedoTexture;
            if (material && !material->textures.empty() && material->textures[0].IsValid()) {
                albedoTexture = material->textures[0];
                materialData.textureMask = 1u;
            }
            resources.Update(s_gpu.materialCB, &materialData, sizeof(materialData));
            dc.shader             = mesh->isSkinned ? s_gpu.skinnedShader : s_gpu.surfaceShader;
            dc.constantBuffers[2] = s_gpu.materialCB;
            dc.textures[0]        = albedoTexture;
        }
        renderer.Submit(dc, resources);
    }

    // WHY: ImGui 描画中の RT 切り替えなので、必ずバックバッファへ戻す。
    renderer.SetRenderTarget(renderer::ResourceHandle<renderer::RenderTargetTag>{}, resources);
    s_state.lastRenderOk = true;
    return true;
}

// 再生時刻を進める。複数箇所 (Inspector + パネル) から呼ばれても 1 フレーム 1 回だけ。
void AdvancePlayback()
{
    if (s_state.lastAdvanceFrame == ImGui::GetFrameCount()) return;
    s_state.lastAdvanceFrame = ImGui::GetFrameCount();
    if (!s_state.playing) return;

    s_state.time += ImGui::GetIO().DeltaTime * s_state.speed;
    if (s_state.time > s_state.timelineLength) {
        if (s_state.loop) {
            s_state.time = WrapTime(s_state.time, s_state.timelineLength);
        } else {
            s_state.time = s_state.timelineLength;
            s_state.playing = false;
        }
    }
}

} // namespace

bool HasAnimationPreviewTarget()
{
    return s_state.target.mode != PreviewTarget::Mode::None;
}

bool DrawAnimationPreviewWidget(EditorContext& ctx, float previewHeight)
{
    UpdatePreviewTarget(ctx);
    if (s_state.target.mode == PreviewTarget::Mode::None) return false;

    ImGui::PushID("##AnimationPreviewWidget");

    // ── ヘッダー: 対象名 + 遷移ブレンド率 ──
    ImGui::TextColored(ImVec4(0.55f, 0.80f, 1.0f, 1.0f), "%s", s_state.target.label.c_str());
    if (s_state.target.mode == PreviewTarget::Mode::Transition) {
        ImGui::SameLine();
        ImGui::TextDisabled("Blend %.0f%%", s_state.currentBlendWeight * 100.0f);
    }

    // ── ジオメトリ枠 (Unity の Preview 下部にあるモデル差し替えと同じ役割) ──
    // WHY: FBZZ は 1 クリップ = 1 FBX なので、.anim の隣にスキンメッシュが無い。
    //   どのモデルで再生しているかを常に見せ、D&D で差し替えられるようにする。
    const bool hasGeometry =
        !s_state.target.modelPath.empty() &&
        LoadsAsPreviewableGeometry(s_state.target.modelPath);
    {
        ImGui::TextDisabled("Model:");
        ImGui::SameLine();
        const std::string geoName = hasGeometry
            ? util::FileSystem::GetFilename(s_state.target.modelPath)
            : std::string("(drop a skinned model here)");
        if (!hasGeometry)
            ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(120, 64, 48, 255));
        ImGui::Button(geoName.c_str(), ImVec2(-1.0f, 0.0f));
        if (!hasGeometry) ImGui::PopStyleColor();
        AcceptPreviewAssetDrop();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "Skinned model used as the preview body.\n"
                "Drag a .fbx / .fzasset here to swap it (the clip is kept).");
    }

    // ジオメトリが無い間は黒画面を出さず、何をすればよいか明示する。
    if (!hasGeometry) {
        const float width  = (std::max)(ImGui::GetContentRegionAvail().x, 64.0f);
        const float height = (std::max)(previewHeight, 96.0f);
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##PreviewDropZone", ImVec2(width, height));
        AcceptPreviewAssetDrop();

        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + height),
                          IM_COL32(28, 30, 34, 255), 4.0f);
        dl->AddRect(origin, ImVec2(origin.x + width, origin.y + height),
                    IM_COL32(120, 130, 145, 200), 4.0f, 0, 1.5f);
        const char* line1 = "No skinned model for this clip";
        const char* line2 = "Drag a model (.fbx / .fzasset) here to preview it";
        const ImVec2 s1 = ImGui::CalcTextSize(line1);
        const ImVec2 s2 = ImGui::CalcTextSize(line2);
        dl->AddText(ImVec2(origin.x + (width - s1.x) * 0.5f,
                           origin.y + height * 0.5f - s1.y),
                    IM_COL32(220, 225, 235, 255), line1);
        dl->AddText(ImVec2(origin.x + (width - s2.x) * 0.5f,
                           origin.y + height * 0.5f + 4.0f),
                    IM_COL32(150, 158, 172, 255), line2);
        ImGui::PopID();
        return true;
    }

    // ── デバッグ表示トグルバー ──
    // WHY: Unity の Preview は絵を見るだけだが、ここではボーン・軌跡・残像・トラック情報を
    //      重ねてアニメーションデータそのものをデバッグできるようにする (本エンジンの差別化)。
    {
        auto overlayToggle = [](const char* label, bool& value, const char* tooltip) {
            const bool active = value;
            if (active) {
                ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(52, 110, 168, 255));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(64, 130, 196, 255));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(74, 148, 220, 255));
            }
            if (ImGui::SmallButton(label)) value = !value;
            if (active) ImGui::PopStyleColor(3);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tooltip);
            ImGui::SameLine();
        };
        overlayToggle("Mesh", s_state.showMesh, "Toggle mesh rendering (Off = skeleton only)");
        overlayToggle("Bones", s_state.showBones,
                      "Skeleton overlay\nGreen: animated by this clip / Gray: bind pose only\nClick a joint to inspect it");
        overlayToggle("Names", s_state.showBoneNames, "Bone name labels");
        overlayToggle("Trail", s_state.showTrail, "Root bone trajectory over the whole clip");
        overlayToggle("Ghost", s_state.showGhost, "Onion skin: skeleton a few frames before / after");
        overlayToggle("Info", s_state.showInfo, "Time / frame / track coverage overlay");
        overlayToggle("Curves", s_state.showCurves,
                      "Position / rotation curves of the selected bone");
        ImGui::NewLine();

        // Names 有効時のみラベル間引きモードを露出する。100+ ボーンで全表示すると潰れるため。
        if (s_state.showBoneNames) {
            ImGui::TextDisabled("Labels:");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(150.0f);
            const char* kLabelModes[] = { "Selected / Hovered", "Animated bones", "All" };
            ImGui::Combo("##LabelMode", &s_state.labelMode, kLabelModes, 3);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "How many bone name labels to show at once.\n"
                    "Dense skeletons stay readable with 'Selected / Hovered' or 'Animated bones'.");
            ImGui::SameLine();
        }
        // Ghost 有効時はオフセット秒数を調整できるようにする (0 = タイムライン長依存の自動)。
        if (s_state.showGhost) {
            ImGui::TextDisabled("Ghost:");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(120.0f);
            ImGui::SliderFloat("##GhostOffset", &s_state.ghostOffsetSeconds,
                               0.0f, 0.5f,
                               s_state.ghostOffsetSeconds <= 0.0001f ? "Auto" : "%.3f s");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "Onion skin offset (seconds before / after current time).\n0 = automatic (timeline / 14).");
        }
        if (s_state.showBoneNames || s_state.showGhost) ImGui::NewLine();
    }

    // ── プレビュー画像 (オービット操作付き) ──
    const float width = (std::max)(ImGui::GetContentRegionAvail().x, 64.0f);
    const float height = (std::max)(previewHeight, 96.0f);
    const ImVec2 imageOrigin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##PreviewImage", ImVec2(width, height));
    const bool imageHovered = ImGui::IsItemHovered();
    const bool imageActive = ImGui::IsItemActive();
    // WHY: Inspector 埋め込み時、ホイールズームが親ウィンドウのスクロールに化けないよう
    //      ホバー中はホイール入力の所有権をこのアイテムに移す。
    if (imageHovered) ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
    // FBX / .anim をプレビュー画面へ直接ドロップして対象を差し替えられるようにする。
    AcceptPreviewAssetDrop();

    if (imageActive && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        const ImVec2 delta = ImGui::GetIO().MouseDelta;
        s_state.yaw -= delta.x * 0.012f;
        s_state.pitch = std::clamp(s_state.pitch + delta.y * 0.010f, -1.35f, 1.35f);
    }
    if (imageHovered && ImGui::GetIO().MouseWheel != 0.0f && s_state.distance > 0.0f) {
        s_state.distance *= (ImGui::GetIO().MouseWheel > 0.0f) ? 0.88f : 1.14f;
        s_state.distance = std::clamp(s_state.distance, 0.05f, 5000.0f);
    }
    if (imageHovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
        s_state.needsFraming = true; // ダブルクリックで再フレーミング (Unity の F 相当)

    AdvancePlayback();
    const bool rendered = RenderPreviewFrame(ctx, width / height);

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImVec2 imageMax(imageOrigin.x + width, imageOrigin.y + height);
    drawList->AddRectFilled(imageOrigin, imageMax, IM_COL32(20, 22, 26, 255), 4.0f);
    if (rendered && ctx.imguiRenderer && ctx.resources) {
        if (void* rawID =
                ctx.imguiRenderer->GetImTextureID(s_gpu.renderTarget, *ctx.resources, 0)) {
            drawList->AddImage(ToImTextureID(rawID), imageOrigin, imageMax);
        }
    } else {
        const char* message = "Preview unavailable (model or clip not found)";
        const ImVec2 textSize = ImGui::CalcTextSize(message);
        drawList->AddText(
            ImVec2(imageOrigin.x + (width - textSize.x) * 0.5f,
                   imageOrigin.y + (height - textSize.y) * 0.5f),
            IM_COL32(150, 158, 170, 255), message);
    }
    drawList->AddRect(imageOrigin, imageMax, IM_COL32(90, 96, 108, 255), 4.0f);
    if (imageHovered) {
        drawList->AddText(
            ImVec2(imageOrigin.x + 8.0f, imageMax.y - ImGui::GetTextLineHeight() - 6.0f),
            IM_COL32(170, 178, 190, 210),
            "Drag: Orbit | Wheel: Zoom | Double Click: Frame");
    }

    // ── デバッグオーバーレイ (ボーン / 軌跡 / ゴースト / 情報) ──
    // メッシュ描画と同じ ViewProjection で CPU 側から投影し、RT の上に 2D で重ねる。
    const asset::Model* overlayModel = nullptr;
    const asset::AnimationClip* overlayClip = nullptr;
    if (rendered) {
        overlayModel = asset::AssetManager::LoadModel(s_state.target.modelPath);
        if (overlayModel)
            overlayClip = ResolveClip(overlayModel, s_state.target.clipName,
                                      s_state.target.animAssetPath);
    }
    if (s_state.debugPoseValid && overlayModel && overlayModel->skeleton) {
        const asset::Skeleton& skeleton = *overlayModel->skeleton;
        drawList->PushClipRect(imageOrigin, imageMax, true);

        auto project = [&](const math::Vector3& world, ImVec2& out) -> bool {
            const math::Vector4 clip = s_state.debugViewProjection *
                math::Vector4{ world.x, world.y, world.z, 1.0f };
            if (clip.w <= 0.0001f) return false;
            out.x = imageOrigin.x + (clip.x / clip.w * 0.5f + 0.5f) * width;
            out.y = imageOrigin.y + (0.5f - clip.y / clip.w * 0.5f) * height;
            return true;
        };

        // 軌跡: クリップ全長の代表ボーン移動をグラデーション付きポリラインで描く。
        if (s_state.showTrail && s_state.trailPoints.size() >= 2) {
            ImVec2 previousPoint{};
            bool previousValid = project(s_state.trailPoints[0], previousPoint);
            for (size_t i = 1; i < s_state.trailPoints.size(); ++i) {
                ImVec2 point;
                const bool valid = project(s_state.trailPoints[i], point);
                if (previousValid && valid) {
                    const float progress =
                        static_cast<float>(i) /
                        static_cast<float>(s_state.trailPoints.size() - 1);
                    drawList->AddLine(
                        previousPoint, point,
                        IM_COL32(255, 176, 84,
                                 60 + static_cast<int>(150.0f * progress)),
                        2.0f);
                }
                previousPoint = point;
                previousValid = valid;
            }
            const float clipLength = ClipDurationSeconds(overlayClip, 1.0f);
            const float normalized =
                std::clamp(WrapTime(s_state.time, clipLength) / clipLength, 0.0f, 1.0f);
            const size_t markerIndex = static_cast<size_t>(std::lround(
                normalized * static_cast<float>(s_state.trailPoints.size() - 1)));
            ImVec2 marker;
            if (project(s_state.trailPoints[markerIndex], marker))
                drawList->AddCircleFilled(marker, 4.0f, IM_COL32(255, 200, 110, 255));
        }

        // オニオンスキン: 過去 = 青、未来 = 橙。動きの速いボーンほど残像が離れて見える。
        auto drawSkeletonPose = [&](const std::vector<math::Vector3>& positions,
                                    ImU32 color,
                                    float thickness) {
            for (size_t i = 0; i < skeleton.nodes.size() && i < positions.size(); ++i) {
                const auto& node = skeleton.nodes[i];
                if (node.boneIndex < 0 || node.parentIndex < 0) continue;
                if (node.parentIndex >= static_cast<int>(positions.size())) continue;
                ImVec2 point, parentPoint;
                if (project(positions[i], point) &&
                    project(positions[static_cast<size_t>(node.parentIndex)], parentPoint))
                    drawList->AddLine(parentPoint, point, color, thickness);
            }
        };
        if (s_state.showGhost) {
            drawSkeletonPose(s_state.ghostPrevPositions, IM_COL32(96, 160, 255, 110), 1.5f);
            drawSkeletonPose(s_state.ghostNextPositions, IM_COL32(255, 168, 92, 110), 1.5f);
        }

        // ボーン本体: トラック有無で色分けし、ホバーで名前、クリックで詳細を選択する。
        // Curves モードもボーンクリックが必要なので、当たり判定・選択を有効化する。
        if (s_state.showBones || s_state.showBoneNames || s_state.showCurves ||
            s_state.selectedBoneNode >= 0) {
            struct JointScreen {
                int nodeIndex = -1;
                ImVec2 pos;
            };
            std::vector<JointScreen> joints;
            joints.reserve(skeleton.nodes.size());
            const ImVec2 mousePos = ImGui::GetIO().MousePos;
            int hoveredJoint = -1;
            float hoveredDistanceSq = 12.0f * 12.0f;
            for (size_t i = 0;
                 i < skeleton.nodes.size() && i < s_state.jointPositions.size(); ++i) {
                if (skeleton.nodes[i].boneIndex < 0) continue;
                ImVec2 point;
                if (!project(s_state.jointPositions[i], point)) continue;
                joints.push_back({ static_cast<int>(i), point });
                const float dx = mousePos.x - point.x;
                const float dy = mousePos.y - point.y;
                const float distanceSq = dx * dx + dy * dy;
                if (imageHovered && distanceSq < hoveredDistanceSq) {
                    hoveredDistanceSq = distanceSq;
                    hoveredJoint = static_cast<int>(i);
                }
            }

            auto isAnimated = [&](int nodeIndex) {
                return nodeIndex >= 0 &&
                       nodeIndex < static_cast<int>(s_state.nodeHasTrack.size()) &&
                       s_state.nodeHasTrack[static_cast<size_t>(nodeIndex)] != 0;
            };

            // ボーン線は Bones モードのみ。関節ドットは Curves モードでも出して
            // クリック対象を示す (Curves は選択ボーンが必要なため)。
            if (s_state.showBones) {
                for (const auto& joint : joints) {
                    const auto& node = skeleton.nodes[static_cast<size_t>(joint.nodeIndex)];
                    if (node.parentIndex < 0 ||
                        node.parentIndex >= static_cast<int>(s_state.jointPositions.size()))
                        continue;
                    ImVec2 parentPoint;
                    if (!project(
                            s_state.jointPositions[static_cast<size_t>(node.parentIndex)],
                            parentPoint))
                        continue;
                    drawList->AddLine(
                        parentPoint, joint.pos,
                        isAnimated(joint.nodeIndex)
                            ? IM_COL32(110, 226, 160, 235)
                            : IM_COL32(150, 156, 168, 140),
                        1.8f);
                }
            }
            if (s_state.showBones || s_state.showCurves) {
                for (const auto& joint : joints) {
                    const bool selected = joint.nodeIndex == s_state.selectedBoneNode;
                    const bool hovered = joint.nodeIndex == hoveredJoint;
                    // Curves のみのときは線が無いので、ドットを少し控えめにして雑然さを抑える。
                    const bool bonesShown = s_state.showBones;
                    const ImU32 jointColor = selected
                        ? IM_COL32(255, 216, 96, 255)
                        : hovered
                            ? IM_COL32(255, 255, 255, 255)
                            : isAnimated(joint.nodeIndex)
                                ? IM_COL32(130, 240, 176, bonesShown ? 255 : 190)
                                : IM_COL32(168, 172, 182, bonesShown ? 190 : 120);
                    drawList->AddCircleFilled(
                        joint.pos, (selected || hovered) ? 4.0f : 2.6f, jointColor);
                    if (selected)
                        drawList->AddCircle(
                            joint.pos, 7.0f, IM_COL32(255, 216, 96, 200), 0, 1.5f);
                }
            }
            if (s_state.showBoneNames) {
                // 密なスケルトンでラベルが潰れないよう、モードで表示対象を絞る。
                //   0: 選択 + ホバーのみ / 1: アニメ有ボーン / 2: 全部
                // さらに 2 (All) では、直前に置いたラベルと近すぎる位置はスキップして重なりを防ぐ。
                std::vector<ImVec2> placedLabels;
                placedLabels.reserve(joints.size());
                const float minLabelGapSq = 18.0f * 18.0f;
                for (const auto& joint : joints) {
                    const bool selected = joint.nodeIndex == s_state.selectedBoneNode;
                    const bool hovered = joint.nodeIndex == hoveredJoint;
                    bool show = selected || hovered;
                    if (!show && s_state.labelMode == 1) show = isAnimated(joint.nodeIndex);
                    else if (!show && s_state.labelMode == 2) show = true;
                    if (!show) continue;

                    // 選択・ホバーは最優先で必ず出す。それ以外は近接ラベルを間引く。
                    if (!selected && !hovered) {
                        bool tooClose = false;
                        for (const ImVec2& placed : placedLabels) {
                            const float dx = placed.x - joint.pos.x;
                            const float dy = placed.y - joint.pos.y;
                            if (dx * dx + dy * dy < minLabelGapSq) { tooClose = true; break; }
                        }
                        if (tooClose) continue;
                    }
                    placedLabels.push_back(joint.pos);

                    const std::string shortName =
                        ShortBoneName(skeleton.nodes[static_cast<size_t>(joint.nodeIndex)].name);
                    const ImU32 labelColor = (selected || hovered)
                        ? IM_COL32(255, 236, 170, 255)
                        : IM_COL32(210, 218, 228, 200);
                    // 選択・ホバー名は背景を敷いて読みやすくする。
                    if (selected || hovered) {
                        const ImVec2 textSize = ImGui::CalcTextSize(shortName.c_str());
                        const ImVec2 tp(joint.pos.x + 5.0f, joint.pos.y - 5.0f);
                        drawList->AddRectFilled(
                            ImVec2(tp.x - 2.0f, tp.y - 1.0f),
                            ImVec2(tp.x + textSize.x + 2.0f, tp.y + textSize.y + 1.0f),
                            IM_COL32(18, 20, 26, 190), 2.0f);
                    }
                    drawList->AddText(
                        ImVec2(joint.pos.x + 5.0f, joint.pos.y - 5.0f),
                        labelColor, shortName.c_str());
                }
            }
            if (hoveredJoint >= 0) {
                const auto& node = skeleton.nodes[static_cast<size_t>(hoveredJoint)];
                ImGui::SetTooltip("%s\n%s", node.name.c_str(),
                                  isAnimated(hoveredJoint)
                                      ? "Animated by this clip"
                                      : "Bind pose only (no track)");
            }
            // ドラッグ (オービット) と区別するため、移動量の小さいリリースだけを選択操作にする。
            if (imageHovered && ImGui::IsMouseReleased(ImGuiMouseButton_Left) &&
                ImGui::GetIO().MouseDragMaxDistanceSqr[ImGuiMouseButton_Left] < 16.0f) {
                s_state.selectedBoneNode = hoveredJoint; // 空クリックで選択解除 (-1)
            }
        }

        // 情報オーバーレイ: 時刻・フレーム・トラックカバレッジ・イベント発火を左上に表示する。
        if (s_state.showInfo) {
            float textY = imageOrigin.y + 6.0f;
            auto infoText = [&](ImU32 color, const char* text) {
                drawList->AddText(
                    ImVec2(imageOrigin.x + 9.0f, textY + 1.0f), IM_COL32(0, 0, 0, 170), text);
                drawList->AddText(ImVec2(imageOrigin.x + 8.0f, textY), color, text);
                textY += ImGui::GetTextLineHeight();
            };
            char line[192]{};
            const float frameRate =
                overlayClip && overlayClip->frameRate > 0.0f ? overlayClip->frameRate : 30.0f;
            const float wrappedTime = WrapTime(s_state.time, s_state.timelineLength);
            std::snprintf(line, sizeof(line), "Clip   %s",
                          overlayClip ? overlayClip->name.c_str() : "<none>");
            infoText(IM_COL32(225, 232, 240, 235), line);
            std::snprintf(line, sizeof(line),
                          "Time   %.3f / %.3f s  (Frame %d @ %.0f fps)",
                          wrappedTime, s_state.timelineLength,
                          static_cast<int>(wrappedTime * frameRate), frameRate);
            infoText(IM_COL32(225, 232, 240, 235), line);
            std::snprintf(line, sizeof(line), "Bones  %d  |  Animated %d (%.0f%%)",
                          s_state.boneNodeCount, s_state.animatedNodeCount,
                          s_state.boneNodeCount > 0
                              ? 100.0f * static_cast<float>(s_state.animatedNodeCount) /
                                    static_cast<float>(s_state.boneNodeCount)
                              : 0.0f);
            infoText(IM_COL32(225, 232, 240, 235), line);
            if (s_state.target.mode == PreviewTarget::Mode::Transition) {
                std::snprintf(line, sizeof(line), "Blend  %.1f%%",
                              s_state.currentBlendWeight * 100.0f);
                infoText(IM_COL32(140, 255, 190, 235), line);
            }
            if (overlayClip) {
                for (const auto& event : overlayClip->events) {
                    if (std::abs(static_cast<float>(event.time) - wrappedTime) < 0.08f) {
                        std::snprintf(line, sizeof(line), "Event  %s", event.name.c_str());
                        infoText(IM_COL32(255, 216, 96, 255), line);
                    }
                }
            }
        }
        drawList->PopClipRect();
    }

    // ── 再生コントロール ──
    if (ImGui::Button(s_state.playing ? "Pause" : "Play", ImVec2(52.0f, 0.0f)))
        s_state.playing = !s_state.playing;
    ImGui::SameLine();
    ImGui::SetNextItemWidth((std::max)(ImGui::GetContentRegionAvail().x - 190.0f, 60.0f));
    float time = s_state.time;
    if (ImGui::SliderFloat("##PreviewTime", &time, 0.0f, s_state.timelineLength, "%.2f s")) {
        s_state.time = std::clamp(time, 0.0f, s_state.timelineLength);
        s_state.playing = false; // スクラブ中は停止 (Unity と同じ操作感)
    }
    // AnimEvent の発火位置をスライダー上へマーカー表示し、スクラブで狙い撃ちできるようにする。
    if (overlayClip && !overlayClip->events.empty() && s_state.timelineLength > 0.0001f) {
        const ImVec2 sliderMin = ImGui::GetItemRectMin();
        const ImVec2 sliderMax = ImGui::GetItemRectMax();
        for (const auto& event : overlayClip->events) {
            const float eventT = std::clamp(
                static_cast<float>(event.time) / s_state.timelineLength, 0.0f, 1.0f);
            const float markerX = sliderMin.x + eventT * (sliderMax.x - sliderMin.x);
            ImGui::GetWindowDrawList()->AddLine(
                ImVec2(markerX, sliderMin.y),
                ImVec2(markerX, sliderMin.y + 5.0f),
                IM_COL32(255, 216, 96, 255), 2.0f);
        }
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(64.0f);
    ImGui::DragFloat("##PreviewSpeed", &s_state.speed, 0.05f, 0.1f, 3.0f, "x%.2f");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Playback speed");
    ImGui::SameLine();
    ImGui::Checkbox("Loop", &s_state.loop);

    // ── 選択中ボーンのライブ詳細 (現在時刻のローカル TRS + キー数) ──
    if (s_state.selectedBoneValid && overlayModel && overlayModel->skeleton &&
        s_state.selectedBoneNode >= 0 &&
        s_state.selectedBoneNode < static_cast<int>(overlayModel->skeleton->nodes.size())) {
        const auto& node =
            overlayModel->skeleton->nodes[static_cast<size_t>(s_state.selectedBoneNode)];
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.38f, 1.0f), "%s",
                           ShortBoneName(node.name).c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("x##DeselectBone")) s_state.selectedBoneNode = -1;
        const math::Vector3 euler = QuaternionToEulerDegrees(s_state.selectedBonePose.rotation);
        ImGui::TextDisabled(
            "T (%.2f, %.2f, %.2f)   R (%.0f, %.0f, %.0f)   S (%.2f, %.2f, %.2f)",
            s_state.selectedBonePose.translation.x,
            s_state.selectedBonePose.translation.y,
            s_state.selectedBonePose.translation.z,
            euler.x, euler.y, euler.z,
            s_state.selectedBonePose.scale.x,
            s_state.selectedBonePose.scale.y,
            s_state.selectedBonePose.scale.z);
        const bool hasAnyKeys = s_state.selectedBoneKeyCounts[0] > 0 ||
                                s_state.selectedBoneKeyCounts[1] > 0 ||
                                s_state.selectedBoneKeyCounts[2] > 0;
        ImGui::TextDisabled("Keys  P:%d  R:%d  S:%d%s",
                            s_state.selectedBoneKeyCounts[0],
                            s_state.selectedBoneKeyCounts[1],
                            s_state.selectedBoneKeyCounts[2],
                            hasAnyKeys ? "" : "  (bind pose only)");

        // ── 選択ボーンの位置 / 回転カーブミニグラフ ──
        // WHY: 数値の一瞬値だけでは補間の質 (急な段差・平坦な区間・往復) が読めない。
        //      クリップ全長の XYZ カーブを重ねて、再生ヘッド位置と合わせて確認できるようにする。
        if (s_state.showCurves && !s_state.curvePosX.empty()) {
            const float wrappedTime = WrapTime(s_state.time, s_state.timelineLength);
            const float clipLen = ClipDurationSeconds(overlayClip, s_state.timelineLength);
            const float playhead =
                clipLen > 0.0001f ? std::clamp(wrappedTime / clipLen, 0.0f, 1.0f) : 0.0f;

            // X=赤 / Y=緑 / Z=青 の 3 系列を 1 枚のグラフへ重ね描きする共通ルーチン。
            auto drawCurveGraph = [&](const char* title,
                                      const std::vector<float>& cx,
                                      const std::vector<float>& cy,
                                      const std::vector<float>& cz,
                                      float vMin, float vMax, const char* unit) {
                ImGui::TextDisabled("%s", title);
                const float graphWidth = (std::max)(ImGui::GetContentRegionAvail().x, 120.0f);
                const float graphHeight = 58.0f;
                const ImVec2 graphOrigin = ImGui::GetCursorScreenPos();
                ImGui::InvisibleButton(title, ImVec2(graphWidth, graphHeight));
                const bool graphHovered = ImGui::IsItemHovered();
                ImDrawList* gl = ImGui::GetWindowDrawList();
                const ImVec2 graphMax(graphOrigin.x + graphWidth, graphOrigin.y + graphHeight);
                gl->AddRectFilled(graphOrigin, graphMax, IM_COL32(22, 24, 30, 255), 3.0f);
                gl->AddRect(graphOrigin, graphMax, IM_COL32(70, 76, 88, 255), 3.0f);

                float range = vMax - vMin;
                if (range < 0.0001f) range = 1.0f; // 定数カーブは中央に平坦線として描く
                const float pad = graphHeight * 0.12f;
                const auto valueToY = [&](float v) {
                    const float t = (v - vMin) / range;
                    return graphMax.y - pad - t * (graphHeight - 2.0f * pad);
                };
                // 0 ライン (値域に 0 が含まれるときだけ) を薄く引いて符号を読めるようにする。
                if (vMin < 0.0f && vMax > 0.0f) {
                    const float zeroY = valueToY(0.0f);
                    gl->AddLine(ImVec2(graphOrigin.x, zeroY), ImVec2(graphMax.x, zeroY),
                                IM_COL32(80, 86, 98, 160));
                }

                const size_t sampleCount = cx.size();
                auto plotSeries = [&](const std::vector<float>& series, ImU32 color) {
                    if (series.size() < 2) return;
                    for (size_t i = 1; i < series.size(); ++i) {
                        const float x0 = graphOrigin.x +
                            static_cast<float>(i - 1) / (sampleCount - 1) * graphWidth;
                        const float x1 = graphOrigin.x +
                            static_cast<float>(i) / (sampleCount - 1) * graphWidth;
                        gl->AddLine(ImVec2(x0, valueToY(series[i - 1])),
                                    ImVec2(x1, valueToY(series[i])), color, 1.5f);
                    }
                };
                plotSeries(cx, IM_COL32(235, 96, 96, 235));   // X 赤
                plotSeries(cy, IM_COL32(120, 224, 120, 235)); // Y 緑
                plotSeries(cz, IM_COL32(110, 170, 255, 235)); // Z 青

                // 再生ヘッド (白い縦線)。スクラブ可能にして、グラフから直接時刻を掴めるようにする。
                const float headX = graphOrigin.x + playhead * graphWidth;
                gl->AddLine(ImVec2(headX, graphOrigin.y), ImVec2(headX, graphMax.y),
                            IM_COL32(245, 245, 245, 220), 1.0f);
                if (graphHovered && ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
                    clipLen > 0.0001f) {
                    const float t =
                        std::clamp((ImGui::GetIO().MousePos.x - graphOrigin.x) / graphWidth,
                                   0.0f, 1.0f);
                    s_state.time = t * clipLen;
                    s_state.playing = false;
                }
                // 右上に値域を表示 (単位付き)。
                char rangeText[64]{};
                std::snprintf(rangeText, sizeof(rangeText), "[%.2f, %.2f]%s", vMin, vMax, unit);
                const ImVec2 rangeSize = ImGui::CalcTextSize(rangeText);
                gl->AddText(ImVec2(graphMax.x - rangeSize.x - 4.0f, graphOrigin.y + 3.0f),
                            IM_COL32(150, 158, 170, 220), rangeText);
            };

            ImGui::Spacing();
            drawCurveGraph("Position  (X / Y / Z)",
                           s_state.curvePosX, s_state.curvePosY, s_state.curvePosZ,
                           s_state.curvePosMin, s_state.curvePosMax, "");
            drawCurveGraph("Rotation  (X / Y / Z, deg)",
                           s_state.curveRotX, s_state.curveRotY, s_state.curveRotZ,
                           s_state.curveRotMin, s_state.curveRotMax, " deg");
        } else if (s_state.showCurves) {
            ImGui::TextDisabled("(no animated track for this bone)");
        }
    } else if (s_state.showCurves) {
        ImGui::TextDisabled("Curves: click a bone in the preview to inspect its channels.");
    }

    // ── クリップ選択 ────────────────────────────────────────────────────────
    // モデル内蔵クリップに加え、パッケージ配下の .anim も列挙する。
    // WHY: FBZZ は 1 クリップ = 1 FBX なのでモデル内蔵クリップは常に空。
    //   MiniBot.fbx を選んだときに Idle / Walk / Run … を切り替えられないと
    //   「モデルは出るが動かせない」状態になる。
    if (s_state.target.mode == PreviewTarget::Mode::Clip) {
        const auto& anims = CollectPackageAnims(s_state.target.modelPath);
        const asset::Model* model = asset::AssetManager::LoadModel(s_state.target.modelPath);
        const bool hasEmbedded = model && model->clips.size() > 1;

        if (hasEmbedded || !anims.empty()) {
            std::string currentLabel = "<bind pose>";
            if (!s_state.target.animAssetPath.empty())
                currentLabel = util::FileSystem::GetFilename(s_state.target.animAssetPath);
            else if (const asset::AnimationClip* c = FindModelClip(model, s_state.target.clipName))
                currentLabel = c->name;

            ImGui::TextDisabled("Clip:");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::BeginCombo("##PreviewClip", currentLabel.c_str())) {
                // バインドポーズ (クリップ無し) へ戻す選択肢
                if (ImGui::Selectable("<bind pose>", s_state.target.animAssetPath.empty() &&
                                                     s_state.target.clipName.empty())) {
                    s_state.target.animAssetPath.clear();
                    s_state.target.clipName.clear();
                    s_state.time = 0.0f;
                }
                if (hasEmbedded) {
                    for (const auto& clip : model->clips) {
                        const bool selected = s_state.target.animAssetPath.empty() &&
                                              clip.name == s_state.target.clipName;
                        if (ImGui::Selectable(clip.name.c_str(), selected)) {
                            s_state.target.animAssetPath.clear();
                            s_state.target.clipName = clip.name;
                            s_state.time = 0.0f;
                        }
                        if (selected) ImGui::SetItemDefaultFocus();
                    }
                }
                for (const auto& animPath : anims) {
                    const std::string name = util::FileSystem::GetFilename(animPath);
                    const bool selected = animPath == s_state.target.animAssetPath;
                    if (ImGui::Selectable(name.c_str(), selected)) {
                        s_state.target.animAssetPath = animPath;
                        s_state.target.clipName.clear();
                        s_state.time = 0.0f;
                        s_state.playing = true;
                    }
                    if (selected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Clips found in this model's package (%zu .anim files).",
                                  anims.size());
        }
    }

    ImGui::PopID();
    return true;
}

void AnimationPreviewPanel::OnRenderContent(EditorContext& ctx)
{
    // ヘッダー + デバッグトグルバー + コントロール行を差し引いた残りをプレビュー画像に使う。
    float controlsHeight = ImGui::GetFrameHeightWithSpacing() * 4.0f;
    // Curves 表示中は 2 枚のミニグラフ + 選択ボーン詳細行の分だけ画像を縮める。
    if (s_state.showCurves && s_state.selectedBoneNode >= 0)
        controlsHeight += 190.0f;
    const float previewHeight =
        (std::max)(ImGui::GetContentRegionAvail().y - controlsHeight, 120.0f);
    if (!DrawAnimationPreviewWidget(ctx, previewHeight)) {
        // 対象が無い間もドロップ領域として機能させ、FBX を落とすだけでプレビューを始められる。
        const ImVec2 zoneSize(
            (std::max)(ImGui::GetContentRegionAvail().x, 64.0f),
            (std::max)(ImGui::GetContentRegionAvail().y, 96.0f));
        const ImVec2 zoneOrigin = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##PreviewDropZone", zoneSize);
        AcceptPreviewAssetDrop();

        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const ImVec2 zoneMax(zoneOrigin.x + zoneSize.x, zoneOrigin.y + zoneSize.y);
        drawList->AddRect(zoneOrigin, zoneMax, IM_COL32(90, 96, 108, 160), 6.0f,
                          0, 1.0f);
        const char* line1 = "Select an Animation State, Transition, or an .anim asset,";
        const char* line2 = "or drop an FBX / .anim here to preview.";
        const ImVec2 size1 = ImGui::CalcTextSize(line1);
        const ImVec2 size2 = ImGui::CalcTextSize(line2);
        const float centerY = zoneOrigin.y + zoneSize.y * 0.5f;
        drawList->AddText(
            ImVec2(zoneOrigin.x + (zoneSize.x - size1.x) * 0.5f,
                   centerY - size1.y),
            IM_COL32(150, 158, 170, 255), line1);
        drawList->AddText(
            ImVec2(zoneOrigin.x + (zoneSize.x - size2.x) * 0.5f,
                   centerY + 4.0f),
            IM_COL32(150, 158, 170, 255), line2);
    }
}

} // namespace fbzz::editor
