/// @file    PrefabPool.hpp
/// @brief   プレファブインスタンスの使い回し (オブジェクトプール)。
/// @author  Hasegawa Jin
/// @date    2026-08-16
///
/// @note 非アクティブな階層を再利用し、プレファブの解析・コンポーネント生成を戦闘中に繰り返さない。
/// @note Scene ごとの待機列と貸出列はプレファブの GUID で識別し、改名・移動後も共有する。
/// @warning Despawn した階層はシーンに残るため、Play 中の生成物を編集シーンへ保存しないこと。
#pragma once

#include <Engine/Scene/Entity.hpp>
#include <Math/Quaternion.hpp>
#include <Math/Vector3.hpp>
#include <cstddef>
#include <string>

namespace fbzz::scene {

class Scene;
class GameObject;

class PrefabPool {
public:
    /// @brief プールから 1 つ取り出す。空なら Instantiate して補充する。
    /// @return 失敗時 nullptr。取り出したインスタンスは activeSelf = true に戻り、指定の位置・回転に置かれる。
    /// @note OnSpawn の前に子孫までワールド座標を同期する。position / rotation はコールバックで変更されないよう値で受ける。
    static GameObject* Spawn(Scene& scene,
                             const std::string& prefabPath,
                             math::Vector3 position,
                             math::Quaternion rotation);

    /// @brief プールへ返す。非アクティブ化して待機列へ積む。
    /// @return prefabAssetPath が空 (プレファブ由来でない) 場合は false。呼び出し側は通常の Destroy にフォールバックすること。
    static bool Despawn(Scene& scene, GameObject& gameObject);

    /// @brief 事前生成。ロード画面など負荷を許容できるタイミングで呼び、戦闘中に Instantiate が走らないようにする。
    /// @return 実際に生成できた数。上限が設定されている場合、上限を超えるぶんは作らない。
    static int Prewarm(Scene& scene, const std::string& prefabPath, int count);

    /// @brief 同時に出ていられる数の上限。0 以下で無制限 (既定 = 従来の挙動)。
    /// @note 空なら無条件に Instantiate するため、撒く頻度が「枠数÷寿命」を超えると実体が増え続け、絵には出ないまま Tick が重くなる。
    /// @note 追い出しは最古を選ぶ。上限は GUID 単位に持ち、Scene 切り替えでも残る (ClearAll だけが捨てる)。
    static void SetLimit(const std::string& prefabPath, int maxLive);

    [[nodiscard]] static int GetLimit(const std::string& prefabPath);

    /// @brief 待機中インスタンス数 (デバッグ・チューニング用)。
    [[nodiscard]] static size_t AvailableCount(const Scene& scene, const std::string& prefabPath);

    /// @brief 貸し出し中インスタンス数 (デバッグ・チューニング用)。
    [[nodiscard]] static size_t LiveCount(const Scene& scene, const std::string& prefabPath);

    /// @brief 待機列を破棄する。シーン切り替え時に呼ぶ (GameObject 自体は Scene が破棄する)。
    static void Clear(const Scene& scene);
    static void ClearAll();
};

} /// @note namespace fbzz::scene
