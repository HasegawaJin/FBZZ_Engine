// FBZZ Engine
// AnimatorComponent.hpp | fbzz::scene
// スケルタルアニメーション再生状態コンポーネント
// Model の AnimationClip を参照し、現在時刻や再生速度を保持する。
// 骨行列の計算と GPU 転送は AnimatorSystem が行う。
//
// ── ステートマシン設計 ──────────────────────────────────────────────────────────
// states が空のとき: clipIndex/clipName/time による後方互換モードで動作する。
// states が存在するとき: AnimatorSystem が InitStateMachine → UpdateStateMachine
//   → EvaluateNBlendedNodeRecursive の順で処理し、Clip / BlendTree を統一評価する。
// WHY: アーキテクチャを壊さずに Unity の Animator Controller 相当の機能を追加するため、
//      後方互換フラグではなく「states が空か否か」で分岐する設計を選んだ。
#pragma once

#include <Engine/Asset/AnimationClip.hpp>
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
    std::vector<BlendTreeMotion> motions;
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

// ── AnimatorComponent ────────────────────────────────────────────────────────

struct AnimatorComponent {
    // ── 後方互換フィールド（states が空のとき使用）──────────────────────────
    bool        enabled    = true;
    // 共有 Animator Controller アセットへの参照。空なら Component 内蔵定義を使う。
    std::string controllerPath;
    // ランタイム専用。参照変更時だけ Controller を再読み込みする。
    std::string loadedControllerPath;
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
    // clips と同じ添字でロード元を保持し、同名クリップを Source Path で識別する。
    std::vector<std::string> clipSourcePaths;
    bool clipsLoaded          = false;
    int  clipsAttemptGeneration = -1; // FlushGeneration at last LoadClips attempt

    std::vector<math::Matrix4> boneMatrices;
    std::vector<math::Matrix4> nodeGlobalTransforms;
    renderer::ResourceHandle<renderer::ConstantBufferTag> skinningBuffer;

    // ── ステートマシン定義（シリアライズ対象）──────────────────────────────
    std::string                    defaultStateName; // 初期ステート名。空なら states[0]
    std::vector<AnimationState>    states;
    // 現在ステートを問わず評価する割り込み遷移。通常遷移より後に評価する。
    std::vector<AnimationTransition> anyStateTransitions;
    std::vector<AnimatorParameter> parameters;
    // 現在の BlendTree 評価結果。Script/Editor のデバッグ表示に使用する。
    std::vector<std::pair<std::string, float>> currentBlendWeights;
    float currentBlendDuration = 0.0f;
    float currentIKWeight = 1.0f;

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
        r.Field("controllerPath",   controllerPath);
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
