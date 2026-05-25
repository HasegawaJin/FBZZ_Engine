// FBZZ Engine
// Random.hpp | fbzz::util
// 擬似乱数ユーティリティ
// mt19937 ベースで数値範囲と単位円・単位球サンプリングを提供する。
// 再現性が必要な場面では SetSeed を先に呼ぶ。
#pragma once
#include <Math/Vector2.hpp>
#include <Math/Vector3.hpp>

namespace fbzz::util {

class Random {
public:
    // [min, max) の float
    static float Range(float min, float max);

    // [min, max] の int
    static int   Range(int min, int max);

    // [0, 1) の float
    static float Value();

    // 単位円の内側のランダム点 (半径 <= 1)
    static math::Vector2 InsideUnitCircle();

    // 単位円の円周上のランダム点
    static math::Vector2 OnUnitCircle();

    // 単位球の内側のランダム点 (半径 <= 1)
    static math::Vector3 InsideUnitSphere();

    // 単位球面上のランダム方向ベクトル
    static math::Vector3 OnUnitSphere();

    // シード設定 (再現性が必要なときに使う)
    static void SetSeed(unsigned int seed);
};

} // namespace fbzz::util
