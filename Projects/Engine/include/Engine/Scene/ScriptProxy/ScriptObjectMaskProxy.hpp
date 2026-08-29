/// @file    ScriptObjectMaskProxy.hpp
/// @brief   Script から「この GameObject のシルエットをマスクへ描け」と申告する。
/// @author  Hasegawa Jin
/// @date    2026-08-28
///
/// エンジンが描くのは «形と、書き手が決めた RGBA» だけ。それを輪郭にするのか、
/// 光らせるのか、ボカさない印にするのかは、マスクを読むカスタムパスが決める。
///
/// WHY 意味をエンジンが持たないか:
///   「誰を印すか」はゲームのルール (極を帯びている / 狙っている / 拾える) で、
///   「どう見せるか」はゲームの絵作り。前者だけをエンジンが持てば、輪郭・選択的
///   グロー・壁越しシルエット・被写界深度の対象指定が、どれも同じ 1 本で書ける。
///
/// WHY 掛け続けるのに毎フレーム呼ぶ必要があるか:
///   申告はそのフレームだけ有効。«消す» 責任を持たせると、対象が壊れた・Play を
///   止めた・スクリプトが無効化されたときに消し忘れが残り、EntityID の再利用で
///   別の GameObject が印され出す。出し続けたい間だけ言い続ける形にする。
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
