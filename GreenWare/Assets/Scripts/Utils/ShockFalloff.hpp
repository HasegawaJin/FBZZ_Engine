/// @file    ShockFalloff.hpp
/// @brief   盤面のどこかで起きた衝撃が、プレイヤーへどれだけ届くかの規約
/// @author  Hasegawa Jin
/// @date    2026-08-27
///
/// WHY 1 箇所へ集めるか:
///   カメラ揺れと振動が別々の式で減衰すると、«画面は静かなのに手だけ震える» 距離が
///   できる。«どこで起きたか» の答えが 2 つあると、プレイヤーはどちらも信じなくなる。
///   1 度だけ近さを出して、揺れにも振動にも同じ数を掛けるのが唯一の正しい使い方。
///
/// WHY 減衰の «聞き手» をプレイヤーにするか (カメラではなく):
///   パッドを持っているのはプレイヤー本人で、震えているのは «その体が受けた衝撃»。
///   カメラで測ると、寄りと引きを切り替えただけで同じ踏みつけの重さが変わる。
///
/// WHY 判定半径と別の値で持つか:
///   衝撃波に «当たった» のと «落ちてきたのが伝わった» のは別の出来事。届く範囲を
///   判定と同じにすると、避けきった瞬間だけ盤面が完全に無音になり、避けた手応えごと
///   消える。避けた側にも «すぐ横に落ちてきた» は必ず返す。
///
/// WHY 振動のマネージャーへ置かないか:
///   ScriptCodeGen は行頭の `namespace ` を拾うだけで閉じ括弧を追わない。登録マクロを
///   持つファイルに入れ子の namespace を書くと、そちらがスクリプトの登録名前空間として
///   書き出される (Combat/BossAnimParams.hpp と同じ理由でここへ出してある)。
#pragma once

#include <Engine/Scene/GameObject.hpp>
#include <Math/MathUtils.hpp>
#include <Math/Vector3.hpp>
#include <algorithm>

namespace sandbox::shock {

using fbzz::math::Vector3;

/// 距離を «近さ» [0,1] へ写す。plateau の内側は 1、range の外は 0、その間は線形。
///
/// WHY 内側に平らな領域を持つか: 真上に落ちてきた衝撃が 1 にならないと、
///     «避けきれなかったのに手応えが薄い» が起きる。既定の 0 は素の線形。
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
