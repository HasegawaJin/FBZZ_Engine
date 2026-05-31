// FBZZ Engine
// AnimatorComponent.hpp | fbzz::scene
// スケルタルアニメーション再生状態コンポーネント
// Model の AnimationClip を参照し、現在時刻や再生速度を保持する。
// 骨行列の計算と GPU 転送は AnimatorSystem が行う。
//
// ── ステートマシン設計 ──────────────────────────────────────────────────────────
// states が空のとき: clipIndex/clipName/time による後方互換モードで動作する。
// states が存在するとき: AnimatorSystem が InitStateMachine → UpdateStateMachine
//   → EvaluateBlendedNodeRecursive の順で処理し、2クリップ間クロスフェードを行う。
// WHY: アーキテクチャを壊さずに Unity の Animator Controller 相当の機能を追加するため、
//      後方互換フラグではなく「states が空か否か」で分岐する設計を選んだ。
#pragma once

#include <Engine/Asset/AnimationClip.hpp>
#include <Engine/Renderer/ResourceHandle.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/Matrix4.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace fbzz::scene {

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
    float                          transitionDuration = 0.25f;  // クロスフェード時間（秒）
    std::vector<AnimatorCondition> conditions;
};

// ステートマシンの1状態。アニメーションクリップと遷移リストを持つ。
// clipName が animator.clips 内の名前と一致しない場合は clipIndex で直接指定する。
// clipIndex = -1 のときは clipName のみで探す。
struct AnimationState {
    std::string                      name;
    std::string                      clipName;
    int                              clipIndex = -1;
    float                            speed    = 1.0f;
    bool                             loop     = true;
    // このステートでの IK ブレンド量。0=FK のみ、1=フル IK。
    // WHY: ステートごとに IK 影響度を制御し、Run や JumpUp で地面スナップを段階的に抑える。
    float                            ikWeight = 1.0f;
    std::vector<AnimationTransition> transitions;
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

// ── AnimatorComponent ────────────────────────────────────────────────────────

struct AnimatorComponent {
    // ── 後方互換フィールド（states が空のとき使用）──────────────────────────
    bool        enabled    = true;
    int         clipIndex  = 0;
    std::string clipName;
    float       time       = 0.0f;
    float       speed      = 1.0f;
    bool        loop       = true;
    bool        playing    = true;

    // アニメーションクリップを含む FBX ファイルパス。シリアライズ対象
    std::vector<std::string> clipSources;

    // ランタイム専用。初回更新時に clipSources から再構築する
    std::vector<asset::AnimationClip> clips;
    bool clipsLoaded = false;

    std::vector<math::Matrix4> boneMatrices;
    std::vector<math::Matrix4> nodeGlobalTransforms;
    renderer::ResourceHandle<renderer::ConstantBufferTag> skinningBuffer;

    // ── ステートマシン定義（シリアライズ対象）──────────────────────────────
    std::string                    defaultStateName; // 初期ステート名。空なら states[0]
    std::vector<AnimationState>    states;
    std::vector<AnimatorParameter> parameters;

    // ── ランタイム専用（シリアライズしない）────────────────────────────────
    // 現在再生中のステート名。空なら後方互換モード
    std::string currentStateName;
    float       stateTime     = 0.0f;  // 現ステートの再生秒数

    // 遷移中クロスフェード管理。blendToState が空でなければ遷移中
    std::string blendToState;
    float       blendToTime   = 0.0f;  // 遷移先ステートの再生秒数
    float       blendWeight   = 0.0f;  // 0=現ステート, 1=遷移先
    float       blendDuration = 0.25f; // 今回の遷移のクロスフェード時間キャッシュ

    const char* GetTypeName() const { return "Animator"; }

    void Reflect(IReflector& r)
    {
        r.Field("enabled",          enabled);
        r.Field("clipName",         clipName);
        r.Field("clipIndex",        clipIndex);
        r.Field("time",             time);
        r.Field("speed",            speed);
        r.Field("loop",             loop);
        r.Field("playing",          playing);
        r.Field("defaultStateName", defaultStateName);
        // clipSources / states / parameters は vector のため SceneSerializer で直接変換する
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

    // 現在ステート（遷移中はブレンド込み）の IK Weight を返す。
    // WHY: Run/JumpUp などステートごとに IK 影響度が変わるため、
    //      クロスフェード中も滑らかに補間する必要がある。
    [[nodiscard]] float GetCurrentIKWeight() const
    {
        auto findWeight = [this](const std::string& stateName) -> float {
            for (const auto& s : states)
                if (s.name == stateName) return s.ikWeight;
            return 1.0f;
        };
        const float weightA = findWeight(currentStateName);
        if (blendToState.empty()) return weightA;
        return weightA + (findWeight(blendToState) - weightA) * blendWeight;
    }

    // 現在ステートの再生位置を 0..1 で返す。クリップ情報が取れない場合は 0。
    // WHY: Script から「アニメーションが何割進んだか」を確認する共通手段として提供する。
    [[nodiscard]] float GetNormalizedTime() const
    {
        if (currentStateName.empty()) return 0.0f;
        // currentStateName のステートを探す
        const AnimationState* st = nullptr;
        for (const auto& s : states)
            if (s.name == currentStateName) { st = &s; break; }
        if (!st) return 0.0f;
        // clipIndex 直接指定 → clipName 名前検索 の順で解決
        const asset::AnimationClip* clip = nullptr;
        if (st->clipIndex >= 0 && st->clipIndex < static_cast<int>(clips.size()))
            clip = &clips[st->clipIndex];
        if (!clip) {
            for (const auto& c : clips)
                if (c.name == st->clipName) { clip = &c; break; }
        }
        if (!clip) return 0.0f;
        const double tps = clip->ticksPerSecond > 0.0 ? clip->ticksPerSecond : 30.0;
        const float dur = static_cast<float>(clip->durationTicks / tps);
        return dur > 0.0f ? std::clamp(stateTime / dur, 0.0f, 1.0f) : 0.0f;
    }
};

} // namespace fbzz::scene
