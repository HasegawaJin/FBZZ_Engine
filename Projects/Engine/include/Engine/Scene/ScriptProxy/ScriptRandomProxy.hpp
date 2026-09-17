/// @file    ScriptRandomProxy.hpp
/// @brief   Script から擬似乱数を引く。
/// @author  Hasegawa Jin
/// @date    2026-08-16
///
/// @note グローバルストリーム (この Proxy のメソッド) と独立ストリーム (`util::RandomStream` をフィールドに持つ)
///       の 2 段構え。演出のばらつきのように毎回違ってよいものはグローバルで足りるが、シードを固定したい
///       ステージ生成・敵の湧きは、グローバルだと他スクリプトの draw 回数に汚染されるため独立ストリームを使う。
#pragma once

#include <Engine/Util/Random.hpp>
#include <Math/Vector2.hpp>
#include <Math/Vector3.hpp>
#include <cstdint>

namespace fbzz::scene {

class Script;

/// スクリプトが RandomStream をそのままフィールドに書けるよう別名を通す。
using RandomStream = util::RandomStream;

struct ScriptRandomProxy {
    Script* script = nullptr;

    /// [0, 1) の float
    [[nodiscard]] float Value() const;
    /// [min, max) の float
    [[nodiscard]] float Range(float min, float max) const;
    /// [min, max] の int (両端含む)
    [[nodiscard]] int Range(int min, int max) const;
    /// probability の確率で true (0.25f なら 25%)
    [[nodiscard]] bool Chance(float probability) const;
    /// 符号をランダムに返す (+1 / -1)。左右振り分けなどに使う。
    [[nodiscard]] float Sign() const;

    [[nodiscard]] math::Vector2 InsideUnitCircle() const;
    [[nodiscard]] math::Vector2 OnUnitCircle() const;
    [[nodiscard]] math::Vector3 InsideUnitSphere() const;
    [[nodiscard]] math::Vector3 OnUnitSphere() const;
    /// 指定軸まわりに maxAngleDeg 以内でばらつかせた方向。射撃の散らばり等。
    [[nodiscard]] math::Vector3 ConeDirection(const math::Vector3& axis, float maxAngleDeg) const;

    /// グローバルストリームのシードを固定する。デバッグ再現用。
    void SetSeed(uint32_t seed) const;

    /// 独立ストリームを作る。フィールドに持たせて使う。
    [[nodiscard]] RandomStream MakeStream(uint64_t seed) const { return RandomStream(seed); }
};

} // namespace fbzz::scene
