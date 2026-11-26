/// @file    AnimatorControllerDefinition.hpp
/// @brief   Controller の保存対象だけを持つ定義型。再生状態や Component を参照しない。
/// @author  Hasegawa Jin
/// @date    2026-10-31
#pragma once
#include <Math/LinearAlgebra/Quaternion.hpp>
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
    std::string paramName; ///< 遷移条件の判定に使う Animator パラメーター名。
    ConditionOp op        = ConditionOp::True; ///< 現在のパラメーター値を判定する比較・論理演算。
    float       threshold = 0.0f; ///< Float/Int の比較閾値。Bool/Trigger では使わない
};

struct AnimationTransition {
    std::string                    toStateName; ///< 遷移の条件を満たしたときに入る状態名。
    bool                           hasExitTime        = false; ///< 現在の状態が設定した終了時刻に達するまで遷移を待つか。
    float                          exitTime           = 1.0f; ///< 0..1 正規化再生位置
    bool                           fixedDuration      = true; ///< true は秒、false は遷移元ステート Length に対する正規化割合として解釈する。
    float                          transitionDuration = 0.25f; ///< 現在の姿勢から遷移先へブレンドする時間 [s]。
    std::vector<AnimatorCondition> conditions; ///< この遷移が成立するため評価するパラメーター条件の列。
    bool                           interruptActiveBlend = false; ///< ベースのその場再生で、補間先ステートからこの遷移へ姿勢を引き継げる。
};

struct BlendTreeMotion {
    float       threshold = 0.0f; ///< 1D ブレンドでこのモーションに対応する入力値。
    float       posX      = 0.0f; ///< 2D ブレンド空間でのこのモーションの X 座標。
    float       posY      = 0.0f; ///< 2D ブレンド空間でのこのモーションの Y 座標。
    /// @note この Motion が直接参照するアニメーションソース。Animator 全体の Clip Sources と
    /// @note index の組み合わせを廃止し、Node 単体で参照を完結させる。
    std::string sourcePath; ///< このモーションのクリップを読み込むアセット参照。
    std::string clipName; ///< アセット内で参照するクリップの名前。
    int         clipIndex = -1; ///< クリップの配列内の添字。-1 の場合は名前で解決する。
    float       speed     = 1.0f; ///< このモーションまたは状態の再生時間に掛ける倍率。
    float       ikWeight  = 1.0f; ///< Motion ごとの IK 寄与率。BlendTree の姿勢 Weight と同じ比率で補間する
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
    std::string name; ///< 条件とブレンドツリーがパラメーターを引く名前。
    ParamType   type       = ParamType::Float; ///< パラメーターの保存値と比較方法を決める型。
    float       floatValue = 0.0f; ///< 浮動小数点パラメーターの値。
    int         intValue   = 0; ///< 整数パラメーターの値。
    bool        boolValue  = false; ///< 真偽値・トリガーパラメーターの真偽状態。
};

enum class AnimationLayerMode : int { Override = 0, Additive = 1 };

struct RetargetBoneMapping {
    std::string sourcePath; ///< 動きをサンプリングする元骨格ノードの相対階層パス。
    std::string targetPath; ///< 動きを適用する先の骨格ノードの相対階層パス。
    math::Quaternion rotationOffset = math::Quaternion::Identity(); ///< 元の回転を対象骨の向きへ揃える補正回転。
    float translationScale = 1.0f; ///< 元のノード位置の変化量に掛ける倍率。
};

struct AdditiveReferencePose {
    std::string sourcePath; ///< 空なら従来動作 (加算クリップ自身の先頭キー) にフォールバックする
    std::string clipName; ///< 加算差分を求める基準姿勢のクリップ名。
    float       time = 0.0f; ///< 基準として抜き出す秒位置
};
inline constexpr float MAX_LAYER_WEIGHT = 4.0f;
} // namespace fbzz::scene
namespace fbzz::asset {
struct BlendTree1DDefinition {
    std::string                  paramName; ///< 1D ブレンド空間の入力値を読むパラメーター名。
    /// @note パラメーターの急変を時間補間し、Idle/Walk/Run の姿勢が瞬時に切り替わるのを防ぐ。0 は
    /// @note 平滑化なし。値は目標へ約63%近づく時定数 [秒] として扱う。
    float                        dampTime = 0.0f; ///< ブレンド用パラメーターの変化を滑らかにする時間 [s]。
    bool                         syncNormalizedTime = false; ///< 全 Motion を同じ正規化位相で評価し、Weight が再上昇した Clip の位相ジャンプを防ぐ
    std::vector<scene::BlendTreeMotion> motions; ///< ブレンド空間上の各位置とそこで採用するクリップの対応。

};

struct BlendTree2DDefinition {
    std::string                  paramX; ///< 2D ブレンドの X 座標を読む Animator パラメーター名。
    std::string                  paramY; ///< 2D ブレンドの Y 座標を読む Animator パラメーター名。
    scene::BlendTree2DType              type = scene::BlendTree2DType::SimpleDirectional; ///< 2D 空間でモーションの重みを求める補間方式。
    /// @note パラメーターの急変を時間補間する (1D と同じ意味・時定数)。生値のままだと入力を離した
    /// @note 瞬間に不連続へ飛び、前進から後退への切り返し1フレームで前後クリップが入れ替わり脚が跳ねる。
    float                        dampTime = 0.0f; ///< ブレンド用パラメーターの変化を滑らかにする時間 [s]。
    /// @note 全 Motion を同じ正規化位相で評価し、Weight が再上昇した Clip の位相ジャンプを防ぐ。歩きと
    /// @note 走りなど長さの違う輪を重ねるとブレンド中に位相がずれて足が滑るため、方向だけでなく速さの
    /// @note 輪を足す構成では必須になる。
    bool                         syncNormalizedTime = false; ///< 異なる長さのクリップを正規化時刻で同期するか。
    std::vector<scene::BlendTreeMotion> motions; ///< ブレンド空間上の各位置とそこで採用するクリップの対応。

};

struct AnimationMaskDefinition {
    std::string path; ///< .mask アセットのパス。空ならマスクなし (全ボーンに効く)

};

struct AnimationSlotDefinition { float fadeInDuration = 0.15f; float fadeOutDuration = 0.15f; };

struct AnimationStateDefinition {
    std::string                      name; ///< 同じレイヤー内で遷移先を照合する状態名。
    scene::AnimationStateMode               mode = scene::AnimationStateMode::Clip; ///< 状態がクリップとブレンドツリーのどれを評価するか。
    std::string                      sourcePath; ///< Clip State が直接参照するアニメーションソース。BlendTree は Motion ごとに保持する
    std::string                      clipName; ///< アセット内で参照するクリップの名前。
    int                              clipIndex = -1; ///< クリップの配列内の添字。-1 の場合は名前で解決する。
    float                            speed    = 1.0f; ///< このモーションまたは状態の再生時間に掛ける倍率。
    bool                             loop     = true; ///< この状態の再生を末尾から先頭へ戻すか。
    /// @note このステートでの IK ブレンド量。0=FK のみ、1=フル IK。ステートごとに影響度を制御し、
    /// @note Run や JumpUp で地面スナップを段階的に抑える。
    float                            ikWeight = 1.0f; ///< この状態で IK 補正に掛ける重み。
    std::vector<scene::AnimationTransition> transitions; ///< この状態から出る遷移の定義列。
    BlendTree1DDefinition                      blendTree1D; ///< 1D ブレンド方式で使うパラメーターとモーションの定義。
    BlendTree2DDefinition                      blendTree2D; ///< 2D ブレンド方式で使うパラメーターとモーションの定義。
};

struct AnimationLayerDefinition {
    std::string name = "Layer"; ///< ツールや制御処理が対象のアニメーションレイヤーを照合する名前。
    float weight = 1.0f; ///< このレイヤーの姿勢を下位レイヤーへ重ねる重み。
    scene::AnimationLayerMode mode = scene::AnimationLayerMode::Override; ///< このレイヤーを上書きまたは加算で合成する方式。
    bool enabled = true; ///< このアニメーションレイヤーを評価するか。

    AnimationMaskDefinition mask; ///< マスク: .mask アセットへの参照。空でなければこちらを優先する。

    scene::AdditiveReferencePose additiveReference; ///< 加算レイヤーの基準ポーズ設定。

    /// @name レイヤー独自ステートマシン
    /// @{
    std::string defaultStateName; ///< このレイヤーの Entry から入る状態名。
    std::vector<AnimationStateDefinition> states; ///< このレイヤー内で遷移先として解決できる状態の定義。
    std::vector<scene::AnimationTransition> anyStateTransitions; ///< このレイヤーで現在状態に依存せず評価する遷移列。
    /// @}

    std::vector<scene::RetargetBoneMapping> retargetMappings; ///< 入力アニメーションと対象骨格のノード間の対応規則。

    AnimationSlotDefinition slot; ///< ワンショット再生スロット。

};
} // namespace fbzz::asset
