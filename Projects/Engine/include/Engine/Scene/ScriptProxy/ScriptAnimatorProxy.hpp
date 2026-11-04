/// @file    ScriptAnimatorProxy.hpp
/// @brief   Script から AnimatorComponent を操作するショートハンド。
/// @author  Hasegawa Jin
/// @date    2026-06-01
#pragma once
#include <Engine/Api.hpp>

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

    FBZZ_ENGINE_API void SetFloat(std::string_view name, float v) const;
    FBZZ_ENGINE_API void SetInt(std::string_view name, int v) const;
    FBZZ_ENGINE_API void SetBool(std::string_view name, bool v) const;
    FBZZ_ENGINE_API void SetTrigger(std::string_view name) const;
    FBZZ_ENGINE_API bool IsInState(std::string_view name) const;
    FBZZ_ENGINE_API void SetFloat(GameObject* go, std::string_view name, float v) const;
    FBZZ_ENGINE_API void SetInt(GameObject* go, std::string_view name, int v) const;
    FBZZ_ENGINE_API void SetBool(GameObject* go, std::string_view name, bool v) const;
    FBZZ_ENGINE_API void SetTrigger(GameObject* go, std::string_view name) const;
    FBZZ_ENGINE_API bool IsInState(GameObject* go, std::string_view name) const;

    FBZZ_ENGINE_API float       GetFloat(std::string_view name) const;
    FBZZ_ENGINE_API int         GetInt  (std::string_view name) const;
    FBZZ_ENGINE_API bool        GetBool (std::string_view name) const;
    FBZZ_ENGINE_API float       GetNormalizedTime() const;
    FBZZ_ENGINE_API float       GetFloat(GameObject* go, std::string_view name) const;
    FBZZ_ENGINE_API int         GetInt  (GameObject* go, std::string_view name) const;
    FBZZ_ENGINE_API bool        GetBool (GameObject* go, std::string_view name) const;
    FBZZ_ENGINE_API float       GetNormalizedTime(GameObject* go) const;
    FBZZ_ENGINE_API std::vector<std::pair<std::string, float>> GetCurrentBlendWeights() const;
    FBZZ_ENGINE_API std::string GetCurrentState() const;
    FBZZ_ENGINE_API std::string GetCurrentState(GameObject* go) const;
    /// @return クロスフェード遷移先ステート名。遷移中でなければ空。
    /// @note コンボの Slash→Slash 遷移中に次段の振りタイミングを判定するために使う。
    FBZZ_ENGINE_API std::string GetBlendToState() const;
    FBZZ_ENGINE_API std::string GetBlendToState(GameObject* go) const;
    FBZZ_ENGINE_API float       GetBlendToNormalizedTime() const;
    FBZZ_ENGINE_API float       GetBlendToNormalizedTime(GameObject* go) const;
    FBZZ_ENGINE_API void        SetSpeed(float speed) const;
    /// @note Freeze 用の speed と独立した時計倍率 [0,8]。1 で通常。
    FBZZ_ENGINE_API void SetLocalTimeScale(float scale) const;
    FBZZ_ENGINE_API float       GetSpeed() const;
    FBZZ_ENGINE_API void        Play(std::string_view stateName) const;
    FBZZ_ENGINE_API void        SetSpeed(GameObject* go, float speed) const;
    FBZZ_ENGINE_API float       GetSpeed(GameObject* go) const;
    FBZZ_ENGINE_API void        Play(GameObject* go, std::string_view stateName) const;
    /// @brief 時間の進行そのものを止める。SetSpeed(0) と違い、再開時に速度を覚えておく必要がない。
    FBZZ_ENGINE_API void        SetPlaying(bool playing) const;
    FBZZ_ENGINE_API bool        IsPlaying() const;
    FBZZ_ENGINE_API void        SetLayerWeight(std::string_view layerName, float weight) const;
    FBZZ_ENGINE_API float       GetLayerWeight(std::string_view layerName) const;

    /// @name レイヤー制御 (上半身 / 下半身の出し分け)
    /// @{
    /// @return レイヤーの現在ステート名。独自ステートマシンを持たないレイヤーは stateName を返す。
    FBZZ_ENGINE_API std::string GetLayerState(std::string_view layerName) const;
    FBZZ_ENGINE_API bool        IsLayerInState(std::string_view layerName, std::string_view stateName) const;
    /// @brief レイヤーのステートマシンを指定ステートへ即座に飛ばす (クロスフェードなし)。
    FBZZ_ENGINE_API void        PlayLayerState(std::string_view layerName, std::string_view stateName) const;
    /// @brief レイヤーのマスクを差し替える (.mask アセットのパス)。空文字列でマスク解除。
    /// @note 同じ上半身レイヤーを武器種で「腕だけ」「腕＋頭」と切り替える用途がある。
    FBZZ_ENGINE_API void        SetLayerMask(std::string_view layerName, std::string_view maskPath) const;
    FBZZ_ENGINE_API std::string GetLayerMask(std::string_view layerName) const;
    /// @}

    /// @name Slot (ワンショット差し込み)
    /// @note 指定レイヤーへクリップを割り込ませる。終端に達すると自動でフェードアウトする。
    /// @note 例: 移動を流したまま上半身レイヤーへ攻撃モーションを差し込む。
    /// @{
    FBZZ_ENGINE_API void  PlaySlot(std::string_view layerName, std::string_view sourcePath,
                   std::string_view clipName,
                   float fadeIn = 0.15f, float fadeOut = 0.15f,
                   float speed = 1.0f, bool loop = false) const;
    FBZZ_ENGINE_API void  StopSlot(std::string_view layerName, float fadeOut = -1.0f) const;
    FBZZ_ENGINE_API bool  IsSlotPlaying(std::string_view layerName) const;
    FBZZ_ENGINE_API float GetSlotWeight(std::string_view layerName) const;
    /// @brief 鳴っている Slot の再生速度を途中で書き換える (PlaySlot の speed を上書き)。
    /// @note «溜めはゆっくり・斬り抜けは速く» のように 1 本のクリップの中で緩急を付ける用途。
    /// @note Slot が鳴っていなければ何もしない。
    FBZZ_ENGINE_API void  SetSlotSpeed(std::string_view layerName, float speed) const;
    /// @return Slot のクリップ内の再生秒数。鳴っていなければ 0。
    FBZZ_ENGINE_API float GetSlotTime(std::string_view layerName) const;
    /// @brief Base Layer のステートの再生速度を実行時に書き換える (.animcontroller の speed を上書き)。
    /// @note SetSpeed は Slot も含む Animator 全体に掛かる。走りを実速に合わせる等
    /// @note «そのステートだけ» 変えたいときに使う。コントローラーを読み直すと既定値へ戻る。
    FBZZ_ENGINE_API void  SetStateSpeed(std::string_view stateName, float speed) const;
    /// @return 見つからなければ 1。
    FBZZ_ENGINE_API float GetStateSpeed(std::string_view stateName) const;
    /// @}
    FBZZ_ENGINE_API void        SetMorphWeight(std::string_view morphName, float weight) const;
    FBZZ_ENGINE_API float       GetMorphWeight(std::string_view morphName) const;

    /// @name Root Motion
    /// @note 移動の権威を Script 側へ持ってくるときは Script::OnAnimatorMove() を使うこと。
    /// @note 下のアクセサは「エンジンが適用した結果を後から読む」用途。AnimatorSystem は
    /// @note Phase::LateUpdate に居るため、OnUpdate から呼ぶと 1 フレーム前の値になる。
    /// @{
    FBZZ_ENGINE_API math::Vector3    GetRootMotionDeltaPosition() const;   ///< @note ローカル空間の移動量
    FBZZ_ENGINE_API math::Quaternion GetRootMotionDeltaRotation() const;
    FBZZ_ENGINE_API math::Vector3    GetRootMotionWorldDelta() const;      ///< @note ワールド空間の移動量
    FBZZ_ENGINE_API math::Vector3    GetRootMotionWorldVelocity() const;   ///< @note ワールド空間の速度 (m/s)
    FBZZ_ENGINE_API float            GetRootMotionDeltaTime() const;
    /// @return エンジンが Transform / RigidBody へ適用済みか。ExtractOnly なら false。
    FBZZ_ENGINE_API bool             IsRootMotionAppliedByEngine() const;

    /// @brief ルートモーションの受け取り方を実行時に切り替える。
    /// @param mode 0=None / 1=ApplyToTransform / 2=ExtractOnly / 3=ApplyToRigidBody
    /// @note 「通常移動はスクリプト制御、攻撃中だけルートモーションに任せる」という
    /// @note 切り替えは Inspector の固定設定では表現できないため実行時 API がある。
    FBZZ_ENGINE_API void  SetRootMotionMode(int mode) const;
    FBZZ_ENGINE_API int   GetRootMotionMode() const;
    /// @brief 抽出したルートモーションへ掛ける倍率。アニメの歩幅とゲーム速度を合わせる調整用。
    FBZZ_ENGINE_API void  SetRootMotionPositionScale(float scale) const;
    FBZZ_ENGINE_API float GetRootMotionPositionScale() const;
    FBZZ_ENGINE_API void  SetRootMotionRotationScale(float scale) const;
    FBZZ_ENGINE_API float GetRootMotionRotationScale() const;
    /// @brief 名前指定でルートモーショントラックを差し替える。空文字列でクリップ指定へ戻す。
    FBZZ_ENGINE_API void        SetRootMotionNodeName(std::string_view nodeName) const;
    /// @return 空文字列ならクリップ指定のトラックを使っている。
    FBZZ_ENGINE_API std::string GetRootMotionNodeName() const;
    /// @}
};

} /// @note namespace fbzz::scene
