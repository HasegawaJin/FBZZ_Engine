/// @file    AnimationPreviewInternal.hpp
/// @brief   Animation Preview の翻訳単位間で共有する内部状態と補助関数。
/// @author  Hasegawa Jin
/// @date    2026-09-16
///
/// プレビューは «対象解決 → GPU 描画 → UI» の 3 段がそれぞれ数百行あり、1 ファイルに畳むと編集のたび全体を読む羽目になる。
/// 一方 3 段は同じ再生状態 (時刻・カメラ・トグル) を見るため、状態を跨がせる口が要る。
/// Editor/Panels/ 側の公開ヘッダーには出さず、この src 配下だけで閉じる。
#pragma once
#include <Editor/Panels/AnimationPreview.hpp>
#include <Engine/Asset/AvatarMaskAsset.hpp>
#include <Engine/Asset/Model.hpp>
#include <Engine/Renderer/DynamicBufferPool.hpp>
#include <Engine/Renderer/Material.hpp>
#include <Engine/Asset/AssetHandle.hpp>
#include <Engine/Asset/MaterialAsset.hpp>
#include <Engine/Renderer/ResourceHandle.hpp>
#include <Engine/Scene/Components/AnimatorComponent.hpp>
#include <Engine/Scene/Entity.hpp>
#include <Editor/EditorContext.hpp>
#include <Math/Matrix4.hpp>
#include <Math/Quaternion.hpp>
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>
#include <imgui.h>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace fbzz::renderer { class ResourceManager; }

namespace fbzz::editor::animpreview {

/// マテリアル未設定時に使うプレビュー用定数バッファ。
/// @note Animation Preview は MaterialPreview.cpp の GPU 状態を共有せず、単体で既定材質を描画する。
struct PreviewMaterialCB {
    math::Vector4 albedo{ 0.72f, 0.72f, 0.75f, 1.0f };
    uint32_t textureMask = 0;
    float _pad[3] = {};
};

struct LocalPose {
    math::Vector3 translation = math::Vector3::ZERO;
    math::Quaternion rotation = math::Quaternion::Identity();
    math::Vector3 scale = math::Vector3::ONE;
};

struct PreviewTarget {
    enum class Mode { None, Clip, Transition };
    Mode mode = Mode::None;
    std::string modelPath;      ///< メッシュとスケルトンの供給元 (.fbx / .asset / .fzasset)
    std::string clipName;       ///< 空 = モデル先頭クリップ
    std::string animAssetPath;  ///< .anim 直接プレビュー時のみ。clipName より優先される
    std::string toClipName;     ///< Transition の遷移先クリップ
    std::string toSourcePath;   ///< 遷移先クリップの供給元 (from と異なる source を許容)
    std::string toAnimAssetPath; ///< 遷移先が外部 .anim の場合の直接参照
    float blendSeconds = 0.25f;
    float transitionStartSeconds = 0.0f;
    std::string label;

    /// 再生時刻をリセットすべき対象変化かどうかの同一性 (パラメーター編集では維持する)
    bool SameIdentity(const PreviewTarget& other) const
    {
        return mode == other.mode && modelPath == other.modelPath &&
               clipName == other.clipName && animAssetPath == other.animAssetPath &&
               toClipName == other.toClipName && toSourcePath == other.toSourcePath &&
               toAnimAssetPath == other.toAnimAssetPath;
    }
};

/// Manual = プレビュー画面へ直接ドロップされた対象。選択に追従せず、次の選択操作まで維持する。
/// GameObject = Animator を持つシーン上のオブジェクト選択由来 (Unity の既定動線)。
enum class TargetOrigin { None, Graph, Asset, GameObject, Manual };

struct PreviewGpu {
    renderer::ResourceHandle<renderer::ShaderTag>         skinnedShader;
    renderer::ResourceHandle<renderer::ShaderTag>         surfaceShader;
    renderer::ResourceHandle<renderer::PipelineStateTag>  pso;
    /// ワイヤーフレーム表示用。スキニング結果のシルエットとエッジ流れを同時に見るため。
    renderer::ResourceHandle<renderer::PipelineStateTag>  wireframePso;
    renderer::ResourceHandle<renderer::ConstantBufferTag> frameCB;
    renderer::ResourceHandle<renderer::ConstantBufferTag> objectCB;
    renderer::ResourceHandle<renderer::ConstantBufferTag> materialCB;
    renderer::ResourceHandle<renderer::ConstantBufferTag> lightCB;
    renderer::ResourceHandle<renderer::ConstantBufferTag> shadowCB;
    /// Spot / Point シャドウ (b12) の無効化用。中身は 0 のまま使う。
    /// 定数バッファの束縛はドローをまたいで残るが SRV は毎回クリアされるので、
    /// シーン描画の b12 が残るとアトラス未束縛のまま「完全な影」を引いて黒くなる。
    renderer::ResourceHandle<renderer::ConstantBufferTag> punctualShadowCB;
    /// ライト供給モード (b9) の無効化用。中身は 0 = FBZZ_LIGHT_MODE_LEGACY のまま使う。
    /// @note シーン描画は Forward でも LINEAR (統合配列) を使うため b9 の束縛は残るが、ライト配列 (t29) は毎回クリアされる。0 のまま渡すとレガシー経路 (b3) へ倒れて安全に無効化される。
    renderer::ResourceHandle<renderer::ConstantBufferTag> clusterCB;
    renderer::ResourceHandle<renderer::ConstantBufferTag> skinningCB;
    renderer::ResourceHandle<renderer::RenderTargetTag>   renderTarget;
};

/// 床グリッド / 接地リング用の線分描画リソース。
/// @note 頂点カラーだけの Unlit 経路でライト・スキニング・マテリアルは不要。同じ PreviewGpu へ混ぜると «この CB はどちらのドローのものか» が読めなくなるため分離する。
struct PreviewLineGpu {
    renderer::ResourceHandle<renderer::ShaderTag>         shader;
    renderer::ResourceHandle<renderer::ConstantBufferTag> cameraCB;
    renderer::ResourceHandle<renderer::PipelineStateTag>  pso;
    renderer::DynamicVertexBufferPool                     pool{ 256 };
};

/// DebugDraw.hlsl の頂点入力と同じ並び。
struct PreviewLineVertex {
    math::Vector3 position;
    math::Vector4 color;
};

struct PreviewSkinningCB {
    math::Matrix4 boneMatrices[asset::MAX_SKINNING_BONES];
};

/// 見た目まわりの設定。再生対象が変わっても保たれ、EditorSettings へ往復する。
struct PreviewViewSettings {
    bool  showGrid       = true;  ///< 床グリッド
    bool  showGroundRing = true;  ///< 接地リング + ルート垂線 (足の高さを読む)
    bool  wireframe      = false;
    bool  showAxisGizmo  = true;  ///< 右下の XYZ トライアド
    int   background     = 0;     ///< kPreviewBackgrounds の添字
    float fovY           = 40.0f;
    float lightYaw       = 0.0f;  ///< キーライトの水平回転オフセット (rad)
};

/// ルートモーション解析の結果。クリップが変わったときだけ組み直す。
/// @note 軌跡のポリラインは «どう動いたか» までしか見せない。ロコモーションに要るのは «1 ループで何 m 進み平均何 m/s か» (Blend Tree の閾値) と «ループ端で飛ぶか»・«接地中に足が滑るか» の判定。
struct RootMotionAnalysis {
    bool  valid = false;
    int   rootNode = -1;       ///< 解析に使った代表ボーン (根元から最初の変形ボーン = 通常 Hips)
    float duration = 0.0f;
    float bodyHeight = 1.0f;   ///< 全ボーンの Y 範囲。しきい値を «身長比» で決めるため

    /// 移動量
    float pathLength = 0.0f;   ///< 水平の経路長 (曲がりも含む実移動)
    float netDistance = 0.0f;  ///< 始点 → 終点の直線距離 (水平)
    math::Vector3 netDelta = math::Vector3::ZERO;
    float netTurnDegrees = 0.0f; ///< ルートの向きが 1 ループで回る角度
    float averageSpeed = 0.0f;
    float maxSpeed = 0.0f;
    float verticalRange = 0.0f;  ///< 上下動の振れ幅 (bob)
    bool  inPlace = false;       ///< ルートがほぼ動かない = その場再生用クリップ

    std::vector<float> speedSamples;    ///< 水平速度 (m/s)
    std::vector<float> verticalSamples; ///< 垂直速度 (符号付き)

    /// ループ整合。ルートボーンは «進む・回る» のが正しいので、この 2 つからは除く。
    float loopPositionGap = 0.0f;  ///< 全ボーンのローカル位置差の RMS
    float loopRotationGap = 0.0f;  ///< 最大の回転差 (degrees)
    std::string loopWorstBone;
    float loopSpeedGap = 0.0f;     ///< 先頭と末尾の速度差 (m/s)

    /// 接地と足滑り
    int   footNodes[2] = { -1, -1 };
    std::string footNames[2];
    float plantedSlide = 0.0f;      ///< 接地判定中に足が水平移動した合計距離
    float plantedSeconds = 0.0f;    ///< 接地判定だったサンプル時間の合計 (両足ぶん)
    float requiredRootSpeed = 0.0f; ///< 滑りを打ち消すのに要るルート速度
};

struct PreviewState {
    PreviewTarget target;
    TargetOrigin origin = TargetOrigin::None;

    /// ユーザーが明示的に指定したジオメトリ (プレビューへ D&D / 選択したモデル)。
    /// @note FBZZ は「1 クリップ = 1 FBX」規約のため .anim の隣にスキンメッシュを持つモデルが無い。Unity の .anim プレビューと同じく、一度指定したジオメトリはセッション中覚えておき以降すべてのクリップに使い回す。
    std::string attachedModelPath;

    /// Animator 付き GameObject の選択変化検出用
    scene::EntityID lastSelectedEntity = scene::EntityID::INVALID;

    /// 直前フレームの選択スナップショット。「後から変わった方」をプレビュー対象にする。
    EditorContext::AnimationGraphSelection::Type lastGraphType =
        EditorContext::AnimationGraphSelection::Type::None;
    int lastGraphState = -1;
    int lastGraphTransition = -1;
    std::string lastGraphAssetPath;
    std::string lastGraphLayerName;
    scene::EntityID lastGraphEntity = scene::EntityID::INVALID;
    std::string lastSelectedAssetPath;

    /// 再生
    float time = 0.0f;
    bool  playing = true;
    bool  loop = true;
    float speed = 1.0f;
    float timelineLength = 1.0f;
    float currentBlendWeight = 0.0f; ///< Transition の現在ブレンド率 (UI 表示用)
    /// タイムラインをドラッグ中か。再生ヘッドのつまみを太くするためだけに持つ。
    bool  scrubbing = false;

    /// カメラ (対象が変わっても維持し、Unity と同じく視点を保つ)
    float yaw = 2.55f;
    float pitch = 0.30f;
    float distance = -1.0f; ///< <0 = 未フレーミング (バウンディングから自動決定)
    math::Vector3 focus = math::Vector3::ZERO;
    bool needsFraming = true;
    /// 次フレームでこの位置へ寄る (ボーンフォーカス)。未指定なら bounds 中心。
    bool  hasPendingFocus = false;
    math::Vector3 pendingFocus = math::Vector3::ZERO;
    float pendingDistance = 0.0f;

    PreviewViewSettings view;

    /// 1 フレーム 1 回だけ時間更新 + 描画するためのガード
    int lastAdvanceFrame = -1;
    int lastRenderFrame = -1;
    std::string lastRenderIdentity;
    bool lastRenderOk = false;

    bool showMesh = true;       ///< Off でスケルトンのみ表示
    bool showBones = false;     ///< ボーンオーバーレイ
    bool showBoneNames = false; ///< ボーン名ラベル
    bool showTrail = false;     ///< 代表ボーンの移動軌跡 (ルートモーション確認)
    bool showGhost = false;     ///< オニオンスキン (前後フレームの残像スケルトン)
    bool showInfo = false;      ///< 時刻 / フレーム / トラック情報オーバーレイ
    bool showCurves = false;    ///< 選択ボーンの位置/回転カーブミニグラフ
    bool showRootMotion = false; ///< 移動量 / 速度グラフ / ループ整合 / 足滑り
    int  selectedBoneNode = -1; ///< クリックで選択中のスケルトンノード index

    /// ラベル間引き設定。100+ ボーンモデルで全名を出すと潰れるため、
    /// label mode で「選択+ホバー / アニメ有 / 全部」を切り替える。
    /// 0=Selected/Hovered only, 1=Animated bones, 2=All
    int labelMode = 1;
    /// オニオンスキンの前後フレームオフセット (秒)。0 = タイムライン長依存の自動。
    float ghostOffsetSeconds = 0.0f;

    /// RenderPreviewFrame が毎フレーム更新するオーバーレイ用データ
    std::vector<math::Vector3> jointPositions;      ///< 現在ポーズの各ノード位置 (ワールド)
    std::vector<math::Vector3> ghostPrevPositions;  ///< オニオンスキン (過去側)
    std::vector<math::Vector3> ghostNextPositions;  ///< オニオンスキン (未来側)
    /// 剛体メッシュの描画姿勢をヒットテストでも再利用する。
    /// @note 描画だけをアニメーション後の位置へ移すと、クリック判定がバインド姿勢に残り、Hierarchy と Preview の連動が部位によって外れる。
    std::vector<math::Matrix4> meshPreviewWorlds;
    math::Matrix4 debugViewProjection = math::Matrix4::Identity();
    bool debugPoseValid = false;
    /// 接地リングと垂線の基準面 (モデル最下端の Y)。オーバーレイの高さ表示にも使う。
    float groundY = 0.0f;
    float boundsRadius = 1.0f;
    float footprintRadius = 1.0f;

    /// クリップが変わったときだけ再計算するキャッシュ
    std::string debugCacheIdentity;
    std::vector<uint8_t> nodeHasTrack; ///< クリップが実際に動かすノードか (トラック有無)
    std::vector<math::Vector3> trailPoints;
    /// 軌跡・接地リングの基準にしている代表ボーン (通常 Hips)。
    int trailNodeIndex = -1;
    int animatedNodeCount = 0;
    int boneNodeCount = 0;

    /// 選択中ボーンのライブ情報 (RenderPreviewFrame がサンプル)
    LocalPose selectedBonePose;
    bool selectedBoneValid = false;
    int selectedBoneKeyCounts[3] = { 0, 0, 0 }; ///< P / R / S キー数

    /// 選択ボーンのカーブグラフ用キャッシュ。ボーンかクリップが変わったときだけ再サンプルする。
    /// @note 毎フレーム全キーを再サンプルするとカーブ描画のためだけに CPU を無駄遣いする。
    /// ルートモーション解析。全ノードを全長サンプルするため、クリップ単位でキャッシュする。
    std::string rootMotionCacheKey;
    RootMotionAnalysis rootMotion;

    std::string curveCacheKey;          ///< "modelPath|clipName|nodeIndex" で識別
    std::vector<float> curvePosX, curvePosY, curvePosZ;   ///< 正規化前の生値
    std::vector<float> curveRotX, curveRotY, curveRotZ;   ///< オイラー角 (degrees)
    float curvePosMin = 0.0f, curvePosMax = 0.0f;
    float curveRotMin = 0.0f, curveRotMax = 0.0f;
};

/// インポーターが書き出した .mat から組んだ描画用マテリアル。添字は model->meshes と同じ。
/// @note `Load<Model>` が返す materials は既定値の空 Material でシェーダーを持たず、それだけではプレビューが常にグレーになる。AssetBrowser のサムネイルやシーン配置と同じ .mat を引いて描く。
struct PreviewMaterialSlot {
    std::string materialPath;
    /// @note 型を fbzz:: から完全修飾するのは、同じクラス内で `asset` が
    ///       メンバー名と名前空間名の 2 通りに読めてしまうのを避けるため。
    fbzz::asset::AssetHandle<fbzz::asset::MaterialAsset> asset;
    uint64_t revision = 0;
    bool built = false;
    std::unique_ptr<renderer::Material> material;
};

struct PreviewMaterialCache {
    std::string modelPath;
    std::vector<PreviewMaterialSlot> slots;
};

/// Avatar Mask Preview は通常の Animation Preview と同じ GPU / カメラを使うが、
/// メッシュをマスクウェイト色で描くための対象だけを別状態で保持する。
struct MaskPreviewState {
    bool active = false;
    bool loaded = false;
    std::string modelPath;
    std::string maskPath;
    std::string selectedNodePath;
    std::string pendingSelectionPath;
    asset::AvatarMaskAsset mask;
};

/// パネルの外から「この .mask を見せろ」と指定された 1 ショット。
struct MaskPreviewRequest {
    bool        pending = false;
    std::string maskPath;
    std::string modelPath;
};

inline constexpr int PREVIEW_RT_SIZE = 512;

/// 背景プリセット。暗すぎる既定だけだと、黒い衣装のモデルがシルエットごと沈む。
inline constexpr const char* kPreviewBackgroundNames[] = {
    "Charcoal", "Graphite", "Studio", "Black"
};
inline constexpr float kPreviewBackgrounds[][4] = {
    { 0.09f, 0.10f, 0.12f, 1.0f },
    { 0.17f, 0.18f, 0.21f, 1.0f },
    { 0.29f, 0.31f, 0.35f, 1.0f },
    { 0.02f, 0.02f, 0.03f, 1.0f },
};
inline constexpr int kPreviewBackgroundCount =
    static_cast<int>(sizeof(kPreviewBackgrounds) / sizeof(kPreviewBackgrounds[0]));

/// 共有状態 (実体は AnimationPreviewCore.cpp)
extern PreviewState        g_state;
extern PreviewGpu          g_gpu;
extern PreviewLineGpu      g_lineGpu;
extern PreviewMaterialCache g_materialCache;
extern MaskPreviewState    g_maskPreview;
extern MaskPreviewRequest  g_maskPreviewRequest;

/// クリップ / ポーズ (AnimationPreviewCore.cpp)
const asset::NodeAnimationTrack* FindTrack(const asset::AnimationClip& clip,
                                           const std::string& nodeName);
LocalPose SamplePose(const asset::SkeletonNode& node,
                     const asset::AnimationClip* clip, double ticks);
double ClipTicksPerSecond(const asset::AnimationClip& clip);
float  ClipDurationSeconds(const asset::AnimationClip* clip, float fallback);
float  WrapTime(float time, float duration);
/// クリップの表示フレームレート。未設定のクリップは 30fps とみなす。
float  ClipFrameRate(const asset::AnimationClip* clip);

math::Vector3 MatrixTranslation(const math::Matrix4& m);
math::Vector3 QuaternionToEulerDegrees(const math::Quaternion& q);
std::string   ShortBoneName(const std::string& name);

/// 代表ボーン (根元から最初に見つかる変形ボーン) の軌跡をクリップ全長でサンプルする。
/// @param outTrailNode 採用した代表ボーンのノード index。見つからなければ -1。
void BuildTrailPoints(const asset::Skeleton& skeleton,
                      const asset::AnimationClip& clip,
                      std::vector<math::Vector3>& outPoints,
                      int& outTrailNode);
/// クリップ全長からルートの移動量・速度・ループ整合・足滑りを求める。
/// 全ノードを 64 分割でサンプルするので、呼び出し側でクリップ単位にキャッシュすること。
void AnalyzeRootMotion(const asset::Skeleton& skeleton,
                       const asset::AnimationClip& clip,
                       RootMotionAnalysis& out);

void BuildBoneCurves(const asset::Skeleton& skeleton,
                     const asset::AnimationClip& clip,
                     int nodeIndex,
                     std::vector<float>& posX, std::vector<float>& posY, std::vector<float>& posZ,
                     std::vector<float>& rotX, std::vector<float>& rotY, std::vector<float>& rotZ,
                     float& posMin, float& posMax, float& rotMin, float& rotMax);

const asset::AnimationClip* FindModelClip(const asset::Model* model, const std::string& clipName);
const asset::AnimationClip* ResolveClip(const asset::Model* model,
                                        const std::string& clipName,
                                        const std::string& animAssetPath);

/// 対象解決 (AnimationPreviewCore.cpp)
bool        IsPreviewableGeometry(const asset::Model* model);
bool        LoadsAsPreviewableGeometry(const std::string& path);
std::string FindGeometryForAnim(const std::string& animPath);
const std::vector<std::string>& CollectPackageAnims(const std::string& modelPath);
bool ResolvePathTarget(const std::string& path, PreviewTarget& out);
void AdoptManualTarget(const PreviewTarget& target, bool attachModel);
void SwapPreviewGeometry(const std::string& modelPath);
bool AcceptPreviewAssetDrop();
void UpdatePreviewTarget(EditorContext& ctx);

/// 描画 (AnimationPreviewCore.cpp)
struct PreviewBounds {
    math::Vector3 center = math::Vector3::ZERO;
    float radius   = 1.0f; ///< 外接球半径 (フレーミング用)
    float minY     = 0.0f; ///< 最下端。床グリッドと接地リングを置く高さ
    float radiusXZ = 1.0f; ///< 水平方向の広がり。接地リングの半径
};

bool RenderPreviewFrame(EditorContext& ctx, float displayAspect);
void AdvancePlayback();
void ComputeModelBounds(const asset::Model& model, PreviewBounds& out);

/// Avatar Mask (AnimationPreviewCore.cpp)
std::string MaskPathForModelNode(const asset::Model& model, int nodeIndex);
ImVec4 MaskWeightColor(float weight);
ImU32  MaskWeightColorU32(float weight, int alpha = 235);
std::string MaskPreviewRevision(const asset::AvatarMaskAsset& mask);

/// カメラ操作 (AnimationPreviewCore.cpp)
/// 現在の yaw / pitch から、画面右方向と上方向のワールドベクトルを求める。
/// パン操作・コーナーの軸ギズモ・ライト回転が同じ基底を見るための唯一の実装。
void PreviewCameraBasis(math::Vector3& outRight, math::Vector3& outUp, math::Vector3& outForward);

} // namespace fbzz::editor::animpreview
