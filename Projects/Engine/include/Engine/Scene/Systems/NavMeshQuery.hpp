// FBZZ Engine
// NavMeshQuery.hpp | fbzz::scene
// NavMesh に対する純粋なクエリ (最近傍ポリゴン / A* / Funnel 平滑化 / 高さサンプル)。
//
// WHY 分離するか:
//   これらは元々 NavigationSystem.cpp の無名名前空間にあり、Agent の毎フレーム更新から
//   しか呼べなかった。しかし「この 2 点間を実際に歩けるのか」は、Play を回して敵の
//   挙動を眺める以外に確かめる手段が無く、AI (Command Bus の navmesh.path) からも
//   Editor のデバッグ表示からも到達できない。ここへ出すことで、ランタイムが実際に
//   使うのと同一のコードへ問い合わせできる。
//
//   別実装で経路を引き直してはならない。A* のコスト・areaMask の解釈・Funnel の
//   ポータル左右判定が少しでも違うと「クエリでは通れるのに Agent は通らない」という、
//   最も原因に辿り着けない食い違いが生まれる。
#pragma once
#include <Engine/Scene/Components/NavMeshSurfaceComponent.hpp>
#include <Math/Vector3.hpp>
#include <vector>

namespace fbzz::scene {

// 指定座標を含むポリゴンを返す。範囲外なら最も近いポリゴンへフォールバックする
// (Agent が NavMesh の境界からわずかに外れているケースを許容する)。
// ポリゴンが 1 つも無ければ -1。
int FindNearestPolygon(const NavMesh& navMesh, const math::Vector3& pos);

// ポリゴン隣接グラフ上の A* 探索。
// areaMask:    ビット i が立っているとき areaType==i のポリゴンを通過可 (-1 = 全通過)
// areaCosts:   nullptr のとき全コスト 1.0f。areaCosts[areaType] がエッジ重みに掛かる (要素数 32)
// agentTypeId: オフメッシュリンクの agentTypeMask フィルタリングに使う
bool FindPolygonPath(const NavMesh& navMesh, int startPoly, int goalPoly, std::vector<int>& outPath,
                     int areaMask, const float* areaCosts, int agentTypeId);

// ポリゴン経路から Funnel Algorithm (Simple Stupid Funnel Algorithm) で
// 直線最短パスへ平滑化する。先頭要素は startPos そのもの。
std::vector<math::Vector3> BuildFunnelPath(const NavMesh& navMesh, const std::vector<int>& polyPath,
                                           const math::Vector3& startPos, const math::Vector3& goalPos);

// NavMesh 面上の XZ 座標から Y 高さをバリセントリック補間で返す。
// 最近傍ポリゴンが見つからない場合は -1e7f を返す (呼び出し側が「面の外」と判定できる番兵)。
float SampleNavMeshHeight(const NavMesh& navMesh, const math::Vector3& pos);

} // namespace fbzz::scene
