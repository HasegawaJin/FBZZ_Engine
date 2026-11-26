/// @file    ScriptPatrolProxy.hpp
/// @brief   Script から NavMeshPatrolComponent の経路と実行状態を操作するプロキシ。
/// @author  Hasegawa Jin
/// @date    2026-07-15
#pragma once
#include <Engine/Api.hpp>

#include <Math/LinearAlgebra/Vector3.hpp>
#include <cstddef>
#include <cstdint>

namespace fbzz::scene {

class Script;

enum class ScriptPatrolMode : uint8_t {
    LOOP = 0,
    PING_PONG
};

struct ScriptPatrolProxy {
    Script* script = nullptr; ///< 巡回操作で実行スコープを照合する、非所有の Script 参照。

    FBZZ_ENGINE_API void SetEnabled(bool enabled) const;
    FBZZ_ENGINE_API void SetMode(ScriptPatrolMode mode) const;
    FBZZ_ENGINE_API void SetWaitTime(float seconds) const;
    /// @note クリア時は INLINE+LIVE を選ぶ。非アクティブなバインドのメタデータと参照先の GameObject は保持する。
    FBZZ_ENGINE_API void ClearWaypoints() const;
    /// @note 数値書き込みは INLINE+LIVE を選ぶ。点の個体参照で構成した経路は、追加の前に数値化する。
    FBZZ_ENGINE_API void AddWaypoint(const math::Vector3& position, float waitTime = -1.0f,
                     float speed = -1.0f) const;
    /// @brief ワールド空間の数値経由点を一つ置き換える。
    /// @return 添字が無効か現在の経路を解決できない場合は false。入力元と数値行は未変更。
    /// @note 点の個体参照で構成した経路は置き換え前に数値化し、他の点と属性を保持する。
    FBZZ_ENGINE_API bool SetWaypoint(size_t index, const math::Vector3& position) const;
    FBZZ_ENGINE_API void Restart(size_t startIndex = 0) const;
    [[nodiscard]] FBZZ_ENGINE_API int GetCurrentIndex() const;
    [[nodiscard]] FBZZ_ENGINE_API bool IsEnabled() const;
    /// @brief 選択中のランタイムまたは制作時の経路の点数を返す。
    [[nodiscard]] FBZZ_ENGINE_API size_t GetWaypointCount() const;
    /// @brief 経由点のワールド座標を取得する。失敗時は出力を変更しない。
    /// @return コンポーネント、経路、添字のいずれかが無効なら false。
    [[nodiscard]] FBZZ_ENGINE_API bool GetWaypoint(size_t index, math::Vector3& position) const;
};

}
