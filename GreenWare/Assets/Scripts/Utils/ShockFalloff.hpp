/// @file    ShockFalloff.hpp
/// @brief   盤面のどこかで起きた衝撃が、プレイヤーへどれだけ届くかの規約
/// @author  Hasegawa Jin
/// @date    2026-08-27
///
/// @note カメラ揺れと振動を別式で減衰させると «画面は静かなのに手だけ震える» 距離ができる
///       ため、1 度だけ近さを出して両方に同じ数を掛ける。聞き手はカメラでなくプレイヤー
///       (パッドを持つ本人が受けた衝撃で、カメラは寄り引きで重さが変わってしまう)。届く
///       範囲は判定半径と別の値にし、避けきった側にも «すぐ横に落ちてきた» を返す (判定と
///       同じにすると避けた瞬間だけ盤面が完全に無音になる)。振動のマネージャーへ置かない
///       のは、ScriptCodeGen が行頭の `namespace ` だけを拾い閉じ括弧を追わず、登録マクロを
///       持つファイルへ入れ子 namespace を書くとそちらが登録名前空間になるため
///       (Combat/BossAnimParams.hpp と同じ理由)。
#pragma once

#include <Engine/Scene/GameObject.hpp>
#include <Math/MathUtils.hpp>
#include <Math/Vector3.hpp>
#include <algorithm>

namespace sandbox::shock {

using fbzz::math::Vector3;

/// 距離を «近さ» [0,1] へ写す。plateau の内側は 1、range の外は 0、その間は線形。
///
/// @note 内側に平らな領域を持つ。真上に落ちてきた衝撃が 1 にならないと «避けきれなかった
///       のに手応えが薄い» が起きる。既定の 0 は素の線形。
[[nodiscard]] inline float Falloff(float distance, float range, float plateau = 0.0f)
{
    const float outer = std::max(range, fbzz::math::EPSILON);
    const float inner = std::clamp(plateau, 0.0f, outer);
    if (distance <= inner) return 1.0f;
    if (distance >= outer) return 0.0f;
    return 1.0f - (distance - inner) / (outer - inner);
}

/// 出来事の場所からプレイヤーまでを測って近さへ写す。
/// プレイヤーが居なければ 0 (受け取る相手が居ないので、何も鳴らさないのが正しい)。
[[nodiscard]] inline float NearnessTo(const fbzz::scene::GameObject* player,
                                      const Vector3& point,
                                      float range, float plateau = 0.0f)
{
    if (!player) return 0.0f;
    return Falloff((player->transform.worldPosition - point).Length(), range, plateau);
}

} // namespace sandbox::shock
