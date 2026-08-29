/// @file    PrefabPool.hpp
/// @brief   プレファブインスタンスの使い回し (オブジェクトプール)。
/// @author  Hasegawa Jin
/// @date    2026-08-16
///
/// 設計意図 (WHY):
/// Instantiate は「.prefab を読む → TOML を書き換える → SceneIO::AppendObjects →
/// 補完のため一時 Scene へ Deserialize → シーン全 GameObject の参照張り直し」まで行う。
/// エディタでオブジェクトを 1 個置く操作としては妥当なコストだが、弾やヒットエフェクトを
/// 毎フレーム出す用途には重すぎる。さらに AppendObjects は GameObject 配列を再確保するため、
/// 生成のたびに既存の GameObject* が無効化される (呼び出し元スクリプトの m_gameObject も含む)。
///
/// プールは「一度作ったインスタンスを非アクティブにして取っておき、次の要求で再利用する」
/// ことで、この重い経路と配列再確保の両方を回避する。定常状態ではファイル I/O も
/// メモリ確保も起きない。
///
/// バケットの鍵は GameObject::prefabAssetPath を使う。インスタンス化時に必ず書かれる
/// 値なので、プール側で別途「どの GO がどのプレファブ由来か」を持たなくてよい。
///
/// 注意: Despawn したインスタンスはシーンに残ったまま (非アクティブ) になる。
/// シーン保存では runtimeGenerated なオブジェクトとして扱われないため、
/// プール由来のインスタンスは保存対象になり得る点に注意 (Play 中の生成物は
/// そもそも保存しない運用が前提)。
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
    // プールから 1 つ取り出す。空なら Instantiate して補充する。失敗時 nullptr。
    // 取り出したインスタンスは activeSelf = true に戻り、指定の位置・回転に置かれる。
    //
    // WHY position / rotation を値で受けるか: プールが空だと内部で Instantiate が走り、
    //     Scene の GameObject 配列が再確保される。呼び出し側が「別 GameObject の
    //     transform.worldPosition」を渡していた場合、参照で受けているとその時点で
    //     参照先が無効になる。値でコピーしておけば再確保の前後で安全。
    static GameObject* Spawn(Scene& scene,
                             const std::string& prefabPath,
                             math::Vector3 position,
                             math::Quaternion rotation);

    // プールへ返す。非アクティブ化して待機列へ積む。
    // prefabAssetPath が空 (プレファブ由来でない) の場合は false を返すので、
    // 呼び出し側は通常の Destroy にフォールバックすること。
    static bool Despawn(Scene& scene, GameObject& gameObject);

    // 事前生成。ロード画面など負荷を許容できるタイミングで呼び、
    // 戦闘中に Instantiate が走らないようにする。実際に生成できた数を返す。
    static int Prewarm(Scene& scene, const std::string& prefabPath, int count);

    // 待機中インスタンス数 (デバッグ・チューニング用)。
    [[nodiscard]] static size_t AvailableCount(const Scene& scene, const std::string& prefabPath);

    // 待機列を破棄する。シーン切り替え時に呼ぶ (GameObject 自体は Scene が破棄する)。
    static void Clear(const Scene& scene);
    static void ClearAll();
};

} // namespace fbzz::scene
