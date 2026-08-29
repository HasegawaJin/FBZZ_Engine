/// @file    AnimatorComponent.hpp
/// @brief   スケルタルアニメーション再生状態コンポーネント。
/// @author  Hasegawa Jin
/// @date    2026-05-24
///
/// Model の AnimationClip を参照し、現在時刻や再生速度を保持する。
/// 骨行列の計算と GPU 転送は AnimatorSystem が行う。
///
/// ── ステートマシン設計 ──────────────────────────────────────────────────────────
/// AnimatorSystem は State / BlendTree を統一したステートマシンとして評価する。
#pragma once

#include <Engine/Asset/AnimationClip.hpp>
#include <Engine/Asset/AvatarMaskAsset.hpp>
#include <Engine/Renderer/ResourceHandle.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/Matrix4.hpp>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fbzz::scene {

// AnimatorSystemが当該フレームに通過したEvent。VFXGraphなどScript以外の購読者も利用する。
struct FiredAnimationEvent {
    std::string name;
    int intParam = 0;
    float floatParam = 0.0f;
    float time = 0.0f;
    std::uint64_t frame = 0;
};

// ── ステートマシン用データ型 ─────────────────────────────────────────────────

// パラメーター比較演算子。
// Greater/Less/Equal/NotEqual は Float/Int パラメーターに使う。
// True/False は Bool/Trigger パラメーターに使う。
enum class ConditionOp : int {
    Greater  = 0,
    Less     = 1,
    Equal    = 2,
    NotEqual = 3,
    True     = 4,
    False    = 5,
};

// 遷移の発火条件を1つ表す。複数条件は AND で評価される。
struct AnimatorCondition {
    std::string paramName;
    ConditionOp op        = ConditionOp::True;
    float       threshold = 0.0f; // Float/Int の比較閾値。Bool/Trigger では使わない
};

// あるステートから別ステートへの遷移定義。
// conditions が空 + hasExitTime=false の場合は無効定義として遷移しない。
struct AnimationTransition {
    std::string                    toStateName;
    bool                           hasExitTime        = false;
    float                          exitTime           = 1.0f;   // 0..1 正規化再生位置
    // true は秒、false は遷移元ステート Length に対する正規化割合として解釈する。
    // WHY: 既存アセットの秒指定を維持しつつ、Unity と同様にクリップ長基準でも調整可能にする。
    bool                           fixedDuration      = true;
    float                          transitionDuration = 0.25f;
    std::vector<AnimatorCondition> conditions;
};

// BlendTree 内の1モーション。1D は threshold、2D は posX / posY を使用する。
struct BlendTreeMotion {
    float       threshold = 0.0f;
    float       posX      = 0.0f;
    float       posY      = 0.0f;
    // この Motion が直接参照するアニメーションソース。
    // WHY: Animator 全体の Clip Sources と index の組み合わせを廃止し、Node 単体で参照を完結させる。
    std::string sourcePath;
    std::string clipName;
    int         clipIndex = -1;
    float       speed     = 1.0f;
    // Motion ごとの IK 寄与率。BlendTree の姿勢 Weight と同じ比率で補間する。
    float       ikWeight  = 1.0f;
};

// Float パラメーター1本で複数モーションを補間する。
struct BlendTree1D {
    std::string                  paramName;
    // パラメーターの急変を時間補間し、Idle / Walk / Run の姿勢が瞬時に切り替わるのを防ぐ。
    // 0 は平滑化なし。値は目標へ約63%近づく時定数（秒）として扱う。
    float                        dampTime = 0.0f;
    // 全Motionを同じ正規化位相で評価し、Weightが再上昇したClipの位相ジャンプを防ぐ。
    bool                         syncNormalizedTime = false;
    std::vector<BlendTreeMotion> motions;
    // ランタイム専用。Controller / Scene には保存しない。
    float                        dampedValue = 0.0f;
    bool                         dampedValueInitialized = false;
    float                        normalizedPhase = 0.0f;
    bool                         normalizedPhaseInitialized = false;
};

// 2D BlendTree の座標解釈方式。
enum class BlendTree2DType : int {
    SimpleDirectional = 0,
    FreeformCartesian = 1
};

// Float パラメーター2本で複数モーションを2D補間する。
struct BlendTree2D {
    std::string                  paramX;
    std::string                  paramY;
    BlendTree2DType              type = BlendTree2DType::SimpleDirectional;
    // パラメーターの急変を時間補間する。1D と同じ意味・同じ時定数の扱い。
    // WHY 2D にも要るか: 移動方向は入力を離した瞬間に不連続へ飛ぶ。生値のままだと
    //     前進から後退へ切り返した 1 フレームで前後のクリップが入れ替わり、脚が跳ねる。
    float                        dampTime = 0.0f;
    // 全Motionを同じ正規化位相で評価し、Weightが再上昇したClipの位相ジャンプを防ぐ。
    // WHY 2D にも要るか: 歩きと走りのように長さの違う輪を重ねると、ブレンド中に
    //     両者の位相がずれて足が滑る。方向 4 本だけなら長さが揃うので不要だが、
    //     速さの輪を足した瞬間に 1D と同じ問題が出る。
    bool                         syncNormalizedTime = false;
    std::vector<BlendTreeMotion> motions;
    // ランタイム専用。Controller / Scene には保存しない。
    float                        dampedX = 0.0f;
    float                        dampedY = 0.0f;
    bool                         dampedValueInitialized = false;
    float                        normalizedPhase = 0.0f;
    bool                         normalizedPhaseInitialized = false;
};

// AnimationState が単一クリップと BlendTree のどれを評価するかを表す。
enum class AnimationStateMode : int {
    Clip        = 0,
    BlendTree1D = 1,
    BlendTree2D = 2
};

// ステートマシンの1状態。アニメーションクリップと遷移リストを持つ。
// clipName が animator.clips 内の名前と一致しない場合は clipIndex で直接指定する。
// clipIndex = -1 のときは clipName のみで探す。
struct AnimationState {
    std::string                      name;
    AnimationStateMode               mode = AnimationStateMode::Clip;
    // Clip State が直接参照するアニメーションソース。BlendTree は Motion ごとに保持する。
    std::string                      sourcePath;
    std::string                      clipName;
    int                              clipIndex = -1;
    float                            speed    = 1.0f;
    bool                             loop     = true;
    // このステートでの IK ブレンド量。0=FK のみ、1=フル IK。
    // WHY: ステートごとに IK 影響度を制御し、Run や JumpUp で地面スナップを段階的に抑える。
    float                            ikWeight = 1.0f;
    std::vector<AnimationTransition> transitions;
    BlendTree1D                      blendTree1D;
    BlendTree2D                      blendTree2D;
};

// パラメーター型。
// Trigger は SetTrigger() で true になり、遷移に消費されると自動で false にリセットされる。
enum class ParamType : int { Float = 0, Int = 1, Bool = 2, Trigger = 3 };

// スクリプトからステートマシンを操作するためのパラメーター。
// WHY: Unity と同様に float/int/bool/trigger を統一構造体で管理し、
//      シリアライズと Inspector UI を単純に保つ。
struct AnimatorParameter {
    std::string name;
    ParamType   type       = ParamType::Float;
    float       floatValue = 0.0f;
    int         intValue   = 0;
    bool        boolValue  = false;
};

// Base Layer の結果へ重ねる追加レイヤー。
enum class AnimationLayerMode : int { Override = 0, Additive = 1 };

/// レイヤー weight の上限。
/// Additive は 1.0 を超えるとクリップの差分をそのまま誇張する (1.0 未満は従来どおり減衰)。
/// Override は補間係数なので 1.0 より上は意味を持たず、適用時に 1.0 へ丸められる。
inline constexpr float MAX_LAYER_WEIGHT = 4.0f;

struct RetargetBoneMapping {
    std::string sourcePath;
    std::string targetPath;
    math::Quaternion rotationOffset = math::Quaternion::Identity();
    float translationScale = 1.0f;
};

// .mask アセットへの参照と、そのロード済みキャッシュ。
// WHY: Base Layer と各 AnimationLayer が同じ「マスク参照 + キャッシュ」を持つ。
//      別々のフィールド名で二重に持つと、ロード処理も差し替え処理も二重化する。
struct AnimationMaskRef {
    // .mask アセットのパス。空ならマスクなし (全ボーンに効く)。
    std::string path;
    // ── ランタイム専用。Controller / Scene には保存しない。────────────────
    std::string            loadedPath;
    bool                   loaded = false;
    asset::AvatarMaskAsset asset;

    // path を書き換えたあと、次フレームに読み直させる。
    void Invalidate()
    {
        loaded = false;
        loadedPath.clear();
    }
};

// ステートマシンのランタイム状態。Base Layer と追加 Layer が同じ形を共有する。
// WHY: レイヤーごとに独立したステートマシンを回すには、currentState / blend の一式を
//      レイヤー本数ぶん持つ必要がある。AnimatorComponent 直下のフィールドは
//      Base Layer 用として残し (既存 Script / Editor / MCP の参照を壊さないため)、
//      追加レイヤーはこの構造体を各自 1 つずつ持つ。
struct AnimatorStateMachineRuntime {
    std::string currentStateName;
    float       stateTime     = 0.0f;
    std::string blendToState;
    float       blendToTime   = 0.0f;
    float       blendWeight   = 0.0f;
    float       blendDuration = 0.25f;
};

// 加算レイヤーの基準ポーズ。加算量は「評価ポーズ - 基準ポーズ」で求める。
// WHY: 基準を加算クリップ自身の先頭フレームに固定すると、そのクリップの 1 フレーム目を
//      必ず「無変化」として作らねばならず、既存のアニメーションを加算に流用できない。
//      別クリップの任意フレームを基準に取れると、素材の制約がなくなる。
struct AdditiveReferencePose {
    // 空なら従来動作 (加算クリップ自身の先頭キー) にフォールバックする。
    std::string sourcePath;
    std::string clipName;
    float       time = 0.0f; // 基準として抜き出す秒位置
};

// レイヤーへ一時的に差し込むワンショット再生 (Unreal の Montage / Slot 相当)。
// WHY: 「移動は流したまま上半身だけ攻撃モーションを差し込み、終わったら戻す」を
//      ステートマシンに専用ステートと復帰遷移を足さずに実現する。スクリプトから
//      1 行で投げて終わり、という導線がないとコンボやリアクションの実装が重くなる。
struct AnimationSlotPlayback {
    std::string sourcePath;
    std::string clipName;
    float speed           = 1.0f;
    bool  loop            = false;
    float fadeInDuration  = 0.15f;
    float fadeOutDuration = 0.15f;
    // ランタイム専用。Controller / Scene には保存しない。
    bool  active   = false;
    bool  stopping = false;  // フェードアウト中
    float time     = 0.0f;   // クリップ内の再生秒数
    float weight   = 0.0f;   // 0..1。ステートマシン出力に対するこの Slot の被せ量
    // SequenceSystem が時刻を握っている間 true。AnimatorSystem は自前で time を進めない。
    bool  driven   = false;
};

struct AnimationLayer {
    std::string name = "Layer";
    float weight = 1.0f;
    AnimationLayerMode mode = AnimationLayerMode::Override;
    bool enabled = true;

    // ── マスク ───────────────────────────────────────────────────────────────
    // .mask アセットへの参照。空でなければこちらを優先する。
    AnimationMaskRef mask;

    // ── 加算 ─────────────────────────────────────────────────────────────────
    AdditiveReferencePose additiveReference;

    // ── レイヤー独自ステートマシン ───────────────────────────────────────────
    std::string defaultStateName;
    std::vector<AnimationState> states;
    std::vector<AnimationTransition> anyStateTransitions;

    std::vector<RetargetBoneMapping> retargetMappings;

    // ── Slot ─────────────────────────────────────────────────────────────────
    AnimationSlotPlayback slot;

    // ── ランタイム専用。Controller / Scene には保存しない。────────────────────
    AnimatorStateMachineRuntime runtime;  // states を使うときのステートマシン状態
};

// ── Root Motion ──────────────────────────────────────────────────────────────
//
// WHY: 旧実装は bool applyRootMotion 1 本だった。true なら「エンジンが owner Transform を
//      直接動かし、delta も公開する」、false なら「Transform も動かさず delta もゼロにする」
//      の 2 択しかなく、
//        - 抽出だけして移動は Script / CharacterController に任せる
//        - RigidBody の速度として消費する
//        - ポーズにルート移動を残したまま抽出を止める
//      のいずれも表現できなかった。適用先 (Mode) / 解決方法 (Source) / 軸マスクを
//      直交した設定に分解する。

// 抽出したルートモーションを誰が消費するか。
enum class RootMotionMode : int {
    // 抽出しない。poseMode に従ってクリップのルート移動を残す / 除去する。
    None             = 0,
    // エンジンが適用先 Transform を直接動かす (従来の applyRootMotion = true)。
    ApplyToTransform = 1,
    // Transform には触れず、delta の公開と OnAnimatorMove の発火だけ行う。
    // 移動の適用は Script / CharacterController の責任になる。
    ExtractOnly      = 2,
    // 適用先の RigidBody 水平速度へ反映する。落下と衝突解決は物理側に任せる。
    // WHY: Transform 直書きはスイープも押し戻しもないテレポートになり、壁を抜ける。
    ApplyToRigidBody = 3,
};

// ルートモーショントラックをどう特定するか。
enum class RootMotionSource : int {
    // .anim が指定したトラックのみを使う (既定・従来動作)。
    ClipDefined = 0,
    // 候補名 → スケルトンのルートノード名の順に自動解決する。
    // WHY: エクスポーターが "rootmotion" 完全一致しか見ていなかった時代の .anim でも、
    //      再インポートせずに Hips / Armature 等からルートモーションを取り出せる。
    AutoDetect  = 1,
    // rootMotionNodeName で明示指定する。
    NodeName    = 2,
};

// クリップ側の軸フラグを Animator から上書きするための三値。
enum class RootMotionAxisOverride : int {
    UseClip  = 0,  // .anim に焼かれた値を使う
    Disabled = 1,  // この軸は抽出しない
    Enabled  = 2,  // この軸を抽出する
};

// mode == None のときにポーズのルート移動をどう扱うか。
enum class RootMotionPoseMode : int {
    // ルート移動をポーズから除去する (その場再生)。
    Strip = 0,
    // クリップのまま残す。ルートごと前進する DCC そのままの見た目になる。
    // WHY: 旧実装は「抽出オフ」にすると除去だけが残り、前進成分がどこにも行かず消滅した。
    Keep  = 1,
};

struct RootMotionSettings {
    RootMotionMode         mode        = RootMotionMode::ApplyToTransform;
    RootMotionSource       source      = RootMotionSource::ClipDefined;
    RootMotionPoseMode     poseMode    = RootMotionPoseMode::Strip;
    // source == NodeName のときに参照するトラック名。正規化一致で解決する。
    std::string            nodeName;
    // 適用先 GameObject への相対パス。空なら Animator 自身。
    // ".." を先頭に並べると親を遡れる (例: Animator が子メッシュ、RigidBody が親のとき "..")。
    std::string            targetPath;
    RootMotionAxisOverride applyXZ       = RootMotionAxisOverride::UseClip;
    RootMotionAxisOverride applyY        = RootMotionAxisOverride::UseClip;
    RootMotionAxisOverride applyRotation = RootMotionAxisOverride::UseClip;
    // 抽出した移動量 / 回転量へ掛ける倍率。アニメの歩幅とゲームの移動速度を合わせる調整用。
    float                  positionScale = 1.0f;
    float                  rotationScale = 1.0f;

    // 抽出そのものを行うか。None 以外なら常に抽出する。
    // 抽出する場合、ポーズからのルート成分除去は強制になる
    // (残すと Transform 移動とポーズ移動で二重に進む)。軸ごとの確定は
    // AnimatorSystem::ResolveRootMotion が行う。
    bool Extracts() const { return mode != RootMotionMode::None; }
};

// AnimatorSystem がクリップごとに保持する前フレームのサンプル。
// WHY: 旧実装は「支配クリップ 1 本 + ステート時刻の前後差分」で delta を求めていたため、
//      BlendTree のクリップが切り替わる / 遷移でステートが変わるたびに移動が飛んだり
//      1 フレーム落ちたりした。クリップ単位で自分の前回サンプル位置を覚えておけば、
//      ブレンド構成が毎フレーム変わっても各クリップの delta は連続する。
struct RootMotionClipSample {
    // 同一性キー。LoadClips でクリップ配列を作り直したときはキャッシュごと破棄する。
    const asset::AnimationClip* clip = nullptr;
    double           ticks    = 0.0;   // 前回サンプルした tick
    math::Vector3    position = math::Vector3::ZERO;
    math::Quaternion rotation = math::Quaternion::Identity();
    std::uint64_t    frame    = 0;     // 前回サンプルしたフレーム番号
};

// ── AnimatorComponent ────────────────────────────────────────────────────────

struct AnimatorComponent {
    // ── 後方互換フィールド（states が空のとき使用）──────────────────────────
    bool        enabled    = true;
    // 共有 Animator Controller アセットへの参照。空なら Component 内蔵定義を使う。
    std::string controllerPath;
    // ランタイム専用。参照変更時だけ Controller を再読み込みする。
    std::string loadedControllerPath;
    float       speed      = 1.0f;
    bool        playing    = true;
    // ルートモーションの適用先・解決方法・軸マスク。
    // VFX の決定論的 Preview は mode = None を使い、姿勢だけを評価して Transform を動かさない。
    RootMotionSettings rootMotion;

    // ステートから参照されないクリップ (演出専用) のソース。
    // LoadClips はステートと Additive 基準からしか集めないので、ここに宣言が無いと
    // Slot へ差し込んでも「参照が解決できない」として黙って畳まれる。
    std::vector<std::string> externalClipSources;

    // ランタイム専用。初回更新時にステート参照から再構築する。
    std::vector<asset::AnimationClip> clips;
    // clips と同じ添字でロード元を保持し、同名クリップを Source Path で識別する。
    std::vector<std::string> clipSourcePaths;
    bool clipsLoaded          = false;
    int  clipsAttemptGeneration = -1; // FlushGeneration at last LoadClips attempt
    // 最後に Controller / クリップ / マスクを取り込んだときのアセット世代。
    // これが AssetManager の現在値と食い違う間は、派生キャッシュが古い。
    int  appliedAssetGeneration = -1;

    std::vector<math::Matrix4> boneMatrices;
    std::vector<math::Matrix4> nodeGlobalTransforms;
    renderer::ResourceHandle<renderer::ConstantBufferTag> skinningBuffer;

    // 前フレームの確定ボーンパレット。VelocityPass が「前フレームの頂点位置」を
    // VS で組み直すために使う (b2 = CB_PREV_SKINNING へ束縛する)。
    //
    // WHY skinnedVertexBuffers の ping-pong にしないか:
    //   コンピュートスキニングの出力は GPU 書き込み頂点バッファ (BufferTag) で、
    //   VS から SRV として読める保証がない。DrawCall::vsBuffers は
    //   StructuredBufferTag しか受け付けないため、前フレームの頂点を頂点入力として
    //   持ち込む経路が存在しない。パレットを 2 本渡して VS で 2 回スキニングする方が
    //   バックエンドの制約に触れずに済む。
    //
    // WHY フレーム末ではなく次フレーム頭でスナップショットするか:
    //   boneMatrices の最終書き込み者は AnimatorSystem ではなく SpringBoneSystem。
    //   AnimatorSystem が上書きする直前に取れば、それが「前フレームの最終ポーズ」になる。
    std::vector<math::Matrix4> prevBoneMatrices;
    renderer::ResourceHandle<renderer::ConstantBufferTag> prevSkinningBuffer;
    // prevBoneMatrices が有効か。初回フレームは前フレームが存在しないので速度 0 にする。
    bool prevBoneMatricesValid = false;

    // ── ステートマシン定義（シリアライズ対象）──────────────────────────────
    std::string                    defaultStateName; // 初期ステート名。空なら states[0]
    std::vector<AnimationState>    states;
    // 現在ステートを問わず評価する割り込み遷移。通常遷移より後に評価する。
    std::vector<AnimationTransition> anyStateTransitions;
    std::vector<AnimatorParameter> parameters;
    // index 0 の Base Layer は既存 state machine が担い、この配列は追加 Layer のみを保持する。
    std::vector<AnimationLayer> layers;
    // Base Layer 自身のマスク。空なら全身に効く (従来動作)。
    // WHY: Unity 同様、Base Layer からも一部のボーンを外せるようにする。
    //      マスク外のボーンはバインドポーズのまま残り、上のレイヤーだけが動かす形になる。
    AnimationMaskRef baseLayerMask;
    // 現在の BlendTree 評価結果。Script/Editor のデバッグ表示に使用する。
    std::vector<std::pair<std::string, float>> currentBlendWeights;
    float currentBlendDuration = 0.0f;
    float currentIKWeight = 1.0f;

    // ── ランタイム専用（シリアライズしない）────────────────────────────────
    // 現在再生中のステート名。
    std::string currentStateName;
    float       stateTime     = 0.0f;  // 現ステートの再生秒数

    // 遷移中クロスフェード管理。blendToState が空でなければ遷移中
    std::string blendToState;
    float       blendToTime   = 0.0f;  // 遷移先ステートの再生秒数
    float       blendWeight   = 0.0f;  // 0=現ステート, 1=遷移先
    float       blendDuration = 0.25f; // 今回の遷移のクロスフェード時間キャッシュ
    // Animation Event の区間評価に使う前フレーム時刻。
    float       previousEventTime = 0.0f;
    std::string previousEventClipName;

    // ── Root Motion 出力 (ランタイム専用) ──────────────────────────────────
    // 適用先 GameObject のローカル空間での 1 フレーム移動量 / 回転量。
    math::Vector3    rootMotionDeltaPosition = math::Vector3::ZERO;
    math::Quaternion rootMotionDeltaRotation = math::Quaternion::Identity();
    // deltaPosition をワールド空間へ変換した値と、それを dt で割った速度。
    // WHY: Script 側で Time::deltaTime を掛け直すと Animator が実際に進めた時間とずれる。
    //      速度制御へそのまま渡せる形で公開する。
    math::Vector3    rootMotionWorldDelta    = math::Vector3::ZERO;
    math::Vector3    rootMotionWorldVelocity = math::Vector3::ZERO;
    float            rootMotionDeltaTime     = 0.0f;
    // エンジンが Transform / RigidBody へ適用したフレームは true。ExtractOnly では false。
    bool             rootMotionAppliedByEngine = false;
    // クリップごとの前フレームサンプル。BlendTree / 遷移をまたいでも delta を連続させる。
    std::vector<RootMotionClipSample> rootMotionSamples;
    // 当該フレームの Skeleton ルートノード名。AutoDetect のフォールバック解決に使う。
    // WHY: トラック解決は BuildStateClips (Skeleton を持たない) からも呼ばれるため、
    //      AnimatorSystem::Update が毎フレーム先頭でここへ焼いておく。
    std::string skeletonRootNodeName;

    std::vector<FiredAnimationEvent> firedEvents;

    const char* GetTypeName() const { return "Animator"; }

    void Reflect(IReflector& r)
    {
        r.Field("enabled",          enabled);
        r.Field("controllerPath",   controllerPath);
        r.Field("speed",            speed);
        r.Field("playing",          playing);
        r.Field("rootMotionMode",     reinterpret_cast<int&>(rootMotion.mode));
        r.Field("rootMotionSource",   reinterpret_cast<int&>(rootMotion.source));
        r.Field("rootMotionPoseMode", reinterpret_cast<int&>(rootMotion.poseMode));
        r.Field("rootMotionNodeName", rootMotion.nodeName);
        r.Field("rootMotionTarget",   rootMotion.targetPath);
        r.Field("rootMotionApplyXZ",  reinterpret_cast<int&>(rootMotion.applyXZ));
        r.Field("rootMotionApplyY",   reinterpret_cast<int&>(rootMotion.applyY));
        r.Field("rootMotionApplyRotation",
                reinterpret_cast<int&>(rootMotion.applyRotation));
        r.Field("rootMotionPositionScale", rootMotion.positionScale);
        r.Field("rootMotionRotationScale", rootMotion.rotationScale);
        r.Field("defaultStateName", defaultStateName);
    // states / parameters は vector のため SceneSerializer で直接変換する
    }

    // ── Script API ──────────────────────────────────────────────────────────
    // AnimatorSystem が毎フレーム更新するため、Set* はフィールド書き換えのみで十分。

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

    // Trigger は true にセットする。消費は AnimatorSystem::ConsumeTriggers() が行う。
    void SetTrigger(std::string_view name)
    {
        for (auto& p : parameters)
            if (p.name == name && p.type == ParamType::Trigger)
                { p.boolValue = true; return; }
    }

    [[nodiscard]] float GetFloat(std::string_view name) const
    {
        for (const auto& p : parameters)
            if (p.name == name && p.type == ParamType::Float) return p.floatValue;
        return 0.0f;
    }

    [[nodiscard]] int GetInt(std::string_view name) const
    {
        for (const auto& p : parameters)
            if (p.name == name && p.type == ParamType::Int) return p.intValue;
        return 0;
    }

    [[nodiscard]] bool GetBool(std::string_view name) const
    {
        for (const auto& p : parameters)
            if (p.name == name &&
                (p.type == ParamType::Bool || p.type == ParamType::Trigger))
                return p.boolValue;
        return false;
    }

    // 現在ステートの名前を確認する。ステートマシン未使用時は常に false。
    [[nodiscard]] bool IsInState(std::string_view stateName) const
    {
        return currentStateName == stateName;
    }

    // ── レイヤー API ────────────────────────────────────────────────────────
    // WHY: 上半身 / 下半身の出し分けはレイヤー操作が入口になる。
    //      名前引きを Script / Editor / MCP がそれぞれ書くと実装が散るため、ここに集約する。

    [[nodiscard]] AnimationLayer* FindLayer(std::string_view layerName)
    {
        for (auto& l : layers)
            if (l.name == layerName) return &l;
        return nullptr;
    }

    [[nodiscard]] const AnimationLayer* FindLayer(std::string_view layerName) const
    {
        for (const auto& l : layers)
            if (l.name == layerName) return &l;
        return nullptr;
    }

    void SetLayerWeight(std::string_view layerName, float w)
    {
        if (AnimationLayer* l = FindLayer(layerName))
            l->weight = std::clamp(w, 0.0f, MAX_LAYER_WEIGHT);
    }

    [[nodiscard]] float GetLayerWeight(std::string_view layerName) const
    {
        const AnimationLayer* l = FindLayer(layerName);
        return l ? l->weight : 0.0f;
    }

    // レイヤーの現在ステート名。
    [[nodiscard]] std::string GetLayerState(std::string_view layerName) const
    {
        const AnimationLayer* l = FindLayer(layerName);
        if (!l) return {};
        return l->runtime.currentStateName;
    }

    // 指定 Layer の現在ステートを、そのステートの実クリップ長で 0..1 に正規化する。
    // WHY: 武器のように「アニメーションの途中で親を差し替える」処理は、フレーム数や
    //      固定秒数ではなく、実際のクリップ再生位置へ同期しないと速度変更・遷移時間変更で
    //      手と銃の位置がずれる。Base Layer 用の GetNormalizedTime と同じ規則を Layer にも公開する。
    [[nodiscard]] float GetLayerNormalizedTime(std::string_view layerName) const
    {
        const AnimationLayer* layer = FindLayer(layerName);
        if (!layer || layer->runtime.currentStateName.empty()) return 0.0f;

        const AnimationState* state = nullptr;
        for (const auto& candidate : layer->states) {
            if (candidate.name == layer->runtime.currentStateName) {
                state = &candidate;
                break;
            }
        }
        if (!state) return 0.0f;
        if (state->mode != AnimationStateMode::Clip) {
            return layer->runtime.blendDuration > 0.0f
                ? std::clamp(layer->runtime.stateTime / layer->runtime.blendDuration, 0.0f, 1.0f)
                : 0.0f;
        }

        const asset::AnimationClip* clip = nullptr;
        if (!state->sourcePath.empty()) {
            for (size_t i = 0; i < clips.size(); ++i) {
                if (i >= clipSourcePaths.size() || clipSourcePaths[i] != state->sourcePath)
                    continue;
                if (!clip) clip = &clips[i];
                if (!state->clipName.empty() && clips[i].name == state->clipName) {
                    clip = &clips[i];
                    break;
                }
            }
        }
        if (!clip && !state->clipName.empty()) {
            for (const auto& candidate : clips)
                if (candidate.name == state->clipName) {
                    clip = &candidate;
                    break;
                }
        }
        if (!clip && state->clipIndex >= 0 &&
            state->clipIndex < static_cast<int>(clips.size()))
            clip = &clips[static_cast<size_t>(state->clipIndex)];
        if (!clip) return 0.0f;

        const float duration = static_cast<float>(clip->GetDurationSeconds());
        return duration > 0.0f
            ? std::clamp(layer->runtime.stateTime / duration, 0.0f, 1.0f)
            : 0.0f;
    }

    [[nodiscard]] bool IsLayerInState(std::string_view layerName, std::string_view stateName) const
    {
        return GetLayerState(layerName) == stateName;
    }

    // レイヤーのステートマシンを指定ステートへ即座に飛ばす (クロスフェードなし)。
    void PlayLayerState(std::string_view layerName, std::string_view stateName)
    {
        AnimationLayer* l = FindLayer(layerName);
        if (!l) return;
        // Controller の初回ロード前でも要求をランタイム状態へ保持する。
        // WHY: Script は AnimatorSystem より先に実行されるため、開始直後の入力で
        //      states がまだ空だと PlayLayerState が無言で消え、次のフレームの
        //      Controller 適用で再生要求を復元できなくなる。
        //      ApplyAnimatorControllerAsset は同名 Layer の runtime を引き継ぐので、
        //      ロード完了後にこの要求をそのまま実行できる。
        if (stateName.empty()) return;
        l->runtime.currentStateName = std::string(stateName);
        l->runtime.stateTime   = 0.0f;
        l->runtime.blendToState.clear();
        l->runtime.blendWeight = 0.0f;
    }

    // ── Slot API ────────────────────────────────────────────────────────────
    // 指定レイヤーへワンショットのクリップを差し込む。既に再生中なら差し替える。
    // WHY: 「上半身だけ攻撃を割り込ませて終わったら元に戻す」を 1 呼び出しで済ませる。
    void PlaySlot(std::string_view layerName,
                  std::string_view sourcePath,
                  std::string_view clipName,
                  float fadeIn  = 0.15f,
                  float fadeOut = 0.15f,
                  float slotSpeed = 1.0f,
                  bool  slotLoop = false)
    {
        AnimationLayer* l = FindLayer(layerName);
        if (!l) return;
        l->slot.sourcePath      = std::string(sourcePath);
        l->slot.clipName        = std::string(clipName);
        l->slot.fadeInDuration  = (std::max)(fadeIn, 0.0f);
        l->slot.fadeOutDuration = (std::max)(fadeOut, 0.0f);
        l->slot.speed           = slotSpeed;
        l->slot.loop            = slotLoop;
        l->slot.time            = 0.0f;
        l->slot.stopping        = false;
        l->slot.active          = true;
        // weight は 0 から立ち上げる。差し替え時も現在の被せ量から続けたいので保持する。
    }

    // Slot をフェードアウトさせる。fadeOut < 0 なら登録済みの値を使う。
    void StopSlot(std::string_view layerName, float fadeOut = -1.0f)
    {
        AnimationLayer* l = FindLayer(layerName);
        if (!l || !l->slot.active) return;
        if (fadeOut >= 0.0f) l->slot.fadeOutDuration = fadeOut;
        l->slot.stopping = true;
    }

    [[nodiscard]] bool IsSlotPlaying(std::string_view layerName) const
    {
        const AnimationLayer* l = FindLayer(layerName);
        return l && l->slot.active && !l->slot.stopping;
    }

    // Slot の被せ量 0..1。フェードの進行を Script から見たいときに使う。
    [[nodiscard]] float GetSlotWeight(std::string_view layerName) const
    {
        const AnimationLayer* l = FindLayer(layerName);
        return l ? l->slot.weight : 0.0f;
    }

    // 現在ステート（遷移中はブレンド込み）の IK Weight を返す。
    // WHY: Run/JumpUp などステートごとに IK 影響度が変わるため、
    //      クロスフェード中も滑らかに補間する必要がある。
    [[nodiscard]] float GetCurrentIKWeight() const
    {
        return currentIKWeight;
    }

    // 現在ステートの再生位置を 0..1 で返す。クリップ情報が取れない場合は 0。
    // WHY: Script から「アニメーションが何割進んだか」を確認する共通手段として提供する。
    [[nodiscard]] float GetNormalizedTime() const
    {
        return NormalizedTimeForState(currentStateName, stateTime);
    }

    // クロスフェード遷移先 (blendToState) の再生位置を 0..1 で返す。遷移中でなければ 0。
    // WHY: コンボのクロスフェード中は currentState が前段のままのため、次段の進行度を
    //      正しく知るには遷移先ステートの blendToTime を別途参照する必要がある。
    [[nodiscard]] float GetBlendToNormalizedTime() const
    {
        return NormalizedTimeForState(blendToState, blendToTime);
    }

    [[nodiscard]] const std::string& GetBlendToState() const { return blendToState; }

private:
    // 指定ステート名・再生秒数から正規化時間 0..1 を求める。GetNormalizedTime /
    // GetBlendToNormalizedTime が現ステートと遷移先ステートそれぞれに対して使う。
    [[nodiscard]] float NormalizedTimeForState(const std::string& stateName, float time) const
    {
        if (stateName.empty()) return 0.0f;
        const AnimationState* st = nullptr;
        for (const auto& s : states)
            if (s.name == stateName) { st = &s; break; }
        if (!st) return 0.0f;
        if (st->mode != AnimationStateMode::Clip) {
            return currentBlendDuration > 0.0f
                ? std::clamp(time / currentBlendDuration, 0.0f, 1.0f)
                : 0.0f;
        }
        // 再生側と同じく、名前の完全一致 → 大文字小文字無視部分一致 → index で解決する。
        const asset::AnimationClip* clip = nullptr;
        if (!st->sourcePath.empty()) {
            for (size_t i = 0; i < clips.size(); ++i) {
                if (i >= clipSourcePaths.size() ||
                    clipSourcePaths[i] != st->sourcePath) continue;
                if (!clip) clip = &clips[i];
                if (!st->clipName.empty() && clips[i].name == st->clipName) {
                    clip = &clips[i];
                    break;
                }
            }
        }
        if (!clip && !st->clipName.empty()) {
            for (const auto& c : clips)
                if (c.name == st->clipName) { clip = &c; break; }
        }
        if (!clip && !st->clipName.empty()) {
            auto toLower = [](std::string value) {
                for (char& ch : value)
                    ch = static_cast<char>(
                        std::tolower(static_cast<unsigned char>(ch)));
                return value;
            };
            const std::string target = toLower(st->clipName);
            for (const auto& c : clips)
                if (toLower(c.name).find(target) != std::string::npos)
                    { clip = &c; break; }
        }
        if (!clip && st->clipIndex >= 0 &&
            st->clipIndex < static_cast<int>(clips.size()))
            clip = &clips[static_cast<size_t>(st->clipIndex)];
        if (!clip) return 0.0f;
        const float dur = static_cast<float>(clip->GetDurationSeconds());
        return dur > 0.0f ? std::clamp(time / dur, 0.0f, 1.0f) : 0.0f;
    }
};

} // namespace fbzz::scene
