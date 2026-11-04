/// @file    AnimatorControllerDefinition.hpp
/// @brief   Controller の保存対象だけを持つ定義型。再生状態や Component を参照しない。
/// @author  Hasegawa Jin
/// @date    2026-10-31
#pragma once
#include <Math/Quaternion.hpp>
#include <string>
#include <vector>
namespace fbzz::scene {
enum class ConditionOp : int {
    Greater  = 0,
    Less     = 1,
    Equal    = 2,
    NotEqual = 3,
    True     = 4,
    False    = 5,
};

struct AnimatorCondition {
    std::string paramName;
    ConditionOp op        = ConditionOp::True;
    float       threshold = 0.0f; ///< @note Float/Int の比較閾値。Bool/Trigger では使わない
};

struct AnimationTransition {
    std::string                    toStateName;
    bool                           hasExitTime        = false;
    float                          exitTime           = 1.0f;   ///< @note 0..1 正規化再生位置
    /// @note true は秒、false は遷移元ステート Length に対する正規化割合として解釈する。
    bool                           fixedDuration      = true;
    float                          transitionDuration = 0.25f;
    std::vector<AnimatorCondition> conditions;
};

struct BlendTreeMotion {
    float       threshold = 0.0f;
    float       posX      = 0.0f;
    float       posY      = 0.0f;
    /// @note この Motion が直接参照するアニメーションソース。Animator 全体の Clip Sources と
    /// @note index の組み合わせを廃止し、Node 単体で参照を完結させる。
    std::string sourcePath;
    std::string clipName;
    int         clipIndex = -1;
    float       speed     = 1.0f;
    float       ikWeight  = 1.0f; ///< @note Motion ごとの IK 寄与率。BlendTree の姿勢 Weight と同じ比率で補間する
};

enum class BlendTree2DType : int {
    SimpleDirectional = 0,
    FreeformCartesian = 1
};

enum class AnimationStateMode : int {
    Clip        = 0,
    BlendTree1D = 1,
    BlendTree2D = 2
};

enum class ParamType : int { Float = 0, Int = 1, Bool = 2, Trigger = 3 };

struct AnimatorParameter {
    std::string name;
    ParamType   type       = ParamType::Float;
    float       floatValue = 0.0f;
    int         intValue   = 0;
    bool        boolValue  = false;
};

enum class AnimationLayerMode : int { Override = 0, Additive = 1 };

struct RetargetBoneMapping {
    std::string sourcePath;
    std::string targetPath;
    math::Quaternion rotationOffset = math::Quaternion::Identity();
    float translationScale = 1.0f;
};

struct AdditiveReferencePose {
    std::string sourcePath; ///< @note 空なら従来動作 (加算クリップ自身の先頭キー) にフォールバックする
    std::string clipName;
    float       time = 0.0f; ///< @note 基準として抜き出す秒位置
};
inline constexpr float MAX_LAYER_WEIGHT = 4.0f;
} /// @note namespace fbzz::scene
namespace fbzz::asset {
struct BlendTree1DDefinition {
    std::string                  paramName;
    /// @note パラメーターの急変を時間補間し、Idle/Walk/Run の姿勢が瞬時に切り替わるのを防ぐ。0 は
    /// @note 平滑化なし。値は目標へ約63%近づく時定数 [秒] として扱う。
    float                        dampTime = 0.0f;
    bool                         syncNormalizedTime = false; ///< @note 全 Motion を同じ正規化位相で評価し、Weight が再上昇した Clip の位相ジャンプを防ぐ
    std::vector<scene::BlendTreeMotion> motions;

};

struct BlendTree2DDefinition {
    std::string                  paramX;
    std::string                  paramY;
    scene::BlendTree2DType              type = scene::BlendTree2DType::SimpleDirectional;
    /// @note パラメーターの急変を時間補間する (1D と同じ意味・時定数)。生値のままだと入力を離した
    /// @note 瞬間に不連続へ飛び、前進から後退への切り返し1フレームで前後クリップが入れ替わり脚が跳ねる。
    float                        dampTime = 0.0f;
    /// @note 全 Motion を同じ正規化位相で評価し、Weight が再上昇した Clip の位相ジャンプを防ぐ。歩きと
    /// @note 走りなど長さの違う輪を重ねるとブレンド中に位相がずれて足が滑るため、方向だけでなく速さの
    /// @note 輪を足す構成では必須になる。
    bool                         syncNormalizedTime = false;
    std::vector<scene::BlendTreeMotion> motions;

};

struct AnimationMaskDefinition {
    std::string path; ///< @note .mask アセットのパス。空ならマスクなし (全ボーンに効く)

};

struct AnimationSlotDefinition { float fadeInDuration = 0.15f; float fadeOutDuration = 0.15f; };

struct AnimationStateDefinition {
    std::string                      name;
    scene::AnimationStateMode               mode = scene::AnimationStateMode::Clip;
    std::string                      sourcePath; ///< @note Clip State が直接参照するアニメーションソース。BlendTree は Motion ごとに保持する
    std::string                      clipName;
    int                              clipIndex = -1;
    float                            speed    = 1.0f;
    bool                             loop     = true;
    /// @note このステートでの IK ブレンド量。0=FK のみ、1=フル IK。ステートごとに影響度を制御し、
    /// @note Run や JumpUp で地面スナップを段階的に抑える。
    float                            ikWeight = 1.0f;
    std::vector<scene::AnimationTransition> transitions;
    BlendTree1DDefinition                      blendTree1D;
    BlendTree2DDefinition                      blendTree2D;
};

struct AnimationLayerDefinition {
    std::string name = "Layer";
    float weight = 1.0f;
    scene::AnimationLayerMode mode = scene::AnimationLayerMode::Override;
    bool enabled = true;

    /// @note マスク: .mask アセットへの参照。空でなければこちらを優先する。
    AnimationMaskDefinition mask;

    /// @note 加算レイヤーの基準ポーズ設定。
    scene::AdditiveReferencePose additiveReference;

    /// @name レイヤー独自ステートマシン
    /// @{
    std::string defaultStateName;
    std::vector<AnimationStateDefinition> states;
    std::vector<scene::AnimationTransition> anyStateTransitions;
    /// @}

    std::vector<scene::RetargetBoneMapping> retargetMappings;

    /// @note ワンショット再生スロット。
    AnimationSlotDefinition slot;

};
} /// @note namespace fbzz::asset
