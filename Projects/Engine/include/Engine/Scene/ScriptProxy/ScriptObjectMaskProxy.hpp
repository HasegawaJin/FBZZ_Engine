/// @file    ScriptObjectMaskProxy.hpp
/// @brief   Script から「この GameObject のシルエットをマスクへ描け」と申告する。
/// @author  Hasegawa Jin
/// @date    2026-08-28
///
/// エンジンは «形と RGBA» だけを描き、輪郭・グロー・壁越しシルエット等の意味づけは
/// マスクを読むカスタムパスへ委ねる (対象選定だけをエンジンが持つ設計)。
/// @note 申告は毎フレーム必要。«消す» 責任を持たせないことで、対象破棄・Play 停止・
///       無効化時の消し忘れ (EntityID 再利用で別 GameObject が印される) を防ぐ。
#pragma once

#include <Math/Vector4.hpp>

namespace fbzz::scene {

class Script;
class GameObject;

struct ScriptObjectMaskProxy {
    Script* script = nullptr;

    /// 自分 (と子) を今フレームだけマスクへ描く。
    /// @param color マスクの RGB。読む側との取り決めで意味が決まる
    /// @param value マスクの A へ入る 0〜1 のスカラー (輪郭なら太さ、グローなら強さ)
    void Set(const math::Vector4& color, float value = 1.0f) const;
    /// @param visibleOnly false なら手前に何かあっても描く (壁越しのシルエット用)
    void Set(GameObject& target, const math::Vector4& color, float value = 1.0f,
             bool includeChildren = true, bool visibleOnly = true) const;

    /// 申告をその場で取り下げる。呼ばなくても言うのをやめれば次のフレームで消えるが、
    /// 1 フレームぶん遅れる。撃破の瞬間のように «同じフレームで消したい» ときに使う。
    void Clear() const;
    void Clear(GameObject& target) const;
};

} // namespace fbzz::scene
