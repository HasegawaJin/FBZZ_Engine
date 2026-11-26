/// @file    AnimatorComponent.hpp
/// @brief   スケルタルアニメーション再生状態コンポーネント。
/// @author  Hasegawa Jin
/// @date    2026-05-24

/// @note Model の AnimationClip を参照し、現在時刻や再生速度を保持する。骨行列の計算と GPU 転送、
/// @note State/BlendTree を統一したステートマシンの評価は AnimatorSystem が行う。
#pragma once
#include <Engine/Scene/Components/Rendering/TransientGpuState.hpp>
#include <Engine/Api.hpp>

#include <Engine/Asset/Animation/Clips/AnimationClip.hpp>
#include <Engine/Asset/Animation/Controllers/AnimatorControllerDefinition.hpp>
#include <Engine/Asset/AssetHandle.hpp>
#include <Engine/Asset/Animation/Rig/AvatarMaskAsset.hpp>
#include <Engine/Scene/Components/Animation/BoneBakeTypes.hpp>
#include <Graphics/Renderer/Resources/ResourceHandle.hpp>
#include <Engine/Reflection/Reflection.hpp>
#include <Math/LinearAlgebra/Matrix4.hpp>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fbzz::asset { struct Model; }

namespace fbzz::scene {

struct AnimatorNodeLocalPose {
    math::Vector3 translation = math::Vector3::ZERO; ///< 親ノードを基準にしたこのノードの平行移動 [m]。
    math::Quaternion rotation = math::Quaternion::Identity(); ///< 親ノードを基準にしたこのノードの回転。
    math::Vector3 scale = math::Vector3::ONE; ///< 親ノードから引き継ぐ伸縮に掛ける軸別のローカル倍率。
};

/// @brief AnimatorSystem が当該フレームに通過した Event。
/// @note VFXGraph など Script 以外の購読者も利用する。
struct FiredAnimationEvent {
    std::string name; ///< クリップのイベントキーで指定したイベント識別名。
    int intParam = 0; ///< クリップのイベントキーに保存された整数の追加引数。
    float floatParam = 0.0f; ///< クリップのイベントキーに保存された浮動小数の追加引数。
    float time = 0.0f; ///< 発火したイベントキーのクリップ内時刻 [秒]。
    std::uint64_t frame = 0; ///< イベントを発火したアニメーション更新のフレーム番号。
};

/// @name ステートマシン用データ型
/// @{





/// @brief Float パラメーター1本で複数モーションを補間する。
struct BlendTree1D {
    std::string                  paramName; ///< 1 次元の補間位置を決める Animator の float パラメーター名。
    /// @note パラメーターの急変を時間補間し、Idle/Walk/Run の姿勢が瞬時に切り替わるのを防ぐ。0 は
    /// @note 平滑化なし。値は目標へ約63%近づく時定数 [秒] として扱う。
    float                        dampTime = 0.0f; ///< パラメーター入力の時間平滑化に使う時定数 [秒]。0 は平滑化なし。
    bool                         syncNormalizedTime = false; ///< 全 Motion を同じ正規化位相で評価し、Weight が再上昇した Clip の位相ジャンプを防ぐ
    std::vector<BlendTreeMotion> motions; ///< 補間のしきい値ごとに設定したクリップの候補。

    /// @name ランタイム専用 (Controller/Scene には保存しない)
    /// @{
    float                        dampedValue = 0.0f; ///< 時間平滑化を適用した 1D パラメーターの現在値。
    bool                         dampedValueInitialized = false; ///< 1D の平滑化に使う前回値を初期化済みかどうか。
    float                        normalizedPhase = 0.0f; ///< 位相同期が有効なときに全クリップへ渡す共通の正規化再生位置。
    bool                         normalizedPhaseInitialized = false; ///< 1D の共通位相を現在の再生状態に初期化したかどうか。
    /// @}
};


/// @brief Float パラメーター2本で複数モーションを2D補間する。
struct BlendTree2D {
    std::string                  paramX; ///< XY 平面で X の補間位置を決める Animator の float パラメーター名。
    std::string                  paramY; ///< XY 平面で Y の補間位置を決める Animator の float パラメーター名。
    BlendTree2DType              type = BlendTree2DType::SimpleDirectional; ///< 二次元の座標からクリップの重みを求めるブレンド方式。
    /// @note パラメーターの急変を時間補間する (1D と同じ意味・時定数)。生値のままだと入力を離した
    /// @note 瞬間に不連続へ飛び、前進から後退への切り返し1フレームで前後クリップが入れ替わり脚が跳ねる。
    float                        dampTime = 0.0f; ///< パラメーターの XY 入力を時間平滑化する時定数 [秒]。0 は平滑化なし。
    /// @note 全 Motion を同じ正規化位相で評価し、Weight が再上昇した Clip の位相ジャンプを防ぐ。歩きと
    /// @note 走りなど長さの違う輪を重ねるとブレンド中に位相がずれて足が滑るため、方向だけでなく速さの
    /// @note 輪を足す構成では必須になる。
    bool                         syncNormalizedTime = false; ///< 全モーションを共通の正規化位相で評価するかどうか。
    std::vector<BlendTreeMotion> motions; ///< 二次元補間の座標ごとに設定したクリップの候補。

    /// @name ランタイム専用 (Controller/Scene には保存しない)
    /// @{
    float                        dampedX = 0.0f; ///< 時間平滑化を適用した X パラメーターの現在値。
    float                        dampedY = 0.0f; ///< 時間平滑化を適用した Y パラメーターの現在値。
    bool                         dampedValueInitialized = false; ///< 2D の平滑化に使う前回の XY 値を初期化済みかどうか。
    float                        normalizedPhase = 0.0f; ///< 位相同期が有効なときに全クリップへ渡す共通の正規化再生位置。
    bool                         normalizedPhaseInitialized = false; ///< 2D の共通位相を現在の再生状態に初期化したかどうか。
    /// @}
};


/// @brief ステートマシンの1状態。アニメーションクリップと遷移リストを持つ。
/// @note clipName が animator.clips 内の名前と一致しない場合は clipIndex で直接指定する。
/// @note       clipIndex = -1 のときは clipName のみで探す。
struct AnimationState {
    std::string                      name; ///< 遷移元や遷移先からこの状態を識別する名前。
    AnimationStateMode               mode = AnimationStateMode::Clip; ///< 単一クリップまたはブレンドツリーのどれで姿勢を評価するか。
    std::string                      sourcePath; ///< Clip State が直接参照するアニメーションソース。BlendTree は Motion ごとに保持する
    std::string                      clipName; ///< 単一クリップモードで参照するクリップ名。
    int                              clipIndex = -1; ///< クリップ名で見つからない場合の Animator クリップ添字。-1 は名前指定だけを使う。
    float                            speed    = 1.0f; ///< このステートで経過時間に掛ける再生速度倍率。
    bool                             loop     = true; ///< ステートのモーションを末尾から先頭へ戻して再生するかどうか。
    /// @note このステートでの IK ブレンド量。0=FK のみ、1=フル IK。ステートごとに影響度を制御し、
    /// @note Run や JumpUp で地面スナップを段階的に抑える。
    float                            ikWeight = 1.0f; ///< このステートで解いた IK を混ぜる割合。0 は FK のみ、1 は全適用。
    std::vector<AnimationTransition> transitions; ///< このステートを起点として評価する遷移候補。
    BlendTree1D                      blendTree1D; ///< 1D ブレンドツリーで姿勢を求める場合の設定と実行時状態。
    BlendTree2D                      blendTree2D; ///< 2D ブレンドツリーで姿勢を求める場合の設定と実行時状態。
};








/// @brief .mask アセットへの参照と、そのロード済みキャッシュ。
/// @note Base Layer と各 AnimationLayer が同じ形を共有する。別フィールドで二重に持つと
/// @note       ロード処理・差し替え処理も二重化するため、この構造体へ集約する。
struct AnimationMaskRef {
    std::string path; ///< .mask アセットのパス。空ならマスクなし (全ボーンに効く)

    /// @name ランタイム専用 (Controller/Scene には保存しない)
    /// @{
    std::string            loadedPath; ///< このキャッシュの状態を最後に更新したアセット参照パス。
    bool                   loaded = false; ///< 現在のパスに対応するマスクをロード済みかどうか。
    /// @note loadedPath のロードが失敗した。再試行とログの抑止に使う。loaded だけでは失敗のたびに
    /// @note 毎フレーム TOML を開き直し、警告も毎フレーム出てしまう。
    bool                   failed = false; ///< loadedPath の読み込みに失敗し、同じ参照の再試行を抑えるかどうか。
    asset::AvatarMaskAsset asset; ///< パスから読み込んだ適用対象ボーンと重みの値。
    /// @}

    /// @brief path を書き換えたあと、次フレームに読み直させる。
    void Invalidate()
    {
        loaded = false;
        failed = false;
        loadedPath.clear();
    }
};

/// @brief ステートマシンのランタイム状態。Base Layer と追加 Layer が同じ形を共有する。
/// @note AnimatorComponent 直下のフィールドは既存 Script/Editor/MCP の参照を壊さないため
/// @note       Base Layer 用として残し、追加レイヤーはこの構造体を各自1つ持つ。
struct AnimatorStateMachineRuntime {
    std::string currentStateName; ///< 現在の再生元ステートの名前。
    float       stateTime     = 0.0f; ///< 現在の再生元ステート内で進んだ時間 [秒]。
    std::string blendToState; ///< クロスフェード先のステート名。空なら遷移中ではない。
    float       blendToTime   = 0.0f; ///< クロスフェード先ステート内で進んだ時間 [秒]。
    float       blendWeight   = 0.0f; ///< クロスフェード先を合成する割合。0 は再生元、1 は遷移先。
    float       blendDuration = 0.25f; ///< 現在のクロスフェードの所要時間 [秒]。
};


/// @brief レイヤーへ一時的に差し込むワンショット再生 (Unreal の Montage/Slot 相当)。
/// @note ステートマシンに専用ステートと復帰遷移を足さずに、スクリプトから1行で
/// @note       差し込み・復帰を済ませるための機構。
struct AnimationSlotPlayback {
    std::string sourcePath; ///< スロットに差し込むアニメーションの参照パス。
    std::string clipName; ///< 参照ソース内のスロット再生対象のクリップ名。
    float speed           = 1.0f; ///< スロットのクリップ内時刻の進行倍率。
    bool  loop            = false; ///< スロットのクリップを末尾から先頭へ戻して再生するかどうか。
    float fadeInDuration  = 0.15f; ///< スロットを通常のレイヤー出力に混ぜ始めるときのフェード時間 [秒]。
    float fadeOutDuration = 0.15f; ///< スロットから通常のレイヤー出力へ戻すときのフェード時間 [秒]。

    /// @name ランタイム専用 (Controller/Scene には保存しない)
    /// @{
    bool  active   = false; ///< 現在スロットのモーションをレイヤー出力へ混ぜているかどうか。
    bool  stopping = false; ///< フェードアウト中
    float time     = 0.0f; ///< クリップ内の再生秒数
    float weight   = 0.0f; ///< 0..1。ステートマシン出力に対するこの Slot の被せ量
    bool  driven   = false; ///< SequenceSystem が時刻を握っている間 true。AnimatorSystem は自前で time を進めない
    /// @}
};

struct AnimationLayer {
    std::string name = "Layer"; ///< スロットやステート制御 API がこの追加レイヤーを特定する名前。
    float weight = 1.0f; ///< 既に合成したレイヤーへこのレイヤーを重ねる割合。
    AnimationLayerMode mode = AnimationLayerMode::Override; ///< 姿勢を置き換えるか、基準姿勢からの差分を加算するか。
    bool enabled = true; ///< この追加レイヤーを姿勢評価と合成の対象にするかどうか。

    AnimationMaskRef mask; ///< マスク: .mask アセットへの参照。空でなければこちらを優先する。

    AdditiveReferencePose additiveReference; ///< 加算レイヤーの基準ポーズ設定。

    /// @name レイヤー独自ステートマシン
    /// @{
    std::string defaultStateName; ///< この追加レイヤーを評価し始めるときの初期ステート名。
    std::vector<AnimationState> states; ///< この追加レイヤーが独自に評価するステートの一覧。
    std::vector<AnimationTransition> anyStateTransitions; ///< このレイヤーの現在ステートを問わず評価する割り込み遷移。
    /// @}

    std::vector<RetargetBoneMapping> retargetMappings; ///< このレイヤーのモーションのボーンを対象リグへ対応付ける設定。

    AnimationSlotPlayback slot; ///< ワンショット再生スロット。

    /// @note ランタイム専用。Controller/Scene には保存しない。
    AnimatorStateMachineRuntime runtime; ///< states を使うときのステートマシン状態
};

/// @}

/// @name ルートモーション
/// @{

/// @note 単一の適用可否フラグでは「抽出のみ」「RigidBody 速度として消費」「ポーズへ残したまま
/// @note       抽出停止」を表現できないため、適用先 (Mode) / 解決方法 (Source) / 軸マスクを直交分解する。

/// @brief 抽出したルートモーションを誰が消費するか。
enum class RootMotionMode : int {
    None             = 0, ///< @note 抽出しない。poseMode に従いクリップのルート移動を残す/除去する
    ApplyToTransform = 1, ///< @note エンジンが適用先 Transform を直接動かす (従来の applyRootMotion = true)
    ExtractOnly      = 2, ///< @note Transform には触れず delta 公開と OnAnimatorMove 発火のみ行う。適用は Script/CharacterController の責任
    ApplyToRigidBody = 3, ///< @note 適用先 RigidBody の水平速度へ反映する。Transform 直書きは押し戻しのないテレポートになり壁を抜けるため、落下と衝突解決は物理側に任せる
};

/// @brief ルートモーショントラックをどう特定するか。
enum class RootMotionSource : int {
    ClipDefined = 0, ///< @note .anim が指定したトラックのみを使う (既定・従来動作)
    AutoDetect  = 1, ///< @note 候補名→スケルトンのルートノード名の順に自動解決する。旧 .anim (rootmotion 完全一致のみ) でも再インポートなしで Hips/Armature 等から抽出できる
    NodeName    = 2, ///< @note rootMotionNodeName で明示指定する
};

/// @brief クリップ側の軸フラグを Animator から上書きするための三値。
enum class RootMotionAxisOverride : int {
    UseClip  = 0, ///< @note .anim に焼かれた値を使う
    Disabled = 1, ///< @note この軸は抽出しない
    Enabled  = 2, ///< @note この軸を抽出する
};

/// @brief mode == None のときにポーズのルート移動をどう扱うか。
enum class RootMotionPoseMode : int {
    Strip = 0, ///< @note ルート移動をポーズから除去する (その場再生)
    Keep  = 1, ///< @note クリップのまま残す。旧実装は抽出オフで除去だけが残り前進成分が消滅したため、DCC そのままの見た目を選べるようにする
};

struct RootMotionSettings {
    RootMotionMode         mode        = RootMotionMode::ApplyToTransform; ///< ルートモーションを抽出するか、抽出した値をどこへ適用するか。
    RootMotionSource       source      = RootMotionSource::ClipDefined; ///< ルートモーションとして評価するトラックの解決方式。
    RootMotionPoseMode     poseMode    = RootMotionPoseMode::Strip; ///< 抽出しない場合に、ポーズのルート移動を残すか除去するか。
    std::string            nodeName; ///< source == NodeName のときに参照するトラック名。正規化一致で解決する
    /// @note 適用先 GameObject への相対パス。空なら Animator 自身。".." を先頭に並べると親を遡れる
    /// @note (例: Animator が子メッシュ、RigidBody が親のとき "..")。
    std::string            targetPath; ///< ルートモーションを適用するエンティティへの相対パス。空なら Animator 自身。
    RootMotionAxisOverride applyXZ       = RootMotionAxisOverride::UseClip; ///< XZ 平面の移動を抽出するかについて、クリップの設定を上書きする。
    RootMotionAxisOverride applyY        = RootMotionAxisOverride::UseClip; ///< Y 方向の移動を抽出するかについて、クリップの設定を上書きする。
    RootMotionAxisOverride applyRotation = RootMotionAxisOverride::UseClip; ///< 回転を抽出するかについて、クリップの設定を上書きする。
    float                  positionScale = 1.0f; ///< 抽出した移動量へ掛ける倍率。アニメの歩幅とゲームの移動速度を合わせる
    float                  rotationScale = 1.0f; ///< 抽出した回転量へ掛ける倍率

    /// @brief 抽出そのものを行うか。
    /// @note None 以外なら常に抽出し、ポーズからのルート成分除去は強制になる (残すと Transform
    /// @note       とポーズで二重に進む)。軸ごとの確定は AnimatorSystem::ResolveRootMotion が行う。
    bool Extracts() const { return mode != RootMotionMode::None; }
};

/// @brief AnimatorSystem がクリップごとに保持する前フレームのサンプル。
/// @note 支配クリップ1本の前後差分では BlendTree 切替や遷移のたびに移動が飛んだため、
/// @note       クリップ単位で前回サンプル位置を持ち、delta を連続させる。
struct RootMotionClipSample {
    const asset::AnimationClip* clip = nullptr; ///< 同一性キー。LoadClips でクリップ配列を作り直すとキャッシュごと破棄する
    double           ticks    = 0.0; ///< 前回サンプルした tick
    math::Vector3    position = math::Vector3::ZERO; ///< 前回サンプルしたルートモーショントラックの位置 [m]。
    math::Quaternion rotation = math::Quaternion::Identity(); ///< 前回サンプルしたルートモーショントラックの姿勢。
    std::uint64_t    frame    = 0; ///< 前回サンプルしたフレーム番号
};

/// @}

/// @brief 存在しないレイヤー名で命令系 API を叩いたことを報告する。同じ (API, 名前) の組は最初の 1 回だけ出す。
/// @note PlaySlot/StopSlot/PlayLayerState/SetLayerWeight は名前不一致だと無言で戻るため、原因切り分けにこの報告が要る。
/// @note 定義は src/Scene/Components/AnimatorComponent.cpp に置く。Logger 依存を広く include されるこのヘッダーへ持ち込まないため。
/// @note 一致に失敗したときしか呼ばれない。成功経路には何も足さない。
FBZZ_ENGINE_API void ReportUnknownAnimationLayer(const char* api,
                                 std::string_view layerName,
                                 const std::vector<AnimationLayer>& available);

/// @brief アニメーション再生・ステートマシン・ルートモーションを統括する主コンポーネント。
/// @note GPU 所有権と再確保用キャッシュは Component の値コピーへ含めない。
struct AnimatorComponentGpuState {
    renderer::OwnedResource<renderer::ConstantBufferTag> skinningBuffer; ///< 現フレームのスキニング行列をシェーダーへ渡す定数バッファの所有ハンドル。
    renderer::OwnedResource<renderer::ConstantBufferTag> prevSkinningBuffer; ///< 速度を求めるために、前フレームのスキニング行列を渡す定数バッファの所有ハンドル。
};

struct AnimatorComponent : TransientGpuState<AnimatorComponentGpuState> {
    /// @name 後方互換フィールド (states が空のとき使用)
    /// @{
    bool        enabled    = true; ///< この Animator の時刻と姿勢の評価を行うかどうか。
    std::string controllerPath; ///< 共有 Animator Controller アセットへの参照。空なら Component 内蔵定義を使う
    std::string loadedControllerPath; ///< ランタイム専用。参照変更時だけ Controller を再読み込みする
    float       speed      = 1.0f; ///< この Animator のアニメーション再生速度の倍率。
    float       localTimeScale = 1.0f; ///< speed による凍結と独立した時計倍率。ランタイム専用。
    bool        playing    = true; ///< 再生時刻を進めるかどうか。停止しても姿勢評価は有効にできる。
    /// @brief クリップの代わりに Script が骨のローカルを書く構成であることを示す。
    /// @note オフだと «評価できるクリップが無い» フレームで骨がバインドポーズへ戻り、Script の
    /// @note       姿勢が毎フレーム消える。かといって Animator を外すとスキニングパレットを
    /// @note       埋める者がいなくなる (Skinning/GeometryPass は親を辿って Animator を探す)。
    /// @note enabled=false は Animator の評価自体を止めパレットが作られない。playing は
    /// @note       再生位置を止めるだけでバインドポーズへ戻す経路は残る。externalPose はどちらとも別軸。
    bool        externalPose = false; ///< スクリプトなどの外部処理から骨のローカル姿勢を与えるかどうか。
    bool poseLodEnabled = false; ///< 任意で姿勢サンプリングの LOD を使う。状態時間とイベントは毎フレーム更新し、距離はワールド空間の [m]、間隔は [s] で指定する。
    int boneBakeMode = static_cast<int>(BoneBakeMode::LEGACY); ///< 0 は通常の Animator、1 は CPU のベイク済みサンプリング、2 は GPU 共有再生。既定値は従来の FBX 再生を維持する。
    float boneBakeSampleRate = 60.0f; ///< ベイク済みボーンアニメーションを生成するサンプルの頻度 [Hz]。
    float boneBakeMinDistance = 20.0f; ///< カメラからこの距離 [m] 以上であればベイク姿勢を使える。
    bool boneBakeRequireCpuBones = true; ///< CPU でボーンやソケットを参照する処理がなければ false にできる。カメラ付近とリグ制御下では通常の順運動学を維持する。
    BoneBakeRuntime boneBake; ///< 実行時のみ。Velocity、専用シェーダーと GPU 再生の失敗に備え、現在と前回の CPU パレットを保持する。
    float poseLodNearDistance = 20.0f; ///< 姿勢を毎フレーム評価する近距離領域の外端までのカメラ距離 [m]。
    float poseLodFarDistance = 60.0f; ///< 遠距離用の評価間隔へ切り替わるカメラ距離 [m]。
    float poseLodMidInterval = 1.0f / 30.0f; ///< 中距離の姿勢サンプリングの間隔 [秒]。
    float poseLodFarInterval = 1.0f / 15.0f; ///< 遠距離の姿勢サンプリングの間隔 [秒]。
    float poseUpdateElapsed = 0.0f; ///< 最後の姿勢評価から蓄積した再評価判定用の時間 [秒]。
    bool poseEvaluatedThisFrame = false; ///< このフレームで新しい姿勢を評価したかどうか。
    std::uint64_t poseEvaluationCount = 0; ///< 診断用に数える姿勢評価の累計回数。
    std::string poseStateName; ///< 最後の姿勢評価時に参照したステートの名前。
    const asset::Skeleton* poseSkeleton = nullptr; ///< 最後の姿勢評価に使ったスケルトンの非所有参照。差し替えの照合に使う。
    /// @brief カメラからの距離に応じて姿勢を更新する間隔を設定する。
    /// @param[in] enabledValue 距離による姿勢更新 LOD を有効にする場合は true。
    /// @param[in] nearDistance 毎フレーム更新する範囲の上限 [ワールド m]。
    /// @param[in] farDistance 中距離の更新間隔を使う範囲の上限 [ワールド m]。
    /// @param[in] midInterval 中距離の姿勢更新間隔 [s]。
    /// @param[in] farInterval 遠距離の姿勢更新間隔 [s]。
    /// @return 距離や間隔が非有限、逆順、負数の場合、または遠距離の間隔が 1 秒を超える場合は false を返し設定を維持する。
    bool SetUpdateLod(bool enabledValue, float nearDistance, float farDistance, float midInterval, float farInterval)
    {
        // 距離と間隔が有限であることを確認した後、それぞれの値域と順序を検証する。
        if (!std::isfinite(nearDistance) || !std::isfinite(farDistance) || !std::isfinite(midInterval)
            || !std::isfinite(farInterval)) {
            return false;
        }
        if (nearDistance < 0 || farDistance < nearDistance) {
            return false;
        }
        if (midInterval < 0 || farInterval < midInterval || farInterval > 1.0f) {
            return false;
        }

        poseLodEnabled = enabledValue;
        poseLodNearDistance = nearDistance;
        poseLodFarDistance = farDistance;
        poseLodMidInterval = midInterval;
        poseLodFarInterval = farInterval;
        poseUpdateElapsed = farInterval;
        return true;
    }
    /// @note ルートモーションの適用先・解決方法・軸マスク。VFX の決定論的 Preview は mode = None を
    /// @note 使い、姿勢だけを評価して Transform を動かさない。
    RootMotionSettings rootMotion; ///< ルートモーションの抽出元、適用先、抽出する軸の設定。

    /// @note ステートから参照されないクリップ (演出専用) のソース。LoadClips はステートと Additive
    /// @note 基準からしか集めないため、ここに宣言が無いと Slot へ差し込んでも参照が解決できず黙って畳まれる。
    std::vector<std::string> externalClipSources; ///< ステートから参照しない、スロットや演出専用クリップのソース。

    std::vector<asset::AnimationClip> clips; ///< ランタイム専用。初回更新時にステート参照から再構築する。
    /// @note 不変キーは AssetManager が所有し、リロードやアンロード後は世代付きハンドルを解決し直す。
    struct ClipReference {
        asset::AssetHandle<asset::AnimationClip> clip; ///< 独立したアニメーションクリップを解決する世代付きアセットハンドル。
        asset::AssetHandle<asset::Model> model; ///< モデル内のクリップを使う場合に所有モデルを解決する世代付きアセットハンドル。
        std::uint32_t modelClipIndex = 0; ///< model が指すモデル内のアニメーションクリップの添字。
    };
    std::vector<ClipReference> sharedClips; ///< 独立したアセットまたはモデルが所有するクリップへの参照一覧。
    [[nodiscard]] std::size_t ClipCount() const { return clips.empty() ? sharedClips.size() : clips.size(); }
    /// @return 無効なハンドルは空のクリップを返す。手動で渡したクリップは値の配列として扱う。
    [[nodiscard]] FBZZ_ENGINE_API const asset::AnimationClip& ClipAt(std::size_t index) const;
    struct ClipRange {
        const AnimatorComponent* owner; ///< この範囲がクリップを列挙する Animator の非所有参照。
        struct Iterator {
            const AnimatorComponent* owner; ///< 反復中にクリップを解決する Animator の非所有参照。
            std::size_t index; ///< 現在のクリップを ClipAt で取得するための添字。
            const asset::AnimationClip& operator*() const { return owner->ClipAt(index); }
            Iterator& operator++() { ++index; return *this; }
            bool operator==(const Iterator&) const = default;
        };
        Iterator begin() const { return {owner, 0}; }
        Iterator end() const { return {owner, owner->ClipCount()}; }
    };
    [[nodiscard]] ClipRange Clips() const { return {this}; }
    std::vector<std::string> clipSourcePaths; ///< clips と同じ添字でロード元を保持し、同名クリップを Source Path で識別する。
    bool clipsLoaded          = false; ///< 現在の設定からクリップ参照を構築済みかどうか。
    int  clipsAttemptGeneration = -1; ///< 最後に LoadClips を試行したときの FlushGeneration。
    /// @note 最後に Controller/クリップ/マスクを取り込んだときのアセット世代。AssetManager の現在値と
    /// @note 食い違う間は派生キャッシュが古い。
    int  appliedAssetGeneration = -1; ///< クリップなどを最後に取り込んだ AssetManager の世代。-1 は未適用。

    /// @brief スキニング後の骨が実際にいる位置 (owner のローカル空間)。
    /// @note カリング球はバインドポーズの submesh バウンズをそのまま運んでいたため、Script が
    /// @note       骨を大きく動かす構成では球と実体がずれ、部位ごとに独立して視錐台外/遮蔽物中へ消える。
    /// @note 半径 0 は「未計算」を意味する。スケルトン未所持や停止中は骨姿勢が確定しないため、
    /// @note       カリング側はこの場合バインドポーズ球へフォールバックする。
    math::Vector3 skinnedBoundsCenter{}; ///< 確定した骨を包むカリング球の中心。オーナーのローカル座標 [m]。
    float         skinnedBoundsRadius = 0.0f; ///< 確定した骨姿勢を囲むカリング球の半径 [m]。0 は未計算。

    std::vector<math::Matrix4> boneMatrices; ///< スキニングで頂点を変形するための、ボーン番号ごとの確定行列。
    std::vector<math::Matrix4> nodeGlobalTransforms; ///< ノード番号ごとの、親階層を積み上げたオーナーのローカル空間での姿勢行列。

    /// @brief 前フレームの確定ボーンパレット。VelocityPass が VS で前フレーム頂点位置を組み直す
    /// @note        際に使う (b2 = CB_PREV_SKINNING へ束縛)。
    /// @note コンピュートスキニング出力は GPU 書き込み頂点バッファで VS の SRV として読めず、
    /// @note       DrawCall::vsBuffers も StructuredBufferTag しか受けないため、パレット2本を VS へ渡し
    /// @note       2回スキニングする形にしている。
    /// @note boneMatrices の最終書き込み者は SpringBoneSystem。AnimatorSystem が上書きする直前に
    /// @note       スナップショットを取ることで「前フレームの最終ポーズ」になる (フレーム末ではなく次フレーム頭)。
    std::vector<math::Matrix4> prevBoneMatrices; ///< 速度計算に使用する前フレームの確定ボーンパレット。
    bool prevBoneMatricesValid = false; ///< prevBoneMatrices が有効か。初回フレームは前フレームが存在しないため速度 0 にする。
    /// @}

    /// @name ステートマシン定義 (シリアライズ対象)
    /// @{
    std::string                    defaultStateName; ///< 初期ステート名。空なら states[0]
    std::vector<AnimationState>    states; ///< ベースレイヤーが評価するステートの一覧。
    std::vector<AnimationTransition> anyStateTransitions; ///< 現在ステートを問わず評価する割り込み遷移。通常遷移より後に評価する。
    std::vector<AnimatorParameter> parameters; ///< 遷移条件とブレンドツリーで評価する名前付きの入力値。
    std::vector<AnimationLayer> layers; ///< index 0 の Base Layer は既存 state machine が担い、この配列は追加 Layer のみを保持する。
    /// @note Base Layer 自身のマスク。空なら全身に効く (従来動作)。マスク外のボーンはバインドポーズの
    /// @note まま残り、上のレイヤーだけが動かす形になる。
    AnimationMaskRef baseLayerMask; ///< ベースレイヤーの姿勢を反映するボーンのマスク。空なら全身。
    std::vector<std::pair<std::string, float>> currentBlendWeights; ///< 現在の BlendTree 評価結果。Script/Editor のデバッグ表示に使用する。
    float currentBlendDuration = 0.0f; ///< 現在のブレンド結果の実効クリップ長 [秒]。
    float currentIKWeight = 1.0f; ///< 現在の姿勢にブレンドした IK の影響度。0 は FK のみ、1 は全適用。
    /// @}

    /// @name ランタイム専用 (シリアライズしない)
    /// @{
    /// @note 現在再生中のステート名。
    std::string currentStateName; ///< ベースレイヤーで現在再生しているステートの名前。
    float       stateTime     = 0.0f; ///< 現ステートの再生秒数

    std::string blendToState; ///< 遷移中クロスフェード管理。blendToState が空でなければ遷移中。
    float       blendToTime   = 0.0f; ///< 遷移先ステートの再生秒数
    float       blendWeight   = 0.0f; ///< 0=現ステート, 1=遷移先
    float       blendDuration = 0.25f; ///< 今回の遷移のクロスフェード時間キャッシュ
    /// @note 割り込み時のベース姿勢。IK や追加レイヤー適用前のローカル TRS を保持する。
    struct InterruptedPose {
        const asset::Skeleton* skeleton = nullptr; ///< 割り込み前のポーズの保存に用いたスケルトンの非所有参照。
        std::vector<AnimatorNodeLocalPose> nodes; ///< 割り込みで保存したノードごとのローカル姿勢。IK と追加レイヤーの適用前。
        std::vector<std::pair<std::string, float>> clipWeights; ///< 割り込みの瞬間のクリップ名とブレンド重みの組。
        float ikWeight = 1.0f; ///< 割り込みで保存したポーズに対応する IK の影響度。
    } interruptedPose;
    float       previousEventTime = 0.0f; ///< Animation Event の区間評価に使う前フレーム時刻。
    std::string previousEventClipName; ///< 時刻の差分からイベントを探すために保持する前回のクリップ名。
    /// @}

    /// @name Root Motion 出力 (ランタイム専用)
    /// @{
    /// @note 適用先 GameObject のローカル空間での1フレーム移動量/回転量。
    math::Vector3    rootMotionDeltaPosition = math::Vector3::ZERO; ///< 適用先のローカル空間での 1 フレーム分のルート移動量 [m]。
    math::Quaternion rootMotionDeltaRotation = math::Quaternion::Identity(); ///< 1 フレームで抽出した、適用先のローカル空間での回転差分。
    /// @note deltaPosition をワールド空間へ変換した値と、それを dt で割った速度。Script 側で
    /// @note Time::deltaTime を掛け直すと Animator が実際に進めた時間とずれるため、速度制御へ
    /// @note そのまま渡せる形で公開する。
    math::Vector3    rootMotionWorldDelta    = math::Vector3::ZERO; ///< 1 フレームのルート移動量をワールド空間へ変換した変位 [m]。
    math::Vector3    rootMotionWorldVelocity = math::Vector3::ZERO; ///< ワールド空間へ変換したルート移動量を実際の更新時間で割った速度 [m/s]。
    float            rootMotionDeltaTime     = 0.0f; ///< ルート移動量と速度の換算に使ったアニメーション更新時間 [秒]。
    bool             rootMotionAppliedByEngine = false; ///< エンジンが Transform/RigidBody へ適用したフレームは true。ExtractOnly では false。
    std::vector<RootMotionClipSample> rootMotionSamples; ///< クリップごとの前フレームサンプル。BlendTree/遷移をまたいでも delta を連続させる。
    /// @note 当該フレームの Skeleton ルートノード名。AutoDetect のフォールバック解決に使う。トラック
    /// @note 解決は Skeleton を持たない BuildStateClips からも呼ばれるため、AnimatorSystem::Update が
    /// @note 毎フレーム先頭でここへ焼いておく。
    std::string skeletonRootNodeName; ///< 現在のスケルトンのルートノード名。トラックの自動解決に使う。
    /// @}

    std::vector<FiredAnimationEvent> firedEvents; ///< このアニメーション更新で発火したイベントの一覧。購読処理へ渡す。

    const char* GetTypeName() const { return "Animator"; }

    /// @note states/parameters は vector のため SceneSerializer が直接変換する。
    void Reflect(IReflector& r)
    {
        r.Field("enabled",          enabled);
        r.Field("controllerPath",   controllerPath);
        r.Field("speed",            speed);
        r.Field("playing",          playing);
        r.Field("externalPose",     externalPose);
        r.Field("poseLodEnabled", poseLodEnabled);
        r.Field("boneBakeMode", boneBakeMode);
        r.Field("boneBakeSampleRate", boneBakeSampleRate);
        r.Field("boneBakeMinDistance", boneBakeMinDistance);
        r.Field("boneBakeRequireCpuBones", boneBakeRequireCpuBones);
        r.Field("poseLodNearDistance", poseLodNearDistance);
        r.Field("poseLodFarDistance", poseLodFarDistance);
        r.Field("poseLodMidInterval", poseLodMidInterval);
        r.Field("poseLodFarInterval", poseLodFarInterval);
        int mode = static_cast<int>(rootMotion.mode);
        int source = static_cast<int>(rootMotion.source);
        int poseMode = static_cast<int>(rootMotion.poseMode);
        r.Field("rootMotionMode", mode);
        r.Field("rootMotionSource", source);
        r.Field("rootMotionPoseMode", poseMode);
        rootMotion.mode = static_cast<RootMotionMode>(mode);
        rootMotion.source = static_cast<RootMotionSource>(source);
        rootMotion.poseMode = static_cast<RootMotionPoseMode>(poseMode);
        r.Field("rootMotionNodeName", rootMotion.nodeName);
        r.Field("rootMotionTarget",   rootMotion.targetPath);
        int applyXZ = static_cast<int>(rootMotion.applyXZ);
        int applyY = static_cast<int>(rootMotion.applyY);
        int applyRotation = static_cast<int>(rootMotion.applyRotation);
        r.Field("rootMotionApplyXZ", applyXZ);
        r.Field("rootMotionApplyY", applyY);
        r.Field("rootMotionApplyRotation", applyRotation);
        rootMotion.applyXZ = static_cast<RootMotionAxisOverride>(applyXZ);
        rootMotion.applyY = static_cast<RootMotionAxisOverride>(applyY);
        rootMotion.applyRotation = static_cast<RootMotionAxisOverride>(applyRotation);
        r.Field("rootMotionPositionScale", rootMotion.positionScale);
        r.Field("rootMotionRotationScale", rootMotion.rotationScale);
        r.Field("defaultStateName", defaultStateName);
    }

    /// @name スクリプト向け API
    /// @{

    /// @note AnimatorSystem が毎フレーム更新するため、Set* はフィールド書き換えのみで十分。

    void SetFloat(std::string_view name, float v)
    {
        for (auto& p : parameters)
            if (p.name == name && p.type == ParamType::Float)
                { p.floatValue = v; return; }
    }

    void SetInt(std::string_view name, int v)
    {
        for (auto& p : parameters)
            if (p.name == name && p.type == ParamType::Int)
                { p.intValue = v; return; }
    }

    void SetBool(std::string_view name, bool v)
    {
        for (auto& p : parameters)
            if (p.name == name && p.type == ParamType::Bool)
                { p.boolValue = v; return; }
    }

    /// @brief Trigger は true にセットする。消費は AnimatorSystem::ConsumeTriggers() が行う。
    void SetTrigger(std::string_view name)
    {
        for (auto& p : parameters)
            if (p.name == name && p.type == ParamType::Trigger)
                { p.boolValue = true; return; }
    }

    [[nodiscard]] float GetFloat(std::string_view name) const
    {
        for (const auto& p : parameters)
            if (p.name == name && p.type == ParamType::Float) {
                return p.floatValue;
            }
        return 0.0f;
    }

    [[nodiscard]] int GetInt(std::string_view name) const
    {
        for (const auto& p : parameters)
            if (p.name == name && p.type == ParamType::Int) {
                return p.intValue;
            }
        return 0;
    }

    [[nodiscard]] bool GetBool(std::string_view name) const
    {
        for (const auto& p : parameters)
            if (p.name == name &&
                (p.type == ParamType::Bool || p.type == ParamType::Trigger)) {
                return p.boolValue;
            }
        return false;
    }

    /// @brief 現在ステートの名前を確認する。ステートマシン未使用時は常に false。
    [[nodiscard]] bool IsInState(std::string_view stateName) const
    {
        return currentStateName == stateName;
    }

    /// @}

    /// @name レイヤー API
    /// @{

    /// @note 上半身/下半身の出し分けはレイヤー操作が入口になる。名前引きの重複実装を避けるためここに集約する。

    [[nodiscard]] AnimationLayer* FindLayer(std::string_view layerName)
    {
        for (auto& l : layers)
            if (l.name == layerName) {
                return &l;
            }
        return nullptr;
    }

    [[nodiscard]] const AnimationLayer* FindLayer(std::string_view layerName) const
    {
        for (const auto& l : layers)
            if (l.name == layerName) {
                return &l;
            }
        return nullptr;
    }

    void SetLayerWeight(std::string_view layerName, float w)
    {
        AnimationLayer* l = FindLayer(layerName);
        if (!l) { ReportUnknownAnimationLayer("SetLayerWeight", layerName, layers); return; }
        l->weight = std::clamp(w, 0.0f, MAX_LAYER_WEIGHT);
    }

    [[nodiscard]] float GetLayerWeight(std::string_view layerName) const
    {
        const AnimationLayer* l = FindLayer(layerName);
        return l ? l->weight : 0.0f;
    }

    /// @brief レイヤーの現在ステート名。
    [[nodiscard]] std::string GetLayerState(std::string_view layerName) const
    {
        const AnimationLayer* l = FindLayer(layerName);
        if (!l) {
            return {};
        }
        return l->runtime.currentStateName;
    }

    /// @brief 指定 Layer の現在ステートを、そのステートの実クリップ長で 0..1 に正規化する。
    /// @note 武器の親差し替えなど、速度・遷移時間の変更に追従するには実クリップ再生位置への
    /// @note       同期が必要。Base Layer 用 GetNormalizedTime と同じ規則を Layer にも公開する。
    [[nodiscard]] float GetLayerNormalizedTime(std::string_view layerName) const
    {
        const AnimationLayer* layer = FindLayer(layerName);
        if (!layer || layer->runtime.currentStateName.empty()) {
            return 0.0f;
        }

        const AnimationState* state = nullptr;
        for (const auto& candidate : layer->states) {
            if (candidate.name == layer->runtime.currentStateName) {
                state = &candidate;
                break;
            }
        }
        if (!state) {
            return 0.0f;
        }
        if (state->mode != AnimationStateMode::Clip) {
            return layer->runtime.blendDuration > 0.0f
                ? std::clamp(layer->runtime.stateTime / layer->runtime.blendDuration, 0.0f, 1.0f)
                : 0.0f;
        }

        const asset::AnimationClip* clip = nullptr;
        if (!state->sourcePath.empty()) {
            for (size_t i = 0; i < ClipCount(); ++i) {
                if (i >= clipSourcePaths.size() || clipSourcePaths[i] != state->sourcePath) {
                    continue;
                }
                if (!clip) {
                    clip = &ClipAt(i);
                }
                if (!state->clipName.empty() && ClipAt(i).name == state->clipName) {
                    clip = &ClipAt(i);
                    break;
                }
            }
        }
        if (!clip && !state->clipName.empty()) {
            for (const auto& candidate : Clips())
                if (candidate.name == state->clipName) {
                    clip = &candidate;
                    break;
                }
        }
        if (!clip && state->clipIndex >= 0 &&
            state->clipIndex < static_cast<int>(ClipCount())) {
            clip = &ClipAt(static_cast<size_t>(state->clipIndex));
        }
        if (!clip) {
            return 0.0f;
        }

        const float duration = static_cast<float>(clip->GetDurationSeconds());
        return duration > 0.0f
            ? std::clamp(layer->runtime.stateTime / duration, 0.0f, 1.0f)
            : 0.0f;
    }

    [[nodiscard]] bool IsLayerInState(std::string_view layerName, std::string_view stateName) const
    {
        return GetLayerState(layerName) == stateName;
    }

    /// @brief レイヤーのステートマシンを指定ステートへ即座に飛ばす (クロスフェードなし)。
    void PlayLayerState(std::string_view layerName, std::string_view stateName)
    {
        AnimationLayer* l = FindLayer(layerName);
        if (!l) { ReportUnknownAnimationLayer("PlayLayerState", layerName, layers); return; }
        // Controller 初回ロード前でも要求をランタイム状態へ保持する。Script は
        // AnimatorSystem より先に実行されるため、states がまだ空だと無言で消えうる。
        // ApplyAnimatorControllerAsset が同名 Layer の runtime を引き継ぎ、ロード後に実行する。
        if (stateName.empty()) {
            return;
        }
        l->runtime.currentStateName = std::string(stateName);
        l->runtime.stateTime   = 0.0f;
        l->runtime.blendToState.clear();
        l->runtime.blendWeight = 0.0f;
    }

    /// @}

    /// @name スロット操作
    /// @{

    /// @brief 指定レイヤーへワンショットのクリップを差し込む。既に再生中なら差し替える。
    /// @note 「上半身だけ攻撃を割り込ませて終わったら元に戻す」を1呼び出しで済ませるための API。
    /// @note weight は 0 から立ち上げない。差し替え時は現在の被せ量から続けるため保持する。
    void PlaySlot(std::string_view layerName,
                  std::string_view sourcePath,
                  std::string_view clipName,
                  float fadeIn  = 0.15f,
                  float fadeOut = 0.15f,
                  float slotSpeed = 1.0f,
                  bool  slotLoop = false)
    {
        AnimationLayer* l = FindLayer(layerName);
        if (!l) { ReportUnknownAnimationLayer("PlaySlot", layerName, layers); return; }
        l->slot.sourcePath      = std::string(sourcePath);
        l->slot.clipName        = std::string(clipName);
        l->slot.fadeInDuration  = (std::max)(fadeIn, 0.0f);
        l->slot.fadeOutDuration = (std::max)(fadeOut, 0.0f);
        l->slot.speed           = slotSpeed;
        l->slot.loop            = slotLoop;
        l->slot.time            = 0.0f;
        l->slot.stopping        = false;
        l->slot.active          = true;
    }

    /// @brief Slot をフェードアウトさせる。
    /// @param[in] fadeOut 負値なら登録済みの値を使う。
    void StopSlot(std::string_view layerName, float fadeOut = -1.0f)
    {
        AnimationLayer* l = FindLayer(layerName);
        if (!l) { ReportUnknownAnimationLayer("StopSlot", layerName, layers); return; }
        // 鳴っていない Slot を止めるのは通常の使い方であり、報告対象の異常ではない。
        if (!l->slot.active) {
            return;
        }
        if (fadeOut >= 0.0f) {
            l->slot.fadeOutDuration = fadeOut;
        }
        l->slot.stopping = true;
    }

    [[nodiscard]] bool IsSlotPlaying(std::string_view layerName) const
    {
        const AnimationLayer* l = FindLayer(layerName);
        return l && l->slot.active && !l->slot.stopping;
    }

    /// @brief Slot の被せ量 0..1。フェードの進行を Script から見たいときに使う。
    [[nodiscard]] float GetSlotWeight(std::string_view layerName) const
    {
        const AnimationLayer* l = FindLayer(layerName);
        return l ? l->slot.weight : 0.0f;
    }

    /// @}

    /// @brief 現在ステート (遷移中はブレンド込み) の IK Weight を返す。
    /// @note Run/JumpUp などステートごとに IK 影響度が変わるため、クロスフェード中も滑らかに補間する。
    [[nodiscard]] float GetCurrentIKWeight() const
    {
        return currentIKWeight;
    }

    /// @brief 現在ステートの再生位置を 0..1 で返す。
    /// @return クリップ情報が取れない場合は 0。
    [[nodiscard]] float GetNormalizedTime() const
    {
        return NormalizedTimeForState(currentStateName, stateTime);
    }

    /// @brief クロスフェード遷移先 (blendToState) の再生位置を 0..1 で返す。
    /// @return 遷移中でなければ 0。
    /// @note コンボのクロスフェード中は currentState が前段のままのため、次段の進行度確認に必要。
    [[nodiscard]] float GetBlendToNormalizedTime() const
    {
        return NormalizedTimeForState(blendToState, blendToTime);
    }

    [[nodiscard]] const std::string& GetBlendToState() const { return blendToState; }

private:
    /// @brief 指定ステート名・再生秒数から正規化時間 0..1 を求める。
    /// @note GetNormalizedTime/GetBlendToNormalizedTime が現ステート・遷移先それぞれに使う。
    [[nodiscard]] float NormalizedTimeForState(const std::string& stateName, float time) const
    {
        if (stateName.empty()) {
            return 0.0f;
        }
        const AnimationState* st = nullptr;
        for (const auto& s : states)
            if (s.name == stateName) { st = &s; break; }
        if (!st) {
            return 0.0f;
        }
        if (st->mode != AnimationStateMode::Clip) {
            return currentBlendDuration > 0.0f
                ? std::clamp(time / currentBlendDuration, 0.0f, 1.0f)
                : 0.0f;
        }
        // 再生側と同じく、名前の完全一致 → 大文字小文字無視部分一致 → index で解決する。
        const asset::AnimationClip* clip = nullptr;
        if (!st->sourcePath.empty()) {
            for (size_t i = 0; i < ClipCount(); ++i) {
                if (i >= clipSourcePaths.size() ||
                    clipSourcePaths[i] != st->sourcePath) {
                    continue;
                }
                if (!clip) {
                    clip = &ClipAt(i);
                }
                if (!st->clipName.empty() && ClipAt(i).name == st->clipName) {
                    clip = &ClipAt(i);
                    break;
                }
            }
        }
        if (!clip && !st->clipName.empty()) {
            for (const auto& c : Clips())
                if (c.name == st->clipName) { clip = &c; break; }
        }
        if (!clip && !st->clipName.empty()) {
            auto toLower = [](std::string value) {
                for (char& ch : value) {
                    ch = static_cast<char>(
                        std::tolower(static_cast<unsigned char>(ch)));
                }
                return value;
            };
            const std::string target = toLower(st->clipName);
            for (const auto& c : Clips())
                if (toLower(c.name).find(target) != std::string::npos)
                    { clip = &c; break; }
        }
        if (!clip && st->clipIndex >= 0 &&
            st->clipIndex < static_cast<int>(ClipCount())) {
            clip = &ClipAt(static_cast<size_t>(st->clipIndex));
        }
        if (!clip) {
            return 0.0f;
        }
        const float dur = static_cast<float>(clip->GetDurationSeconds());
        return dur > 0.0f ? std::clamp(time / dur, 0.0f, 1.0f) : 0.0f;
    }
};

}
