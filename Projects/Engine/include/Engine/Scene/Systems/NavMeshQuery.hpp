/// @file    NavMeshQuery.hpp
/// @brief   NavMesh に対する純粋なクエリ (最近傍ポリゴン / A* / Funnel 平滑化 / 高さサンプル)。
/// @author  Hasegawa Jin
/// @date    2026-08-14
///
/// @note 元は NavigationSystem.cpp の無名名前空間で Agent 更新からしか呼べず、AI/Editor から
///       «歩けるか» を確かめる手段が無かった。ランタイム本体と同じコードへ問い合わせできるよう出す。
/// @warning 別実装で経路を引き直さない。A* コスト・areaMask・Funnel 判定が違うと
///          「クエリでは通れるが Agent は通れない」食い違いが生まれる。
#pragma once
#include <Engine/Scene/Components/NavMeshSurfaceComponent.hpp>
#include <Math/Vector3.hpp>
#include <vector>

namespace fbzz::scene {

/// 指定座標を含むポリゴンを返す。範囲外なら最も近いポリゴンへフォールバックする
/// (Agent が NavMesh の境界からわずかに外れているケースを許容する)。
/// ポリゴンが 1 つも無ければ -1。
int FindNearestPolygon(const NavMesh& navMesh, const math::Vector3& pos);

/// ポリゴン隣接グラフ上の A* 探索。
/// areaMask:    ビット i が立っているとき areaType==i のポリゴンを通過可 (-1 = 全通過)
/// areaCosts:   nullptr のとき全コスト 1.0f。areaCosts[areaType] がエッジ重みに掛かる (要素数 32)
/// agentTypeId: オフメッシュリンクの agentTypeMask フィルタリングに使う
bool FindPolygonPath(const NavMesh& navMesh, int startPoly, int goalPoly, std::vector<int>& outPath,
                     int areaMask, const float* areaCosts, int agentTypeId);

/// ポリゴン経路から Funnel Algorithm (Simple Stupid Funnel Algorithm) で
/// 直線最短パスへ平滑化する。先頭要素は startPos そのもの。
std::vector<math::Vector3> BuildFunnelPath(const NavMesh& navMesh, const std::vector<int>& polyPath,
                                           const math::Vector3& startPos, const math::Vector3& goalPos);

/// NavMesh 面上の XZ 座標から Y 高さをバリセントリック補間で返す。
/// 最近傍ポリゴンが見つからない場合は -1e7f を返す (呼び出し側が「面の外」と判定できる番兵)。
float SampleNavMeshHeight(const NavMesh& navMesh, const math::Vector3& pos);

} // namespace fbzz::scene
