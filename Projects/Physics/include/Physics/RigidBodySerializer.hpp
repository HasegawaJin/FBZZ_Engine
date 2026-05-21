// FBZZ Engine
// RigidBodySerializer.hpp | fbzz::physics
// RigidBody の TOML シリアライズ / デシリアライズ
#pragma once
#include <Physics/RigidBody.hpp>
#include <string>
#include <string_view>

namespace fbzz::physics
{

// TOML 形式の文字列に変換・復元するユーティリティ。
// 保存対象: 状態 (position/velocity/rotation/angular_velocity) と
//           設定 (mass/is_static/charge/gravitational)。
// m_force・m_torque・m_userData は対象外 (一時値またはポインタ)。
class RigidBodySerializer
{
public:
    // RigidBody を TOML 文字列に変換する。失敗時は空文字列を返す。
    static std::string Serialize(const RigidBody& body);

    // TOML 文字列から body の状態・設定を上書きする。
    // 成功時 true を返す。
    static bool Deserialize(RigidBody& body, std::string_view tomlText);
};

} // namespace fbzz::physics
