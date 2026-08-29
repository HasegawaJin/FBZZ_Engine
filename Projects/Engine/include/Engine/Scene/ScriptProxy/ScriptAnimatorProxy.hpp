/// @file    ScriptAnimatorProxy.hpp
/// @brief   Script から AnimatorComponent を操作するショートハンド。
/// @author  Hasegawa Jin
/// @date    2026-06-01
#pragma once

#include <Math/Quaternion.hpp>
#include <Math/Vector3.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fbzz::scene {

class GameObject;
class Script;

struct ScriptAnimatorProxy {
    Script* script = nullptr;

    void SetFloat(std::string_view name, float v) const;
    void SetInt(std::string_view name, int v) const;
    void SetBool(std::string_view name, bool v) const;
    void SetTrigger(std::string_view name) const;
    bool IsInState(std::string_view name) const;
    void SetFloat(GameObject* go, std::string_view name, float v) const;
    void SetInt(GameObject* go, std::string_view name, int v) const;
    void SetBool(GameObject* go, std::string_view name, bool v) const;
    void SetTrigger(GameObject* go, std::string_view name) const;
    bool IsInState(GameObject* go, std::string_view name) const;

    float       GetFloat(std::string_view name) const;
    int         GetInt  (std::string_view name) const;
    bool        GetBool (std::string_view name) const;
    float       GetNormalizedTime() const;
    float       GetFloat(GameObject* go, std::string_view name) const;
    int         GetInt  (GameObject* go, std::string_view name) const;
    bool        GetBool (GameObject* go, std::string_view name) const;
    float       GetNormalizedTime(GameObject* go) const;
    std::vector<std::pair<std::string, float>> GetCurrentBlendWeights() const;
    std::string GetCurrentState() const;
    std::string GetCurrentState(GameObject* go) const;
    // クロスフェード遷移先ステート名 (遷移中でなければ空) とその正規化時間 0..1。
    // コンボの Slash→Slash 遷移中に次段の振りタイミングを正しく判定するために使う。
    std::string GetBlendToState() const;
    std::string GetBlendToState(GameObject* go) const;
    float       GetBlendToNormalizedTime() const;
    float       GetBlendToNormalizedTime(GameObject* go) const;
    void        SetSpeed(float speed) const;
    float       GetSpeed() const;
    void        Play(std::string_view stateName) const;
    void        SetSpeed(GameObject* go, float speed) const;
    float       GetSpeed(GameObject* go) const;
    void        Play(GameObject* go, std::string_view stateName) const;
    // 時間の進行そのものを止める。SetSpeed(0) と違い、再開時に速度を覚えておく必要がない。
    void        SetPlaying(bool playing) const;
    bool        IsPlaying() const;
    void        SetLayerWeight(std::string_view layerName, float weight) const;
    float       GetLayerWeight(std::string_view layerName) const;

    // ── レイヤー制御 (上半身 / 下半身の出し分け) ─────────────────────────────
    // レイヤーの現在ステート名。独自ステートマシンを持たないレイヤーは stateName を返す。
    std::string GetLayerState(std::string_view layerName) const;
    bool        IsLayerInState(std::string_view layerName, std::string_view stateName) const;
    // レイヤーのステートマシンを指定ステートへ即座に飛ばす (クロスフェードなし)。
    void        PlayLayerState(std::string_view layerName, std::string_view stateName) const;
    // レイヤーのマスクを差し替える (.mask アセットのパス)。空文字列でマスク解除。
    // WHY: 同じ上半身レイヤーを、武器種によって「腕だけ」「腕＋頭」と切り替えたいことがある。
    void        SetLayerMask(std::string_view layerName, std::string_view maskPath) const;
    std::string GetLayerMask(std::string_view layerName) const;

    // ── Slot (ワンショット差し込み) ──────────────────────────────────────────
    // 指定レイヤーへクリップを割り込ませる。終端に達すると自動でフェードアウトする。
    // 例: 移動を流したまま上半身レイヤーへ攻撃モーションを差し込む。
    void  PlaySlot(std::string_view layerName, std::string_view sourcePath,
                   std::string_view clipName,
                   float fadeIn = 0.15f, float fadeOut = 0.15f,
                   float speed = 1.0f, bool loop = false) const;
    void  StopSlot(std::string_view layerName, float fadeOut = -1.0f) const;
    bool  IsSlotPlaying(std::string_view layerName) const;
    float GetSlotWeight(std::string_view layerName) const;
    void        SetMorphWeight(std::string_view morphName, float weight) const;
    float       GetMorphWeight(std::string_view morphName) const;

    // ── Root Motion ─────────────────────────────────────────────────────────
    // 移動の権威を Script 側へ持ってくるときは Script::OnAnimatorMove() を使うこと。
    // 下のアクセサは「エンジンが適用した結果を後から読む」用途で、AnimatorSystem が
    // Phase::LateUpdate に居るため OnUpdate から呼ぶと 1 フレーム前の値になる。
    math::Vector3    GetRootMotionDeltaPosition() const;   // ローカル空間の移動量
    math::Quaternion GetRootMotionDeltaRotation() const;
    math::Vector3    GetRootMotionWorldDelta() const;      // ワールド空間の移動量
    math::Vector3    GetRootMotionWorldVelocity() const;   // ワールド空間の速度 (m/s)
    float            GetRootMotionDeltaTime() const;
    // エンジンが Transform / RigidBody へ適用済みか。ExtractOnly なら false。
    bool             IsRootMotionAppliedByEngine() const;

    // ルートモーションの受け取り方を実行時に切り替える。
    // mode: 0=None / 1=ApplyToTransform / 2=ExtractOnly / 3=ApplyToRigidBody
    // WHY: 「通常移動はスクリプト制御、攻撃モーションの間だけルートモーションに任せる」
    //      という切り替えは Inspector の固定設定では表現できない。
    void  SetRootMotionMode(int mode) const;
    int   GetRootMotionMode() const;
    // 抽出したルートモーションへ掛ける倍率。アニメの歩幅とゲーム速度を合わせる調整用。
    void  SetRootMotionPositionScale(float scale) const;
    float GetRootMotionPositionScale() const;
    void  SetRootMotionRotationScale(float scale) const;
    float GetRootMotionRotationScale() const;
    // 名前指定でルートモーショントラックを差し替える。空文字列でクリップ指定へ戻す。
    void        SetRootMotionNodeName(std::string_view nodeName) const;
    // 空文字列ならクリップ指定のトラックを使っている。
    std::string GetRootMotionNodeName() const;
};

} // namespace fbzz::scene
