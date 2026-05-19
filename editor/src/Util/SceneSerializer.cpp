// FBZZ Engine
// SceneSerializer.cpp | fbzz::editor
// シーンの TOML シリアライズ / デシリアライズ (実装は各コンポーネント追加時に拡充)
#include <editor/Util/SceneSerializer.hpp>
#include <engine/Core/Logger.hpp>

namespace fbzz::editor {

bool SceneSerializer::Save(const scene::Scene& /*scene*/, const std::string& path)
{
    FBZZ_LOG_WARN("SceneSerializer::Save: 未実装 (%s)", path.c_str());
    return false;
}

bool SceneSerializer::Load(scene::Scene& /*scene*/, const std::string& path)
{
    FBZZ_LOG_WARN("SceneSerializer::Load: 未実装 (%s)", path.c_str());
    return false;
}

std::string SceneSerializer::Serialize(const scene::Scene& /*scene*/)
{
    return "";
}

bool SceneSerializer::Deserialize(scene::Scene& /*scene*/, const std::string& /*toml*/)
{
    return false;
}

} // namespace fbzz::editor
