/// @file    ScriptRandomProxy.hpp
/// @brief   Script から擬似乱数を引く。
/// @author  Hasegawa Jin
/// @date    2026-08-16

/// @note グローバルストリーム (この Proxy のメソッド) と独立ストリーム (`util::RandomStream` をフィールドに持つ)
/// @note の 2 段構え。演出のばらつきのように毎回違ってよいものはグローバルで足りるが、シードを固定したい
/// @note ステージ生成・敵の湧きは、グローバルだと他スクリプトの draw 回数に汚染されるため独立ストリームを使う。
#pragma once
#include <Engine/Api.hpp>

#include <Engine/Util/Random.hpp>
#include <Math/Vector2.hpp>
#include <Math/Vector3.hpp>
#include <cstdint>

namespace fbzz::scene {

class Script;

/// @note スクリプトが RandomStream をそのままフィールドに書けるよう別名を通す。
using RandomStream = util::RandomStream;

struct ScriptRandomProxy {
    Script* script = nullptr;

    /// @note [0, 1) の float
    [[nodiscard]] FBZZ_ENGINE_API float Value() const;
    /// @note [min, max) の float
    [[nodiscard]] FBZZ_ENGINE_API float Range(float min, float max) const;
    /// @note [min, max] の int (両端含む)
    [[nodiscard]] FBZZ_ENGINE_API int Range(int min, int max) const;
    /// @note probability の確率で true (0.25f なら 25%)
    [[nodiscard]] FBZZ_ENGINE_API bool Chance(float probability) const;
    /// @note 符号をランダムに返す (+1 / -1)。左右振り分けなどに使う。
    [[nodiscard]] FBZZ_ENGINE_API float Sign() const;

    [[nodiscard]] FBZZ_ENGINE_API math::Vector2 InsideUnitCircle() const;
    [[nodiscard]] FBZZ_ENGINE_API math::Vector2 OnUnitCircle() const;
    [[nodiscard]] FBZZ_ENGINE_API math::Vector3 InsideUnitSphere() const;
    [[nodiscard]] FBZZ_ENGINE_API math::Vector3 OnUnitSphere() const;
    /// @note 指定軸まわりに maxAngleDeg 以内でばらつかせた方向。射撃の散らばり等。
    [[nodiscard]] FBZZ_ENGINE_API math::Vector3 ConeDirection(const math::Vector3& axis, float maxAngleDeg) const;

    /// @note グローバルストリームのシードを固定する。デバッグ再現用。
    FBZZ_ENGINE_API void SetSeed(uint32_t seed) const;

    /// @note 独立ストリームを作る。フィールドに持たせて使う。
    [[nodiscard]] RandomStream MakeStream(uint64_t seed) const { return RandomStream(seed); }
};

} /// @note namespace fbzz::scene
