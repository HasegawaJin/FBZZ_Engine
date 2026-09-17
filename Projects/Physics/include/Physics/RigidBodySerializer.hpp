/// @file    RigidBodySerializer.hpp
/// @brief   RigidBody の TOML シリアライズ / デシリアライズ。
/// @author  Hasegawa Jin
/// @date    2026-05-22
#pragma once
#include <Physics/RigidBody.hpp>
#include <string>
#include <string_view>

namespace fbzz::physics
{

/// @brief TOML 形式の文字列に変換・復元するユーティリティ。
/// @note 保存対象は状態 (position/velocity/rotation/angular_velocity) と
///       設定 (mass/is_static/charge/gravitational)。m_force・m_torque・m_userData は
///       フレーム一時値または非所有ポインタなので保存しない。
class RigidBodySerializer
{
public:
    /// @brief RigidBody を TOML 文字列に変換する。デフォルト値の拡張プロパティは省略する。
    static std::string Serialize(const RigidBody& body);

    /// @brief TOML 文字列から body の状態・設定を上書きする。不足キーは既定値で補うため、
    ///        古いシーンデータも読みやすい。
    /// @return 成功時 true。
    static bool Deserialize(RigidBody& body, std::string_view tomlText);
};

} // namespace fbzz::physics
